#!/usr/bin/env python3
"""Verdicts for an unattended ds5_autorun.sh run.

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
        for ts, ms, _out in gaps:
            if s <= ts <= e:
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
