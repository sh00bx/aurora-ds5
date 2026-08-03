#pragma once

#include <stdbool.h>

/* Arbiter for the DS5 lightbar colour ds5_txd paints while it considers a pad
 * IDLE (nobody feeding the daemon).
 *
 * Three states have to map onto one bar, and only this process can tell them
 * apart -- the daemon sees SDL traffic not at all (it goes through the kernel)
 * and sees a sparse passthrough session as indistinguishable from idle:
 *
 *   passthrough session owns the pad -> daemon must not paint (the host does)
 *   held open as one of our SDL pads  -> dark red
 *   connected but unused             -> dark blue
 *
 * We never paint here. The daemon stays the only writer on the pad; this just
 * selects. Best-effort: with no daemon the datagram goes nowhere.
 *
 * Called from two threads (SDL main loop for the gamepad state, the ctm session
 * thread for ownership), so the state is mutex-guarded and only sent on change.
 */

/* A passthrough session has taken/released this pad (the host owns the bar). */
void ds5_idle_lb_set_owned(bool owned);

/* We hold at least one DualSense open as an SDL gamepad. */
void ds5_idle_lb_set_sdl_open(bool open);

/* Shutdown: hand the bar back to the "connected, unused" colour. */
void ds5_idle_lb_release(void);
