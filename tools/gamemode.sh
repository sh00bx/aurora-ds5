#!/bin/sh
# aurora gamemode toggle for rooted webOS (LG G4 / webOS 25)
# usage: gamemode.sh on | off | enforce | recover | status
#        gamemode.sh picture-on | picture-off | picture-status   (picture half only)
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
	echo "--- cpu hotplug ---"
	echo "mp_enable: $(cat /proc/lg/pm/mp_enable 2>/dev/null || echo n/a)"
	for c in 1 2 3; do printf 'cpu%s=%s ' "$c" "$(cat /sys/devices/system/cpu/cpu$c/online 2>/dev/null)"; done; echo
	echo "--- memory ---"
	free -m | awk '/Mem:/{print "mem: "$4"MB free, "$7"MB avail"} /Swap:/{print "swap: "$3"MB used"}'
	echo "--- load ---"; uptime
}

# LG's MP governor (LGDTV-PMDRV-TAS kernel thread, knobs in /proc/lg/pm)
# offlines CPU2/3 on low load and back every 2-4s; each offline transition is a
# stop_machine() pause across ALL cores (measured 2026-07-04: online-mask
# flapped every ~2s idle AND in-stream, zero flaps for >3min with mp_enable=0).
# freq control (freq_enable) and thermal scaling (ts_enable) are separate knobs
# and stay untouched. Idempotent: safe from the enforce tick; onlining a core
# does not stop_machine (only offlining does).
pin_cpus() {
	[ -e /proc/lg/pm/mp_enable ] || { log "pin_cpus: no /proc/lg/pm/mp_enable on this fw"; return; }
	changed=""
	[ "$(cat /proc/lg/pm/mp_enable 2>/dev/null)" = "0" ] || { echo 0 > /proc/lg/pm/mp_enable 2>/dev/null && changed=1; }
	for c in 1 2 3; do
		f=/sys/devices/system/cpu/cpu$c/online
		[ "$(cat "$f" 2>/dev/null)" = "1" ] || { echo 1 > "$f" 2>/dev/null && changed=1; }
	done
	[ -n "$changed" ] && log "cpus pinned online (LG MP governor off)"
}

unpin_cpus() {
	[ -e /proc/lg/pm/mp_enable ] || return 0
	echo 1 > /proc/lg/pm/mp_enable 2>/dev/null
	log "LG MP governor restored (cores scale down on their own)"
}

# ------------------------------------------------------------- picture / sound
#
# The TV's own picture pipeline is the one latency source this script could not
# reach before: noise reduction, the enhancers and the 24p cadence logic all sit
# between the decoded frame and the panel. LG exposes them through
# com.webos.settingsservice, which only answers to root -- so it belongs here,
# next to the other root-only levers, rather than in the app.
#
# WHAT THIS DOES AND WHY IT IS ONLY TWO KEYS. Measured on this TV (LG G4,
# webOS 25), picture settings are stored per DIMENSION, and the dimension for a
# picture key includes the picture mode itself:
#
#   "dimension":{"dynamicRange":"sdr","pictureMode":"expert2","input":"default"}
#
# So every key like noiseReduction or superResolution exists once per picture
# mode, and switching the mode brings that mode's whole set with it. The TV's
# own "game" preset already IS the low-processing configuration -- that is what
# it is for -- so asking for the mode does the entire job in one write.
#
# Writing those keys individually, the way the obvious implementation (and
# upstream's) does, is actively harmful: the write lands in whatever bucket is
# live at that moment, which during a mode switch is not the one you think. In
# testing that clobbered the user's calibrated expert2 values with game-mode
# ones, and "restoring" them afterwards wrote them into the game preset instead.
# Two keys, each in its own bucket, is both the smaller and the safer change.
#
# Two more measured facts shape the rest:
#   * A write needs the dimension COMPLETE or absent. {"dynamicRange":"sdr"} on
#     its own is refused with "ERROR!! sending a request to DB"; dynamicRange +
#     input is accepted, and so is omitting it. Applying omits it (the live
#     dimension is the one we mean); the restore names it in full, because by
#     then the panel has usually dropped back out of HDR and the HDR bucket
#     would otherwise never be put back.
#   * The panel switches to the HDR dimension a second or two AFTER the stream
#     starts, and that dimension has its own pictureMode. picture_enforce
#     therefore re-checks instead of trusting the value written at stream start,
#     and records the HDR bucket as a second entry to restore.
#
# Everything written is recorded WITH its dimension in PIC_STATE. The state file
# deliberately does not live in /tmp: if the app dies mid-stream, the next app
# start still has to be able to hand the user their picture back.
if [ -z "$PIC_STATE" ]; then
	if [ -d /var/lib/webosbrew ]; then
		PIC_STATE=/var/lib/webosbrew/aurora-gamemode.state
	else
		PIC_STATE=/tmp/aurora-gamemode.state
	fi
fi
SS="luna://com.webos.settingsservice"

# luna-send RETURNS BEFORE ITS REPLY IS READABLE, and back-to-back calls are
# what breaks this: measured here, the same request answers 5/5 with a second of
# air around it and 0/13 in a tight loop, with rc=0 and an empty file every
# time. So each call gets a short settle first, writes to a FILE (a pipe loses
# the reply far more often), and then WAITS for that file instead of trusting
# rc. Both delays are bounded -- this runs off the UI thread, but it must never
# hang a stream on a sulking bus.
SS_TMP="${SS_TMP:-/tmp/.aurora-ss.$$}"

ss_nap() { usleep "${1:-20000}" 2>/dev/null || sleep 1; }

ss_call() {   # method payload -> reply json on stdout
	_i=0
	while [ $_i -lt 2 ]; do
		ss_nap 400000
		rm -f "$SS_TMP"
		luna-send -n 1 -w 4000 "$SS/$1" "$2" >"$SS_TMP" 2>&1
		_j=0
		while [ ! -s "$SS_TMP" ] && [ $_j -lt 60 ]; do
			ss_nap 50000
			_j=$((_j + 1))
		done
		if [ -s "$SS_TMP" ]; then
			cat "$SS_TMP"
			rm -f "$SS_TMP"
			return 0
		fi
		log "picture: no reply to $1 (attempt $((_i + 1)))"
		_i=$((_i + 1))
	done
	rm -f "$SS_TMP"
	return 1
}

# First "key":"value" hit. grep -o before sed on purpose: a greedy sed would
# return the LAST match, which for pictureMode is the copy inside the dimension
# object rather than the setting itself.
json_field() {   # json key -> value
	printf '%s' "$1" | grep -o "\"$2\":\"[^\"]*\"" | head -1 | sed 's/.*:"//; s/"$//'
}

# "dynamicRange:input", or "-" when the reply does not name a dynamicRange.
# Naming half a dimension on the way back is worse than naming none.
pic_dim() {   # reply -> dim
	_dr=$(json_field "$1" dynamicRange)
	[ -z "$_dr" ] && { echo "-"; return; }
	echo "$_dr:$(json_field "$1" input)"
}

pic_dim_json() {   # dim -> ,"dimension":{...} or nothing
	[ "$1" = "-" ] && return
	_in=${1##*:}
	[ -z "$_in" ] && return
	echo ",\"dimension\":{\"dynamicRange\":\"${1%%:*}\",\"input\":\"$_in\"}"
}

pic_record() { printf '%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" >>"$PIC_STATE"; }

# game / hdrGame / dolbyHdrGame all exist on this panel; the live dynamic range
# decides which one to ask for. A model without them refuses the write, which we
# log and let go -- an unknown mode name is not worth guessing around.
pic_mode_target() {
	case "$1" in
	*dolby*|*Dolby*) echo dolbyHdrGame ;;
	hdr*|HDR*) echo hdrGame ;;
	*) echo game ;;
	esac
}

# Switch one mode-style key to its game value and remember what it was. Used for
# picture.pictureMode (re-checked on every enforce tick, because the HDR
# dimension arrives late) and once for sound.soundMode.
pic_switch() {   # category key want-value-or-empty-for-auto
	_reply=$(ss_call getSystemSettings "{\"category\":\"$1\",\"keys\":[\"$2\"]}") || {
		log "picture: $1.$2 not readable, left alone"
		return
	}
	_cur=$(json_field "$_reply" "$2")
	[ -z "$_cur" ] && return
	_dim=$(pic_dim "$_reply")
	_want=$3
	[ -z "$_want" ] && _want=$(pic_mode_target "${_dim%%:*}")
	# Already there -- ours from an earlier tick, or the user's own choice.
	# Either way: nothing to change and nothing to remember.
	[ "$_cur" = "$_want" ] && return
	case "$_cur" in game|hdrGame|dolbyHdrGame) return ;; esac
	# This bucket may already be recorded from an earlier tick that got
	# reverted by the user; do not stack a second entry on top of it.
	grep -q "^$1|$2|$_dim|" "$PIC_STATE" 2>/dev/null && return
	_res=$(ss_call setSystemSettings "{\"category\":\"$1\",\"settings\":{\"$2\":\"$_want\"}}")
	case "$_res" in
	*'"returnValue":true'*)
		pic_record "$1" "$2" "$_dim" "$_cur"
		log "picture: $1.$2 $_cur -> $_want (dim=$_dim)"
		;;
	*) log "picture: $1.$2=$_want rejected, left at $_cur" ;;
	esac
}

picture_on() {
	# A leftover state file means a previous session never restored. Put that
	# back first, or its values would be overwritten with game-mode values and
	# lost for good.
	[ -f "$PIC_STATE" ] && { log "picture: stale state from an earlier session, restoring it first"; picture_off; }
	: >"$PIC_STATE" 2>/dev/null || { log "picture: cannot write $PIC_STATE - picture left alone"; return; }
	pic_switch picture pictureMode
	pic_switch sound soundMode game
	[ -s "$PIC_STATE" ] || log "picture: already in game mode, nothing to change"
}

# Cheap re-assert: one read, and a write only on drift. This is what catches the
# HDR dimension, which only appears once the panel has actually switched.
picture_enforce() {
	[ -f "$PIC_STATE" ] || return
	pic_switch picture pictureMode
}

picture_off() {
	[ -f "$PIC_STATE" ] || return
	# Restore each recorded bucket by name. If the TV refuses the named
	# dimension, fall back to the live one -- putting the value back in the
	# wrong bucket is still better than leaving the user in game mode.
	while IFS='|' read -r c k d o; do
		[ -n "$k" ] || continue
		_res=$(ss_call setSystemSettings "{\"category\":\"$c\",\"settings\":{\"$k\":\"$o\"}$(pic_dim_json "$d")}")
		case "$_res" in
		*'"returnValue":true'*) log "picture: restored $c.$k=$o (dim=$d)" ;;
		*)
			_res2=$(ss_call setSystemSettings "{\"category\":\"$c\",\"settings\":{\"$k\":\"$o\"}}")
			case "$_res2" in
			*'"returnValue":true'*) log "picture: restored $c.$k=$o (live dimension; $d was refused)" ;;
			*) log "picture: RESTORE FAILED $c.$k=$o (dim=$d): $_res / $_res2" ;;
			esac
			;;
		esac
	done <"$PIC_STATE"
	rm -f "$PIC_STATE"
}

picture_status() {
	echo "--- picture/sound game mode ---"
	if [ -f "$PIC_STATE" ]; then
		echo "state: ENGAGED ($PIC_STATE)"
		sed 's/^/  restores /' "$PIC_STATE"
	else
		echo "state: not engaged"
	fi
	echo "  now: $(json_field "$(ss_call getSystemSettings '{"category":"picture","keys":["pictureMode"]}')" pictureMode) / $(json_field "$(ss_call getSystemSettings '{"category":"sound","keys":["soundMode"]}')" soundMode)"
}


case "$1" in
on)
	log "=== GAME MODE ON ==="
	stop_services
	quiet_p2p
	pin_cpus
	# close_apps is DISABLED. It killed shared infrastructure: blocklisted
	# WebApp ids (e.g. com.webos.app.browser) are hosted IN the main WebAppMgr
	# process = the ExecStart of webapp-mgr.service. Killing it (SIGTERM) took
	# webapp-mgr.service dead with no auto-restart -> ALL WebView apps (ARD,
	# homebrew store, YouTube, ...) broke until the service was manually
	# restarted. The RAM it reclaimed is cold/parked swap that wasn't hurting
	# latency anyway. Not worth the risk. (Function kept below, unused.)
	boost_game
	tame_quickset
	picture_on
	free -m | awk '/Mem:/{print "[gamemode] mem: "$4"MB free, "$7"MB avail"} /Swap:/{print "[gamemode] swap: "$3"MB used"}'
	log "on: done"
	;;
enforce)
	# idempotent quiet variant for the guard loop (close_apps is disabled; only
	# safe re-assertions here -- they don't touch other apps).
	quiet_p2p >/dev/null 2>&1
	pin_cpus >/dev/null 2>&1
	boost_game >/dev/null 2>&1
	tame_quickset >/dev/null 2>&1
	picture_enforce
	;;
off)
	log "=== GAME MODE OFF ==="
	picture_off
	restore_game
	restore_quickset
	start_services
	unpin_cpus
	log "off: done (background apps not relaunched - open them yourself)"
	;;
recover)
	# Conditional "off", for app start-up: put things back only if a previous
	# session died without doing it. Cheap to call when nothing is engaged,
	# which is the normal case.
	engaged=""
	[ -f "$PIC_STATE" ] && engaged="picture"
	[ "$(cat /proc/lg/pm/mp_enable 2>/dev/null)" = "0" ] && engaged="$engaged cpus"
	for u in $EVICT_SERVICES; do
		systemctl is-enabled "$u" >/dev/null 2>&1 || continue
		systemctl is-active "$u" >/dev/null 2>&1 || { engaged="$engaged services"; break; }
	done
	if [ -z "$engaged" ]; then
		log "recover: nothing left engaged"
		exit 0
	fi
	log "=== RECOVER ($engaged) ==="
	picture_off
	restore_game
	restore_quickset
	start_services
	unpin_cpus
	log "recover: done"
	;;
status)
	show_status
	picture_status
	;;
# The picture half on its own -- for trying a key list out on a new panel
# without stopping services or touching scheduling.
picture-on)
	picture_on
	;;
picture-off)
	picture_off
	;;
picture-status)
	picture_status
	;;
*)
	echo "usage: $0 on|off|enforce|recover|status|picture-on|picture-off|picture-status"
	exit 1
	;;
esac
