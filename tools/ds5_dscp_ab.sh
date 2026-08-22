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
# EVERY ssh in this script carries these, not just the reachability probe. A TV
# that has fallen off the air does not refuse a connection, it stops answering:
# the kernel then works through its full SYN retry ladder, minutes per call, and
# a block that drains over three tries can sit there for the better part of the
# rig's remaining --seconds budget before the abort below ever prints. BatchMode
# is the other half of the same rule — an unattended run must never park on a
# password prompt — and the ServerAlive pair bounds a session that established
# and then went quiet, which ConnectTimeout alone does not cover.
SSHO="-o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
BLAST="$(dirname "$0")/ds5_udp_blast.py"
OUT="/tmp/dscpab-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
say(){ echo "$(date '+%F %T') $*"; }

ssh $SSHO "$TV" true || { say "TV unreachable"; exit 1; }
if ssh $SSHO "$TV" 'ps ax' | grep -q "[d]s5_autorun"; then
    say "REFUSING: an autorun measurement is running on the TV"; exit 1
fi

# 10 s per block on top of BLOCK+GUARD: each block spends ~5 unbudgeted ssh
# roundtrips, and the 15 s warm-up below comes out of the same budget. A rig
# that outlives the last block by a minute is harmless — cleanup kills it — but
# one that dies mid-block turns every later block into a silent instrument.
TOTAL=$(( BLOCKS * (BLOCK + GUARD + 10) + 60 ))
say "pinning cores, arming gaplog, starting rig for ${TOTAL}s"
ssh $SSHO "$TV" 'echo 0 > /proc/lg/pm/mp_enable; for c in 1 2 3; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done; echo 10 > /tmp/ds5_inject_fifo; echo 1 > /tmp/ds5_gaplog; rm -f /tmp/ds5_r36 /tmp/ds5_ptype /tmp/ds5_linkq_ms /tmp/ds5_gaps.snap.*; : > /tmp/ds5_gaps.log'
ssh $SSHO "$TV" "nohup /tmp/ds5_synth_audio --b 60 --seconds $TOTAL --stats 30 --mute >/tmp/rig_dscp.log 2>&1 &"
sleep 15
ssh $SSHO "$TV" 'tail -1 /tmp/rig_dscp.log'

BLAST_PID=""
cleanup(){
    [ -n "$BLAST_PID" ] && kill "$BLAST_PID" 2>/dev/null
    ssh $SSHO "$TV" 'rigs=$(ps ax | awk "/[s]ynth_audio/ {print \$1}"); [ -n "$rigs" ] && kill $rigs; echo 1 > /proc/lg/pm/mp_enable' 2>/dev/null
    say "cleanup done"
}
trap 'cleanup; exit 130' INT TERM

# Draining the TV's gap log is TWO ssh calls on purpose. `cat …; : > …` in ONE
# call loses every record the daemon flushes between the two commands —
# gaplog_flush runs once per capture wakeup, and a block boundary is when the
# link is busiest. Renaming instead is atomic and stays entirely on the TV, so no
# record is ever in flight over ssh while it is being discarded; the daemon holds
# no descriptor across flushes (it opens /tmp/ds5_gaps.log O_CREAT|O_APPEND,
# writes and closes inside one call), so its next flush creates the path anew and
# nothing has to tell it to reopen. The split is also what makes this safe over a
# link that can drop: the second call only READS a file nothing writes to any
# more, so it can be repeated until the link cooperates — a dropped connection
# costs a retry, never a record, because the snapshot is still lying on the TV
# under a name this block owns. The `[ -e ]` guard keeps a retried rotate from
# rotating a SECOND time and dropping the first snapshot on the floor, and the
# `: >>` guarantees the snapshot exists even for a block that produced no gaps at
# all, so a failing read means the link and nothing else.
#
# What the retry does NOT repair is the bookkeeping. This block's exposure window
# S..E was fixed the moment E was read, one line above the call; when it is the
# FIRST call that fails the mv has not run yet, so the rotate that closes the
# block lands two 3 s sleeps and two failed attempts' ssh timeouts later —
# roughly 20 s at the ConnectTimeout above, and only that because of it. Every
# record the daemon flushes in that window is then filed under a block whose
# seconds ran out at E, so a retried block reports a rate that is too high by
# exactly that foreign exposure. It is bounded, and it is announced in the log
# above so the block is identifiable afterwards — which makes it the cheaper
# error than discarding a complete block over a link hiccup. Only a drain that
# never lands at all is worth aborting for.
gap_fetch(){   # $1 = block index, $2 = arm
    gf_snap="/tmp/ds5_gaps.snap.$1"
    gf_n=0
    while :; do
        gf_n=$((gf_n+1))
        if ssh $SSHO "$TV" "[ -e '$gf_snap' ] || mv -f /tmp/ds5_gaps.log '$gf_snap' 2>/dev/null; : >> '$gf_snap'" \
           && ssh $SSHO "$TV" "cat '$gf_snap'" > "$OUT/gaps_$1_$2.log"; then
            ssh $SSHO "$TV" "rm -f '$gf_snap'" >/dev/null 2>&1
            return 0
        fi
        [ "$gf_n" -ge 3 ] && break
        say "gap drain for block $1 failed (attempt $gf_n) — retrying in 3s"
        sleep 3
    done
    # Standing rule: a run that cannot prove its own preconditions aborts loudly.
    # The records are NOT lost — they are on the TV, named — but a block whose
    # drain never landed cannot be pooled against one whose drain did.
    say "ABORTING: could not drain the gap log for block $1 from the TV after $gf_n attempts"
    say "  they are still on the TV — in $gf_snap if the rotate landed, in /tmp/ds5_gaps.log if"
    say "  it did not — so recover them by hand before pooling anything from this run"
    cleanup
    exit 1
}

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
    ssh $SSHO "$TV" ': > /tmp/ds5_gaps.log'            # discard guard-period records
    RX0=$(ssh $SSHO "$TV" "awk '/wlan0/{print \$2}' /proc/net/dev")
    S=$(date +%s)
    sleep "$BLOCK"
    E=$(date +%s)
    RX1=$(ssh $SSHO "$TV" "awk '/wlan0/{print \$2}' /proc/net/dev")
    gap_fetch "$i" "$ARM"
    kill "$BLAST_PID" 2>/dev/null; wait "$BLAST_PID" 2>/dev/null; BLAST_PID=""
    # Liveness BEFORE the row (ds5_autorun.sh's rule): the rig stops itself on
    # link drops, delivery shortfalls, Aurora starts and its --seconds budget,
    # and the daemon stops binning 150 ms later — a block the rig did not
    # outlive would pool full exposure with near-zero events into whichever arm
    # was running. Abort, because every later block is dead the same way.
    if ! ssh $SSHO "$TV" 'ps ax | grep -q "[s]ynth_audio" && ! grep -q "STOPPING\|stopping\|DONE" /tmp/rig_dscp.log'; then
        say "rig not alive after block $i — block NOT recorded, aborting"
        ssh $SSHO "$TV" 'tail -2 /tmp/rig_dscp.log'
        break
    fi
    MB=$(( (RX1 - RX0) * 8 / (E - S) / 1000000 ))
    printf '%s\t%s\t%s\t%s\t%s\n' "$i" "$ARM" "$S" "$E" "$MB" >> "$OUT/windows.tsv"
    say "block $i/$BLOCKS arm=$ARM dscp=$DSCP tv_rx=${MB}Mbit/s records=$(grep -c '^G ' "$OUT/gaps_${i}_${ARM}.log")"
done
cleanup
say "done -> $OUT"
