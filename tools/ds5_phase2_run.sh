#!/bin/sh
# ds5_phase2_run.sh — one Phase-2 measurement slice on the TV.
#
# Captures a labelled, self-describing run: the passive HCI sniff (ds5_sniff),
# the daemon's own ledger delta over the SAME window, and a manifest of the
# machine state that would otherwise be reconstructed from memory afterwards.
# Everything Phase 2 needs comes out of one directory per run.
#
# It NEVER touches ds5_txd: no restart, no signal, no binary swap. The only
# thing it may write is /tmp/ds5_inject_maxq, which the daemon re-reads ~1/s by
# design (2.6's ladder is zero-code precisely because of that file).
#
#   ds5_phase2_run.sh <label> [seconds] [maxq]
#
#   ds5_phase2_run.sh base 1800          # 30-min baseline slice
#   ds5_phase2_run.sh maxq8 1800 8       # 2.6 ladder rung, restored on exit
#
# Runs unattended in the background is the intended mode:
#   nohup /tmp/ds5_phase2_run.sh base 1800 >/dev/null 2>&1 &
set -u

LABEL="${1:-run}"
DUR="${2:-1800}"
MAXQ="${3:-}"
OUT="/tmp/phase2/${LABEL}"
TXDLOG=/tmp/ds5_txd.log
SNIFF=/tmp/ds5_sniff
MAXQF=/tmp/ds5_inject_maxq

[ "$(id -u)" = "0" ] || { echo "must run as root (monitor socket + root-owned maxq file)"; exit 1; }
[ -x "$SNIFF" ] || { echo "missing $SNIFF — scp tools/ds5_sniff there first"; exit 1; }

# One run per label, and never silently merge two captures into one directory:
# a half-overwritten slice is worse than no slice.
if [ -d "$OUT" ]; then echo "$OUT exists — pick another label or rm it"; exit 1; fi
mkdir -p "$OUT" || exit 1

# The daemon log is append-only for the life of the daemon, so the delta over
# the window is exactly the tail past this mark. Line count, not byte offset:
# the supervisor may also write to it.
# (Shell redirection failure is not wc's stderr, so this needs the -f test: the
# daemon log does not exist until the daemon has started at least once.)
if [ -f "$TXDLOG" ]; then MARK=$(wc -l < "$TXDLOG"); else MARK=0; fi

restore() {
    if [ -n "$MAXQ" ]; then
        rm -f "$MAXQF"                       # absence = compiled-in default 12
        echo "$(date +%H:%M:%S) maxq restored to default" >> "$OUT/manifest.txt"
    fi
    kill "$SNIFF_PID" 2>/dev/null
    wait "$SNIFF_PID" 2>/dev/null
    tail -n "+$((MARK+1))" "$TXDLOG" > "$OUT/txd.log" 2>/dev/null
    {
        echo "--- state at end $(date '+%F %T')"
        echo "eth0_carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null)"
        echo "eth0_speed=$(cat /sys/class/net/eth0/speed 2>/dev/null)"
        echo "wlan0=$(iw dev wlan0 link 2>/dev/null | tr '\n' ' ')"
        echo "txd_pids=$(pidof ds5_txd 2>/dev/null)"
    } >> "$OUT/manifest.txt"
    echo "run '$LABEL' done -> $OUT"
    exit 0
}
trap restore INT TERM

{
    echo "label=$LABEL dur=$DUR maxq=${MAXQ:-default}"
    echo "start=$(date '+%F %T')  uptime=$(uptime)"
    echo "--- state at start"
    echo "daemon_md5=$(md5sum /media/developer/apps/usr/palm/services/com.aurora.ds5.txd/ds5_txd 2>/dev/null)"
    echo "app_version=$(grep -o '"version": "[^"]*"' /media/developer/apps/usr/palm/applications/com.aurora.ds5/appinfo.json 2>/dev/null)"
    echo "txd_pids=$(pidof ds5_txd 2>/dev/null)"
    echo "eth0_carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null)"
    echo "eth0_speed=$(cat /sys/class/net/eth0/speed 2>/dev/null)"
    echo "wlan0=$(iw dev wlan0 link 2>/dev/null | tr '\n' ' ')"
    echo "txd_log_mark_lines=$MARK"
} > "$OUT/manifest.txt"

# 2.6's actuator. Written BEFORE the sniffer starts and removed on the way out,
# so the capture window and the setting window are the same window.
if [ -n "$MAXQ" ]; then
    echo "$MAXQ" > "$MAXQF"
    chown 0:0 "$MAXQF" 2>/dev/null      # the daemon ignores a non-root-owned file
    chmod 644 "$MAXQF"
    echo "maxq set to $MAXQ at $(date +%H:%M:%S)" >> "$OUT/manifest.txt"
    sleep 2                              # the daemon caches it ~1/s; let it land
fi

"$SNIFF" -d "$DUR" -o "$OUT/sniff.log" &
SNIFF_PID=$!
echo "capturing '$LABEL' for ${DUR}s (sniff pid $SNIFF_PID) -> $OUT"
wait "$SNIFF_PID"
restore
