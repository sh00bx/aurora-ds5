#!/bin/sh
# ds5_phase2_eth.sh — the 2.1 eth0 ladder, safely.
#
# Rungs (the whole point is that they separate three different mechanisms):
#   a  wlan0 carries the stream, eth0 unused          — today's baseline
#   b  eth0 carries the stream, wlan0 associated-idle — isolates WiFi TRAFFIC
#   c  eth0 carries the stream, wlan0 down            — isolates the WiFi RADIO
#
# b minus a = the coex cost of WiFi *traffic*. c minus b = the cost of the radio
# merely being enabled and scanning. If c does not move the tail either, WiFi
# coex is exonerated and half the HCI lever list drops in priority.
#
#   ds5_phase2_eth.sh precheck        # headroom + carrier, changes nothing
#   ds5_phase2_eth.sh a|b|c [minutes] # set the rung (auto-restores)
#   ds5_phase2_eth.sh restore         # undo now
#
# SAFETY: rung c takes wlan0 down. If your shell is on wlan0 you lose it — and
# so does any way to bring it back. Every state change therefore arms an
# unconditional background restore; even a dropped SSH session or a killed
# script leaves the TV reachable again within the timeout.
set -u

WATCHDOG=/tmp/ds5_phase2_eth_watchdog.pid
STATE=/tmp/ds5_phase2_eth.state

[ "$(id -u)" = "0" ] || { echo "must run as root"; exit 1; }

rate_of() {   # $1=iface -> measured kbit/s over 3s, both directions
    rx1=$(cat "/sys/class/net/$1/statistics/rx_bytes" 2>/dev/null || echo 0)
    tx1=$(cat "/sys/class/net/$1/statistics/tx_bytes" 2>/dev/null || echo 0)
    sleep 3
    rx2=$(cat "/sys/class/net/$1/statistics/rx_bytes" 2>/dev/null || echo 0)
    tx2=$(cat "/sys/class/net/$1/statistics/tx_bytes" 2>/dev/null || echo 0)
    echo $(( ((rx2-rx1) + (tx2-tx1)) * 8 / 3000 ))
}

precheck() {
    echo "── 2.1 pre-check ──"
    for i in eth0 wlan0; do
        [ -d "/sys/class/net/$i" ] || { echo "  $i: absent"; continue; }
        echo "  $i carrier=$(cat /sys/class/net/$i/carrier 2>/dev/null) \
speed=$(cat /sys/class/net/$i/speed 2>/dev/null)Mb \
operstate=$(cat /sys/class/net/$i/operstate 2>/dev/null)"
    done
    # Measured, not configured: the real question is whether the stream FITS,
    # and only the wire knows what the stream is actually doing right now.
    echo "  measuring live throughput (3s)…"
    W=$(rate_of wlan0); E=$(rate_of eth0)
    echo "  wlan0 ${W} kbit/s   eth0 ${E} kbit/s"
    SPD=$(cat /sys/class/net/eth0/speed 2>/dev/null || echo -1)
    if [ "$SPD" = "-1" ] || [ "$SPD" = "" ]; then
        echo "  ⚠ eth0 has no link — plug the cable, then re-run precheck."
        echo "    Without the negotiated speed the headroom question is unanswerable."
    elif [ "$SPD" -le 100 ] 2>/dev/null; then
        echo "  ⚠ eth0 negotiated ${SPD}Mb. Stream is ${W} kbit/s = $((W * 100 / (SPD * 1000)))% of the link."
        echo "    Above ~60% a 100BASE-T link adds queueing delay of its own and rung b"
        echo "    would measure the CABLE, not the coex. Cap the bitrate or accept the confound."
    else
        echo "  eth0 ${SPD}Mb — ample headroom for a ${W} kbit/s stream."
    fi
    echo "  ⚠ Rung b also needs wlan to stay genuinely idle: a background scan is"
    echo "    radio activity, and it will show up as coex you attributed to nothing."
    echo "    Check afterwards:  iw dev wlan0 scan dump | grep -c BSS"
}

arm_watchdog() {   # $1 = minutes until unconditional restore
    mins="$1"
    [ -f "$WATCHDOG" ] && kill "$(cat $WATCHDOG)" 2>/dev/null
    ( sleep $((mins * 60 + 60))
      ip link set wlan0 up 2>/dev/null
      echo "$(date +%H:%M:%S) watchdog restored wlan0" >> "$STATE"
      rm -f "$WATCHDOG" ) &
    echo $! > "$WATCHDOG"
    echo "  watchdog armed: wlan0 comes back in $((mins + 1)) min no matter what"
}

case "${1:-precheck}" in
  precheck) precheck ;;
  a)
    ip link set wlan0 up 2>/dev/null
    echo "rung a: wlan0 up (baseline). Make sure the TV is actually streaming over wlan0."
    echo "a $(date '+%F %T')" >> "$STATE"
    ;;
  b)
    [ "$(cat /sys/class/net/eth0/carrier 2>/dev/null)" = "1" ] || {
        echo "eth0 has no carrier — plug the cable first. Refusing."; exit 1; }
    ip link set wlan0 up 2>/dev/null
    echo "rung b: eth0 carries the stream, wlan0 associated-idle."
    echo "  webOS prefers the wired route on its own once eth0 has a lease;"
    echo "  VERIFY before trusting the slice:  ds5_phase2_eth.sh precheck"
    echo "  (eth0 must carry the traffic, wlan0 must be near zero)"
    echo "b $(date '+%F %T')" >> "$STATE"
    ;;
  c)
    [ "$(cat /sys/class/net/eth0/carrier 2>/dev/null)" = "1" ] || {
        echo "eth0 has no carrier — taking wlan0 down would strand the TV. Refusing."; exit 1; }
    arm_watchdog "${2:-30}"
    ip link set wlan0 down
    echo "rung c: wlan0 DOWN, eth0 only. The radio is off, not just quiet."
    echo "c $(date '+%F %T')" >> "$STATE"
    ;;
  restore)
    [ -f "$WATCHDOG" ] && kill "$(cat $WATCHDOG)" 2>/dev/null
    rm -f "$WATCHDOG"
    ip link set wlan0 up 2>/dev/null
    echo "wlan0 up, watchdog cleared."
    echo "restore $(date '+%F %T')" >> "$STATE"
    ;;
  *) echo "usage: $0 precheck|a|b|c [minutes]|restore"; exit 2 ;;
esac
