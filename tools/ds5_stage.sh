#!/bin/sh
# ds5_stage.sh — rebuild the measurement kit and put it on the TV. Runs on the
# ANALYSIS HOST (not on the TV), talks to it over the `LG` ssh alias.
#
#   tools/ds5_stage.sh [--check-only]
#
# Why this exists: /tmp on webOS does not survive a power cycle, and the TV power
# cycles whenever someone turns it off at the wall or it drops out of standby the
# hard way. Every time that happens the rig, the harness, the probe and the
# daemon's gap-log arming are gone, and the first run afterwards silently measures
# a daemon with no per-gap log at all.
#
# Everything the TV needs to run an unattended A/B:
#   /tmp/ds5_synth_audio   the workload            (built here from tools/)
#   /tmp/ds5_clock_probe   the two-clock check     (built here from tools/)
#   /tmp/ds5_autorun.sh    the harness
#   /tmp/ds5_gaplog = 1    per-gap wall-clock log armed in the daemon
#   /tmp/ds5_inject_fifo = 10   production FIFO depth (the file beats the datagram)
#
# NB: remote commands that mention process names are uploaded as FILES and run as
# `sh /tmp/x.sh`. An ssh one-liner carries its own text in argv, so any ps/pkill
# pattern inside it matches the shell executing it — that self-match has killed
# two cleanup sessions here already.
set -u
cd "$(dirname "$0")/.." || exit 1

SDK=/opt/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc
TV=LG
CHECK_ONLY=0
[ "${1:-}" = "--check-only" ] && CHECK_ONLY=1

say(){ printf '%s\n' "$*"; }
[ -x "$SDK" ] || { say "REFUSING: no SDK compiler at $SDK"; exit 1; }

ssh -o BatchMode=yes -o ConnectTimeout=8 "$TV" true 2>/dev/null || {
    say "TV not reachable — it is off or still coming up. Nothing staged."; exit 2; }

if [ "$CHECK_ONLY" = "0" ]; then
    say "building rig + probe..."
    "$SDK" -O2 -Wall -Wextra tools/ds5_synth_audio.c -o /tmp/ds5_synth_audio.build \
        -ldl -lm -lpthread || exit 1
    "$SDK" -O2 -Wall -Wextra tools/ds5_clock_probe.c -o /tmp/ds5_clock_probe.build || exit 1

    # Upload under a scratch name and rename on the device: busybox sh reads a
    # script lazily BY OFFSET, so overwriting one that is still running makes the
    # running instance execute the new file's tail. That happened here once and
    # left three zombie shells behind.
    scp -q -o BatchMode=yes /tmp/ds5_synth_audio.build "$TV":/tmp/ds5_synth_audio.new || exit 1
    scp -q -o BatchMode=yes /tmp/ds5_clock_probe.build "$TV":/tmp/ds5_clock_probe.new || exit 1
    scp -q -o BatchMode=yes tools/ds5_autorun.sh       "$TV":/tmp/ds5_autorun.sh.new  || exit 1
fi

cat > /tmp/ds5_stage_remote.sh <<'REMOTE'
#!/bin/sh
# runs ON the TV
SELF=$$
busy=$(ps ax | awk -v me="$SELF" '$1!=me && ($0 ~ /synth_audio/ || $0 ~ /autorun\.sh/) {print $1}')
if [ -n "$busy" ]; then echo "BUSY: a measurement is still running (pids: $busy)"; exit 3; fi
for f in ds5_synth_audio ds5_clock_probe ds5_autorun.sh; do
    [ -f "/tmp/$f.new" ] && { mv "/tmp/$f.new" "/tmp/$f"; chmod +x "/tmp/$f"; }
done
echo 1  > /tmp/ds5_gaplog
echo 10 > /tmp/ds5_inject_fifo
rm -f /tmp/ds5_ptype /tmp/ds5_linkq_ms
echo "daemon:    $(pidof ds5_txd >/dev/null 2>&1 && echo "running pid $(pidof ds5_txd)" || echo "NOT RUNNING")"
echo "kit:       $(ls -la /tmp/ds5_synth_audio /tmp/ds5_clock_probe /tmp/ds5_autorun.sh 2>&1 | awk '{print $NF}' | tr '\n' ' ')"
echo "cores:     $(cat /sys/devices/system/cpu/online) (mp_enable=$(cat /proc/lg/pm/mp_enable 2>/dev/null))"
echo "guard:     $(ps ax | awk -v me="$SELF" '$1!=me && $0 ~ /moonlight-guard/ {print "running"; exit}')"
# The link is the precondition nothing else can substitute for: a bound template
# means the daemon has seen an on-air report from the pad and can inject.
echo "link:      $(tail -40 /tmp/ds5_txd.log 2>/dev/null | grep -o 'have=[01]' | tail -1) $(tail -40 /tmp/ds5_txd.log 2>/dev/null | grep -o 'L0 [0-9a-f:]*' | tail -1)"
REMOTE
scp -q -o BatchMode=yes /tmp/ds5_stage_remote.sh "$TV":/tmp/ds5_stage_remote.sh || exit 1
ssh -o BatchMode=yes "$TV" 'sh /tmp/ds5_stage_remote.sh'
