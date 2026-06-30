#!/bin/sh
# aurora gamemode toggle for rooted webOS (LG G4 / webOS 25)
# usage: gamemode.sh on | off | enforce | status
#
# on : evict discovery/cast + non-essential broadcast services (WiFi<->BT coex
#      relief), kill preloaded streaming apps (RAM relief -> stops swap churn),
#      boost the aurora stream threads, tame the QuickSet busy-loop.
#      off restores services + scheduling (apps are not relaunched).
#
# Rationale (2026-06-29 diagnosis, aurora 1.0.10 / Vibepollo 1.18.0-beta.2):
#   * miracast/airplay/appcasting/chromecast/ssdp do P2P remain-on-channel +
#     SSDP chatter on the MT7921 combo chip -> WiFi<->BT coex stalls = DS5 input
#     micro-hitches (ctm qpeak spiked 21-24/32). Stopping them -> qpeak ~2/32.
#   * netflix/youtube/browser sit PRELOADED (~175MB RSS) -> ~33MB free + active
#     swap-out -> video micro-stalls. closeByAppId does NOT evict preloads on
#     this webOS, so we kill the resident procs directly -> swap-out stops.
#   * tuner demod reacq-storm is kernel-level (tvrm/tvpowerd/DEMODTASK), 0% CPU,
#     NOT safely stoppable mid-game -> left alone (log noise only, harmless).
#
# History: started 2026-05-24 for the moonlight client (uid 6756, in-app
# SCHED_RR patch failed EPERM). Rewritten 2026-06-29 for aurora: pidof trigger
# (getForegroundAppInfo returns nothing on webOS 25), direct-kill app eviction
# (closeByAppId no-op for preloads), service eviction for coex + RAM.

APPMGR="luna://com.webos.applicationManager"

# services to stop during a session (coex + background TV); restarted on "off".
EVICT_SERVICES="miracast.service airplay-adaptor.service appcasting.service \
chromecast-provisioning.service ssdp-discovery-lgtv.service \
broadcast-conf-downloader.service broadcast-channel-mixer.service \
broadcast-systemui-manager.service tv-reservation-agent.service \
tvlinkcmdprocessor.service"

# preloaded/background apps to evict (RAM). matched against proc cmdline since
# closeByAppId is a no-op for preloads here. over-listing is harmless.
BLOCKLIST="netflix youtube.leanback.v4 com.webos.app.browser amazon \
com.webos.app.lgchannels com.webos.app.livetv com.disney.disneyplus-prod \
com.disney.disneyplus.firsttime com.apple.appletv com.spotify.spotify.tvv2 \
tv.twitch.tv.webos com.wuaki.tv com.3827136.103251"

NCPU=$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 4)
ALLMASK=$(printf '%x' $(( (1 << NCPU) - 1 )))   # 4 cores -> f
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
# Grace period: never evict a blocklisted app launched within the last N seconds.
# SAM only writes last_foreground_app_id.json once an app reaches foreground
# STABLY -- it never records an app we kill mid-launch -- so the fg-trigger alone
# can't protect an app the user is opening (chicken-and-egg: killed before it can
# become foreground). This grace lets a just-opened app survive its launch until
# it becomes foreground, after which the guard's fg-trigger stops enforce entirely.
APP_GRACE_SEC="${APP_GRACE_SEC:-20}"

log() { echo "[gamemode] $*"; }

# process age in seconds (uptime - starttime). starttime = stat field 22; comm
# (field 2, may contain spaces/parens) is stripped via the last ')' so field
# offsets are stable. Returns a large number on any parse failure (=> not young).
proc_age_sec() {
	st=$(cat "/proc/$1/stat" 2>/dev/null) || { echo 999999; return; }
	rest=${st##*) }
	start=$(echo "$rest" | awk '{print $20}')   # field 22 overall = 20th after comm
	case "$start" in ''|*[!0-9]*) echo 999999; return ;; esac
	up=$(awk '{print int($1)}' /proc/uptime 2>/dev/null)
	echo $(( up - start / HZ ))
}

# stream client process; aurora is current, fall back to moonlight for old builds
game_pid() {
	for n in aurora moonlight; do
		p=$(pidof "$n" 2>/dev/null | tr ' ' '\n' | sort -n | head -1)
		[ -n "$p" ] && { echo "$p"; return; }
	done
}

stop_services() {
	for u in $EVICT_SERVICES; do
		systemctl is-active "$u" >/dev/null 2>&1 || continue
		systemctl stop "$u" >/dev/null 2>&1 && log "stopped $u"
	done
}

start_services() {
	for u in $EVICT_SERVICES; do
		systemctl start "$u" >/dev/null 2>&1 && log "started $u"
	done
}

# wpa_supplicant keeps a WiFi-Direct P2P listen cycle on p2p0 (driven by connman)
# that fires a remain_on_channel on the combo chip every ~10s = periodic radio
# contention with the stream + DS5 BT. p2p_stop_find halts it (proven to hold);
# harmless during gaming (only disables WiFi-Direct discoverability), reversible.
quiet_p2p() {
	for i in p2p0 wlan0; do
		wpa_cli -i "$i" p2p_stop_find >/dev/null 2>&1
	done
	wpa_cli -i p2p0 p2p_flush >/dev/null 2>&1
}

# kill any process whose cmdline carries a blocklisted app id. precise matches
# (--app-id=, "appId":"id", install path) so shared WebAppMgr zygote/network
# procs - which carry no specific id - are never hit. single /proc pass.
# NEVER evict the app the user currently has in the foreground (defense in depth:
# the guard already only enforces while aurora is foreground, but this guarantees
# a blocklisted app the user just switched to - e.g. youtube.leanback.v4 - is
# never killed out from under them).
FG_FILE="${FG_FILE:-/var/luna/preferences/last_foreground_app_id.json}"
close_apps() {
	fg=$(cat "$FG_FILE" 2>/dev/null)
	for id in $BLOCKLIST; do
		case "$fg" in *"\"$id\""*) continue ;; esac
		luna-send -n 1 "$APPMGR/closeByAppId" "{\"id\":\"$id\"}" >/dev/null 2>&1
	done
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		cl=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
		for id in $BLOCKLIST; do
			case "$fg" in *"\"$id\""*) continue ;; esac   # skip foreground app
			case "$cl" in
			*"app-id=$id"*|*"\"$id\""*|*"/applications/$id/"*|*"/$id/bin/"*)
				pid=${p##*/}
				age=$(proc_age_sec "$pid")
				if [ "$age" -lt "$APP_GRACE_SEC" ]; then
					echo "$(cut -d. -f1 /proc/uptime) SPARED $id pid=$pid age=${age}s" >> /tmp/gamemode-evict.log
					log "spared $id (pid $pid, just launched) - user is opening it"
					break
				fi
				echo "$(cut -d. -f1 /proc/uptime) EVICT $id pid=$pid age=${age}s fg=$fg" >> /tmp/gamemode-evict.log
				kill "$pid" 2>/dev/null && log "evicted $id (pid $pid)"
				break ;;
			esac
		done
	done
}

boost_game() {
	mp=$(game_pid)
	[ -z "$mp" ] && { log "stream client not running - skip boost"; return; }
	nok=0; rrok=0; n=0
	for t in /proc/"$mp"/task/*; do
		tid=${t##*/}; n=$((n + 1))
		renice -n -10 -p "$tid" >/dev/null 2>&1 && nok=$((nok + 1))
		chrt -r -p 20 "$tid" >/dev/null 2>&1 && rrok=$((rrok + 1))
	done
	log "stream pid=$mp: $n threads, renice -10 ok=$nok, SCHED_RR ok=$rrok"
	[ "$rrok" -eq 0 ] && log "  (kernel refused SCHED_RR - relying on renice -10)"
}

restore_game() {
	mp=$(game_pid)
	[ -z "$mp" ] && return
	for t in /proc/"$mp"/task/*; do
		tid=${t##*/}
		chrt -o -p 0 "$tid" >/dev/null 2>&1
		renice -n 0 -p "$tid" >/dev/null 2>&1
	done
	log "stream pid=$mp: restored SCHED_OTHER nice 0"
}

tame_quickset() {
	for ip in $(pidof iconnectivity 2>/dev/null); do
		for t in /proc/"$ip"/task/*; do
			renice -n 19 -p "${t##*/}" >/dev/null 2>&1
		done
		taskset -a -p 1 "$ip" >/dev/null 2>&1
		log "iconnectivity pid=$ip: renice +19 + pinned to CPU0"
	done
}

restore_quickset() {
	for ip in $(pidof iconnectivity 2>/dev/null); do
		for t in /proc/"$ip"/task/*; do
			renice -n 0 -p "${t##*/}" >/dev/null 2>&1
		done
		taskset -a -p "$ALLMASK" "$ip" >/dev/null 2>&1
		log "iconnectivity pid=$ip: restored nice 0 + all cores"
	done
}

show_status() {
	mp=$(game_pid)
	echo "--- stream client (pid=${mp:-none}) thread scheduling ---"
	if [ -n "$mp" ]; then
		for t in /proc/"$mp"/task/*; do
			chrt -p "${t##*/}" 2>/dev/null | grep -oE "SCHED_[A-Z]+"
		done | sort | uniq -c
	fi
	echo "--- evicted services still active ---"
	for u in $EVICT_SERVICES; do
		systemctl is-active "$u" >/dev/null 2>&1 && echo "ACTIVE: $u"
	done
	echo "--- blocklist apps still resident ---"
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		cl=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
		for id in $BLOCKLIST; do
			case "$cl" in
			*"app-id=$id"*|*"\"$id\""*|*"/applications/$id/"*|*"/$id/bin/"*)
				echo "RESIDENT: $id (pid ${p##*/})"; break ;;
			esac
		done
	done
	echo "--- memory ---"
	free -m | awk '/Mem:/{print "mem: "$4"MB free, "$7"MB avail"} /Swap:/{print "swap: "$3"MB used"}'
	echo "--- load ---"; uptime
}

case "$1" in
on)
	log "=== GAME MODE ON ==="
	stop_services
	quiet_p2p
	# close_apps is DISABLED. It killed shared infrastructure: blocklisted
	# WebApp ids (e.g. com.webos.app.browser) are hosted IN the main WebAppMgr
	# process = the ExecStart of webapp-mgr.service. Killing it (SIGTERM) took
	# webapp-mgr.service dead with no auto-restart -> ALL WebView apps (ARD,
	# homebrew store, YouTube, ...) broke until the service was manually
	# restarted. The RAM it reclaimed is cold/parked swap that wasn't hurting
	# latency anyway. Not worth the risk. (Function kept below, unused.)
	boost_game
	tame_quickset
	free -m | awk '/Mem:/{print "[gamemode] mem: "$4"MB free, "$7"MB avail"} /Swap:/{print "[gamemode] swap: "$3"MB used"}'
	log "on: done"
	;;
enforce)
	# idempotent quiet variant for the guard loop (close_apps is disabled; only
	# safe re-assertions here -- they don't touch other apps).
	quiet_p2p >/dev/null 2>&1
	boost_game >/dev/null 2>&1
	tame_quickset >/dev/null 2>&1
	;;
off)
	log "=== GAME MODE OFF ==="
	restore_game
	restore_quickset
	start_services
	log "off: done (background apps not relaunched - open them yourself)"
	;;
status)
	show_status
	;;
*)
	echo "usage: $0 on|off|enforce|status"
	exit 1
	;;
esac
