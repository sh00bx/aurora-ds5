#!/bin/sh
# Presentation-side sampler for the LG G4 video output (root on the TV, NOT from the
# Aurora jail). Reconstruction of the 2026-09-22 /tmp/vosample.sh, whose source was
# lost with /tmp: every 250 ms one CSV row with the counters of /proc/lxvo/vdisp/*.
#
#   usage: lxvo_sample.sh <seconds> <out.csv>
#
# Counters are per decoder instance and reset on stream start; diff consecutive rows.
#   repeat_cnt          deif: a frame shown again on the next vsync (a hitch)
#   frame_count         vpts: frames presented
#   smooth_enable       vpts: the VO's own output smoothing is active
#   smooth_wait_cnt     vpts: frames the smoother held back
#   smooth_discard_cnt  vpts: frames the smoother dropped
#   match_mode          vpts: how PTS are matched to vsync
#   adp_max_qlen        vpts: adaptive queue limit
#   qlen                vdisp: frames queued before display
#   phase_shift         vdisp/vsync: vsync phase-locked to content
#   intv                vsync: vsync interval in 27 MHz ticks (187500 = 144 Hz)
# Field names are matched loosely ("name: 12", "name=12", "name 12"); a field the
# driver does not print stays empty rather than breaking the row.
set -u
DUR=${1:-30}
OUT=${2:-/tmp/lxvo.csv}
D=/proc/lxvo/vdisp
FIELDS="repeat_cnt field_cnt frame_count smooth_enable smooth_wait_cnt smooth_discard_cnt match_mode adp_max_qlen disp_offset framerate qlen phase_shift intv"

field() { # file name
    tr '\n,;' '   ' < "$1" 2>/dev/null | grep -oE "(^| )$2[ :=]+-?[0-9]+" | head -n1 | grep -oE -- '-?[0-9]+$'
}

echo "t_s,$(echo $FIELDS | tr ' ' ',')" > "$OUT"
END=$(( $(cut -d. -f1 /proc/uptime) + DUR ))
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$END" ]; do
    row="$(cut -d' ' -f1 /proc/uptime)"
    for f in $FIELDS; do
        v=""
        for src in deif vpts vdisp vsync; do
            [ -r "$D/$src" ] || continue
            v=$(field "$D/$src" "$f")
            [ -n "$v" ] && break
        done
        row="$row,$v"
    done
    echo "$row" >> "$OUT"
    usleep 250000 2>/dev/null || sleep 1
done
echo "wrote $OUT"
