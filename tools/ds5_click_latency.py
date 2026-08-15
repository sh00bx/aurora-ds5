#!/usr/bin/env python3
r"""Measure DS5 pad-speaker latency from a single microphone recording.

WHY THIS EXISTS
---------------
The DualSense exposes no consumption feedback of any kind: nothing in the whole
stack can read back how deep its audio jitter buffer actually is. Every number
we have is inferred (slider value, credit-window occupancy, FIFO depth), and the
central open question of the latency programme -- whether a mid-stream buffer
raise does anything, and whether post-underrun depth equals the delivered
backlog -- cannot be answered by any counter on the TV. This rig is the only
objective probe: it measures the pad's real acoustic latency.

THE TRICK: NO CLOCK SYNC NEEDED
-------------------------------
Absolute latency would need a shared time base between the host and the
recorder. Instead, put the microphone where it hears BOTH sound sources:

    click track --> host --> [ TV speakers ]      (reference, ~constant)
                         \-> [ pad speaker ]      (the path under test)

Each click then appears TWICE in one recording, and the gap between the pair is
the pad chain's EXTRA latency over the reference path. Same file, same clock, no
synchronisation, and any recorder drift cancels.

Air time matters at this precision: 34 cm of extra distance is 1 ms. Put the mic
roughly equidistant, or pass --mic-offset-ms to correct a known difference.

USAGE
-----
    # record a wav (mono or stereo, any rate), then:
    ./ds5_click_latency.py recording.wav
    ./ds5_click_latency.py recording.wav --expect 60 --mic-offset-ms -1.2
    ./ds5_click_latency.py recording.wav --csv out.csv     # per-click detail

Click track: short, sharp, well separated -- e.g. 5 ms of white noise or a 2 kHz
tone burst every 750 ms. Separation must exceed the largest latency you expect to
see, or pairs get mis-associated (the script refuses ambiguous pairings rather
than guessing).

READING THE RESULT
------------------
The median is the number to quote. The SPREAD is the interesting part: the pad
buffer is supposed to be a fixed depth, so a wide or drifting distribution means
the depth is moving -- which is exactly the effect the "delivered backlog becomes
the new depth" hypothesis predicts after an underrun. Take a baseline run, then
inject a gap and re-measure: a permanent step in the median is the whole finding.
"""

import argparse
import csv
import math
import sys
import wave

import numpy as np


def read_wav(path):
    """Return (mono float32 in -1..1, sample_rate). Any bit depth, any channels."""
    with wave.open(path, "rb") as w:
        nch, width, rate, nframes = (
            w.getnchannels(),
            w.getsampwidth(),
            w.getframerate(),
            w.getnframes(),
        )
        raw = w.readframes(nframes)

    if width == 1:  # 8-bit wav is unsigned
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        data = v.astype(np.float32) / 8388608.0
    else:
        sys.exit(f"unsupported sample width: {width} bytes")

    if nch > 1:
        data = data.reshape(-1, nch).mean(axis=1)
    return data, rate


def onset_envelope(x, rate, win_ms=1.0):
    """Short-window energy envelope, in dB, one value per hop.

    A click is a broadband energy step, so plain windowed energy finds it as well
    as anything fancier and has no tuning knobs to get wrong. The hop sets the
    resolution: 0.25 ms is far finer than the effect being measured (~10 ms), so
    detection resolution is never the limiting factor here -- the microphone's
    distance to the two speakers is.
    """
    win = max(8, int(rate * win_ms / 1000.0))
    hop = max(1, win // 4)
    n = 1 + (len(x) - win) // hop if len(x) >= win else 0
    if n <= 0:
        sys.exit("recording is shorter than one analysis window")
    idx = np.arange(n) * hop
    frames = np.lib.stride_tricks.sliding_window_view(x, win)[::hop][:n]
    energy = np.sqrt(np.mean(frames.astype(np.float64) ** 2, axis=1))
    db = 20.0 * np.log10(np.maximum(energy, 1e-12))
    return db, idx / float(rate), hop / float(rate)


def find_onsets(db, times, rise_db, floor_db, refractory_s):
    """Onsets = the first frame of each run that jumps rise_db above the local
    noise floor. The refractory window is what keeps a single click's decay tail
    from registering as several onsets."""
    thresh = floor_db + rise_db
    hot = db > thresh
    onsets = []
    last = -1e9
    for i in range(1, len(hot)):
        if hot[i] and not hot[i - 1] and (times[i] - last) > refractory_s:
            # Walk back to where the rise actually began, so the timestamp is the
            # attack and not the moment it crossed an arbitrary threshold.
            j = i
            while j > 0 and db[j - 1] > db[j] - 12.0 and db[j - 1] > floor_db + 3.0:
                j -= 1
            onsets.append(times[j])
            last = times[j]
    return onsets


def pair_onsets(onsets, max_gap_s, min_gap_s):
    """Associate each reference click with the pad echo that follows it.

    Refuses to guess: a click with two candidates inside the window, or none, is
    reported as unpaired rather than silently matched. A run with many unpaired
    clicks means the click spacing is too tight for the latency being measured --
    fix the click track, do not lower the threshold.
    """
    pairs, unpaired = [], []
    i = 0
    while i < len(onsets):
        t0 = onsets[i]
        cands = [t for t in onsets[i + 1:] if min_gap_s <= (t - t0) <= max_gap_s]
        if len(cands) == 1:
            pairs.append((t0, cands[0]))
            i = onsets.index(cands[0]) + 1
        elif len(cands) == 0:
            unpaired.append(t0)
            i += 1
        else:
            unpaired.append(t0)
            i = onsets.index(cands[-1]) + 1
    return pairs, unpaired


def main():
    ap = argparse.ArgumentParser(
        description="DS5 pad-speaker latency from a click-pair recording",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("wav", help="recording containing both TV and pad clicks")
    ap.add_argument("--rise-db", type=float, default=18.0,
                    help="dB above the noise floor that counts as an onset (default 18)")
    ap.add_argument("--max-latency-ms", type=float, default=400.0,
                    help="largest pad latency to consider a valid pairing (default 400)")
    ap.add_argument("--min-latency-ms", type=float, default=2.0,
                    help="smallest gap that is a pair and not one click detected twice (default 2)")
    ap.add_argument("--mic-offset-ms", type=float, default=0.0,
                    help="subtract known mic-distance difference, ms "
                         "(positive = mic is closer to the TV; 34 cm = 1 ms)")
    ap.add_argument("--expect", type=float, default=None,
                    help="expected latency in ms (e.g. the slider value) — reports the delta")
    ap.add_argument("--csv", help="write per-click measurements here")
    args = ap.parse_args()

    x, rate = read_wav(args.wav)
    db, times, hop = onset_envelope(x, rate)

    # Noise floor from the 20th percentile: robust while clicks occupy a small
    # fraction of the recording, and it needs no silent lead-in.
    floor_db = float(np.percentile(db, 20))

    # Refractory = half the smallest pair gap, so the two halves of a pair can
    # never be collapsed into one detection.
    refractory = (args.min_latency_ms / 1000.0) / 2.0
    onsets = find_onsets(db, times, args.rise_db, floor_db, refractory)

    if len(onsets) < 2:
        sys.exit(f"found {len(onsets)} onsets — check --rise-db "
                 f"(noise floor {floor_db:.1f} dB, threshold {floor_db + args.rise_db:.1f} dB)")

    pairs, unpaired = pair_onsets(
        onsets, args.max_latency_ms / 1000.0, args.min_latency_ms / 1000.0
    )
    if not pairs:
        sys.exit(f"{len(onsets)} onsets but no valid pairs — is the click spacing "
                 f"larger than --max-latency-ms ({args.max_latency_ms:.0f} ms)?")

    lat = np.array([(b - a) * 1000.0 for a, b in pairs]) - args.mic_offset_ms
    lat.sort()

    def pct(p):
        return float(np.percentile(lat, p))

    print(f"file            : {args.wav}  ({len(x) / rate:.1f}s @ {rate} Hz)")
    print(f"noise floor     : {floor_db:.1f} dB   onset threshold {floor_db + args.rise_db:.1f} dB")
    print(f"clicks paired   : {len(pairs)}   unpaired {len(unpaired)}")
    if args.mic_offset_ms:
        print(f"mic offset      : {args.mic_offset_ms:+.2f} ms (applied)")
    print()
    print(f"  median        : {np.median(lat):8.2f} ms   <-- quote this")
    print(f"  mean +/- sd   : {lat.mean():8.2f} +/- {lat.std(ddof=1) if len(lat) > 1 else 0.0:.2f} ms")
    print(f"  min / max     : {lat.min():8.2f} / {lat.max():.2f} ms")
    print(f"  p10 / p90     : {pct(10):8.2f} / {pct(90):.2f} ms")
    print(f"  spread (p90-p10): {pct(90) - pct(10):6.2f} ms")

    if args.expect is not None:
        d = float(np.median(lat)) - args.expect
        print()
        print(f"  vs expected {args.expect:.0f} ms : {d:+.2f} ms")
        # The pad buffer is nominally a fixed depth, so a large standing excess is
        # the signature the programme is looking for: depth that was re-primed to
        # a delivered backlog instead of to the configured value.
        if d > 20.0:
            print("  -> standing depth well ABOVE the configured buffer: consistent with"
                  " a re-prime to delivered backlog rather than to the slider value.")
        elif d < -20.0:
            print("  -> standing depth BELOW the configured buffer: consistent with"
                  " erosion (dropped/flushed frames with no refill path).")

    if len(unpaired) > len(pairs) * 0.2:
        print()
        print(f"  WARNING: {len(unpaired)} unpaired onsets vs {len(pairs)} pairs."
              " Widen the click spacing or raise --rise-db; do not trust a run"
              " where most clicks failed to pair.")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["ref_time_s", "pad_time_s", "latency_ms"])
            for a, b in pairs:
                w.writerow([f"{a:.6f}", f"{b:.6f}", f"{(b - a) * 1000.0 - args.mic_offset_ms:.3f}"])
        print(f"\nper-click detail -> {args.csv}")


if __name__ == "__main__":
    main()
