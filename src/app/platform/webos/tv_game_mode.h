#pragma once

#include <stdbool.h>

/*
 * Drive the TV's game mode for the duration of a stream.
 *
 * Everything this does needs root: stopping the discovery/cast services that
 * fight the DS5 for the combo chip's airtime, pinning the cores LG's power
 * governor keeps hotplugging, boosting the stream threads, and switching the
 * panel to its game picture preset. The work itself lives in gamemode.sh, which
 * ships inside this IPK; this is the part that decides WHEN it runs.
 *
 * Begin/end are safe to call unbalanced: end without begin does nothing, and a
 * begin while the previous cycle is still restoring is deferred to that worker
 * (it runs a fresh cycle when its teardown is done) rather than dropped.
 *
 * Begin also hands the page cache back (sync + drop_caches, with MemAvailable
 * and SwapFree logged either side of it, because the point of that exercise is
 * numbers about this TV's memory pressure). It lives here and not in the
 * pre-connection call below because none of it has to happen before the sockets
 * exist, and its cost has never been measured on a TV this deep into swap.
 */
void tv_game_mode_stream_begin(void);

void tv_game_mode_stream_end(void);

/*
 * Tell a running cycle that the stream's HDR state just changed. The panel
 * moves to its HDR dimension a beat after HDR engages, and that dimension
 * carries its own picture mode, so the mode has to be asked for again over
 * there. The enforce schedule finds that by itself, but only at its next tick
 * -- up to fifteen seconds later once the early ticks are spent, which is what
 * a mid-session HDR switch runs into. This makes it immediate instead. Cheap
 * when nothing is streaming: no thread is woken and no shell runs.
 *
 * Returns true only when a running cycle actually took the kick. False means it
 * was dropped -- nothing is streaming, or the kicks are coming too fast to be
 * worth a round trip each -- and the panel is left to the ordinary enforce
 * schedule, which is the behaviour this call is an optimisation over. A caller
 * that latches "already notified" must latch on the return value, so that a
 * dropped kick is offered again at the next transition.
 *
 * Rate limiting is done in here, on purpose: a caller sees one transition at a
 * time and cannot tell that its source is flapping.
 */
bool tv_game_mode_notify_hdr(bool hdr_active);

/*
 * Lift the kernel's receive buffers before the session opens its UDP sockets.
 *
 * webOS clamps SO_RCVBUF to net.core.rmem_max, and the stock 512 KiB is about
 * ten milliseconds of buffer at 400 Mbps: one hiccup on the receive path and
 * the kernel has dropped the video packets before moonlight asks for them. That
 * clamp is a sysctl, so root can lift it -- but only for sockets created AFTER
 * the write, which is what pins this call to one exact place: the session
 * thread, immediately before LiStartConnection.
 *
 * It therefore blocks the caller, but never without a bound: the luna call has
 * no timeout of its own, so it runs on a thread of its own and this waits a few
 * seconds for it and then goes on without it. What it waits for is six /proc
 * writes and their read-backs -- plus, only when the close-apps marker file is
 * set, one luna round trip per running app, which is why that is off by
 * default. A stream on the stock receive buffer is a bad outcome; a connection
 * that cannot be cancelled is a much worse one.
 *
 * Independent of the game-mode cycle above: it may run for a session that never
 * calls begin, and tv_game_mode_stream_end() puts it back on every path,
 * including a connection that fails before it reaches STREAMING. A TV without
 * root says so once and then keeps quiet; a script that was merely busy says so
 * per stream, because that one is not the same thing and comes back.
 */
void tv_game_mode_net_prepare(void);

/*
 * Undo a game mode that outlived the app. Call once at start-up: if a previous
 * run was killed mid-stream, the TV is still on stopped services and a game
 * picture preset, and nothing else will ever put that back.
 */
void tv_game_mode_recover_stale(void);
