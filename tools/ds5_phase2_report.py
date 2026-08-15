#!/usr/bin/env python3
"""ds5_phase2_report.py — turn Phase-2 captures into the verdicts they were taken for.

Reads one or more run directories produced by tools/ds5_phase2_run.sh
(sniff.log + txd.log + manifest.txt) and answers:

  2.3  Is the 0x31 wire rate elevated while our credits starve?  (X4 lives or dies)
  2.4  Master or slave?                                          (unlocks L11)
  2.5  Does foreign LE airtime cluster on the gaps?              (explains burstiness)
  2.6  maxq ladder: compare rungs                                (pass several dirs)
  --   cross-check: the daemon's own gap ledger vs the independent monitor ledger

Methodology follows the project's standing rules, which is most of why this file
exists rather than a handful of greps:

  * rates against rates, never counts against counts — slices differ in length;
  * activity-filtered — idle seconds have no audio to starve, and averaging them
    in is exactly the error that produced the bogus "-73%" RT-boost result;
  * in-gap traffic is pooled (total events / total gap seconds), not averaged
    per gap, so one 2-second episode cannot be outvoted by thirty 30ms ones.

Usage:
  ds5_phase2_report.py RUNDIR [RUNDIR ...] [--active-min RATE] [--verbose]
"""

import argparse
import os
import re
import sys
from collections import Counter

# A second counts as "active" when audio is actually flowing; below this the link
# has nothing to starve. 10/s is far under the ~47/s batched cadence and far over
# the ~1/s rumble-only floor, so it separates play from menus without tuning.
ACTIVE_MIN_AUDIO_RATE = 10.0

RE_GAP = re.compile(
    r"^GAP (?P<t>\S+) h=(?P<h>\S+) gap_ms=(?P<gap>\d+) out=(?P<out>-?\d+) "
    r"role=(?P<role>\S+) mode=(?P<mode>\d+)(?P<learned> \[learned\])? in_gap "
    r"tx31=(?P<tx31>\d+) tx32=(?P<tx32>\d+) tx36=(?P<tx36>\d+) tx39=(?P<tx39>\d+) "
    r"txo=(?P<txo>\d+) rx=(?P<rx>\d+) le_rx=(?P<lerx>\d+) le_tx=(?P<letx>\d+) "
    r"oth_rx=(?P<othrx>\d+) oth_tx=(?P<othtx>\d+)")
RE_SEC = re.compile(
    r"^SEC (?P<t>\S+) t=(?P<rel>[\d.]+) links=(?P<links>\d+) out_max=(?P<out>-?\d+) "
    r"tx31/s=(?P<tx31>[\d.]+) tx36/s=(?P<tx36>[\d.]+) tx39/s=(?P<tx39>[\d.]+) "
    r"rx/s=(?P<rx>[\d.]+) le_rx/s=(?P<lerx>[\d.]+) le_tx/s=(?P<letx>[\d.]+) "
    r"oth/s=(?P<oth>[\d.]+) gaps/s=(?P<gaps>[\d.]+)")
RE_EV = re.compile(r"^EV (?P<t>\S+) (?P<body>.*)$")
# daemon ledger: "... maxq=12 fifo=2 ... | L0 xx:.. have=1 q=3 ... gaps=a/b/c gmax=N drops=x/y/z"
RE_LEDGER_LINK = re.compile(
    r"\| L(?P<slot>\d+) (?P<addr>[0-9a-f:]{17}) have=(?P<have>\d+) q=(?P<q>\d+) "
    r"rq=(?P<rq>\d+) fifo=(?P<fifo>\d+) gaps=(?P<g30>\d+)/(?P<g50>\d+)/(?P<g80>\d+) "
    r"gmax=(?P<gmax>\d+) drops=(?P<dage>\d+)/(?P<dovf>\d+)/(?P<doth>\d+)")
RE_LEDGER_HEAD = re.compile(r"^\[txd\] inj=(?P<inj>\d+) drop=(?P<drop>\d+).*maxq=(?P<maxq>\d+)")
RE_EPISODE_END = re.compile(r"EPISODE end t=(?P<t>\d+) dur=(?P<dur>\d+)ms rx=(?P<rx>\d+) orx=(?P<orx>\d+)")


def hhmmss(s):
    """'04:58:25.458' -> seconds since midnight (float)."""
    h, m, rest = s.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def band(gap_ms):
    return "≥80" if gap_ms >= 80 else ("50-79" if gap_ms >= 50 else "30-49")


BANDS = ("30-49", "50-79", "≥80")


class Run:
    def __init__(self, path, active_min):
        self.path = path
        self.name = os.path.basename(os.path.normpath(path))
        self.active_min = active_min
        self.secs = []          # parsed SEC rows
        self.gaps = []          # parsed GAP rows
        self.events = []
        self.roles = Counter()
        self.learned_only = False
        self.maxq = None
        self.ledger = []        # (inj, drop, maxq, [links])
        self.episodes = []
        self._read_sniff(os.path.join(path, "sniff.log"))
        self._read_txd(os.path.join(path, "txd.log"))
        self._read_manifest(os.path.join(path, "manifest.txt"))

    # ---- parsing ---------------------------------------------------------
    def _read_sniff(self, p):
        if not os.path.exists(p):
            raise SystemExit(f"{p}: no sniff.log — was the run started with ds5_phase2_run.sh?")
        prev = None
        day = 0.0
        for line in open(p, errors="replace"):
            m = RE_SEC.match(line)
            if m:
                t = hhmmss(m.group("t"))
                if prev is not None and t + day < prev:
                    day += 86400.0            # capture crossed midnight
                t += day
                prev = t
                d = {k: float(m.group(k)) for k in
                     ("rel", "tx31", "tx36", "tx39", "rx", "lerx", "letx", "oth", "gaps")}
                d["t"] = t
                d["links"] = int(m.group("links"))
                d["out"] = int(m.group("out"))
                self.secs.append(d)
                continue
            m = RE_GAP.match(line)
            if m:
                t = hhmmss(m.group("t")) + day
                g = {k: int(m.group(k)) for k in
                     ("gap", "out", "tx31", "tx32", "tx36", "tx39", "txo", "rx",
                      "lerx", "letx", "othrx", "othtx")}
                g["t"] = t
                g["role"] = m.group("role")
                g["mode"] = int(m.group("mode"))
                g["learned"] = bool(m.group("learned"))
                self.gaps.append(g)
                continue
            m = RE_EV.match(line)
            if m:
                body = m.group("body")
                self.events.append((m.group("t"), body))
                r = re.search(r"role=(master|slave)|new_role=(master|slave)", body)
                if r:
                    self.roles[r.group(1) or r.group(2)] += 1

    def _read_txd(self, p):
        if not os.path.exists(p):
            return
        for line in open(p, errors="replace"):
            h = RE_LEDGER_HEAD.match(line)
            if h:
                links = [m.groupdict() for m in RE_LEDGER_LINK.finditer(line)]
                self.ledger.append((int(h.group("inj")), int(h.group("drop")),
                                    int(h.group("maxq")), links))
            e = RE_EPISODE_END.search(line)
            if e:
                self.episodes.append(int(e.group("dur")))

    def _read_manifest(self, p):
        self.manifest = {}
        if not os.path.exists(p):
            return
        for line in open(p, errors="replace"):
            if "=" in line:
                k, _, v = line.partition("=")
                self.manifest.setdefault(k.strip(), v.strip())
        mq = self.manifest.get("maxq")
        if mq and mq != "default":
            self.maxq = mq

    # ---- derived ---------------------------------------------------------
    @property
    def active(self):
        """SEC rows with audio actually flowing."""
        return [s for s in self.secs if s["tx36"] + s["tx39"] >= self.active_min]

    @property
    def active_seconds(self):
        return set(int(s["t"]) for s in self.active)

    def active_gaps(self):
        """Gaps that closed during active play.

        A gap is attributed to the second it CLOSED and to the one before it: a
        gap spans time, and a 2s stall closing in the first idle second is still
        a stall of the active period that preceded it."""
        act = self.active_seconds
        return [g for g in self.gaps if int(g["t"]) in act or int(g["t"]) - 1 in act]

    @staticmethod
    def split_audio(gaps):
        """Separate gaps that starved AUDIO from gaps with nothing to send.

        Measured 2026-08-15: a third of the >=80ms events, and TWO THIRDS of the
        >=80ms time, were ~5s windows carrying no audio at all — one 0x31
        heartbeat out, pad input flowing normally, outstanding stuck at 1-2. The
        credit clock keeps running while the link is audio-idle, so the gate
        "outstanding > 0" alone counts silence as starvation. Nothing can
        underrun when nothing is being sent.

        This is safe to gate on precisely because the window is never full
        (observed outstanding 0-4 against maxq=12, daemon fifo=0 in 71/71
        samples): audio that existed WOULD have gone out, so no audio on the
        wire means no audio to send, not audio blocked behind credits."""
        return ([g for g in gaps if g["tx36"] + g["tx39"] > 0],
                [g for g in gaps if g["tx36"] + g["tx39"] == 0])

    def baseline_rates(self):
        a = self.active
        if not a:
            return None
        n = float(len(a))
        return {k: sum(s[k] for s in a) / n for k in ("tx31", "tx36", "tx39", "rx", "lerx", "letx", "oth")}

    def in_gap_rates(self, gaps):
        """Pooled rate: total events over total gap seconds."""
        tot = sum(g["gap"] for g in gaps) / 1000.0
        if tot <= 0:
            return None, 0.0
        keys = {"tx31": "tx31", "rx": "rx", "lerx": "lerx", "letx": "letx"}
        out = {k: sum(g[src] for g in gaps) / tot for k, src in keys.items()}
        out["oth"] = sum(g["othrx"] + g["othtx"] for g in gaps) / tot
        return out, tot


def fmt_ratio(ing, base):
    if base is None or base <= 0.001:
        return f"{ing:8.1f}/s      (no baseline)"
    return f"{ing:8.1f}/s vs {base:6.1f}/s  ×{ing / base:4.2f}"


def report_run(r, verbose):
    print(f"\n{'=' * 78}\n RUN  {r.name}   ({r.path})\n{'=' * 78}")
    if r.manifest:
        keys = [k for k in ("label", "start", "app_version", "maxq", "eth0_carrier",
                            "eth0_speed", "txd_pids") if k in r.manifest]
        for k in keys:
            print(f"   {k:14s} {r.manifest[k]}")
    dur = (r.secs[-1]["t"] - r.secs[0]["t"]) if len(r.secs) > 1 else 0
    act = r.active
    print(f"   capture        {dur / 60:.1f} min, {len(act)} active s "
          f"({100.0 * len(act) / max(1, len(r.secs)):.0f}% of the slice)")
    if not act:
        print("\n   ⚠ NO ACTIVE SECONDS — no audio was flowing during this capture.")
        print("     Nothing below is interpretable; re-run during actual play.")
        return

    base = r.baseline_rates()
    ag = r.active_gaps()
    print(f"   audio          tx36={base['tx36']:.1f}/s tx39={base['tx39']:.1f}/s "
          f"(0x39 = batched) rx={base['rx']:.1f}/s")

    # ---- gap ledger, activity-filtered, as rates ------------------------
    per_min = 60.0 / max(1.0, float(len(act)))
    counts = Counter(band(g["gap"]) for g in ag)
    aud, silent = r.split_audio(ag)
    ca, cs = Counter(band(g["gap"]) for g in aud), Counter(band(g["gap"]) for g in silent)
    print(f"\n   ── gaps (monitor ground truth, active-filtered) ──")
    print(f"      {'band':>6}  {'AUDIO starved':>14}   {'silent (nothing to send)':>26}")
    for b in BANDS:
        st = sum(g["gap"] for g in silent if band(g["gap"]) == b) / 1000.0
        print(f"      {b:>6}  {ca[b] * per_min:7.1f}/min n={ca[b]:<5d}"
              f"{cs[b] * per_min:9.1f}/min n={cs[b]:<5d} ({st:.0f}s)")
    if aud:
        print(f"      max (audio-starved) {max(g['gap'] for g in aud)} ms"
              + (f" · max silent {max(g['gap'] for g in silent)} ms" if silent else ""))
    if cs["≥80"]:
        share = 100.0 * sum(g["gap"] for g in silent if g["gap"] >= 80) / max(
            1.0, sum(g["gap"] for g in ag if g["gap"] >= 80))
        print(f"      ⚠ {cs['≥80']} of {counts['≥80']} ≥80ms events carried NO audio "
              f"— {share:.0f}% of the ≥80 TIME. Nothing can underrun there;")
        print(f"        the credit clock simply keeps running while the link is audio-idle."
              f"\n        The daemon's own gaps= counter does NOT make this distinction either.")

    # ---- 2.4 role -------------------------------------------------------
    print(f"\n   ── 2.4 role ──")
    roles = set(g["role"] for g in ag) - {"?"}
    if r.roles or roles:
        for who, n in r.roles.most_common():
            print(f"      announced: {who} ({n}×)")
        if roles:
            print(f"      during gaps: {', '.join(sorted(roles))}")
        verdict = "slave" if "slave" in roles or r.roles.get("slave") else "master"
        print(f"      ⇒ TV is {verdict.upper()}."
              + ("  L11/Switch_Role is unlocked (pad paged the TV, TV stayed slave)."
                 if verdict == "slave" else
                 "  L11 is moot — the TV already holds the master role."))
    else:
        print("      UNKNOWN — no connection or role event in this capture.")
        print("      The pad was already connected when the sniffer attached.")
        print("      Fix: start ds5_phase2_run.sh BEFORE switching the pad on.")

    # ---- 2.3 / 2.5 in-gap traffic --------------------------------------
    # Silent gaps are excluded here: they contain exactly one 0x31 heartbeat by
    # construction, which would show up as "0x31 is elevated during starvation"
    # — a pure artefact of the wrong denominator.
    ing, gsecs = r.in_gap_rates(aud)
    print(f"\n   ── 2.3 / 2.5 in-gap airtime  ({gsecs:.1f}s pooled across {len(aud)} "
          f"AUDIO-starved gaps; {len(silent)} silent gaps excluded) ──")
    if ing:
        print(f"      0x31 out   {fmt_ratio(ing['tx31'], base['tx31'])}   ← 2.3 (X4)")
        print(f"      pad rx     {fmt_ratio(ing['rx'], base['rx'])}")
        print(f"      LE rx      {fmt_ratio(ing['lerx'], base['lerx'])}   ← 2.5")
        print(f"      LE tx      {fmt_ratio(ing['letx'], base['letx'])}")
        print(f"      other      {fmt_ratio(ing['oth'], base['oth'])}   (unclassified handles)")
        # Per-band, because the tail is where the audible damage is
        print(f"      by band (0x31 out /s · LE+other /s):")
        for b in BANDS:
            sub = [g for g in aud if band(g["gap"]) == b]
            sr, st = r.in_gap_rates(sub)
            if sr:
                print(f"        {b:>6}  n={len(sub):4d}  {st:6.1f}s   "
                      f"0x31 {sr['tx31']:6.1f}/s   LE+oth {sr['lerx'] + sr['letx'] + sr['oth']:6.1f}/s")
        # Judge the ≥80 band, not the pool. The 30-49 gaps are benign by volume
        # (~300/min) and would drown any tail signal in a pooled ratio; the ≥80
        # band is the one that is actually audible, and it is where a real
        # mechanism has to show up. Pool only when the tail is too thin to read.
        tail = [g for g in aud if g["gap"] >= 80]
        tr, tsecs = r.in_gap_rates(tail)
        TAIL_MIN_N = 8

        def verdict(name, ing_pooled, ing_tail, base_rate, yes, no, maybe):
            if base_rate <= 0.001 and ing_pooled <= 0.001:
                # Absence of the thing is not evidence about its timing. Saying
                # "flat" here would dress a vacuous question as an answer.
                print(f"\n      ⇒ {name}: NOT APPLICABLE — zero such traffic anywhere in this "
                      f"capture,\n         inside the gaps or outside. Exonerated by absence for "
                      f"THIS session's conditions only;\n         a session where the source is "
                      f"actually active is a different measurement.")
                return
            pooled = ing_pooled / base_rate if base_rate > 0.001 else 0
            if ing_tail is not None and len(tail) >= TAIL_MIN_N:
                v, src = (ing_tail / base_rate if base_rate > 0.001 else 0), f"≥80 band, n={len(tail)}"
            else:
                v, src = pooled, ("pooled — tail too thin to judge (n=%d)" % len(tail))
            print(f"\n      ⇒ {name}: " + (yes if v >= 1.5 else (no if v <= 1.1 else maybe))
                  + f"  (×{v:.2f} on the {src}; pooled ×{pooled:.2f})")

        verdict("2.3", ing["tx31"], tr["tx31"] if tr else None, base["tx31"],
                "0x31 is ELEVATED while credits starve — X4 stays alive.",
                "0x31 is at/below baseline while credits starve — X4 is DEAD, close it.",
                "0x31 is mildly elevated — inconclusive, needs more slices.")
        lb = base["lerx"] + base["letx"] + base["oth"]
        verdict("2.5", ing["lerx"] + ing["letx"] + ing["oth"],
                (tr["lerx"] + tr["letx"] + tr["oth"]) if tr else None, lb,
                "foreign airtime CLUSTERS on the audible gaps — L12 is worth testing.",
                "foreign airtime is flat across gaps — LE/Magic-Remote does NOT explain the burstiness.",
                "foreign airtime is mildly clustered — inconclusive.")
    else:
        print("      no gaps in the active window — nothing to attribute.")

    # ---- cross-check against the daemon's own ledger --------------------
    if r.ledger:
        first, last = r.ledger[0], r.ledger[-1]
        print(f"\n   ── cross-check: daemon ledger vs monitor ground truth ──")
        print(f"      ledger lines {len(r.ledger)}   maxq={last[2]}   "
              f"inj Δ={last[0] - first[0]}  drop Δ={last[1] - first[1]}")
        for slot in sorted({l["slot"] for _, _, _, ls in r.ledger for l in ls}):
            def pick(entry):
                for l in entry[3]:
                    if l["slot"] == slot:
                        return l
                return None
            a, b = pick(first), pick(last)
            if not a or not b:
                continue
            d30 = int(b["g30"]) - int(a["g30"])
            d50 = int(b["g50"]) - int(a["g50"])
            d80 = int(b["g80"]) - int(a["g80"])
            print(f"      L{slot} daemon  {d30 * per_min:7.1f} / {d50 * per_min:7.1f} / "
                  f"{d80 * per_min:6.1f} per min   gmax={b['gmax']}ms  "
                  f"drops age/ovf/oth = {int(b['dage']) - int(a['dage'])}/"
                  f"{int(b['dovf']) - int(a['dovf'])}/{int(b['doth']) - int(a['doth'])}")
            # The daemon counter spans the whole window and is NOT activity-
            # filtered, so it must be compared against the unfiltered monitor
            # totals. Comparing it to the filtered rate manufactures a
            # divergence that is only the filter.
            allc = Counter(band(g["gap"]) for g in r.gaps)
            print(f"      L{slot} daemon  counts {d30} / {d50} / {d80}   (whole window)")
            print(f"      L{slot} monitor counts {allc['30-49']} / {allc['50-79']} / "
                  f"{allc['≥80']}   (whole window, unfiltered)")
            for lbl, dv, mv in (("30-49", d30, allc["30-49"]), ("50-79", d50, allc["50-79"]),
                                ("≥80", d80, allc["≥80"])):
                if mv or dv:
                    print(f"           {lbl:>6} agreement {100.0 * min(dv, mv) / max(dv, mv, 1):5.1f}%")
            print("           (independent readers of the same NOCP stream. A large divergence "
                  "would mean the\n            daemon's capture thread is missing NOCPs — i.e. "
                  "it is being starved of CPU itself.)")
        if r.episodes:
            ep = sorted(r.episodes)
            print(f"      EPISODEs n={len(ep)} median={ep[len(ep) // 2]}ms "
                  f"p90={ep[int(len(ep) * 0.9)]}ms max={ep[-1]}ms")
    else:
        print("\n   ── cross-check ── no txd.log in this run directory.")

    if verbose:
        print("\n   ── events ──")
        for t, b in r.events[:60]:
            print(f"      {t} {b}")


def compare(runs, active_min):
    """2.6: the ladder table. Rungs are only comparable as rates."""
    print(f"\n{'=' * 78}\n LADDER COMPARISON (2.6)\n{'=' * 78}")
    hdr = f"{'run':<12}{'maxq':>5}{'active_s':>10}{'30-49':>9}{'50-79':>9}{'≥80':>8}{'drops a/o/x':>16}{'0x31 ×':>9}"
    print(hdr)
    print("-" * len(hdr))
    for r in runs:
        act = r.active
        if not act:
            print(f"{r.name:<12}{str(r.maxq or '?'):>5}{0:>10}   (no active seconds)")
            continue
        per_min = 60.0 / len(act)
        c = Counter(band(g["gap"]) for g in r.active_gaps())
        drops = "-"
        if r.ledger:
            f, l = r.ledger[0], r.ledger[-1]
            def sl(e):
                return e[3][0] if e[3] else None
            if sl(f) and sl(l):
                drops = "/".join(str(int(sl(l)[k]) - int(sl(f)[k])) for k in ("dage", "dovf", "doth"))
        base = r.baseline_rates()
        ing, _ = r.in_gap_rates(r.active_gaps())
        ratio = (ing["tx31"] / base["tx31"]) if (ing and base["tx31"] > 0.001) else 0
        print(f"{r.name:<12}{str(r.maxq or '-'):>5}{len(act):>10}"
              f"{c['30-49'] * per_min:>9.1f}{c['50-79'] * per_min:>9.1f}{c['≥80'] * per_min:>8.1f}"
              f"{drops:>16}{ratio:>9.2f}")
    print("\n Adopt the smallest maxq whose clean-phase ≥80 rate matches the 12 rung.")
    print(" Age-drops appearing at the low rungs are INTENDED (that is the shed), not")
    print(" the abort criterion — that is why drop_total was split in Phase 1.")
    print(" Alternate rungs across days: coex weather drifts, and a rung measured only")
    print(" on one evening is a measurement of that evening.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rundirs", nargs="+")
    ap.add_argument("--active-min", type=float, default=ACTIVE_MIN_AUDIO_RATE,
                    help="audio packets/s for a second to count as active (default %(default)s)")
    ap.add_argument("--verbose", action="store_true", help="also dump link events")
    a = ap.parse_args()

    runs = [Run(d, a.active_min) for d in a.rundirs]
    for r in runs:
        report_run(r, a.verbose)
    if len(runs) > 1:
        compare(runs, a.active_min)
    return 0


if __name__ == "__main__":
    sys.exit(main())
