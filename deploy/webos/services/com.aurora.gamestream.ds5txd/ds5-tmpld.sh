#!/bin/sh
# Respawn the DS5 raw-ACL transport forwarder (root) + low-priority watchdog for
# the jail-tmp-remount dropout (see history). Plus: drop ds5_txd off REAL-TIME
# scheduling.
#
# Why the priority drop (2026-06-21): ds5_txd self-sets SCHED_FIFO prio 14 for
# all its threads. Its HCI-monitor thread wakes on every BT packet (~420/s) and,
# at RT prio, PREEMPTS the CTM app's usbip input-handling thread -> the streamed
# DS5 input gets polled late -> input "aussetzer". The old SDL input path had no
# such RT daemon and was smooth; severity scaled with ds5_txd's RT load (old=
# slight, self-heal-binary=severe) -> RT preemption is the fingerprint. Running
# ds5_txd at NORMAL priority lets it interleave with (not preempt) the input
# thread; injection is only ~94/s and the BT controller meters output to ~10ms,
# so haptic stays smooth. Revert = delete the chrt loop.
SOCK=/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl.sock
TMPL=/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl_tmpl
HIDFD=/var/palm/jail/com.aurora.gamestream/tmp/ds5_hidfd.sock
LOG=/tmp/ds5_txd.log
PIDFILE=/tmp/ds5-tmpld.pid

# The daemon now ships inside the aurora IPK, next to this script, so resolve it
# relative to $0 instead of the old hand-installed /var/lib/webosbrew location.
# TXD_BIN lets the caller (service.js) pin an absolute path; the dirname fallback
# keeps the script runnable standalone.
#
# The PIDFILE deliberately keeps its old name: it is the mutual-exclusion token
# against a still-installed boot-hook supervisor. The two must never both drive
# ds5_txd — they would unlink-steal each other's rendezvous sockets on every
# remount, which is the exact failure this guard was written for.
TXD_BIN="${TXD_BIN:-$(dirname "$0")/ds5_txd}"

# Singleton guard. The boot hook's start-stop-daemon --exec check can NEVER dedup
# a shell script (busybox matches --exec against /proc/<pid>/exe, which for a
# running script is the INTERPRETER /bin/sh -> busybox, never the script path), so
# a hook re-run (manual start, hbchannel restart, webosbrew re-elevation) would
# spawn a second supervisor + ds5_txd, and the pair would unlink-steal the
# rendezvous sockets from each other on every remount. Guard here instead.
#
# Acquisition is ATOMIC (noclobber create): a plain read-check-write had a
# TOCTOU hole — two simultaneous starts both saw no live owner and both wrote
# the pidfile, i.e. exactly the dual-supervisor failure this guard exists to
# stop. A stale file (crash leftover, recycled pid) is CLAIMED by rename first:
# mv is atomic, so of several racers exactly one removes it; the rest re-loop
# and either lose the create (and find the winner live) or exit.
#
# Liveness anchors on the process IDENTITY, not just cmdline bytes: a recycled
# pid whose cmdline merely mentions the name (vi/tail/grep on this file) must
# not count as a supervisor, or a crash could never be recovered from. A real
# supervisor is the script exec'd directly (the kernel sets comm to the script
# basename for a shebang exec) or a shell interpreting it.
is_live_supervisor() {
  kill -0 "$1" 2>/dev/null || return 1
  grep -q "ds5-tmpld" "/proc/$1/cmdline" 2>/dev/null || return 1
  case "$(cat "/proc/$1/comm" 2>/dev/null)" in
    ds5-tmpld*|sh|ash|dash|busybox) return 0 ;;
  esac
  return 1
}

# Report a startup failure to BOTH sinks and give up: the boot hook's stderr may be
# /dev/null, and $LOG may itself be unwritable (that is one of the failures below).
fatal() {
  echo "$(date +%H:%M:%S) [tmpld] $*" >&2
  echo "$(date +%H:%M:%S) [tmpld] $*" >>"$LOG" 2>/dev/null
  exit 1
}

# The noclobber create can fail for two different reasons and only ONE of them the
# loop can resolve: "file exists" (a live or stale owner -> checked/claimed below)
# vs "cannot write" (read-only /tmp, or ENOSPC, where the create still leaves an
# EMPTY file: OLD is then empty, the is_live_supervisor exit is skipped, and mv/rm
# make no progress). Without the distinction the second case spun tight as root
# forever and ds5_txd was never started. Bound the loop and back off, so a genuinely
# broken /tmp fails LOUDLY instead of silently pegging a core.
tries=0
while :; do
  if ( set -C && echo "$$" > "$PIDFILE" ) 2>/dev/null; then
    break   # we own the pidfile
  fi
  if [ ! -e "$PIDFILE" ]; then
    # Create failed AND the file is absent — ambiguous: a broken /tmp, OR a
    # concurrent starter that mv'd the same stale pidfile away (below) in
    # exactly the create->test window, in which case fataling here misdiagnoses
    # a lost race as a dead /tmp. Re-loop and retry the create once (the racer's
    # winner then owns the file and is found live); only a create that fails
    # AGAIN with the file still absent is a really broken /tmp. Bounded by the
    # same tries cap as everything else.
    if [ "${ABSENT_RETRY:-0}" -ge 1 ]; then
      fatal "cannot create $PIDFILE (read-only or missing /tmp?) -> not starting"
    fi
    ABSENT_RETRY=1
    tries=$((tries+1))
    continue
  fi
  ABSENT_RETRY=0
  OLD=$(cat "$PIDFILE" 2>/dev/null)
  if [ -n "$OLD" ] && is_live_supervisor "$OLD"; then
    exit 0  # a live supervisor already owns it
  fi
  # Claim the stale file. On success re-try the create IMMEDIATELY (the common
  # crash-leftover path must not pay a boot delay); only a claim that made no
  # progress backs off.
  if mv -f "$PIDFILE" "$PIDFILE.stale.$$" 2>/dev/null; then
    rm -f "$PIDFILE.stale.$$"
  else
    sleep 1
  fi
  tries=$((tries+1))
  if [ $tries -ge 10 ]; then
    fatal "pidfile $PIDFILE unclaimable after $tries rounds (disk full?) -> not starting"
  fi
done

# Take ds5_txd off real-time AND split priorities so neither thread hurts the
# CTM usbip input thread, while keeping haptic tight:
#   main (inject loop, ~94/s): SCHED_OTHER nice -5 -> responsive, tight 0x36 on-air
#     timing, but normal policy so it never preempts the input thread.
#   other threads (HCI-monitor capture wakes ~420/s): SCHED_OTHER nice 19 -> yields
#     to the input thread so it stops nibbling input poll latency.
#
# 2026-06-22 (V5, audit wf_3b16cc9b): MEASURED in-jail during live Ratchet — the CTM
# input/session threads run at nice -20 (the aurora app's ctl_set_rt_prio()
# setpriority(-20) fallback SUCCEEDS; the jail grants CAP_SYS_NICE for nice even
# though it denies SCHED_FIFO). So inject at -5 sits BELOW input (-20) = input wins,
# no cross-process preemption. The earlier "inject -5 out-prioritizes input" concern
# was refuted on this hardware; -5 is correct and kept (still under input).
#
# main is deniced ONCE (denice_main): `renice -n` is RELATIVE on busybox/toybox, so
# re-running `renice -n -5` would drive -5 -> -10 -> -15 -> -20 and TIE the input
# thread (reintroducing the aussetzer). The main thread always exists at startup, so
# one call is correct. Workers are re-asserted (reassert_workers): they may not be
# visible at the first pass (created just after the socket binds), and nice clamps at
# +19 so relative-renice cannot compound them.
denice_main() {
  chrt -o -p 0 "$1" 2>/dev/null
  renice -n -5 -p "$1" >/dev/null 2>&1
}
reassert_workers() {
  for tid in $(ls /proc/"$1"/task 2>/dev/null); do
    [ "$tid" = "$1" ] && continue
    chrt -o -p 0 "$tid" 2>/dev/null
    renice -n 19 -p "$tid" >/dev/null 2>&1
  done
}

# Boot default: SHALLOW elastic audio FIFO (~32ms parked worst case). Only a
# rate-servo host (Vibepollo >= Phase 2b) bounds a deep FIFO's parked latency —
# against a fixed-100/s host, depth 10 pegs full and parks ~107ms permanently.
# The app raises the depth to 10 via the ACL_TAG_CTRL datagram once the host
# advertises the servo capability in HOST_CONFIG.
echo 3 > /tmp/ds5_inject_fifo 2>/dev/null
while true; do
  # Rotate (never truncate) at spawn so a crashed instance's log survives one
  # respawn generation — the old O_TRUNC redirect wiped the crash evidence ~1s
  # after every death. Open in APPEND mode: the in-loop 1MB cap truncates the
  # live file, and only an O_APPEND fd follows that truncation back to offset 0.
  [ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
  "$TXD_BIN" "$SOCK" "$TMPL" "$HIDFD" >>"$LOG" 2>&1 &
  TXD=$!
  # wait for bind + thread creation, then take ds5_txd off real-time priority
  i=0
  while [ $i -lt 10 ]; do
    [ -S "$SOCK" ] && break
    kill -0 "$TXD" 2>/dev/null || break
    sleep 1; i=$((i+1))
  done
  # Main thread off RT once (relative-renice would compound it on busybox).
  denice_main "$TXD"
  # Workers may not exist at the first pass (created just after the socket binds), so
  # re-assert a SHORT, BOUNDED handful of times to catch a late-spawned thread that
  # would otherwise stay at SCHED_FIFO prio 14 and preempt the CTM input thread
  # (= aussetzer). Then STOP — no steady-state syscall churn (the O2 win); ds5_txd
  # never spawns threads after startup, and +19 clamps so this can't compound.
  n=0
  while [ $n -lt 5 ]; do
    reassert_workers "$TXD"
    n=$((n+1))
    kill -0 "$TXD" 2>/dev/null || break
    [ $n -lt 5 ] && sleep 1
  done
  echo "$(date +%H:%M:%S) [tmpld] ds5_txd $TXD -> SCHED_OTHER (main nice -5, workers nice 19, ${n}x)" >>"$LOG"
  # Supervise. ds5_txd now self-heals the jail-tmp remount IN-PROCESS (mountinfo
  # poll -> rebind ds5_acl.sock + re-publish, in microseconds). A momentarily-gone
  # socket node is therefore NORMAL and must NOT trigger a restart: the old
  # kill-on-first-vanish raced that in-process rebind and forced a full restart
  # (re-seed, lost template, hidraw fallback) -> the very dropout the self-heal
  # exists to avoid. Only restart if the process dies OR the socket stays gone for a
  # sustained window (~10s = self-heal genuinely stuck).
  gone=0
  while kill -0 "$TXD" 2>/dev/null; do
    if [ ! -S "$SOCK" ]; then
      gone=$((gone+1))
      if [ $gone -ge 6 ]; then   # 6 consecutive misses ~= 10s of 2s ticks
        echo "$(date +%H:%M:%S) [tmpld] ds5_acl.sock gone >10s (in-process self-heal stuck) -> restarting ds5_txd" >>"$LOG"
        kill "$TXD" 2>/dev/null
        break
      fi
    else
      gone=0
    fi
    # Size guard: bound the live log within one long-lived run (the 10s stats line
    # accumulates over gaming hours). Truncate in place — the daemon holds an
    # O_APPEND fd, so its next write resumes at offset 0 (no lost/dangling fd).
    sz=$(wc -c <"$LOG" 2>/dev/null || echo 0)
    [ "$sz" -gt 1048576 ] && : >"$LOG"
    sleep 2
  done
  wait "$TXD" 2>/dev/null
  sleep 1
done
