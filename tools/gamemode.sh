#!/bin/sh
# aurora gamemode toggle for rooted webOS (LG G4 / webOS 25)
# usage: gamemode.sh on | off | enforce | recover | status
#        gamemode.sh netprio | netprio-off              (receive buffers only)
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

# The prefix is part of the app's contract with this script, not decoration:
# Homebrew Channel hands our stdout back in its reply, and the app looks for a
# line carrying it to tell "the script ran and said no" (a lost lock race, which
# is normal) from "there is no root here" (which is permanent).
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

# --------------------------------------------------------------------- picture
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
# Everything written is recorded WITH its dimension in PIC_STATE, and with the
# request shape the TV accepted it in -- one line per bucket,
# category|key|dimension|old-value|shape. The state file deliberately does not
# live in /tmp: if the app dies mid-stream, the next app start still has to be
# able to hand the user their picture back.
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

# Which shape the last ss_try was accepted in ("global" or "app", see below).
# It cannot be a shell variable: every ss_try runs inside a command
# substitution, i.e. a subshell, and what a subshell assigns dies with it. So it
# travels through a file, and both files go when the script exits.
SS_SHAPE_FILE="${SS_SHAPE_FILE:-$SS_TMP.shape}"
trap 'rm -f "$SS_TMP" "$SS_SHAPE_FILE"' EXIT
ss_shape() { cat "$SS_SHAPE_FILE" 2>/dev/null; }

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

# Two firmware generations, two accepted shapes. webOS 10.3 (the C5) refuses a
# picture read or write that does not carry "current_app":true, while every
# firmware before it refuses exactly that form ("???" / DBTYPE errors) and wants
# the global one. Neither shape works everywhere, so a call goes out global
# first and is repeated with current_app only when the TV actually ANSWERED and
# said no: silence is not a shape problem, and retrying it would double the
# worst case of a verb the app is waiting on.
#
# Which shape won is recorded, because the two are not interchangeable: an
# app-scoped write leaves an override that a global write does not, and a global
# restore of it can be ACCEPTED and still leave that override in place -- the
# user would keep game mode for good. So a restore repeats the shape its write
# was accepted in (ss_restore below), instead of probing again.
ss_try() {   # method inner-json (no braces) -> reply on stdout, rc 0 = accepted
	rm -f "$SS_SHAPE_FILE"
	_t1=$(ss_call "$1" "{$2}") || return 1
	case "$_t1" in *'"returnValue":true'*)
		echo global 2>/dev/null >"$SS_SHAPE_FILE"; printf '%s' "$_t1"; return 0 ;;
	esac
	_t2=$(ss_call "$1" "{$2,\"current_app\":true}") || { printf '%s' "$_t1"; return 1; }
	case "$_t2" in *'"returnValue":true'*)
		echo app 2>/dev/null >"$SS_SHAPE_FILE"; printf '%s' "$_t2"; return 0 ;;
	esac
	printf '%s / %s' "$_t1" "$_t2"
	return 1
}

# One shape, no probing: for a restore that has to land where its write did.
ss_one() {   # method inner-json shape -> reply on stdout, rc 0 = accepted
	case "$3" in
	app) _s1=$(ss_call "$1" "{$2,\"current_app\":true}") || return 1 ;;
	*)   _s1=$(ss_call "$1" "{$2}") || return 1 ;;
	esac
	printf '%s' "$_s1"
	case "$_s1" in *'"returnValue":true'*) return 0 ;; esac
	return 1
}

# Put one recorded value back. A line written by this build names the shape it
# was accepted in and is repeated in exactly that one; a line from a state file
# that predates the shape column names none, and keeps the global-then-app probe
# that wrote it -- an upgrade mid-session must still be able to restore.
ss_restore() {   # inner-json shape -> reply on stdout, rc 0 = accepted
	case "$2" in
	global|app) ss_one setSystemSettings "$1" "$2" ;;
	*) ss_try setSystemSettings "$1" ;;
	esac
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

# category|key|dimension|old-value|shape
pic_record() { printf '%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" >>"$PIC_STATE"; }

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
# picture.pictureMode, re-checked on every enforce tick because the HDR
# dimension arrives late.
pic_switch() {   # category key want-value-or-empty-for-auto
	_reply=$(ss_try getSystemSettings "\"category\":\"$1\",\"keys\":[\"$2\"]") || {
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
	# return 0, not a bare return: the last command run is the failed test
	# above, so a bare one hands back ITS status. This function is the last
	# command of the enforce verb, and a non-zero exit there is read by the app
	# as "root is gone" -- it would stop enforcing for the rest of the session.
	case "$_cur" in game|hdrGame|dolbyHdrGame) return 0 ;; esac
	# This bucket may already be recorded from an earlier tick that got
	# reverted by the user; do not stack a second entry on top of it.
	grep -q "^$1|$2|$_dim|" "$PIC_STATE" 2>/dev/null && return
	if ss_try setSystemSettings "\"category\":\"$1\",\"settings\":{\"$2\":\"$_want\"}" >/dev/null; then
		pic_record "$1" "$2" "$_dim" "$_cur" "$(ss_shape)"
		log "picture: $1.$2 $_cur -> $_want (dim=$_dim)"
	else
		# A panel without the game presets refuses the name in both shapes,
		# which is that panel's normal answer, not a fault. Nothing was
		# written, so there is nothing recorded to put back either.
		log "picture: $1.$2=$_want not accepted, left at $_cur"
	fi
}

picture_on() {
	# A leftover state file means a previous session never restored. Put that
	# back first, or its values would be overwritten with game-mode values and
	# lost for good.
	[ -f "$PIC_STATE" ] && { log "picture: stale state from an earlier session, restoring it first"; picture_off; }
	if [ -f "$PIC_STATE" ]; then
		# picture_off kept what it could not restore -- still the only record
		# of the user's preset, so it must not be truncated away. pic_switch
		# skips buckets that are already recorded, so appending on top is safe.
		log "picture: keeping the unrestored state for the next off"
	else
		# "true", not ":", and stderr redirected BEFORE the target. ":" is a
		# SPECIAL built-in: a redirection that fails on one takes the whole
		# non-interactive shell down with it (verified in dash and busybox
		# ash), so the handler on the right would never run and the verb would
		# die on a read-only state directory instead of leaving the picture
		# alone. The stderr order is the same trap as in net_raise: the message
		# about the refused open comes from the shell, before the 2>/dev/null
		# on the right of it is in force.
		true 2>/dev/null >"$PIC_STATE" || { log "picture: cannot write $PIC_STATE - picture left alone"; return 0; }
	fi
	# SOUND IS DELIBERATELY LEFT ALONE. The TV's "game" sound preset sounds bad
	# on this panel, and the audio path has no latency problem that the preset
	# would fix -- the picture pipeline was the one this script had to reach.
	# (picture_off still restores a sound.soundMode line from a state file
	# written by an older build, so an interrupted upgrade puts it back.)
	pic_switch picture pictureMode
	[ -s "$PIC_STATE" ] || log "picture: already in game mode, nothing to change"
}

# Cheap re-assert: one read, and a write only on drift. This is what catches the
# HDR dimension, which only appears once the panel has actually switched.
picture_enforce() {
	# return 0, not bare return: this is the enforce verb's last command, and a
	# missing state file would otherwise exit the whole script with status 1 --
	# which the app reads as "root is gone" and stops the cycle over.
	[ -f "$PIC_STATE" ] || return 0
	pic_switch picture pictureMode
}

picture_off() {
	[ -f "$PIC_STATE" ] || return 0
	# Restore each recorded bucket by name. If the TV refuses the named
	# dimension, fall back to the live one -- putting the value back in the
	# wrong bucket is still better than leaving the user in game mode.
	# A line that fails BOTH ways is carried into a fresh state file instead of
	# being dropped with the old one: the state file is the only record of the
	# user's preset, so it may only die with a successful restore. The next
	# off/recover retries whatever is left.
	_keep="$PIC_STATE.retry"
	rm -f "$_keep"
	while IFS='|' read -r c k d o shape; do
		[ -n "$k" ] || continue
		_set="\"category\":\"$c\",\"settings\":{\"$k\":\"$o\"}"
		if _res=$(ss_restore "$_set$(pic_dim_json "$d")" "$shape"); then
			log "picture: restored $c.$k=$o (dim=$d, ${shape:-probed})"
		elif _res2=$(ss_restore "$_set" "$shape"); then
			log "picture: restored $c.$k=$o (live dimension; $d was refused)"
		else
			log "picture: RESTORE FAILED $c.$k=$o (dim=$d): $_res / $_res2"
			printf '%s|%s|%s|%s|%s\n' "$c" "$k" "$d" "$o" "$shape" >>"$_keep"
		fi
	done <"$PIC_STATE"
	if [ -f "$_keep" ]; then
		mv -f "$_keep" "$PIC_STATE"
		log "picture: kept the failed buckets for the next off/recover"
	else
		rm -f "$PIC_STATE"
	fi
}

picture_status() {
	echo "--- picture game mode ---"
	if [ -f "$PIC_STATE" ]; then
		echo "state: ENGAGED ($PIC_STATE)"
		sed 's/^/  restores /' "$PIC_STATE"
	else
		echo "state: not engaged"
	fi
	echo "  now: $(json_field "$(ss_try getSystemSettings '"category":"picture","keys":["pictureMode"]')" pictureMode)"
}


# --------------------------------------------------------------------- netprio
#
# Receive-buffer headroom, put in place BEFORE the session opens its UDP
# sockets. webOS clamps SO_RCVBUF to net.core.rmem_max, and the stock 512 KiB is
# roughly ten milliseconds of buffer at 400 Mbps -- one scheduling hiccup on the
# receive path, and this TV has plenty of those because it swaps under its own
# weight, and the kernel has already dropped the video packets before moonlight
# asks for them. That clamp is the other half of our own long-standing finding
# that webOS caps SO_RCVBUF at 512 KiB: the cap IS a sysctl, and a sysctl is
# liftable once you have root -- which this script has and the app does not.
#
# The raise only ever reaches sockets created AFTER it. That is why the app runs
# this verb just before LiStartConnection rather than from the game-mode thread
# (which starts once the stream is already up), and why it is kept down to the
# six /proc writes and their read-backs: the app waits for this one, so
# everything that does NOT have to happen before the sockets exist was moved to
# "on", which runs on the app's own thread. What is left on this path besides
# the writes is close_other_apps, and that one is off unless its marker file
# says otherwise, for exactly this reason.
#
# NO USB-NIC TUNING HERE. Upstream's version also matches the r8152 / ax88179 /
# cdc_* drivers and tunes tx_queue_len, rps_cpus and USB autosuspend. This TV
# streams over wlan0 by standing decision and has no USB Ethernet adapter, so
# that whole block would be dead code on the only device this script runs on.
#
# The old values go into their own state file, and for the same reason PIC_STATE
# is not in /tmp: if the app dies mid-stream, the next app start still has to be
# able to hand the kernel back. Both "off" and "recover" restore it, and
# recover's engaged-detection counts this file, so a raise is put back even when
# the game-mode cycle behind it never engaged -- or never ran, which is the
# normal case for a connection that fails before it reaches STREAMING.
if [ -z "$NET_STATE" ]; then
	if [ -d /var/lib/webosbrew ]; then
		NET_STATE=/var/lib/webosbrew/aurora-netprio.state
	else
		NET_STATE=/tmp/aurora-netprio.state
	fi
fi

# rmem_max/wmem_max 8 MiB and rmem_default 2 MiB are upstream's v1.2.1 values
# (doubled from v1.2.0 after their own measurements). udp_rmem_min is the floor
# a UDP socket keeps no matter what the app asked for; netdev_max_backlog and
# netdev_budget decide how much the receive softirq may drain per poll before it
# yields -- the burst half of the same problem.
NET_KNOBS="net.core.rmem_max=8388608 net.core.rmem_default=2097152 \
net.core.wmem_max=8388608 net.core.netdev_max_backlog=10000 \
net.core.netdev_budget=600 net.ipv4.udp_rmem_min=16384"

# Overridable like every other absolute path in here, so the verb can be
# exercised against a fake tree on a workstation -- there is no dry run for a
# sysctl, and the TV is not always reachable.
NET_SYSCTL_DIR="${NET_SYSCTL_DIR:-/proc/sys}"

net_knob_file() { echo "$NET_SYSCTL_DIR/$(echo "$1" | tr . /)"; }

net_raise() {
	# A leftover state file means a previous session never restored. Put that
	# back first: otherwise the values recorded below would be OUR raised ones,
	# and the user's kernel would keep them until the next reboot.
	[ -f "$NET_STATE" ] && { log "netprio: stale state from an earlier session, restoring it first"; net_restore; }
	if [ -f "$NET_STATE" ]; then
		# net_restore kept what it could not put back. That is still the only
		# record of the original values, so it must not be truncated away; the
		# loop below skips knobs it already names.
		log "netprio: keeping the unrestored state for the next off"
	else
		# "true" and not ":" -- see picture_on: a failed redirection on a
		# special built-in ends the whole script, handler and all.
		true 2>/dev/null >"$NET_STATE" || {
			log "netprio: cannot write $NET_STATE - buffers left alone (a raise nothing records is a raise nothing can undo)"
			return 1
		}
	fi
	for _kv in $NET_KNOBS; do
		_k=${_kv%%=*}
		_v=${_kv#*=}
		_f=$(net_knob_file "$_k")
		[ -f "$_f" ] || { log "netprio: $_k does not exist on this kernel"; continue; }
		# Already recorded by an earlier raise whose restore failed: the
		# recorded value is the user's, the live one is ours, leave both.
		grep -q "^$_k|" "$NET_STATE" 2>/dev/null && continue
		_old=$(cat "$_f" 2>/dev/null)
		case "$_old" in ''|*[!0-9]*) log "netprio: $_k reads '$_old' - left alone"; continue ;; esac
		[ "$_old" = "$_v" ] && continue
		# Record BEFORE writing. A record without a write restores a value that
		# is already there, which is a no-op; a write without a record is a
		# change to the user's kernel that nothing can undo.
		printf '%s|%s\n' "$_k" "$_old" >>"$NET_STATE"
		# stderr is redirected BEFORE the target: a refused open is reported by
		# the shell itself, not by echo, so the usual trailing 2>/dev/null comes
		# too late to keep it out of the app log.
		echo "$_v" 2>/dev/null >"$_f"
		# Read back rather than trust the write: the kernel clamps some of
		# these to a ceiling of its own, and a refused write is silent here.
		_now=$(cat "$_f" 2>/dev/null)
		if [ "$_now" = "$_v" ]; then
			log "netprio: $_k $_old -> $_now"
		else
			log "netprio: $_k $_old -> $_now (asked for $_v)"
		fi
	done
	return 0
}

# Same shape as picture_off, and for the same reason: a line that could not be
# put back is carried into a fresh state file instead of dying with the old one,
# because the state file is the only record of what the kernel looked like
# before us. The next off/recover retries whatever is left.
net_restore() {
	[ -f "$NET_STATE" ] || return 0
	_keep="$NET_STATE.retry"
	rm -f "$_keep"
	while IFS='|' read -r _k _old; do
		[ -n "$_k" ] || continue
		_f=$(net_knob_file "$_k")
		echo "$_old" 2>/dev/null >"$_f"
		_now=$(cat "$_f" 2>/dev/null)
		if [ "$_now" = "$_old" ]; then
			log "netprio: restored $_k=$_old"
		else
			log "netprio: RESTORE FAILED $_k=$_old (still ${_now:-unreadable})"
			printf '%s|%s\n' "$_k" "$_old" >>"$_keep"
		fi
	done <"$NET_STATE"
	if [ -f "$_keep" ]; then
		mv -f "$_keep" "$NET_STATE"
		log "netprio: kept the failed knobs for the next off/recover"
	else
		rm -f "$NET_STATE"
	fi
}

# ------------------------------------------------------------ memory reclaim
#
# Part of "on", not of netprio, and kept here only because it shares the
# NET_SYSCTL_DIR override with the knobs above.
#
# Both numbers on one line, before and after: the point of the reclaim below is
# to produce numbers about this TV's chronic memory pressure (measured
# 2026-08-18: 400 of 599 MB swap used after 1:19 h), not opinions about it. The
# uptime goes on the line too, because the app receives the whole run as one
# blob of stdout and the two timestamps are then the only way to tell how long
# the sync took.
mem_log() {   # label
	awk -v tag="$1" -v up="$(cut -d' ' -f1 /proc/uptime 2>/dev/null)" '
		/^MemAvailable:/ {a=$2}
		/^SwapFree:/     {s=$2}
		/^SwapTotal:/    {t=$2}
		END {printf "[gamemode] reclaim: %s t=%s MemAvailable=%s kB SwapFree=%s kB of %s kB\n", tag, up, a, s, t}
	' /proc/meminfo 2>/dev/null
}

# sync first, or drop_caches walks straight past every dirty page it cannot
# free. Neither is free, and how long a sync takes on a TV that is 400 MB into
# its swap is the one number here nobody has measured yet -- the two log lines
# bracket exactly that, so the first stream after this lands answers it.
#
# THAT is why this belongs to "on" and not to netprio: netprio is the verb the
# session thread waits for, right before it opens its sockets, and an unmeasured
# sync has no business on that path. Nothing here has to happen before the
# sockets exist. "on" runs once the stream is already up, on the app's own
# detached worker, where a slow sync costs nobody the connection. It stays out
# of the enforce tick either way.
reclaim_caches() {
	_dc="$NET_SYSCTL_DIR/vm/drop_caches"
	mem_log "before"
	sync
	if [ -w "$_dc" ]; then
		# stderr before the target, as in net_raise: a refused open is
		# reported by the shell itself, too early for a trailing 2>/dev/null.
		echo 3 2>/dev/null >"$_dc" || log "reclaim: drop_caches write refused"
	else
		log "reclaim: no $_dc - only sync ran"
	fi
	mem_log "after"
}

# Upstream closes every other LS2 app before the stream so Flutter Home and the
# preloaded media apps hand their RAM back. OFF BY DEFAULT here, and it stays
# off until somebody has numbers for it. close_apps below is the cautionary
# tale: it killed processes, took webapp-mgr.service down with them and broke
# every WebView app on the TV until a manual restart. This variant cannot do
# that -- it only ASKS the application manager to close, by id, and never
# touches a pid. What it still costs is one luna round trip per running app on
# a path the stream is waiting on, which is the second reason it is not on by
# default. The allowlist is upstream's, unverified on this panel -- note that it
# does NOT spare the launcher (com.webos.app.home): the Flutter home is the
# biggest resident on this TV, so it is exactly what upstream is after and
# exactly what to watch when the marker goes on.
#
# Turn it on for a measurement run by creating the marker file. A file and not
# an env var on purpose: the app invokes this script through Homebrew Channel's
# exec with a fixed command line, so there is no environment to set.
NETPRIO_CLOSE_MARK="${NETPRIO_CLOSE_MARK:-/var/lib/webosbrew/aurora-close-apps.enable}"
close_other_apps() {
	[ -f "$NETPRIO_CLOSE_MARK" ] || return 0
	_run=$(luna-send -n 1 -w 4000 "$APPMGR/running" '{}' 2>/dev/null)
	case "$_run" in
	*'"returnValue":true'*) ;;
	*) log "netprio: running-app list unavailable, closed nothing"; return 0 ;;
	esac
	for _id in $(printf '%s' "$_run" | grep -o '"id":"[^"]*"' | sed 's/.*:"//; s/"$//'); do
		case "$_id" in
		''|com.aurora.*|org.webosbrew.*) continue ;;
		com.webos.app.volume|com.webos.app.notification) continue ;;
		com.webos.app.voice*|com.webos.app.input*|com.webos.app.lsa*) continue ;;
		esac
		luna-send -n 1 -w 4000 "$APPMGR/close" "{\"id\":\"$_id\"}" >/dev/null 2>&1
		log "netprio: asked $_id to close"
	done
	# Explicit: this is the last command of the netprio verb, and whatever the
	# loop above happens to leave behind is not an answer to "did netprio run".
	return 0
}

netprio_status() {
	echo "--- net buffers (netprio) ---"
	if [ -f "$NET_STATE" ]; then
		echo "state: RAISED ($NET_STATE)"
		sed 's/^/  restores /' "$NET_STATE"
	else
		echo "state: not raised"
	fi
	for _kv in $NET_KNOBS; do
		_k=${_kv%%=*}
		printf '  %s=%s\n' "$_k" "$(cat "$(net_knob_file "$_k")" 2>/dev/null || echo n/a)"
	done
}

# --------------------------------------------------------------- legacy guard
#
# Before 1.5.3 game mode was driven from OUTSIDE the app: a hand-installed boot
# hook (/var/lib/webosbrew/init.d/40-moonlight-guard) started moonlight-guard.sh,
# which polled `pidof aurora` and drove its own copy of this script,
# /var/lib/webosbrew/gamemode.sh. Upgrading the app does not remove any of that,
# so on a TV that once had it BOTH controllers run: the old guard turns game
# mode on the whole time the app is merely open (menus included), re-asserts
# every 3s -- so seconds after the in-app "off" has restored, the cores are
# pinned and P2P discovery is stopped again -- and eventually runs its own "off"
# against a TV that was already put back. Which controller wrote last is a
# coin toss, which is exactly how it looks to the user.
#
# Recover is the only place that can end this without a second hand-install,
# and it runs at app start (plus at the end of a session whose "on" never
# engaged -- the buffers-only teardown has its own verb and does not come
# through here). Deliberately narrow: the guard's own pidfile
# (and only if that pid really is the guard -- pidfiles outlive their process
# and pids get reused), and the hook file only if it is still the one that
# starts the guard. The guard script itself is left on disk: it is not ours to
# delete, and the ds5 measurement rig still starts it deliberately.
#
# A child the guard had already spawned can still finish behind the restore
# below -- that leftover is undone by the next stream's "off", instead of being
# re-applied every three seconds for the rest of the app's life.
LEGACY_GUARD_PID="${LEGACY_GUARD_PID:-/tmp/moonlight-guard.pid}"
LEGACY_GUARD_HOOK="${LEGACY_GUARD_HOOK:-/var/lib/webosbrew/init.d/40-moonlight-guard}"

stop_legacy_guard() {
	_gp=$(cat "$LEGACY_GUARD_PID" 2>/dev/null)
	case "$_gp" in ''|*[!0-9]*) _gp="" ;; esac
	if [ -n "$_gp" ] && [ -r "/proc/$_gp/cmdline" ]; then
		case "$(tr '\0' ' ' < "/proc/$_gp/cmdline" 2>/dev/null)" in
		*moonlight-guard*)
			kill "$_gp" 2>/dev/null
			rm -f "$LEGACY_GUARD_PID"
			log "LEGACY GUARD: stopped the pre-1.5.3 boot-hook guard (pid $_gp) - it was driving a second, older copy of this script against the app's own"
			;;
		esac
	fi
	# Renaming is the whole disable: the hook's filename has no dot on purpose
	# because busybox run-parts skips names that have one. It also leaves the
	# file readable for anyone who wants to see what was there.
	if [ -f "$LEGACY_GUARD_HOOK" ] && grep -q moonlight-guard "$LEGACY_GUARD_HOOK" 2>/dev/null; then
		mv -f "$LEGACY_GUARD_HOOK" "$LEGACY_GUARD_HOOK.disabled" 2>/dev/null &&
			log "LEGACY GUARD: disabled the boot hook ($LEGACY_GUARD_HOOK -> .disabled) - it would start that second controller again on the next boot"
	fi
}

# ----------------------------------------------------------------------- lock
#
# State-changing verbs must not interleave: two shells racing can strand the
# eviction ("on" stops a service, a parallel "recover" restarts it and it then
# stays up for the whole session -- enforce never stops services) or fight over
# PIC_STATE. The app serialises its own calls on one worker thread, so
# contention here means a manual run or a stray legacy guard -- rare, but the
# damage lasts a whole session, so lock anyway. flock is proven on this exact
# TV (moonlight-guard.sh single-instances through `flock -x -n` on an fd), and
# ONLY those proven flags are used: busybox flock has no -w, so the bounded
# wait is a retry loop. Bounded, because a verb holds the lock for however
# long its luna work takes (a worst-case off is tens of seconds) and the app's
# luna call would otherwise hang forever behind a wedged holder. If flock is
# missing we run unlocked, exactly as before -- worse than locking, far better
# than never running.
#
# netprio gets far less patience than the rest: it is the one verb the stream
# itself is waiting on (it runs just before LiStartConnection), and losing the
# race is a normal outcome -- an "off" that is still restoring holds the lock
# for tens of seconds. Giving up costs that session some receive headroom;
# waiting ninety seconds would cost it the connection attempt.
GM_LOCK="${GM_LOCK:-/tmp/aurora-gamemode.lock}"
case "$1" in
netprio) _lmax=5 ;;
on|off|enforce|recover|netprio-off|picture-on|picture-off) _lmax=90 ;;
*) _lmax="" ;;
esac
# `exec 9>file` is a SPECIAL builtin: if the redirection fails, a non-interactive
# shell exits on the spot -- before a single [gamemode] line has been printed,
# which is exactly what the app reads as "there is no root here". Probe the path
# with a plain command first (a failed redirection on those is survivable) and
# run unlocked if it is not writable, the same way a TV without flock does.
if [ -n "$_lmax" ] && ! true 2>/dev/null >>"$GM_LOCK"; then
	log "lock: cannot open $GM_LOCK - running '$1' unlocked"
	_lmax=""
fi
if [ -n "$_lmax" ] && command -v flock >/dev/null 2>&1; then
	exec 9>"$GM_LOCK"
	_lw=0
	until flock -x -n 9 2>/dev/null; do
		_lw=$((_lw + 1))
		if [ "$_lw" -gt "$_lmax" ]; then
			log "lock: $GM_LOCK still held after ${_lmax}s - giving up on '$1'"
			exit 1
		fi
		sleep 1
	done
fi

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
	# After the evictions above, so the numbers say what they freed, and before
	# the picture work, which is seconds of luna round trips.
	reclaim_caches
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
netprio)
	# Deliberately NOT folded into "on": "on" runs once the stream is up, by
	# which time the UDP sockets exist and a bigger rmem_max no longer reaches
	# them. This one is called just before the sockets are created, which is the
	# only moment at which it does anything -- and it is the one verb a stream
	# waits for, so nothing that can wait belongs in here.
	#
	# The exit status is net_raise's, not the last command's: a raise that never
	# happened (no writable state file, i.e. nothing could record the values it
	# would overwrite) must not come back as success, or the app would arm a
	# restore for a change nobody made.
	_nrc=0
	net_raise || _nrc=1
	close_other_apps
	exit $_nrc
	;;
netprio-off)
	# The receive buffers and nothing else, for a connection that never reached
	# STREAMING: game mode was never turned on for it, so there are no services
	# to restart, no governor to hand back and no picture state to put back, and
	# "recover" would go looking for all three.
	net_restore
	;;
off)
	log "=== GAME MODE OFF ==="
	picture_off
	net_restore
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
	#
	# First get any pre-1.5.3 guard out of the way: whatever it left engaged is
	# then picked up by the detection below, and nothing re-engages it behind
	# this restore three seconds later.
	stop_legacy_guard
	engaged=""
	[ -f "$PIC_STATE" ] && engaged="picture"
	# netprio runs before the connection, so it can be the ONLY thing a session
	# left behind: one that never reached STREAMING never turned game mode on.
	[ -f "$NET_STATE" ] && engaged="$engaged netprio"
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
	net_restore
	restore_game
	restore_quickset
	start_services
	unpin_cpus
	log "recover: done"
	;;
status)
	show_status
	picture_status
	netprio_status
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
	echo "usage: $0 on|off|enforce|recover|netprio|netprio-off|status|picture-on|picture-off|picture-status"
	exit 1
	;;
esac
