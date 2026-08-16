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
"""
import math
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

    windows = []
    for line in win_f.read_text().splitlines():
        if not line.strip():
            continue
        f = line.split("\t")
        arm, s, e = f[0], f[1], f[2]
        wifi = int(f[3]) if len(f) > 3 else None
        fg = f[4] if len(f) > 4 else None
        windows.append((arm, int(s) * 1000, int(e) * 1000, wifi, fg))
    if not windows:
        print("no blocks recorded")
        return 1

    gaps = []
    for line in gap_f.read_text().splitlines():
        # G <epoch_ms> gap=<ms> out=<n> h=<handle>
        if not line.startswith("G "):
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

    # The daemon's gap log is a capped ring that drops its OLDEST entries. A block
    # that ended before the log's first surviving record contributed its exposure
    # to the denominator and nothing to the numerator, which halves a rate without
    # touching a single counter. Blocks like that are dropped, loudly.
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
    for arm, s, e, wifi, fg in windows:
        a = arms.setdefault(arm, {"secs": 0.0, "wifi": [], **{k: 0 for k in EDGES}})
        if wifi is not None:
            a["wifi"].append(wifi)
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
    print("\nE1 tail shape (halving constant over 55-85 ms; gameplay reference 5.4 ms):")
    for a in sorted(arms):
        span = [(s, e) for arm, s, e, *_ in windows if arm == a]
        bins = {}
        for ts, ms, o in gaps:
            if any(s <= ts <= e for s, e in span) and o >= STARVE_MIN_OUT and 55 <= ms < 85:
                bins[(ms - 55) // 5] = bins.get((ms - 55) // 5, 0) + 1
        pts = [(k * 5 + 57.5, v) for k, v in sorted(bins.items()) if v > 0]
        if len(pts) < 3:
            print(f"  {a}: too few tail events to fit")
            continue
        n = len(pts)
        sx = sum(x for x, _ in pts); sy = sum(math.log(y) for _, y in pts)
        sxx = sum(x * x for x, _ in pts); sxy = sum(x * math.log(y) for x, y in pts)
        denom = n * sxx - sx * sx
        slope = (n * sxy - sx * sy) / denom if denom else 0.0
        if slope >= 0:
            print(f"  {a}: tail does not decay (slope {slope:+.3f}/ms) — not the gameplay shape")
        else:
            half = math.log(2) / -slope
            ok = "within +-30 % of 5.4 ms" if 3.8 <= half <= 7.0 else "OUTSIDE the +-30 % band"
            print(f"  {a}: halves every {half:.1f} ms   ({ok})")

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

    if "on" in arms and "off" in arms:
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
