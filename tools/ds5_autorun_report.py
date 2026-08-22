#!/usr/bin/env python3
"""Verdicts for an unattended ds5_autorun.sh run.

Gaps are filtered the way the project's own pipeline filters them: only gaps with
outstanding >= 2 count. A gap whose credit window held one packet or none is the
producer pausing, not the link starving, and dropping that filter is what made
earlier numbers non-comparable. The reference arms below were computed with the
same rule, so the two sides of any comparison are the same measurement.

Cuts the daemon's per-gap wall-clock log by the run's block windows and pools
blocks into arms. Pooling is on ADDITIVE quantities -- event counts and exposed
seconds -- never on a mean of per-block rates, which would weight a short block
like a long one.

The gap log only records gaps at or above its arming threshold (55 ms), so the
bands below that are simply absent here; the >=60 and >=70 ms bands, which are
where every lever in this programme is judged, are exact.

    ds5_autorun_report.py /path/to/autorun-ptype-20260816-1900

Exit status is part of the output: 0 = a verdict was produced, 1 = the run could
not be read (missing files, or nothing survives the coverage filters), 2 = usage,
3 = the run was read but the verdict it asks for is REFUSED by the preregistration
(today that is lever=r36, which may not be judged on shared fixed-ms bins).
"""
import math
import re
import sys
from pathlib import Path


# The five preserved gameplay arms, per minute, at edges 30/40/50/60/70/80,
# recomputed with STARVE_MIN_OUT=2 over ds5-phase2-captures/. Quoting one of them
# as "the" reference is cherry-picking: between arms the spread is 1.1x at 50 ms,
# 1.25x at 60 ms and 5x at 80 ms, and ratchet1 is the extreme arm at both ends.
REFERENCE_ARMS = {
    "ratchet1": (671.2, 497.2, 261.1, 96.1, 25.5, 12.7),
    "ab_off1":  (500.6, 462.3, 255.7, 84.2, 17.0, 2.8),
    "ab_off2":  (477.1, 444.5, 240.2, 80.4, 18.0, 3.2),
    "ab_on1":   (499.0, 461.4, 254.8, 92.6, 19.4, 3.3),
    "ab_on2":   (512.4, 476.0, 263.9, 77.0, 15.4, 2.5),
}
REFERENCE_EDGES = (30, 40, 50, 60, 70, 80)
STARVE_MIN_OUT = 2


def poisson_ratio_ci(k1, t1, k2, t2, z=1.96):
    """95 % CI for (k1/t1)/(k2/t2). Log-scale normal approximation on the counts,
    which is the standard treatment for a ratio of two Poisson rates and needs no
    assumption about how the events are spread inside a block."""
    if k1 == 0 or k2 == 0 or t1 <= 0 or t2 <= 0:
        return None, None, None
    r = (k1 / t1) / (k2 / t2)
    se = math.sqrt(1.0 / k1 + 1.0 / k2)
    return r, r * math.exp(-z * se), r * math.exp(z * se)


def censored_log_slope(pts):
    """The old estimator, kept only as a sensitivity number: OLS on log(count)
    over the OCCUPIED bins. It cannot represent an empty bin at all."""
    p = [(x, k) for x, k in pts if k > 0]
    if len(p) < 2:
        return None
    n = float(len(p))
    sx = sum(x for x, _ in p)
    sy = sum(math.log(k) for _, k in p)
    sxx = sum(x * x for x, _ in p)
    sxy = sum(x * math.log(k) for x, k in p)
    den = n * sxx - sx * sx
    return (n * sxy - sx * sy) / den if den else None


def poisson_log_slope(pts, iters=60, tol=1e-10):
    """Slope of log(rate) against gap width, fitted as a Poisson GLM (log link).

    An empty tail bin is DATA, not a missing value: zero events at 80 ms is the
    strongest evidence there is that the tail decays fast. Ordinary least
    squares on log(count) cannot hold such a bin (log 0), so it silently fits
    only the occupied ones — and conditioning on k>0 lifts every thin bin to at
    least one event, which tilts the line FLAT and reports the halving constant
    systematically too slow. Over the 5-6 sparsely filled bins this window
    produces, that bias is the difference between passing and failing the
    +-30 % transfer criterion, i.e. it decides whether a lever verdict is
    declared transferable to gameplay.

    The 5.4 ms gameplay reference was fitted on bins with hundreds of events
    each, where both estimators agree to well inside a decimal; the divergence
    is a property of the rig's thin tail, not of the reference.

    Newton-Raphson on (a, b) of log mu = a + b*x. Two parameters, so the step is
    a closed-form 2x2 solve; x is centred first because x ~ 80 and x^2 ~ 6400
    otherwise make the information matrix needlessly ill-conditioned. Returns
    the slope per ms, or None if it did not converge.
    """
    if len(pts) < 2:
        return None
    xbar = sum(x for x, _ in pts) / float(len(pts))
    p = [(x - xbar, float(k)) for x, k in pts]
    # Start on OLS over log(k + 0.5): defined for empty bins and close enough
    # that Newton needs a handful of steps.
    n = float(len(p))
    sx = sum(x for x, _ in p)
    sy = sum(math.log(k + 0.5) for _, k in p)
    sxx = sum(x * x for x, _ in p)
    sxy = sum(x * math.log(k + 0.5) for x, k in p)
    den = n * sxx - sx * sx
    if not den:
        return None
    b = (n * sxy - sx * sy) / den
    a = (sy - b * sx) / n
    for _ in range(iters):
        s0 = s1 = h00 = h01 = h11 = 0.0
        for x, k in p:
            mu = math.exp(max(-60.0, min(60.0, a + b * x)))
            s0 += k - mu
            s1 += (k - mu) * x
            h00 += mu
            h01 += mu * x
            h11 += mu * x * x
        det = h00 * h11 - h01 * h01
        if det <= 0:
            return None
        da = (h11 * s0 - h01 * s1) / det
        db = (h00 * s1 - h01 * s0) / det
        a += da
        b += db
        if abs(da) < tol and abs(db) < tol:
            return b
    return None


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    d = Path(argv[1])
    win_f, gap_f = d / "windows.tsv", d / "gaps.log"
    for f in (win_f, gap_f):
        if not f.exists():
            print(f"missing {f}")
            return 1

    # WHICH lever a run moved decides which verdicts it is allowed to produce,
    # so it is read out of the run instead of assumed. ds5_autorun.sh writes
    # conditions.txt for exactly this reason; the directory name
    # (autorun-<lever>-<stamp>) is the fallback for runs taken before it did.
    lever = None
    cond = d / "conditions.txt"
    if cond.exists():
        m = re.search(r"\blever=(\S+)", cond.read_text(errors="ignore"))
        if m:
            lever = m.group(1)
    if lever is None:
        m = re.match(r"autorun-([A-Za-z0-9]+)-\d", d.name)
        if m:
            lever = m.group(1)
    print(f"run {d.name}   lever={lever or 'UNKNOWN'}")

    windows = []
    for line in win_f.read_text().splitlines():
        if not line.strip():
            continue
        f = line.split("\t")
        arm, s, e = f[0], f[1], f[2]
        wifi = int(f[3]) if len(f) > 3 else None
        fg = f[4] if len(f) > 4 else None
        # cores*10, sampled every 10 s through the block (absent in older runs)
        cores = int(f[5]) / 10.0 if len(f) > 5 and f[5].strip() else None
        # busy % of all cores over the block, from /proc/stat
        busy = int(f[6]) if len(f) > 6 and f[6].strip() else None
        windows.append((arm, int(s) * 1000, int(e) * 1000, wifi, fg, cores, busy))
    if not windows:
        print("no blocks recorded")
        return 1
    # The rig-time anchor must hang off the CONFIGURED first block, taken here
    # BEFORE the lost/partial and HOLE filters below can drop it: windows.tsv
    # rows are written sequentially from block 1, but the report's windows list
    # shrinks, and an anchor read off "the first survivor" is late by a whole
    # block whenever block 1 fell — which lands every rig q-sample in the
    # OPPOSITE (alternating) arm.
    rig0_ms = windows[0][1]

    gaps, truncated = [], 0
    for line in gap_f.read_text().splitlines():
        # G <epoch_ms> gap=<ms> out=<n> h=<handle>, or the daemon's wipe marker
        if not line.startswith("G "):
            continue
        if line.startswith("G TRUNCATED"):
            truncated += 1          # NEVER let this fall into the except below
            continue
        p = line.split()
        try:
            gaps.append((int(p[1]), int(p[2].split("=")[1]), int(p[3].split("=")[1])))
        except (IndexError, ValueError):
            continue
    if not gaps:
        print("gap log has no records — was /tmp/ds5_gaplog armed?")
        return 1
    gaps.sort()

    # The daemon's gap log is NOT a ring that sheds its oldest entries: at 256 KiB
    # gaplog_flush() reopens it O_TRUNC and throws the whole file away, leaving one
    # "G TRUNCATED" marker (ds5_txd.c:1664). So the hole lands wherever the wipe
    # happened — mid-run, mid-block — while the block keeps its full exposure in
    # the denominator. That is not a hypothetical: it ate 171 s of an OFF block on
    # 2026-08-16 19:55 and 89 s of one at 19:10, both times inflating ON/OFF.
    # A block that ended before the earliest surviving record is the other half of
    # the same failure and is still checked below.
    first_ms = gaps[0][0]
    lost = [w for w in windows if w[2] <= first_ms]
    partial = [w for w in windows if w[1] < first_ms < w[2]]
    if lost or partial:
        print(f"  !! the gap log starts at {first_ms} — it lost earlier records "
              f"(ring cap). Dropping {len(lost)} block(s) with no coverage and "
              f"{len(partial)} partially covered block(s).")
        windows = [w for w in windows if w[1] >= first_ms]
        if not windows:
            print("     nothing survives — rerun with per-block snapshots")
            return 1

    # Lost data shows up at a block's EDGES, not in its middle. A wipe deletes
    # everything written before it, so a block it hit has no records until the
    # wipe point; a rig that died leaves none after it. Interior quiet is DATA:
    # this link really does go 13-37 s at a stretch without a single gap over
    # 55 ms, in both arms, and an earlier version of this check threw those
    # blocks away as "holes" — discarding exactly the calmest evidence.
    HOLE_S = 20
    holed = []
    for w in list(windows):
        inside = sorted(g[0] for g in gaps if w[1] <= g[0] <= w[2])
        lead = (inside[0] - w[1]) / 1000.0 if inside else (w[2] - w[1]) / 1000.0
        tail = (w[2] - inside[-1]) / 1000.0 if inside else 0.0
        if lead >= HOLE_S or tail >= HOLE_S:
            holed.append((w, lead, tail))
            windows.remove(w)
    if truncated or holed:
        if truncated:
            print(f"  !! the daemon WIPED the gap log {truncated}x during this run "
                  f"(256 KiB cap, whole file discarded)")
        for w, lead, tail in holed:
            print(f"  !! dropping {w[0]} block at {w[1]//1000}: log covers it only from "
                  f"+{lead:.0f}s to -{tail:.0f}s — its events are gone but its seconds are not")
        if not windows:
            print("     nothing survives")
            return 1

    # A run whose rig did not deliver the offered load measured a starved
    # transport, and no ratio computed from it means anything. Say so loudly
    # rather than printing a tidy number over a broken premise.
    synth = (d / "synth.log")
    if synth.exists():
        last = [l for l in synth.read_text().splitlines() if l.startswith("[synth] DONE")]
        if last:
            print(f"rig: {last[-1][len('[synth] '):]}")
            if "drop=0" not in last[-1]:
                print("  !! the daemon dropped frames: delivery was short of the offered load,")
                print("     so these blocks describe a starved transport, not the lever.")

    EDGES = (60, 70, 80, 100)
    arms = {}
    for arm, s, e, wifi, fg, cores, busy in windows:
        a = arms.setdefault(arm, {"secs": 0.0, "wifi": [], "cores": [], "busy": [],
                                  **{k: 0 for k in EDGES}})
        if wifi is not None:
            a["wifi"].append(wifi)
        if cores is not None:
            a["cores"].append(cores)
        if busy is not None:
            a["busy"].append(busy)
        a["secs"] += (e - s) / 1000.0
        for ts, ms, out in gaps:
            if s <= ts <= e and out >= STARVE_MIN_OUT:
                for k in EDGES:
                    if ms >= k:
                        a[k] += 1

    # A rebind zeroes the daemon's own histograms and starts a new binding; the
    # gap log survives it, but a block that spans one was not one experiment.
    ledger = d / "ledger.log"
    if ledger.exists():
        rebinds = sum(1 for l in ledger.read_text(errors="ignore").splitlines()
                      if "template handle=" in l and "bound" in l)
        if rebinds:
            print(f"  !! {rebinds} rebind(s) during the run — the link flapped, "
                  f"so blocks spanning one mix two bindings")

    print(f"\n{len(windows)} blocks: " + ", ".join(f"{a}={sum(1 for w in windows if w[0]==a)}"
                                                   for a in sorted(arms)))
    for a in sorted(arms):
        w = arms[a]["wifi"]
        if w:
            print(f"  {a}: wlan0 rx {min(w)}-{max(w)} kbit/s per block"
                  + ("   (quiet radio)" if max(w) < 500 else "   (SHARED RADIO WAS BUSY)"))
        c = arms[a]["cores"]
        if c:
            # The gap ledger is timed by the daemon's event loop, so how many
            # cores it competed for belongs next to every rate on this page.
            print(f"  {a}: {min(c):.1f}-{max(c):.1f} cores online (sampled every 10 s)")
        b = arms[a]["busy"]
        if b:
            print(f"  {a}: {min(b)}-{max(b)} % cpu busy per block")
    if lever == "r36":
        print("  !! lever=r36 halves the send cadence in the ON arm (0x36 every 10.67 ms")
        print("     against 0x39 every 21.33 ms). The columns below are each arm's own")
        print("     measurement; they are NOT in the same currency and must not be divided.")
    print(f"{'edge':>6} " + " ".join(f"{a:>18}" for a in sorted(arms)))
    for k in EDGES:
        row = f">={k:3d}ms "
        for a in sorted(arms):
            v = arms[a]
            row += f"{v[k]:6d} ev {v[k]/v['secs']*60:7.1f}/min "
        print(row)

    # E1, the transfer criterion: gameplay's distribution decays smoothly across
    # the tail, halving every 5.4 ms when fitted over 44-80 ms. The per-gap log
    # only keeps gaps at or above its 55 ms arming threshold, so the fit here runs
    # over 55-85 ms — the overlapping part of the same slope. A synthetic workload
    # whose tail decays at a different rate is shaped by a different mechanism,
    # and no lever verdict measured under it transfers to gameplay.
    print("\nE1 tail shape (halving constant over 55-85 ms, Poisson fit over ALL six")
    print("bins including the empty ones; gameplay reference 5.4 ms):")
    # Amendment 7 again, and for the same reason as the ON/OFF ratio below: these
    # are shared fixed-ms bins. The r36 ON arm's gaps carry a different in-flight
    # term (B + 10.67 ms against B + 21.33 ms), so the 55-85 ms window cuts the
    # two arms' distributions in different places — the +-30 % transfer verdict
    # would then be reporting the cadence. It is a per-arm figure rather than a
    # ratio, which is exactly why it needs refusing explicitly: it is the last
    # fixed-bin number on this page an operator could still quote for an r36 run.
    if lever == "r36":
        print("  REFUSED for lever=r36 (Amendment 7, preregistered): same shared fixed-ms")
        print("  bins as the ratio below, so the +-30 % pass/fail would be about the send")
        print("  cadence and not the tail shape. The bin counts are printed instead —")
        print("  they are the observation; only a pad-currency refit (each arm's own")
        print("  B + in-flight term, which this report does not compute) may turn them")
        print("  into a halving constant.")
    elif lever is None:
        print("  !! lever UNKNOWN — if this was an r36 run the fit below is invalid by")
        print("     preregistration (shared fixed-ms bins). Identify the run first.")
    for a in sorted(arms):
        span = [(s, e) for arm, s, e, *_ in windows if arm == a]
        # Every bin of the window exists, occupied or not. An empty 80-85 bin is
        # the fastest-decay evidence in the sample and dropping it is what made
        # the old fit read slow; see poisson_log_slope().
        bins = {k: 0 for k in range(6)}
        for ts, ms, o in gaps:
            if any(s <= ts <= e for s, e in span) and o >= STARVE_MIN_OUT and 55 <= ms < 85:
                bins[(ms - 55) // 5] += 1
        pts = [(k * 5 + 57.5, v) for k, v in sorted(bins.items())]
        occupied = [x for x in pts if x[1] > 0]
        if lever == "r36":
            print(f"  {a}: bin counts 55-85 ms (5 ms bins) "
                  + " ".join(f"{55 + k * 5}-{60 + k * 5}:{bins[k]}" for k in range(6))
                  + f"   ({sum(v for _, v in pts)} events, no constant fitted)")
            continue
        if len(occupied) < 3:
            print(f"  {a}: too few tail events to fit "
                  f"({sum(v for _, v in pts)} events in {len(occupied)} of 6 bins)")
            continue
        slope = poisson_log_slope(pts)
        if slope is None:
            print(f"  {a}: tail fit did not converge — report the bin counts, not a constant")
            continue
        if slope >= 0:
            print(f"  {a}: tail does not decay (slope {slope:+.3f}/ms) — not the gameplay shape")
            continue
        half = math.log(2) / -slope
        ok = "within +-30 % of 5.4 ms" if 3.8 <= half <= 7.0 else "OUTSIDE the +-30 % band"
        # The censored number is printed alongside precisely because the verdict
        # moved when the estimator did: an operator comparing this run against a
        # note written before this change has to see both, and a large spread
        # between them means the tail is too thin for either to be quoted.
        cens = censored_log_slope(pts)
        sens = (f"   [old censored log-OLS: {math.log(2) / -cens:.1f} ms]"
                if cens is not None and cens < 0 else "")
        print(f"  {a}: halves every {half:.1f} ms   ({ok}){sens}")
        if len(occupied) < 6:
            print(f"       ({6 - len(occupied)} of 6 bins empty, "
                  f"{sum(v for _, v in pts)} events — the empty ones are IN the fit)")

    # E2: level against the reference, stated as the band the arms actually span.
    base = "off" if "off" in arms else (sorted(arms)[0] if arms else None)
    if base:
        print(f"\nE2 level: '{base}' arm against the five gameplay arms "
              f"(min-max across arms, same out>={STARVE_MIN_OUT} filter):")
        for k in (60, 70, 80):
            i = REFERENCE_EDGES.index(k)
            lo = min(v[i] for v in REFERENCE_ARMS.values())
            hi = max(v[i] for v in REFERENCE_ARMS.values())
            mine = arms[base][k] / arms[base]["secs"] * 60
            where = ("inside" if lo <= mine <= hi else
                     "below" if mine < lo else "above")
            print(f"  >={k:3d}ms  synthetic {mine:6.1f}/min   gameplay {lo:5.1f}-{hi:5.1f}/min"
                  f"   -> {where} the reference band")

    # Pipeline depth, per arm, from the rig's own status lines. For the `feed`
    # lever this is the witness the ratio is not allowed to be read without: the
    # lever claims to empty the credit window, and either it did or it did not.
    synth_f = d / "synth.log"
    if synth_f.exists() and windows:
        rig0 = rig0_ms // 1000 - 35              # rig starts ~35 s before block 1
        qs = {}
        for line in synth_f.read_text().splitlines():
            m = re.match(r"\[synth\] t=(\d+)s .*\bq=(-?\d+)", line)
            if not m:
                continue
            ts = (rig0 + int(m.group(1))) * 1000
            for arm, s_, e_, *_ in windows:
                if s_ <= ts <= e_:
                    qs.setdefault(arm, []).append(int(m.group(2)))
        if qs:
            print("\ncredit window (packets in flight, from the rig's status lines):")
            for a in sorted(qs):
                v = qs[a]
                print(f"  {a}: mean q={sum(v)/len(v):.1f}  (n={len(v)}, min {min(v)} max {max(v)})")

    if "on" in arms and "off" in arms:
        # Amendment 7, declared in ds5_autorun.sh's r36 comment BEFORE the lever
        # was ever run: a lever that changes the send cadence moves these bins by
        # construction, so an ON/OFF ratio taken across shared fixed-ms edges
        # measures the re-binning and not the lever. This report cannot compute
        # the pad-currency edges (each arm's own B + in-flight term), so the only
        # honest thing left is to refuse the number rather than print one that
        # looks like every other verdict on this page.
        if lever == "r36":
            print("\nratio ON/OFF: REFUSED for lever=r36 (Amendment 7, preregistered).")
            print("  This lever is judged in PAD CURRENCY with each arm's own in-flight")
            print("  term (B + 21.33 ms OFF vs B + 10.67 ms ON), never on shared fixed-ms")
            print("  bins — the ON arm's bins are displaced by the cadence itself, so a")
            print("  ratio across them would be an artefact with a confidence interval.")
            print("  Still readable above: each arm's own event table, the rig's delivery")
            print("  line and the credit-window means. The verdict needs the pad-currency")
            print("  edges, which this report does not compute.")
            return 3
        if lever is None:
            print("\n  !! lever UNKNOWN (no conditions.txt, and the directory name does not")
            print("     name one). If this was an r36 run the block below is invalid by")
            print("     preregistration — identify the run before quoting it.")
        print("\nratio ON/OFF (Poisson, 95 % CI) — the L18 witness is the >=60 ms band:")
        for k in EDGES:
            r, lo, hi = poisson_ratio_ci(arms["on"][k], arms["on"]["secs"],
                                         arms["off"][k], arms["off"]["secs"])
            if r is None:
                print(f"  >={k:3d}ms   too few events")
            else:
                sig = "" if lo <= 1.0 <= hi else "  <- significant"
                print(f"  >={k:3d}ms   x{r:.2f} [{lo:.2f}-{hi:.2f}]{sig}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
