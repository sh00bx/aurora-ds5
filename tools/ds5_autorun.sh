#!/bin/sh
# ds5_autorun.sh — an unattended interleaved A/B on the TV, with the workload
# manufactured locally by ds5_synth_audio instead of by a human playing a game.
#
#   ds5_autorun.sh <lever> <blocks> <block_seconds> [B_ms]
#
#   lever          ptype | wifi | cores | cpu | feed | none
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
#                  feed  = the rig's own send period, nominal vs the measured
#                          gameplay rate. The only lever that moves the INSTRUMENT
#                          rather than the machine, because the 18-minute session
#                          showed the real client holding one packet in flight
#                          where the rig holds four — and that, not CPU and not
#                          airtime, is what still separates them.
#                  none  = pure baseline / equivalence run
#   blocks         how many blocks TOTAL (alternating OFF/ON, so use an even number)
#   block_seconds  length of one block. Run length is a POWER decision, not a
#                  habit: the script prints, before the first block, the smallest
#                  ratio each edge could resolve at the measured event rates.
#                  Rules of thumb at parity rates (>=60 ~80/min, >=70 ~2.5/min):
#                    4 x 120s  =  9 min  ->  >=60 down to x0.86  (plenty for L18)
#                    6 x 120s  = 13 min  ->  >=70 down to x0.49  (tail levers)
#                    6 x 300s  = 32 min  ->  >=70 down to x0.64, >=80 to x0.40
#                  The tail is rare, so resolving SMALL tail effects costs half an
#                  hour and there is no way around it but time.
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
#   gaps.log      the daemon's per-gap wall-clock log, rotated out of /tmp
#                 after every block and merged (whole-line dedupe) at the end
#   ledger.log    every daemon status line seen during the run
#   synth.log     the rig's own delivery accounting — READ THIS FIRST: a block
#                 whose rig did not deliver ~46.9/s with drop=0 measured a
#                 starved transport, not the lever.
set -u

LEVER="${1:-none}"
BLOCKS="${2:-6}"
BLOCK="${3:-120}"
B_MS="${4:-60}"
GUARD=10

RIG=/tmp/ds5_synth_audio
[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
[ -x "$RIG" ] || { echo "REFUSING: $RIG missing"; exit 1; }

LOAD_URL="${LOAD_URL:-http://192.168.0.218:8001/ds5_loadtest.bin}"
case "$LEVER" in
    ptype) TOG=/tmp/ds5_ptype;  ON_VALUE=1 ;;
    wifi)  TOG=""; ON_VALUE="" ;;
    cores) TOG=""; ON_VALUE="" ;;
    cpu)   TOG=""; ON_VALUE="" ;;
    # 24050 us = 41.6 reports/s, the rate the real client fed during the 18-minute
    # Ratchet recording. Not tuned: it is the measured gameplay rate, and the
    # witness is whether the credit window falls from ~4 to ~1 as it did there.
    feed)  TOG=/tmp/ds5_period_us; ON_VALUE=24050 ;;
    # 3000 ms on / 700 ms off at 19200 us = bursts of 52/s (the game's p75-p90)
    # averaging 42/s (the game's mean). Both numbers are read off the recording,
    # not tuned. The period file is set alongside, so the ON arm is one declared
    # pattern rather than two knobs.
    burst) TOG=/tmp/ds5_burst; ON_VALUE="3000,700" ;;
    # 9 extra 0x32 per second: the gap between the game's peak injection rate
    # (56.3/s) and what audio alone needs (46.9/s), i.e. the co-traffic the real
    # client puts on the same link. Read off the recording, not chosen.
    cotraf) TOG=/tmp/ds5_cotraffic; ON_VALUE=9 ;;
    # Report format: ON = single-frame 0x36 at 10.67 ms cadence (~93.7/s),
    # OFF = batched 0x39 at 21.33 ms (~46.9/s). The daemon reads only the report
    # id, so this is purely the rig's producer — but the verdict is judged in PAD
    # CURRENCY with each arm's own in-flight term (B + 21.33 vs B + 10.67), never
    # on shared fixed-ms bins: a lever that changes the send cadence moves the
    # bins by construction (the Amendment-7 standing rule).
    r36)   TOG=/tmp/ds5_r36; ON_VALUE=1 ;;
    none)  TOG=""; ON_VALUE="" ;;
    *) echo "unknown lever '$LEVER' (ptype|wifi|cores|cpu|feed|burst|cotraf|r36|none)"; exit 1 ;;
esac

# Every toggle file a lever can arm — the daemon reads ptype, the rig reads the
# other four. The burst arm writes TWO of them (its TOG plus period_us), so
# "remove $TOG" was never the whole cleanup: a knob that survives the run
# silently redefines the next run's baseline in BOTH arms.
LEVER_FILES="/tmp/ds5_ptype /tmp/ds5_period_us /tmp/ds5_burst /tmp/ds5_cotraffic /tmp/ds5_r36"

CPU_N="${CPU_N:-2}"          # busy loops on the ON arm of lever=cpu
CPU_PIDS=/tmp/ds5_autorun_cpu.pids
cpu_start(){
    # NOT `i`: sh functions share one scope, and the block loop counts in `i`.
    # Using it here reset the counter on every ON arm (4 -> 2), so the loop
    # alternated 2,3,2,3 forever and only stopped when the rig's own budget ran
    # out. The blocks themselves were fine — the arms still alternated and the
    # witnesses were right — but the run would never have ended on its own.
    : > "$CPU_PIDS"
    cpu_i=0
    while [ "$cpu_i" -lt "$CPU_N" ]; do
        cpu_i=$((cpu_i+1))
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
# The current lever's own TOG is refused too: armed before the run began it is
# undeclared state all the same, poisoning the warm-up and the first OFF block.
for other in /tmp/ds5_flush_ms /tmp/ds5_ghost_ttl_ms /tmp/ds5_inject_maxq /tmp/ds5_gap_inject $LEVER_FILES; do
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
# The daemon does NOT shed the oldest records at its 256 KiB cap — gaplog_flush()
# reopens the file O_TRUNC and throws the WHOLE thing away, leaving one marker.
# A wipe therefore lands mid-run and deletes a block's events while the block
# keeps its full exposure in the denominator. It already did: it ate 171 s of an
# OFF block in the 19:55 run and 89 s of one in the 19:10 run, both times
# inflating the ON/OFF ratio. Start clean and drain after every block, so the
# live file never gets near the cap.
: > /tmp/ds5_gaps.log
# The rotate below leaves its last snapshot lying in /tmp on purpose, so a clean
# start has to remove that as deliberately as it empties the live log: merging a
# previous run's leftovers into this run's gaps.raw would date events into blocks
# that never saw them.
GAPSNAP=/tmp/ds5_gaps.snap
rm -f "$GAPSNAP"

# The per-block snapshot RENAMES the daemon's log, it never copies-then-truncates
# it: gaplog_flush() runs once per capture wakeup, so it can append between the
# copy and the ': >', and the truncate then throws those entries away having
# never reached gaps.raw — the same silent loss the per-block drain exists to
# prevent, only smaller. rename() is atomic, and the daemon holds NO descriptor
# across flushes (gaplog_flush opens /tmp/ds5_gaps.log O_CREAT|O_APPEND, writes
# and closes inside one call), so its next flush simply creates the path anew and
# nothing has to tell it to reopen. The only records that can still land in the
# renamed file come from a flush already inside its own open/write/close when the
# rename happened; the NEXT rotate collects those by appending the leftover
# snapshot before replacing it. Re-reading a snapshot costs duplicate lines, and
# duplicate lines cost nothing because the merge dedupes whole lines.
gap_rotate(){
    [ -f "$GAPSNAP" ] && cat "$GAPSNAP" >> "$OUT/gaps.raw" 2>/dev/null
    if mv -f /tmp/ds5_gaps.log "$GAPSNAP" 2>/dev/null; then
        cat "$GAPSNAP" >> "$OUT/gaps.raw" 2>/dev/null
    fi                       # no live log = nothing rotated; the snapshot stands
    return 0
}

# Both pids are declared before the trap is installed: cleanup() runs under
# `set -u`, and an INT arriving before the rig or the ledger tail was started
# would otherwise abort the trap on an unbound variable instead of disarming.
RIG_PID=""
LEDGER_PID=""
RIG_DIED=0
BUDGET_OUT=0
cleanup(){
    rm -f $LEVER_FILES   # the whole inventory: burst arms period_us alongside its TOG
    load_stop            # unconditional: background conditions end with the run
    cores_unpin          # four hot cores on someone's TV is not a default
    cpu_stop             # busy loops outlive their shell if nobody reaps them
    [ -n "$LEDGER_PID" ] && kill "$LEDGER_PID" 2>/dev/null
    [ -n "$RIG_PID" ] && kill "$RIG_PID" 2>/dev/null
    say "cleanup: lever disarmed, load stopped, cores released, rig+ledger stopped"
}
trap 'cleanup; exit 130' INT TERM

[ "$BG_LOAD" = "1" ] && { load_start; say "background: wlan0 load held up in BOTH arms"; }
[ "$BG_PIN" = "1" ]  && { cores_pin;  say "background: cpu1-3 pinned online in BOTH arms"; }

# The rig's --seconds budget has to cover everything this loop spends, not just
# the blocks: the 20 s warm-up before the first one, and per block a guard, the
# block itself and all the shell work around it — a cores_now subshell every
# 10 s of the block, three date/awk pairs, the gap rotate. On a TV that runs on
# two cores those spawns are not free, and the old +30 s left about ten seconds
# of margin for the lot, so a perfectly healthy run could reach its last block
# with the rig already finished. That is the expensive direction to be wrong in:
# the block is lost and, through ds5_campaign.sh's gate on the positive control,
# so is the rest of the night. A rig that outlives the loop costs nothing — it
# is TERMed after the last block and still prints its DONE accounting.
TOTAL=$(( 20 + BLOCKS * (BLOCK + GUARD + 10) + 60 ))

# What this run can and cannot see, stated BEFORE it produces a number. Expected
# events per arm k = rate * exposure; the 95 % CI of a Poisson rate ratio excludes
# 1 once |ln r| > 1.96*sqrt(2/k). Rates are the measured unlevered ones under
# parity (>=60 ~80/min, >=70 ~2.5/min, >=80 ~0.6/min) — indicative, not a promise:
# the >=60 level itself drifts ~1.5x across hours on this link.
awk -v n="$BLOCKS" -v b="$BLOCK" -v t="$TOTAL" 'BEGIN{
    split("60 70 80",E," "); split("80 2.5 0.6",R," ");
    printf "power: %d x %ds = %.0f min exposure per arm;", n, b, (n/2*b)/60;
    for(i=1;i<=3;i++){ k=R[i]/60*(n/2*b);
        if(k<1){ printf "  >=%sms: too few events", E[i]; continue }
        d=1.96*sqrt(2/k); printf "  >=%sms: resolves x<%.2f or x>%.2f", E[i], exp(-d), exp(d) }
    printf "\n" }'
say "starting rig for ${TOTAL}s (B=$B_MS), $BLOCKS x ${BLOCK}s blocks, lever=$LEVER"
"$RIG" --b "$B_MS" --seconds "$TOTAL" --stats 10 >"$OUT/synth.log" 2>&1 &
RIG_PID=$!

# Let delivery settle before the first block: the first seconds include the
# bootstrap, the SetState prime and any sniff exit the daemon still has to do.
sleep 20
if ! kill -0 "$RIG_PID" 2>/dev/null; then
    say "FATAL: the rig died during warm-up — see $OUT/synth.log"
    cleanup; exit 1          # else the load keeps running and the cores stay pinned
fi

# Warm-up acceptance: if the transport is not carrying the offered load there is
# no point running blocks. This is the guard that would have caught the sniffed
# link that produced 15 of 46 reports per second.
DELIVER=$(awk '/^\[synth\] t=/{r=$4} END{print r}' "$OUT/synth.log")
say "warm-up delivery: ${DELIVER:-unknown} (want ~rate=46.9/s)"

# tail runs DIRECTLY, never wrapped in a restart loop. Killing the wrapper only
# kills the wrapper: the inner tail is reparented to init with its fd on THIS
# run's ledger.log still open, and it then appends every future daemon line to a
# finished run — a later re-analysis of that directory counts foreign rebinds and
# reads foreign flush states, and the leaked processes stack up run after run on
# the TV. -F already retries by itself if the daemon's log goes away, which is
# all the loop ever bought.
nohup tail -n 0 -F /tmp/ds5_txd.log >"$OUT/ledger.log" 2>&1 </dev/null &
LEDGER_PID=$!

i=0
while [ "$i" -lt "$BLOCKS" ]; do
    i=$((i+1))
    if [ $((i % 2)) -eq 0 ] && [ "$LEVER" != "none" ]; then
        ARM=on
        [ -n "$TOG" ] && { echo "$ON_VALUE" > "$TOG"; chown 0:0 "$TOG" 2>/dev/null; chmod 644 "$TOG"; }
        [ "$LEVER" = "burst" ] && echo 19200 > /tmp/ds5_period_us
        load_on
        cores_on
        cpu_on
    else
        ARM=off
        [ -n "$TOG" ] && rm -f "$TOG"
        [ "$LEVER" = "burst" ] && rm -f /tmp/ds5_period_us
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
    # Liveness BEFORE the row: the daemon stops binning gaps within 150 ms of the
    # audio stopping, so a block the rig did not outlive pools as full exposure
    # with almost no events — and lands in whichever arm was running.
    #
    # kill -0 alone cannot answer WHY it is gone. A background child that exited
    # normally fails the signal exactly like one that crashed, because the shell
    # has reaped both — so a rig that merely ran off the end of its budget looks
    # identical to a dead instrument, and treating that as a dead instrument
    # throws away every completed block plus, via the campaign gate, the night.
    # The rig's own log is the discriminator (the same one ds5_dscp_ab.sh greps):
    # it announces STOPPING — or a lowercase "stopping" from its Aurora check —
    # before it gives up for cause, and it prints DONE on the way out of its send
    # loop. DONE is printed on BOTH paths, so it means "the loop ended", not "the
    # loop ended well": STOPPING has to be tested first and wins where both are
    # present. Only that one says the run is broken; a DONE with no STOPPING is a
    # rig that simply reached its --seconds, which costs the block that was under
    # way and leaves every earlier block exactly as valid as it was.
    #
    # That reading only holds because every abnormal exit of the rig announces
    # itself: the guards, the readiness refusal and the encode failure all print
    # STOPPING before they leave the loop. An exit that leaves DONE alone behind
    # would be filed here as a healthy short run, which is why a silent break in
    # ds5_synth_audio.c is a bug in THIS discriminator too.
    if ! kill -0 "$RIG_PID" 2>/dev/null; then
        gap_rotate
        # -iF, not a BRE alternation: this also has to run under busybox grep.
        if grep -qiF stopping "$OUT/synth.log" 2>/dev/null; then
            say "rig STOPPED FOR CAUSE during block $i — block NOT recorded"
            tail -3 "$OUT/synth.log"
            RIG_DIED=1
        elif grep -qF '[synth] DONE' "$OUT/synth.log" 2>/dev/null; then
            say "rig reached the end of its ${TOTAL}s budget during block $i — block NOT recorded"
            BUDGET_OUT=1
        else
            # No DONE, no STOPPING: it did not finish its loop and it did not say
            # why — killed from outside, or gone with the process. Broken run.
            say "rig vanished during block $i with no DONE line — block NOT recorded"
            tail -3 "$OUT/synth.log"
            RIG_DIED=1
        fi
        break
    fi
    FG=$(sed -n 's/.*"appId":"\([^"]*\)".*/\1/p' /var/luna/preferences/last_foreground_app_id.json 2>/dev/null)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$ARM" "$S" "$E" \
        "$(( (RX1 - RX0) * 8 / BLOCK / 1000 ))" "${FG:-none}" \
        "$(( CSUM * 10 / CN ))" "$(cpu_busy_pct "$CPU0" "$CPU1")" >> "$OUT/windows.tsv"
    # The daemon's per-gap log is a CAPPED RING: at 256 KiB it drops the OLDEST
    # entries and leaves one TRUNCATED marker behind. Copying it once at the end
    # therefore loses whole early blocks while their exposure still counts — which
    # silently halves the measured rate. Rotate it after every block instead and
    # let the report dedupe; overlap is cheap, a lost block is not.
    gap_rotate
done

rm -f $LEVER_FILES   # ALL lever files: an even block count ends ON, knobs armed
load_stop            # the background conditions end with the run, BG flags or not
cores_unpin
cpu_stop
kill "$LEDGER_PID" 2>/dev/null
# The rig is now budgeted deliberately PAST the last block, so waiting for it to
# end by itself would idle here for the whole headroom. TERM makes it fall out of
# its send loop and print the DONE line the operator is told to read first, so
# the delivery accounting survives the shortcut; the wait only reaps it.
kill "$RIG_PID" 2>/dev/null
wait "$RIG_PID" 2>/dev/null
gap_rotate
# Dedupe on the WHOLE line, never on the stamp alone: with a key, `sort -u`
# compares only that key, and the daemon stamps EVERY link of one NOCP event with
# the same wall-clock ms — a radio-global blackout that ends both pads of a
# two-pad session writes two records with an identical stamp and a different h=,
# of which the keyed dedupe silently kept one. That halved exactly the event
# class this programme is here to count. Making the stamp artificially unique
# would falsify it instead; whole-line dedupe cannot drop a real record, because
# two gaps of the same length on the same handle in the same millisecond cannot
# both exist (a record only enters at >=20 ms, so consecutive gaps are that far
# apart), while the overlap the rotate deliberately produces is byte-identical
# and collapses as intended.
sort -u "$OUT/gaps.raw" > "$OUT/gaps.log" 2>/dev/null
say "done -> $OUT"
tail -1 "$OUT/synth.log"
# A run that lost blocks is not a run, and the exit status has to say so:
# ds5_campaign.sh gates the whole campaign on the positive control, and a
# control that died half way through must not read as a control that passed.
if [ "$RIG_DIED" = "1" ]; then
    say "INCOMPLETE: the rig died mid-run — only $((i - 1)) of $BLOCKS blocks are recorded"
    exit 1
fi
# Running out of budget inside the last block is NOT that failure: the instrument
# worked for every block it reported, and those blocks are a valid interleaved
# run. Exiting 1 here would abort the campaign over a stopwatch, so say what is
# missing — nobody may quote $BLOCKS when fewer were measured — and pass.
if [ "$BUDGET_OUT" = "1" ]; then
    say "SHORT: the rig's budget ended inside block $i — $((i - 1)) of $BLOCKS blocks recorded, all of them good"
fi
