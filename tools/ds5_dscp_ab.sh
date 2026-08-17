#!/bin/sh
# ds5_dscp_ab.sh — VM-side driver: does the WMM CLASS of competing wifi traffic
# move the audio-gap tail, at constant volume and constant burst pattern?
#
#   OFF arm: 60 Hz UDP bursts, DSCP 0  (best effort, AC_BE)
#   ON  arm: same bursts, same Mbit/s, DSCP 40 (CS5 -> AC_VI, the video class)
#
# Runs FROM the VM because the load has to originate on the LAN side of the AP —
# the AP's downlink WMM queue is where DSCP becomes airtime priority. The rig on
# the TV supplies the continuous audio; gap records are drained per block over
# ssh, so no clock alignment between VM and TV is ever needed.
#
#   ds5_dscp_ab.sh [blocks] [block_s] [mbps] [dscp_on]
set -u
BLOCKS="${1:-6}"
BLOCK="${2:-120}"
MBPS="${3:-65}"
DSCP_ON="${4:-40}"
GUARD=5
TV=LG
TVIP=192.168.0.128
BLAST="$(dirname "$0")/ds5_udp_blast.py"
OUT="/tmp/dscpab-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
say(){ echo "$(date '+%F %T') $*"; }

ssh -o BatchMode=yes -o ConnectTimeout=8 "$TV" true || { say "TV unreachable"; exit 1; }
if ssh "$TV" 'ps ax' | grep -q "[d]s5_autorun"; then
    say "REFUSING: an autorun measurement is running on the TV"; exit 1
fi

TOTAL=$(( BLOCKS * (BLOCK + GUARD) + 60 ))
say "pinning cores, arming gaplog, starting rig for ${TOTAL}s"
ssh "$TV" 'echo 0 > /proc/lg/pm/mp_enable; for c in 1 2 3; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done; echo 10 > /tmp/ds5_inject_fifo; echo 1 > /tmp/ds5_gaplog; rm -f /tmp/ds5_r36 /tmp/ds5_ptype /tmp/ds5_linkq_ms; : > /tmp/ds5_gaps.log'
ssh "$TV" "nohup /tmp/ds5_synth_audio --b 60 --seconds $TOTAL --stats 30 >/tmp/rig_dscp.log 2>&1 &"
sleep 15
ssh "$TV" 'tail -1 /tmp/rig_dscp.log'

BLAST_PID=""
cleanup(){
    [ -n "$BLAST_PID" ] && kill "$BLAST_PID" 2>/dev/null
    ssh "$TV" 'rigs=$(ps ax | awk "/[s]ynth_audio/ {print \$1}"); [ -n "$rigs" ] && kill $rigs; echo 1 > /proc/lg/pm/mp_enable' 2>/dev/null
    say "cleanup done"
}
trap 'cleanup; exit 130' INT TERM

i=0
while [ "$i" -lt "$BLOCKS" ]; do
    i=$((i+1))
    if [ $((i % 2)) -eq 0 ]; then ARM=on;  DSCP=$DSCP_ON
    else                          ARM=off; DSCP=0
    fi
    python3 "$BLAST" --target "$TVIP" --mbps "$MBPS" --dscp "$DSCP" \
        --seconds $((BLOCK + GUARD + 5)) > "$OUT/blast_$i.log" 2>&1 &
    BLAST_PID=$!
    sleep "$GUARD"
    ssh "$TV" ': > /tmp/ds5_gaps.log'            # discard guard-period records
    RX0=$(ssh "$TV" "awk '/wlan0/{print \$2}' /proc/net/dev")
    S=$(date +%s)
    sleep "$BLOCK"
    E=$(date +%s)
    RX1=$(ssh "$TV" "awk '/wlan0/{print \$2}' /proc/net/dev")
    ssh "$TV" 'cat /tmp/ds5_gaps.log; : > /tmp/ds5_gaps.log' > "$OUT/gaps_${i}_${ARM}.log"
    kill "$BLAST_PID" 2>/dev/null; wait "$BLAST_PID" 2>/dev/null; BLAST_PID=""
    MB=$(( (RX1 - RX0) * 8 / (E - S) / 1000000 ))
    printf '%s\t%s\t%s\t%s\t%s\n' "$i" "$ARM" "$S" "$E" "$MB" >> "$OUT/windows.tsv"
    say "block $i/$BLOCKS arm=$ARM dscp=$DSCP tv_rx=${MB}Mbit/s records=$(grep -c '^G ' "$OUT/gaps_${i}_${ARM}.log")"
done
cleanup
say "done -> $OUT"
