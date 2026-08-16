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
# programme sees, so drain it after every window instead of discovering the hole
# in the analysis.
echo 1 > /tmp/ds5_gaplog
: > /tmp/ds5_gaps.log

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

nohup sh -c 'while :; do tail -n 0 -F /tmp/ds5_txd.log; done' >"$OUT/ledger.log" 2>&1 &
LEDGER=$!
trap 'kill "$LEDGER" 2>/dev/null; exit 130' INT TERM

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
    cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
    : > /tmp/ds5_gaps.log
    say "window $i recorded (${D}s)"
done

cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
sort -u -k2,2 "$OUT/gaps.raw" > "$OUT/gaps.log" 2>/dev/null
kill "$LEDGER" 2>/dev/null
say "client gone -> done -> $OUT"
