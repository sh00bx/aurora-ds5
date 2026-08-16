#!/bin/sh
# ds5_autorun.sh — an unattended interleaved A/B on the TV, with the workload
# manufactured locally by ds5_synth_audio instead of by a human playing a game.
#
#   ds5_autorun.sh <lever> <blocks> <block_seconds> [B_ms]
#
#   lever          ptype | wifi | none
#                  ptype = the L18 packet-type clamp
#                  wifi  = POSITIVE CONTROL: bulk download over wlan0 during ON
#                          blocks. WiFi and bluetooth are one combo module here,
#                          so this is a real coex perturbation even on 5 GHz. An
#                          instrument that cannot be moved by it is a null
#                          instrument, and nothing it says about a lever counts.
#                  none  = pure baseline / equivalence run
#   blocks         how many blocks TOTAL (alternating OFF/ON, so use an even number)
#   block_seconds  length of one block (>=120 recommended; the >=60ms band needs
#                  a few hundred events per arm before a ratio means anything)
#   B_ms           pad buffer depth to advertise (default 60)
#
#   nohup /tmp/ds5_autorun.sh ptype 8 300 40 >/tmp/autorun.log 2>&1 &
#
# Why the toggling lives here and not in the rig: the rig is an INSTRUMENT and
# must stay identical across arms. A program that both produces the workload and
# moves the lever can hide a coupling between them, and this project has already
# paid for that class of mistake more than once.
#
# Output: /tmp/autorun-<lever>-<stamp>/
#   windows.tsv   arm, start_epoch_s, end_epoch_s   (guards excluded)
#   gaps.log      the daemon's per-gap wall-clock log, copied at the end
#   ledger.log    every daemon status line seen during the run
#   synth.log     the rig's own delivery accounting — READ THIS FIRST: a block
#                 whose rig did not deliver ~46.9/s with drop=0 measured a
#                 starved transport, not the lever.
set -u

LEVER="${1:-none}"
BLOCKS="${2:-4}"
BLOCK="${3:-300}"
B_MS="${4:-60}"
GUARD=15

RIG=/tmp/ds5_synth_audio
[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
[ -x "$RIG" ] || { echo "REFUSING: $RIG missing"; exit 1; }

LOAD_URL="${LOAD_URL:-http://192.168.0.218:8001/ds5_loadtest.bin}"
case "$LEVER" in
    ptype) TOG=/tmp/ds5_ptype;  ON_VALUE=1 ;;
    wifi)  TOG=""; ON_VALUE="" ;;
    none)  TOG=""; ON_VALUE="" ;;
    *) echo "unknown lever '$LEVER' (ptype|wifi|none)"; exit 1 ;;
esac

load_on(){
    [ "$LEVER" = "wifi" ] || return 0
    nohup sh -c "while :; do curl -s -o /dev/null '$LOAD_URL' || sleep 1; done" \
        >/dev/null 2>&1 </dev/null &
    echo $! > /tmp/ds5_autorun_load.pid
}
load_off(){
    [ -f /tmp/ds5_autorun_load.pid ] || return 0
    kill "$(cat /tmp/ds5_autorun_load.pid)" 2>/dev/null
    pkill -f "$LOAD_URL" 2>/dev/null
    rm -f /tmp/ds5_autorun_load.pid
}

# One lever at a time: the ledger cannot attribute a delta to two of them.
# Lever state is DECLARED, never discovered: a knob left armed by a previous
# session silently redefines the baseline, and this has already happened here.
for other in /tmp/ds5_flush_ms /tmp/ds5_ghost_ttl_ms /tmp/ds5_inject_maxq /tmp/ds5_gap_inject; do
    [ -e "$other" ] && { echo "REFUSING: $other is armed — declare it or remove it"; exit 1; }
done
# The link-quality poller can trip the daemon's HCI command guard ("COMMAND PATH
# DEAD"), which pauses exactly the sniff and packet-type management an HCI lever
# needs. Instruments are welcome; this one is not, during a lever run.
if [ -e /tmp/ds5_linkq_ms ] && [ "$LEVER" != "none" ]; then
    echo "REFUSING: /tmp/ds5_linkq_ms armed — it can park the HCI command path"; exit 1
fi
pidof ds5_txd >/dev/null 2>&1 || { echo "REFUSING: ds5_txd is not running"; exit 1; }

OUT="/tmp/autorun-$LEVER-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 1
say(){ echo "$(date '+%F %T') $*"; }

# Production parity: the supervisor's boot default is a SHALLOW audio FIFO (3);
# the app deepens it to 10 against a rate-servo host, and every reference number
# was measured that way. The file wins over the control datagram, so set it here.
echo 10 > /tmp/ds5_inject_fifo
echo 1 > /tmp/ds5_gaplog          # per-gap wall-clock records are how blocks get cut

cleanup(){
    [ -n "$TOG" ] && rm -f "$TOG"
    load_off
    kill "$RIG_PID" 2>/dev/null
    say "cleanup: lever disarmed, load stopped, rig stopped"
}
trap 'cleanup; exit 130' INT TERM

TOTAL=$(( BLOCKS * (BLOCK + GUARD) + 30 ))
say "starting rig for ${TOTAL}s (B=$B_MS), $BLOCKS x ${BLOCK}s blocks, lever=$LEVER"
"$RIG" --b "$B_MS" --seconds "$TOTAL" --stats 10 >"$OUT/synth.log" 2>&1 &
RIG_PID=$!

# Let delivery settle before the first block: the first seconds include the
# bootstrap, the SetState prime and any sniff exit the daemon still has to do.
sleep 20
if ! kill -0 "$RIG_PID" 2>/dev/null; then
    say "FATAL: the rig died during warm-up — see $OUT/synth.log"; exit 1
fi

# Warm-up acceptance: if the transport is not carrying the offered load there is
# no point running blocks. This is the guard that would have caught the sniffed
# link that produced 15 of 46 reports per second.
DELIVER=$(awk '/^\[synth\] t=/{r=$4} END{print r}' "$OUT/synth.log")
say "warm-up delivery: ${DELIVER:-unknown} (want ~rate=46.9/s)"

nohup sh -c 'while :; do tail -n 0 -F /tmp/ds5_txd.log; done' >"$OUT/ledger.log" 2>&1 &
LEDGER_PID=$!

i=0
while [ "$i" -lt "$BLOCKS" ]; do
    i=$((i+1))
    if [ $((i % 2)) -eq 0 ] && [ "$LEVER" != "none" ]; then
        ARM=on
        [ -n "$TOG" ] && { echo "$ON_VALUE" > "$TOG"; chown 0:0 "$TOG" 2>/dev/null; chmod 644 "$TOG"; }
        load_on
    else
        ARM=off
        [ -n "$TOG" ] && rm -f "$TOG"
        load_off
    fi
    say "block $i/$BLOCKS arm=$ARM (guard ${GUARD}s, then ${BLOCK}s)"
    sleep "$GUARD"                      # the lever needs a reconcile pass to land
    S=$(date +%s)
    RX0=$(awk '/wlan0/{print $2}' /proc/net/dev)
    sleep "$BLOCK"
    E=$(date +%s)
    RX1=$(awk '/wlan0/{print $2}' /proc/net/dev)
    # WiFi and bluetooth share the MT7921, so what else was on the air during a
    # block belongs in the record rather than in hindsight: a run taken while the
    # TV was streaming is not the same experiment as one taken on a quiet radio.
    FG=$(sed -n 's/.*"appId":"\([^"]*\)".*/\1/p' /var/luna/preferences/last_foreground_app_id.json 2>/dev/null)
    printf '%s\t%s\t%s\t%s\t%s\n' "$ARM" "$S" "$E" \
        "$(( (RX1 - RX0) * 8 / BLOCK / 1000 ))" "${FG:-none}" >> "$OUT/windows.tsv"
    kill -0 "$RIG_PID" 2>/dev/null || { say "rig died mid-run"; break; }
done

[ -n "$TOG" ] && rm -f "$TOG"
load_off
kill "$LEDGER_PID" 2>/dev/null
wait "$RIG_PID" 2>/dev/null
cp /tmp/ds5_gaps.log "$OUT/gaps.log" 2>/dev/null
say "done -> $OUT"
tail -1 "$OUT/synth.log"
