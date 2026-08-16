#!/bin/sh
# ds5_ab_ptype.sh — interleaved within-session A/B for the L18 packet-type clamp.
#
# Same protocol as ds5_ab_flush.sh (interleaved beats one-arm-per-evening
# because coex weather drifts between evenings), driving /tmp/ds5_ptype
# instead. The lever needs one Change_Connection_Packet_Type per flip
# (>=3s reconcile throttle), so the guard covers toggle -> reconcile -> 0x1D.
#
#   ds5_ab_ptype.sh [block_seconds]
#   nohup /tmp/ds5_ab_ptype.sh 420 >/tmp/ab_ptype.log 2>&1 &
#
# Start it when the game is ALREADY running AND under CONTINUOUS pad audio
# (Ratchet hoverboots): the efficacy witness is the gapge distribution under
# load — silence-heavy blocks measure nothing (pre-registration W1 in
# workspace/ds5-l17-l18-preregistration-2026-08-16.md).
set -u

BLOCK="${1:-420}"
GUARD=15
TOG=/tmp/ds5_ptype
RUN=/tmp/ds5_phase2_run.sh

[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
[ -x "$RUN" ] || { echo "missing $RUN"; exit 1; }

# Never two levers at once: the ledger cannot attribute the delta.
# (Instruments — linkq, gaplog — are fine and WANTED: the per-gap log is how
# the blocks are cut in time.)
for other in /tmp/ds5_ghost_ttl_ms /tmp/ds5_flush_ms; do
    if [ -e "$other" ]; then
        echo "REFUSING: $other exists (another lever armed). One lever at a time."
        exit 1
    fi
done
if ! pidof ds5_txd >/dev/null 2>&1; then
    echo "REFUSING: ds5_txd is not running — launch the app and start the stream first."
    exit 1
fi

say(){ echo "$(date '+%F %T') $*"; }

# Disarmed on ANY exit path: the daemon then writes the full mask back
# (PTYPE_ALL) on its next reconcile pass, no restart needed.
cleanup(){ rm -f "$TOG"; say "toggle disarmed (cleanup)"; }
trap 'cleanup; exit 130' INT TERM

block(){   # $1 = label, $2 = on|off
    if [ "$2" = "on" ]; then
        echo 1 > "$TOG"; chown 0:0 "$TOG" 2>/dev/null; chmod 644 "$TOG"
        say "ARM   L18 ptype clamp  -> block $1"
    else
        rm -f "$TOG"
        say "DISARM L18             -> block $1"
    fi
    sleep "$GUARD"
    "$RUN" "$1" "$BLOCK"
}

say "interleaved L18 A/B: 4 x ${BLOCK}s (+${GUARD}s guards)"
block pt_off1 off
block pt_on1  on
block pt_off2 off
block pt_on2  on
cleanup
say "A/B complete -> /tmp/phase2/pt_off1 pt_on1 pt_off2 pt_on2"
