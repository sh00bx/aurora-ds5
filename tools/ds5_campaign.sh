#!/bin/sh
# ds5_campaign.sh — run several ds5_autorun.sh campaigns back to back, unattended.
#
#   ds5_campaign.sh [block_seconds] [B_ms]
#
# Order is deliberate and is the order the preregistration requires:
#
#   1. wifi   — the POSITIVE CONTROL. Until the rig demonstrably responds to a
#               perturbation known to move the air, every other verdict it
#               produces is uninterpretable: a null instrument reports "no
#               effect" for everything, with confidence intervals.
#   2. none   — the equivalence baseline for this time of day.
#   3. ptype  — the L18 replication, which is the point of the exercise.
#
# Between campaigns everything is disarmed and the pad is given ~60 s of quiet,
# so a campaign cannot inherit the previous one's state.
#
#   nohup /tmp/ds5_campaign.sh 300 60 >/tmp/campaign.log 2>&1 &
set -u

BLOCK="${1:-120}"
B_MS="${2:-60}"
RUN=/tmp/ds5_autorun.sh
[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }
[ -x "$RUN" ] || { echo "REFUSING: $RUN missing"; exit 1; }

say(){ echo "$(date '+%F %T') [campaign] $*"; }

cleanup(){
    rm -f /tmp/ds5_ptype
    [ -f /tmp/ds5_autorun_load.pid ] && kill "$(cat /tmp/ds5_autorun_load.pid)" 2>/dev/null
    rm -f /tmp/ds5_autorun_load.pid
    pkill -x ds5_synth_audio 2>/dev/null
    say "cleanup done"
}
trap 'cleanup; exit 130' INT TERM

for phase in wifi none ptype; do
    case "$phase" in
        wifi)  N=6 ;;   # the control only needs to show a signed response
        none)  N=4 ;;   # a baseline has no arms to interleave
        ptype) N=4 ;;   # judged on >=60, where 4 x 120s already resolves x0.86
    esac
    say "=== $phase: $N x ${BLOCK}s (B=$B_MS) ==="
    "$RUN" "$phase" "$N" "$BLOCK" "$B_MS" 2>&1 | sed "s/^/[$phase] /"
    say "=== $phase done, 30 s quiet before the next ==="
    sleep 30
done

cleanup
say "campaign complete — collect /tmp/autorun-*"
