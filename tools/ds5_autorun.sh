#!/bin/sh
# ds5_autorun.sh — an unattended interleaved A/B on the TV, with the workload
# manufactured locally by ds5_synth_audio instead of by a human playing a game.
#
#   ds5_autorun.sh <lever> <blocks> <block_seconds> [B_ms]
#
#   lever          ptype | wifi | cores | cpu | none
#                  ptype = the L18 packet-type clamp
#                  wifi  = POSITIVE CONTROL: bulk download over wlan0 during ON
#                          blocks. WiFi and bluetooth are one combo module here,
#                          so this is a real coex perturbation even on 5 GHz. An
#                          instrument that cannot be moved by it is a null
#                          instrument, and nothing it says about a lever counts.
#                  cores = LG's MP governor off + cpu1-3 forced online (what the
#                          game-mode guard's pin_cpus does). The gap ledger is
#                          timed by the daemon's own event loop, so the number of
#                          cores it competes for can change the measurement as
#                          well as the transport. Left free, this TV runs on two.
#                  cpu   = CPU_N busy loops during ON blocks. The last untested
#                          candidate for the one thing the rig cannot reproduce:
#                          gameplay's tail beyond 70 ms is 5-10x fatter than the
#                          rig's even with the radio loaded and the cores pinned,
#                          and what a stream adds that a curl does not is a
#                          decoder and a renderer competing for the same cores.
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
    cores) TOG=""; ON_VALUE="" ;;
    cpu)   TOG=""; ON_VALUE="" ;;
    none)  TOG=""; ON_VALUE="" ;;
    *) echo "unknown lever '$LEVER' (ptype|wifi|cores|cpu|none)"; exit 1 ;;
esac

CPU_N="${CPU_N:-2}"          # busy loops on the ON arm of lever=cpu
CPU_PIDS=/tmp/ds5_autorun_cpu.pids
cpu_start(){
    : > "$CPU_PIDS"
    i=0
    while [ "$i" -lt "$CPU_N" ]; do
        i=$((i+1))
        nohup sh -c 'while :; do :; done' >/dev/null 2>&1 </dev/null &
        echo $! >> "$CPU_PIDS"
    done
}
cpu_stop(){
    [ -f "$CPU_PIDS" ] || return 0
    while read -r p; do [ -n "$p" ] && kill "$p" 2>/dev/null; done < "$CPU_PIDS"
    rm -f "$CPU_PIDS"
}
cpu_on(){  [ "$LEVER" = "cpu" ] && cpu_start; return 0; }
cpu_off(){ [ "$LEVER" = "cpu" ] && cpu_stop;  return 0; }
# Busy fraction over a block, from /proc/stat: idle is field 5, and everything
# else the kernel counts is work. A lever that says "the CPU was loaded" has to
# show it in the same file the scheduler bills against.
cpu_busy_pct(){   # $1 = snapshot before, $2 = snapshot after ("total idle")
    set -- $1 $2
    t0=$1; i0=$2; t1=$3; i1=$4
    dt=$((t1-t0)); di=$((i1-i0))
    [ "$dt" -gt 0 ] && echo $(( (dt-di)*100/dt )) || echo 0
}
cpu_snap(){ awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=$i;print t, $5}' /proc/stat; }

MP=/proc/lg/pm/mp_enable
cores_pin(){
    [ -e "$MP" ] && echo 0 > "$MP" 2>/dev/null
    for c in 1 2 3; do echo 1 > "/sys/devices/system/cpu/cpu$c/online" 2>/dev/null; done
    return 0
}
cores_unpin(){
    # The governor is handed back, not overridden the other way: hotplugging
    # cores down is this TV's own idle behaviour and is what the OFF arm is
    # supposed to measure.
    [ -e "$MP" ] && echo 1 > "$MP" 2>/dev/null
    return 0
}
cores_on(){  [ "$LEVER" = "cores" ] && cores_pin; return 0; }
cores_off(){
    [ "$BG_PIN" = "1" ] && return 0          # parity condition outranks the arm
    [ "$LEVER" = "cores" ] || return 0       # never touch a knob this run isn't using
    cores_unpin
}
# How many cores are online right now: "0-1" -> 2, "0,2-3" -> 3.
cores_now(){
    awk '{s=0;n=split($0,a,",");for(i=1;i<=n;i++){m=split(a[i],b,"-");s+=(m==2?b[2]-b[1]+1:1)}print s}' \
        /sys/devices/system/cpu/online 2>/dev/null
}

load_start(){
    # Rate-limited to ~72 Mbit/s on purpose rather than saturating: that is the
    # order of the 4K60 + FEC stream the reference sessions carried on this same
    # radio, so the control reproduces production's load instead of inventing a
    # worst case (and it leaves the household's wifi usable).
    nohup sh -c "while :; do curl -s --limit-rate 9M -o /dev/null '$LOAD_URL' || sleep 1; done" \
        >/dev/null 2>&1 </dev/null &
    echo $! > /tmp/ds5_autorun_load.pid
}
load_stop(){
    [ -f /tmp/ds5_autorun_load.pid ] || return 0
    kill "$(cat /tmp/ds5_autorun_load.pid)" 2>/dev/null
    pkill -f "$LOAD_URL" 2>/dev/null
    rm -f /tmp/ds5_autorun_load.pid
}
load_on(){  [ "$LEVER" = "wifi" ] && load_start; return 0; }
load_off(){ [ "$BG_LOAD" = "1" ] && return 0; load_stop; }

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
# The game-mode guard re-asserts pin_cpus from its own loop. That is exactly the
# knob the cores lever toggles, and a lever the machine keeps putting back is not
# a lever — the OFF arm would silently be an ON arm.
if [ "$LEVER" = "cores" ] && ps ax 2>/dev/null | grep -q "[g]amemode.sh"; then
    echo "REFUSING: a game-mode guard loop is running — it would fight the OFF arm"; exit 1
fi
pidof ds5_txd >/dev/null 2>&1 || { echo "REFUSING: ds5_txd is not running"; exit 1; }

# Background conditions, held in BOTH arms for the whole run. Gameplay is never a
# quiet TV: the guard pins the cores for the life of the client and the video
# stream sits on the shared radio the whole time. A lever measured on an idle
# machine is measured somewhere the game never goes.
BG_LOAD="${BG_LOAD:-0}"     # hold the wlan0 load up in every block
BG_PIN="${BG_PIN:-0}"       # hold cpu1-3 online in every block
[ "$BG_LOAD" = "1" ] && [ "$LEVER" = "wifi" ] && \
    { echo "REFUSING: BG_LOAD and lever=wifi are the same knob"; exit 1; }
[ "$BG_PIN" = "1" ] && [ "$LEVER" = "cores" ] && \
    { echo "REFUSING: BG_PIN and lever=cores are the same knob"; exit 1; }

OUT="/tmp/autorun-$LEVER-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 1
say(){ echo "$(date '+%F %T') $*"; }

# The conditions a run was taken under belong IN the run, not in whoever's memory
# analyses it later. Two runs of "the same" lever an hour apart already differed
# by a knob nobody wrote down.
printf 'lever=%s blocks=%s block_s=%s b_ms=%s bg_load=%s bg_pin=%s\nstarted=%s\n' \
    "$LEVER" "$BLOCKS" "$BLOCK" "$B_MS" "$BG_LOAD" "$BG_PIN" "$(date '+%F %T')" \
    > "$OUT/conditions.txt"

# Production parity: the supervisor's boot default is a SHALLOW audio FIFO (3);
# the app deepens it to 10 against a rate-servo host, and every reference number
# was measured that way. The file wins over the control datagram, so set it here.
echo 10 > /tmp/ds5_inject_fifo
echo 1 > /tmp/ds5_gaplog          # per-gap wall-clock records are how blocks get cut

cleanup(){
    [ -n "$TOG" ] && rm -f "$TOG"
    load_stop            # unconditional: background conditions end with the run
    cores_unpin          # four hot cores on someone's TV is not a default
    cpu_stop             # busy loops outlive their shell if nobody reaps them
    kill "$RIG_PID" 2>/dev/null
    say "cleanup: lever disarmed, load stopped, cores released, rig stopped"
}
trap 'cleanup; exit 130' INT TERM

[ "$BG_LOAD" = "1" ] && { load_start; say "background: wlan0 load held up in BOTH arms"; }
[ "$BG_PIN" = "1" ]  && { cores_pin;  say "background: cpu1-3 pinned online in BOTH arms"; }

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
        cores_on
        cpu_on
    else
        ARM=off
        [ -n "$TOG" ] && rm -f "$TOG"
        load_off
        cores_off
        cpu_off
    fi
    say "block $i/$BLOCKS arm=$ARM (guard ${GUARD}s, then ${BLOCK}s)"
    sleep "$GUARD"                      # the lever needs a reconcile pass to land
    S=$(date +%s)
    RX0=$(awk '/wlan0/{print $2}' /proc/net/dev)
    CPU0=$(cpu_snap)
    # Sample the online-core set through the block instead of sleeping blind:
    # mp_enable is a request to a governor, and the daemon's event loop — which
    # is what timestamps every gap in this programme — competes for whatever
    # cores are actually there. So the core count is a witness, not a setting.
    CSUM=0; CN=0; t=0
    while [ "$t" -lt "$BLOCK" ]; do
        sleep 10; t=$((t+10))
        c=$(cores_now); CSUM=$((CSUM + ${c:-0})); CN=$((CN+1))
    done
    E=$(date +%s)
    RX1=$(awk '/wlan0/{print $2}' /proc/net/dev)
    CPU1=$(cpu_snap)
    # WiFi and bluetooth share the MT7921, so what else was on the air during a
    # block belongs in the record rather than in hindsight: a run taken while the
    # TV was streaming is not the same experiment as one taken on a quiet radio.
    FG=$(sed -n 's/.*"appId":"\([^"]*\)".*/\1/p' /var/luna/preferences/last_foreground_app_id.json 2>/dev/null)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$ARM" "$S" "$E" \
        "$(( (RX1 - RX0) * 8 / BLOCK / 1000 ))" "${FG:-none}" \
        "$(( CSUM * 10 / CN ))" "$(cpu_busy_pct "$CPU0" "$CPU1")" >> "$OUT/windows.tsv"
    # The daemon's per-gap log is a CAPPED RING: at 256 KiB it drops the OLDEST
    # entries and leaves one TRUNCATED marker behind. Copying it once at the end
    # therefore loses whole early blocks while their exposure still counts — which
    # silently halves the measured rate. Snapshot after every block instead and
    # let the report dedupe; overlap is cheap, a lost block is not.
    cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
    kill -0 "$RIG_PID" 2>/dev/null || { say "rig died mid-run"; break; }
done

[ -n "$TOG" ] && rm -f "$TOG"
load_stop            # the background conditions end with the run, BG flags or not
cores_unpin
cpu_stop
kill "$LEDGER_PID" 2>/dev/null
wait "$RIG_PID" 2>/dev/null
cat /tmp/ds5_gaps.log >> "$OUT/gaps.raw" 2>/dev/null
sort -u -k2,2 "$OUT/gaps.raw" > "$OUT/gaps.log" 2>/dev/null
say "done -> $OUT"
tail -1 "$OUT/synth.log"
