#!/bin/sh
# ds5_session_ab.sh — the one experiment the rig cannot do: toggle the game-mode
# guard's RT boost during a REAL streaming session and watch the audio tail.
#
#   nohup /tmp/ds5_session_ab.sh [blocks] [block_s] >/tmp/session_ab.log 2>&1 &
#
# Why. Synthetic loops at SCHED_RR 20 reproduced gameplay's >=70 ms tail almost
# exactly (14.5/min against the game's 14.3) where the same loops at nice 0 did
# not (6.2/min at 70 % cpu busy). The guard's boost_game puts EVERY thread of the
# client on SCHED_RR 20 for the life of a session; ds5_txd runs SCHED_OTHER with
# its capture thread at nice 19. RR preempts SCHED_OTHER whatever the nice, so
# the transport loses to the client exactly while audio is flowing. Two spinning
# loops are an upper bound on that, though — the client's RT threads mostly block
# — and only a real session can say what it is worth in practice.
#
# ON  arm = boosted   (SCHED_RR 20 + nice -10 on every client thread: what the
#                      guard does today, and the state the user normally plays in)
# OFF arm = unboosted (SCHED_OTHER nice 0: what removing the boost would give)
#
# The guard's own loop re-asserts the boost every 3 s, so it is stopped for the
# duration and restarted afterwards — an OFF arm the machine keeps undoing is not
# an OFF arm. Everything else the guard applied (pinned cores, stopped services)
# persists on its own and is left alone, so the arms differ in ONE thing.
set -u
BLOCKS="${1:-6}"
BLOCK="${2:-90}"
GUARD_S=10
[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
pidof ds5_txd >/dev/null 2>&1 || { echo "REFUSING: ds5_txd is not running"; exit 1; }

OUT="/tmp/sessionab-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 1
say(){ echo "$(date '+%F %T') $*"; }
GUARD_PIDFILE=/tmp/moonlight-guard.pid
GUARD_SH=/var/lib/webosbrew/moonlight-guard.sh

client_pid(){ pidof aurora 2>/dev/null || pidof moonlight 2>/dev/null; }

boost_on(){                     # what gamemode.sh boost_game does, verbatim
    mp=$(client_pid); [ -z "$mp" ] && return 0
    for t in /proc/"$mp"/task/*; do
        tid=${t##*/}
        renice -n -10 -p "$tid" >/dev/null 2>&1
        chrt -r -p 20 "$tid" >/dev/null 2>&1
    done
}
boost_off(){                    # what restore_game does
    mp=$(client_pid); [ -z "$mp" ] && return 0
    for t in /proc/"$mp"/task/*; do
        tid=${t##*/}
        chrt -o -p 0 "$tid" >/dev/null 2>&1
        renice -n 0 -p "$tid" >/dev/null 2>&1
    done
}
# Read-back is not effect: count how many of the client's threads REALLY hold a
# real-time policy right now. A block whose witness disagrees with its arm is not
# evidence, and this project has paid for that lesson more than once.
rr_threads(){
    mp=$(client_pid); [ -z "$mp" ] && { echo 0; return; }
    n=0
    for t in /proc/"$mp"/task/*; do
        case "$(chrt -p "${t##*/}" 2>/dev/null | sed -n 's/.*policy: //p')" in
            SCHED_RR|SCHED_FIFO) n=$((n+1)) ;;
        esac
    done
    echo "$n"
}
cores_now(){
    awk '{s=0;n=split($0,a,",");for(i=1;i<=n;i++){m=split(a[i],b,"-");s+=(m==2?b[2]-b[1]+1:1)}print s}' \
        /sys/devices/system/cpu/online 2>/dev/null
}
cpu_snap(){ awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=$i;print t, $5}' /proc/stat; }
busy_pct(){ set -- $1 $2; d=$(( $3 - $1 )); di=$(( $4 - $2 ));
            [ "$d" -gt 0 ] && echo $(( (d-di)*100/d )) || echo 0; }

restore_all(){
    boost_on                                   # leave the user in the state they play in
    [ -x "$GUARD_SH" ] && { nohup "$GUARD_SH" >/dev/null 2>&1 & }
    say "restored: client boosted, guard restarted"
}
trap 'restore_all; exit 130' INT TERM

echo 1 > /tmp/ds5_gaplog
: > /tmp/ds5_gaps.log
printf 'lever=boost blocks=%s block_s=%s\nstarted=%s\n' \
    "$BLOCKS" "$BLOCK" "$(date '+%F %T')" > "$OUT/conditions.txt"

say "waiting for the stream client..."
n=0
while [ -z "$(client_pid)" ]; do
    sleep 2; n=$((n+1))
    [ "$n" -gt 900 ] && { say "no client after 30 min — giving up"; exit 1; }
done
CP=$(client_pid)
say "client up (pid $CP) — waiting for audio to actually flow"
# The client process existing is not the same as a stream running: at the app's
# own menu nothing is injected, the daemon's audio gate suppresses binning, and
# the blocks would measure an idle link with a full denominator. Wait for the
# injection counter to move like real audio (>20/s) before starting the clock.
n=0
while [ "$n" -lt 90 ]; do
    a=$(grep -o '^\[txd\] inj=[0-9]*' /tmp/ds5_txd.log | tail -1 | cut -d= -f2)
    sleep 10
    b=$(grep -o '^\[txd\] inj=[0-9]*' /tmp/ds5_txd.log | tail -1 | cut -d= -f2)
    [ -n "$a" ] && [ -n "$b" ] && [ "$((b - a))" -gt 200 ] && break
    [ -z "$(client_pid)" ] && { say "client gone while waiting for audio"; exit 1; }
    n=$((n+1))
done
say "audio flowing -> $BLOCKS x ${BLOCK}s into $OUT"

# The guard would re-assert the boost inside 3 s and quietly turn every OFF arm
# into an ON arm.
if [ -f "$GUARD_PIDFILE" ]; then
    kill "$(cat "$GUARD_PIDFILE")" 2>/dev/null && say "guard loop stopped for the run"
fi
# Hold the cores constant across arms so the only difference is the policy.
[ -e /proc/lg/pm/mp_enable ] && echo 0 > /proc/lg/pm/mp_enable
for c in 1 2 3; do echo 1 > "/sys/devices/system/cpu/cpu$c/online" 2>/dev/null; done

nohup sh -c 'while :; do tail -n 0 -F /tmp/ds5_txd.log; done' >"$OUT/ledger.log" 2>&1 &
LEDGER=$!

i=0
while [ "$i" -lt "$BLOCKS" ]; do
    i=$((i+1))
    if [ $((i % 2)) -eq 0 ]; then ARM=on;  boost_on
    else                          ARM=off; boost_off
    fi
    say "block $i/$BLOCKS arm=$ARM (guard ${GUARD_S}s, then ${BLOCK}s)"
    sleep "$GUARD_S"
    [ -z "$(client_pid)" ] && { say "client gone during the guard window"; break; }
    S=$(date +%s); RX0=$(awk '/wlan0/{print $2}' /proc/net/dev); C0=$(cpu_snap)
    RR0=$(rr_threads)
    CSUM=0; CN=0; t=0
    while [ "$t" -lt "$BLOCK" ]; do
        sleep 10; t=$((t+10))
        c=$(cores_now); CSUM=$((CSUM + ${c:-0})); CN=$((CN+1))
        [ -z "$(client_pid)" ] && break
    done
    E=$(date +%s); RX1=$(awk '/wlan0/{print $2}' /proc/net/dev); C1=$(cpu_snap)
    RR1=$(rr_threads)
    if [ -z "$(client_pid)" ]; then
        say "client exited during block $i — block NOT recorded"
        cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
        break
    fi
    D=$((E - S)); [ "$D" -lt 1 ] && D=1
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$ARM" "$S" "$E" \
        "$(( (RX1 - RX0) * 8 / D / 1000 ))" "aurora" "$(( CSUM * 10 / CN ))" \
        "$(busy_pct "$C0" "$C1")" "$RR0/$RR1" >> "$OUT/windows.tsv"
    cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
    : > /tmp/ds5_gaps.log
    say "block $i recorded (${D}s, rt-threads $RR0->$RR1)"
done

cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
sort -u -k2,2 "$OUT/gaps.raw" > "$OUT/gaps.log" 2>/dev/null
kill "$LEDGER" 2>/dev/null
restore_all
say "done -> $OUT"
