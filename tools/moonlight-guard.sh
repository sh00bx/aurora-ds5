#!/bin/sh
# moonlight-guard: persistent root watcher for rooted webOS (LG G4).
# While the aurora/moonlight client PROCESS EXISTS, continuously enforce game
# mode (evict streaming/cast background apps + services on entry, keep the client
# on SCHED_RR, confine the QuickSet busy-loop). When aurora exits, restore. (SAM's
# last_foreground file is unreliable here -- doesn't update on switch-to-YouTube,
# goes stale on aurora -- so process-presence is the dependable trigger; safe now
# that close_apps is ON-only and never re-kills the app you switch to.)
#
# Started once per boot from /var/lib/webosbrew/init.d (see install notes).
# Single-instance guarded via flock.

GM="${GM:-/var/lib/webosbrew/gamemode.sh}"
APPMGR="luna://com.webos.applicationManager"
# Aurora build app id (CMakeLists.txt WEBOS_APPINFO_ID). Used as a prefix match
# so the foreground check fires for this build.
MOON_ID="com.aurora.ds5"
POLL="${POLL:-3}"          # seconds between checks
LOCK=/tmp/moonlight-guard.lock

exec 9>"$LOCK"
if ! flock -x -n 9; then
	echo "[guard] already running" >&2
	exit 0
fi

# Own the log file regardless of how we were launched (start-stop-daemon
# --background sends stdout to /dev/null; run-parts inherits boot stdout).
LOGFILE="${LOGFILE:-/tmp/moonlight-guard.log}"
: >"$LOGFILE" 2>/dev/null && exec >>"$LOGFILE" 2>&1

# Pidfile for clean management (pkill -f moonlight-guard self-matches the
# killing shell's own argv, so stop via: kill $(cat /tmp/moonlight-guard.pid)).
# NB: signal traps must exit() explicitly - a bare trap handler resumes the
# script after returning, so SIGTERM alone would not stop the loop.
echo $$ >/tmp/moonlight-guard.pid
trap 'rm -f /tmp/moonlight-guard.pid; exit 0' INT TERM
trap 'rm -f /tmp/moonlight-guard.pid' EXIT

log() { echo "[guard $(date '+%H:%M:%S')] $*"; }

# Foreground detection. webOS 25 getForegroundAppInfo returns NOTHING to a root
# LS2 caller, and a CPU-activity proxy mis-fired: it kept game mode ON during a
# hysteresis window while the user switched AWAY to a blocklisted app (e.g.
# YouTube = youtube.leanback.v4) -> enforce's close_apps killed the app they
# were switching to, flipping them back to aurora. SAM writes the LIVE foreground
# app id to this file (root-readable, updated on every switch) -> exact signal.
# Game mode ON only while aurora is foreground; the instant the user switches
# away, foreground_is_moonlight() goes false -> the loop stops calling enforce
# (so close_apps never touches the app being switched to) and restores after a
# short debounce.
FG_FILE="${FG_FILE:-/var/luna/preferences/last_foreground_app_id.json}"
MOON_ID="${MOON_ID:-com.aurora.ds5}"
OFF_DEBOUNCE="${OFF_DEBOUNCE:-2}"   # polls of non-foreground before restoring (rides transient fg blips)

# Trigger = aurora PROCESS PRESENCE. The SAM last_foreground file proved
# unreliable (it does NOT update when you switch TO YouTube, and goes stale on
# aurora so the guard would never turn off after aurora exits). The earlier
# objection to pidof ("backgrounded app stays resident -> keeps killing other
# apps") is now MOOT because close_apps is ON-only (it no longer runs on enforce
# -> switching to a blocklisted app is never killed). So: game mode ON while
# aurora exists, OFF when it exits. (Note: aurora holds the HW video decoder
# while streaming even when backgrounded, so other video apps stay grey until
# aurora is fully closed -- that is aurora app behaviour, not the guard.)
foreground_is_moonlight() {
	pidof aurora >/dev/null 2>&1 && return 0
	pidof moonlight >/dev/null 2>&1 && return 0
	return 1
}

log "started (poll=${POLL}s, pidof trigger, off-debounce=${OFF_DEBOUNCE})"

# Startup reconcile: a previous guard instance may have died AFTER "on" (OOM,
# init.d re-run) leaving the CPUs pinned (mp_enable=0) with aurora already
# gone — a fresh instance starts at state=off and would never call "off". If
# aurora is absent but the pin is active, restore now.
if ! foreground_is_moonlight; then
	if [ "$(cat /proc/lg/pm/mp_enable 2>/dev/null)" = "0" ]; then
		log "stale game mode from a previous guard (mp_enable=0, no aurora) -> GAME MODE OFF"
		sh "$GM" off
	fi
fi

state=off
off_cnt=0
while true; do
	if foreground_is_moonlight; then
		off_cnt=0
		if [ "$state" = off ]; then
			log "aurora foreground -> GAME MODE ON"
			sh "$GM" on
			state=on
		else
			# periodic re-enforce: preload manager relaunches evicted apps in bg
			sh "$GM" enforce
		fi
	elif [ "$state" = on ]; then
		# user switched away: stop enforcing immediately (above branch no longer
		# runs close_apps), restore after a short debounce to ride out fg blips.
		off_cnt=$((off_cnt + 1))
		if [ "$off_cnt" -ge "$OFF_DEBOUNCE" ]; then
			log "aurora left foreground -> GAME MODE OFF"
			sh "$GM" off
			state=off
		fi
	fi
	sleep "$POLL"
done
