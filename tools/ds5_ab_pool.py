#!/usr/bin/env python3
"""ds5_ab_pool.py — pool the blocks of an interleaved A/B and judge the lever.

ds5_phase2_report.py reports one slice at a time, which is the right unit for
the Phase-2 questions (role, in-gap airtime) but the wrong one for a lever A/B:
tools/ds5_ab_flush.sh deliberately chops a session into OFF/ON/OFF/ON blocks so
the two arms share the same radio conditions, and those blocks have to be pooled
back together before anything can be concluded.

Pooling is done on the two quantities that are actually additive — audio-starved
gap COUNT and ACTIVE SECONDS — never by averaging the per-block rates. Blocks
differ in how much of them was real play (a menu stretch shrinks a block's
active seconds without shrinking its wall clock), and an unweighted mean of
rates would silently upweight the emptiest block.

The comparison is a Poisson rate ratio. Gap events in a band are counts over an
exposure (active seconds), which is exactly the model's shape, and it gives an
interval rather than the bare percentage that has misled this project before:
with ~180 events per arm a 10% "improvement" is inside the noise, and the CI
says so out loud.

  ds5_ab_pool.py --off ab_off1 ab_off2 --on ab_on1 ab_on2

Reuses Run from ds5_phase2_report.py, so the activity filter and the
audio-starved/silent split are the same code, not a second implementation.
"""

import argparse
import math
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ds5_phase2_report import Run, BANDS, band, ACTIVE_MIN_AUDIO_RATE  # noqa: E402


# A gap counts as real starvation only if something was queued BEHIND the packet
# in flight. Measured 2026-08-16 over 16683 gaps: the presence filter
# (tx36+tx39 > 0) still admits windows carrying ONE audio packet in 4.8 s —
# 0.2/s against a 47/s baseline — because a lone trailing packet passes ">0"
# exactly the way a lone 0x31 heartbeat passed the filter before it.
#
# In-gap audio RATE cannot be the discriminator: genuine credit starvation
# suppresses in-gap traffic too, so both cases look identical from that side.
# `outstanding` separates them and does so with the sign inverted from the
# artefact: the healthy 30-79 ms bands sit at out=2-3 (real pipelining) while
# the whole >=80 tail collapses to out=1 (nothing queued, stale credit).
#
# out>=2 keeps 31% of the >=80 band. The other 69% is silence, and every
# audible counter (conceal/fill/audio_omit) was zero across that session.
STARVE_MIN_OUT = 2


class Arm:
    """One side of the A/B: several blocks pooled into counts + exposure."""

    def __init__(self, name, dirs, active_min):
        self.name = name
        self.blocks = [Run(d, active_min) for d in dirs]
        self.active_s = 0
        self.audio = Counter()      # band -> starved gap count (out>=STARVE_MIN_OUT)
        self.presence = Counter()   # band -> old presence-filtered count, for continuity
        self.silent = Counter()
        self.out_max = 0
        self.gmax = 0
        self.drops = [0, 0, 0]      # age / ovf / other, summed over block deltas
        self.flush_states = set()
        for r in self.blocks:
            act = r.active
            self.active_s += len(act)
            gaps = r.active_gaps()
            aud, sil = Run.split_audio(gaps)
            starved = [g for g in aud if g["out"] >= STARVE_MIN_OUT]
            self.audio.update(band(g["gap"]) for g in starved)
            self.presence.update(band(g["gap"]) for g in aud)
            self.silent.update(band(g["gap"]) for g in sil)
            if gaps:
                self.out_max = max(self.out_max, max(g["out"] for g in gaps))
            if starved:
                self.gmax = max(self.gmax, max(g["gap"] for g in starved))
            d = block_drops(r)
            self.drops = [a + b for a, b in zip(self.drops, d)]
            self.flush_states |= block_flush_states(r.path)

    def rate(self, b):
        """Events per minute of ACTIVE play."""
        return 60.0 * self.audio[b] / self.active_s if self.active_s else 0.0


def block_drops(r):
    """Drops accumulated over the block, as a sum of increments.

    The ledger counters are NOT monotone: they accumulate and are periodically
    zeroed (observed live 2026-08-16: gaps=2838/1635/126 followed by 0/0/1 on
    the next line). A last-minus-first delta therefore reports whatever
    happened since the most recent reset and silently discards everything
    before it — for a 7-minute block that can be most of the block.

    Summing increments and treating any decrease as a reset boundary (the new
    value is then itself the increment) survives both the periodic zeroing and
    a mid-block rebind. It over-reports only if a counter both resets AND is
    already non-zero on its first line after the reset, which costs at most one
    interval's worth.
    """
    tot = [0, 0, 0]
    prev = [0, 0, 0]
    for entry in r.ledger:
        links = entry[3]
        if not links:
            continue
        cur = [sum(int(l[k]) for l in links) for k in ("dage", "dovf", "doth")]
        for i in range(3):
            tot[i] += cur[i] - prev[i] if cur[i] >= prev[i] else cur[i]
        prev = cur
    return tuple(tot)


def block_flush_states(path):
    """Every distinct flush=want/sent/readback triple the ledger published.

    This is the block's own witness that the lever was where we think it was:
    an ON block whose ledger only ever says flush=0/0/? did not measure L1, it
    measured a failed arm.
    """
    out = set()
    p = os.path.join(path, "txd.log")
    if not os.path.exists(p):
        return out
    for line in open(p, errors="replace"):
        i = line.find("flush=")
        if i >= 0:
            out.add(line[i:].split()[0])
    return out


def poisson_ratio_ci(k1, t1, k2, t2):
    """Rate ratio (arm2/arm1) with a 95% interval, log-normal approximation.

    Exact conditional tests exist, but at n in the hundreds the normal
    approximation on log(rate) is within a percent of them, and being explicit
    about the approximation is worth more here than the last decimal.
    """
    if k1 == 0 or k2 == 0 or t1 == 0 or t2 == 0:
        return None, None, None
    r = (k2 / t2) / (k1 / t1)
    se = math.sqrt(1.0 / k1 + 1.0 / k2)
    return r, r * math.exp(-1.96 * se), r * math.exp(1.96 * se)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--off", nargs="+", required=True, help="baseline block dirs")
    ap.add_argument("--on", nargs="+", required=True, help="lever-armed block dirs")
    ap.add_argument("--active-min", type=float, default=ACTIVE_MIN_AUDIO_RATE)
    a = ap.parse_args()

    off = Arm("OFF", a.off, a.active_min)
    on = Arm("ON", a.on, a.active_min)

    print(f"\n{'=' * 78}\n INTERLEAVED A/B — pooled\n{'=' * 78}")
    for arm in (off, on):
        print(f"   {arm.name:<4} {len(arm.blocks)} block(s): "
              f"{', '.join(r.name for r in arm.blocks)}")
        print(f"        active play {arm.active_s / 60.0:.1f} min · "
              f"drops age/ovf/oth {'/'.join(str(x) for x in arm.drops)} · "
              f"max starved gap {arm.gmax} ms · max outstanding {arm.out_max}")
        print(f"        ledger flush= {', '.join(sorted(arm.flush_states)) or '(none seen)'}")

    maxq = next((int(r.maxq) for r in off.blocks + on.blocks if r.maxq), 12)
    if max(off.out_max, on.out_max) < maxq - 2:
        print(f"\n   ⚠ the credit window never bound: max outstanding "
              f"{max(off.out_max, on.out_max)} against maxq={maxq}.")
        print("     Whatever these gaps are, they are not the send window filling up.")

    if not off.active_s or not on.active_s:
        print("\n   ⚠ one arm has NO active play — nothing to compare.")
        return 1

    print(f"\n   ── STARVED gap rate per minute of active play (out>={STARVE_MIN_OUT}) ──")
    print(f"      {'band':>6} {'OFF':>12} {'ON':>12}   {'ratio (95% CI)':>26}")
    for b in BANDS:
        r, lo, hi = poisson_ratio_ci(off.audio[b], off.active_s, on.audio[b], on.active_s)
        ci = f"×{r:.2f}  [{lo:.2f}, {hi:.2f}]" if r else "(too few events)"
        print(f"      {b:>6} {off.rate(b):7.1f} n={off.audio[b]:<4d}"
              f"{on.rate(b):7.1f} n={on.audio[b]:<4d}   {ci:>26}")

    # Shown only so older numbers remain comparable — this is the filter that
    # counts silence as starvation. Never quote it as the result.
    print(f"\n      for continuity, the old presence filter (INFLATED, do not quote):")
    for b in BANDS:
        po = 60.0 * off.presence[b] / off.active_s
        pn = 60.0 * on.presence[b] / on.active_s
        keep = (100.0 * (off.audio[b] + on.audio[b]) /
                max(1, off.presence[b] + on.presence[b]))
        print(f"      {b:>6} {po:7.1f} n={off.presence[b]:<4d}{pn:7.1f} "
              f"n={on.presence[b]:<4d}   {keep:>5.0f}% survive out>={STARVE_MIN_OUT}")

    print(f"\n   ── verdict on the ≥80 band (the audible one) ──")
    r, lo, hi = poisson_ratio_ci(off.audio["≥80"], off.active_s,
                                 on.audio["≥80"], on.active_s)
    if not r:
        print("      Too few ≥80 events in one arm to judge. Longer blocks.")
    elif hi < 1.0:
        print(f"      L1 REDUCES ≥80 starvation by {100 * (1 - r):.0f}% "
              f"[{100 * (1 - hi):.0f}%, {100 * (1 - lo):.0f}%] — interval excludes 1.")
    elif lo > 1.0:
        print(f"      L1 makes it WORSE by {100 * (r - 1):.0f}% — interval excludes 1.")
        print("      Disarm and do not pursue without a mechanism for the regression.")
    else:
        print(f"      NO EFFECT DEMONSTRATED (×{r:.2f}, CI [{lo:.2f}, {hi:.2f}] spans 1).")
        print("      This is a null, not a refutation: the interval says what")
        print("      effect sizes this much play could still have hidden.")
    print("\n   Age-drops rising in the ON arm are the shed working as designed —")
    print("   L1 trades stale audio for a shorter stall. Read them together.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
