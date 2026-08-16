#!/bin/sh
# ds5_ab_flush.sh — interleaved within-session A/B for the L1 auto-flush lever.
#
# Why interleaved and not one arm per evening: the cross-day protocol in the
# Phase-2 handover exists because coex weather drifts between evenings. That
# same drift is what makes a single-arm evening unreadable when only one
# evening is available. L1 was built to be reversible without a daemon restart
# (see flush_ms_want() in ds5_txd.c: cached ~1/s, disarm writes the infinite
# timeout back), so OFF/ON/OFF/ON inside one session is the cheaper control —
# it pairs the arms against the same radio conditions and the same player.
#
# Each block is a full, independent ds5_phase2_run.sh slice, so every block
# carries its own sniff log, its own daemon-ledger delta and its own manifest.
# Nothing here touches the daemon: the only write is the operator toggle the
# daemon polls by design.
#
#   ds5_ab_flush.sh [block_seconds] [flush_ms]
#   nohup /tmp/ds5_ab_flush.sh 420 80 >/tmp/ab_flush.log 2>&1 &
#
# Start it when the game is ALREADY running: connect-time churn (bind storm,
# max_slots re-pricing) would otherwise land entirely in the first OFF block
# and bias the baseline against the lever.
set -u

BLOCK="${1:-420}"
FLUSH_MS="${2:-80}"
GUARD=15                       # toggle -> reconcile (>=3s throttle) -> read-back
TOG=/tmp/ds5_flush_ms
RUN=/tmp/ds5_phase2_run.sh

[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
[ -x "$RUN" ] || { echo "missing $RUN"; exit 1; }

# Never two levers at once: the ledger cannot attribute the delta.
if [ -e /tmp/ds5_ghost_ttl_ms ]; then
    echo "REFUSING: /tmp/ds5_ghost_ttl_ms exists (L4 armed). One lever at a time."
    exit 1
fi
if ! pidof ds5_txd >/dev/null 2>&1; then
    echo "REFUSING: ds5_txd is not running — launch the app and start the stream first."
    exit 1
fi

say(){ echo "$(date '+%F %T') $*"; }

# The toggle is left disarmed on ANY exit path, including Ctrl-C: an armed
# lever surviving the capture would silently contaminate whatever runs next.
cleanup(){ rm -f "$TOG"; say "toggle disarmed (cleanup)"; }
trap 'cleanup; exit 130' INT TERM

block(){   # $1 = label, $2 = on|off
    if [ "$2" = "on" ]; then
        echo "$FLUSH_MS" > "$TOG"; chown 0:0 "$TOG" 2>/dev/null; chmod 644 "$TOG"
        say "ARM   L1 flush=${FLUSH_MS}ms  -> block $1"
    else
        rm -f "$TOG"
        say "DISARM L1                     -> block $1"
    fi
    sleep "$GUARD"
    "$RUN" "$1" "$BLOCK"
}

say "interleaved L1 A/B: 4 x ${BLOCK}s (+${GUARD}s guards), flush=${FLUSH_MS}ms"
block ab_off1 off
block ab_on1  on
block ab_off2 off
block ab_on2  on
cleanup
say "A/B complete -> /tmp/phase2/ab_off1 ab_on1 ab_off2 ab_on2"
