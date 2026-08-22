#!/bin/sh
# ds5_session_record.sh — record a REAL streaming session. Passive: no rig, no
# lever, nothing touched. Runs on the TV.
#
#   nohup /tmp/ds5_session_record.sh [window_s] >/tmp/session.log 2>&1 &
#
# Why passive. Everything the synthetic rig cannot reproduce lives in a real
# session: the decoder's and renderer's CPU, the client's SCHED_RR threads, and
# the stream's own airtime. Measured on 2026-08-16, CPU contention alone moved
# the >=70 ms tail x3.37 and cost up to 13 ms of pad buffer — more than any
# bluetooth-side lever in this programme. So the question this recording answers
# is not "does a lever work" but "how much of the machine is left during play,
# and what is the tail while it is not".
#
# It waits for the client, then writes one row per window and a CPU census
# alongside, and stops when the client exits. Toggling anything during someone
# else's game would be both rude and unattributable, so it toggles nothing.
set -u
W="${1:-120}"
[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
pidof ds5_txd >/dev/null 2>&1 || { echo "REFUSING: ds5_txd is not running"; exit 1; }

OUT="/tmp/session-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 1
say(){ echo "$(date '+%F %T') $*"; }

# The per-gap log is the whole point, and the daemon WIPES it whole at 256 KiB
# (gaplog_flush reopens O_TRUNC). Gameplay's gap rate is the highest this
# programme sees, so rotate it away after every window instead of discovering the
# hole in the analysis.
echo 1 > /tmp/ds5_gaplog
: > /tmp/ds5_gaps.log
# The rotate below leaves its last snapshot lying in /tmp on purpose, so a clean
# start has to remove that as deliberately as it empties the live log: a previous
# recording's leftovers merged into this one's gaps.raw would date events into
# windows that never saw them.
GAPSNAP=/tmp/ds5_gaps.snap
rm -f "$GAPSNAP"

# The per-window snapshot RENAMES the daemon's log, it never copies-then-
# truncates it: gaplog_flush() runs once per capture wakeup, so it can append
# between the copy and the ': >', and the truncate then throws those entries away
# having never reached gaps.raw. rename() is atomic, and the daemon holds NO
# descriptor across flushes (it opens /tmp/ds5_gaps.log O_CREAT|O_APPEND, writes
# and closes inside one call), so its next flush creates the path anew and
# nothing has to tell it to reopen. Records from a flush already inside its own
# open/write/close at the instant of the rename land in the renamed file and are
# collected by the NEXT rotate, which appends the leftover snapshot before
# replacing it; the duplicate lines that costs are free, because the merge
# dedupes whole lines.
gap_rotate(){
    [ -f "$GAPSNAP" ] && cat "$GAPSNAP" >> "$OUT/gaps.raw" 2>/dev/null
    if mv -f /tmp/ds5_gaps.log "$GAPSNAP" 2>/dev/null; then
        cat "$GAPSNAP" >> "$OUT/gaps.raw" 2>/dev/null
    fi                       # no live log = nothing rotated; the snapshot stands
    return 0
}

cores_now(){
    awk '{s=0;n=split($0,a,",");for(i=1;i<=n;i++){m=split(a[i],b,"-");s+=(m==2?b[2]-b[1]+1:1)}print s}' \
        /sys/devices/system/cpu/online 2>/dev/null
}
cpu_snap(){ awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=$i;print t, $5}' /proc/stat; }
busy_pct(){ set -- $1 $2; d=$(( $3 - $1 )); di=$(( $4 - $2 ));
            [ "$d" -gt 0 ] && echo $(( (d-di)*100/d )) || echo 0; }

say "waiting for the stream client..."
n=0
while ! pidof aurora >/dev/null 2>&1 && ! pidof moonlight >/dev/null 2>&1; do
    sleep 2; n=$((n+1))
    [ "$n" -gt 900 ] && { say "no client after 30 min — giving up"; exit 1; }
done
say "client up -> recording ${W}s windows into $OUT"

# tail runs DIRECTLY, never wrapped in a restart loop. Killing the wrapper kills
# only the wrapper: the inner tail is reparented to init with its fd on THIS
# run's ledger.log still open, and it then appends every future daemon line to a
# recording that ended long ago — a later re-analysis of that directory counts
# foreign rebinds and reads foreign flush states, and the leaked processes stack
# up recording after recording on someone's television. -F already retries by
# itself if the daemon's log goes away, which is all the loop ever bought.
nohup tail -n 0 -F /tmp/ds5_txd.log >"$OUT/ledger.log" 2>&1 </dev/null &
LEDGER_PID=$!
trap 'kill "$LEDGER_PID" 2>/dev/null; exit 130' INT TERM

i=0
while pidof aurora >/dev/null 2>&1 || pidof moonlight >/dev/null 2>&1; do
    i=$((i+1))
    S=$(date +%s); RX0=$(awk '/wlan0/{print $2}' /proc/net/dev); C0=$(cpu_snap)
    CSUM=0; CN=0; t=0
    while [ "$t" -lt "$W" ]; do
        sleep 10; t=$((t+10))
        c=$(cores_now); CSUM=$((CSUM + ${c:-0})); CN=$((CN+1))
        # A census, not a total: the total says whether the machine is busy, the
        # census says who is holding it — which is the only version of the
        # question that turns into a change anybody can make.
        if [ $((t % 30)) -eq 0 ]; then
            { echo "--- $(date '+%H:%M:%S') window $i"
              top -b -n 1 2>/dev/null | sed -n '7,16p'; } >> "$OUT/cpu.log"
        fi
        pidof aurora >/dev/null 2>&1 || pidof moonlight >/dev/null 2>&1 || break
    done
    E=$(date +%s); RX1=$(awk '/wlan0/{print $2}' /proc/net/dev); C1=$(cpu_snap)
    D=$((E - S)); [ "$D" -lt 1 ] && D=1
    printf 'play\t%s\t%s\t%s\t%s\t%s\t%s\n' "$S" "$E" \
        "$(( (RX1 - RX0) * 8 / D / 1000 ))" "$(pidof aurora >/dev/null 2>&1 && echo aurora || echo moonlight)" \
        "$(( CSUM * 10 / CN ))" "$(busy_pct "$C0" "$C1")" >> "$OUT/windows.tsv"
    gap_rotate
    say "window $i recorded (${D}s)"
done

gap_rotate
# Dedupe on the WHOLE line, never on the stamp alone: with a key, `sort -u`
# compares only that key, and the daemon stamps EVERY link of one NOCP event with
# the same wall-clock ms — a radio-global blackout that ends both pads of a
# two-pad session writes two records with an identical stamp and a different h=,
# of which the keyed dedupe silently kept one. That halved exactly the event
# class this recording exists to capture. Making the stamp artificially unique
# would falsify it instead; whole-line dedupe cannot drop a real record, because
# two gaps of the same length on the same handle in the same millisecond cannot
# both exist, while the overlap the rotate deliberately produces is byte-identical
# and collapses as intended.
sort -u "$OUT/gaps.raw" > "$OUT/gaps.log" 2>/dev/null
kill "$LEDGER_PID" 2>/dev/null
say "client gone -> done -> $OUT"
