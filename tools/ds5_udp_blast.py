#!/usr/bin/env python3
"""ds5_udp_blast — a video-stream-shaped airtime load, from the LAN side.

Every wifi perturbation this programme has run so far was bulk TCP best-effort
(curl), and it moved the >=70ms tail by nothing (x1.05 n.s.) while a real
session shows 14/min. What a real Moonlight stream adds that a curl does not,
on the AIR specifically: (1) 60 Hz microbursts of UDP instead of a smooth
ACK-clocked stream, and (2) a DSCP marking that the AP's WMM mapping turns
into a higher access category (AC_VI/AC_VO), which arbitrates against
bluetooth differently than best effort inside the MT7921 coex. This tool
reproduces exactly those two properties and nothing else.

Usage: ds5_udp_blast.py --target IP [--port 9] [--mbps 65] [--hz 60]
                        [--dscp 0] [--seconds 120]

DSCP is the DSCP value (0..63); it is shifted into the TOS byte here.
40 = CS5 -> WMM AC_VI (the video class), 46 = EF -> AC_VO.
Sends fixed 1400-byte datagrams in per-frame bursts; prints achieved Mbit/s
once per second so the driver can witness delivery.
"""
import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--port", type=int, default=9)
    ap.add_argument("--mbps", type=float, default=65.0)
    ap.add_argument("--hz", type=float, default=60.0)
    ap.add_argument("--dscp", type=int, default=0)
    ap.add_argument("--seconds", type=int, default=120)
    a = ap.parse_args()

    payload = bytes(1400)
    per_burst = max(1, round(a.mbps * 1e6 / 8 / a.hz / len(payload)))
    period = 1.0 / a.hz

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, (a.dscp & 0x3F) << 2)
    s.connect((a.target, a.port))

    t0 = time.monotonic()
    next_t = t0
    sent_bytes = 0
    last_report = t0
    while True:
        now = time.monotonic()
        if now - t0 >= a.seconds:
            break
        if now < next_t:
            time.sleep(next_t - now)
        next_t += period
        for _ in range(per_burst):
            try:
                s.send(payload)
                sent_bytes += len(payload)
            except OSError:
                # ICMP unreachable bounce-back surfaces as ECONNREFUSED on a
                # connected UDP socket; the datagrams still went out. Reconnect
                # semantics are unchanged — just keep sending.
                pass
        now = time.monotonic()
        if now - last_report >= 1.0:
            print(f"[blast] {sent_bytes * 8 / (now - last_report) / 1e6:.1f} Mbit/s "
                  f"dscp={a.dscp} burst={per_burst}x1400B @ {a.hz:.0f}Hz", flush=True)
            sent_bytes = 0
            last_report = now
    return 0


if __name__ == "__main__":
    sys.exit(main())
