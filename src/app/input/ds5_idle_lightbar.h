#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Arbiter for the DS5 lightbar colour ds5_txd paints while it considers a pad
 * IDLE (nobody feeding the daemon).
 *
 * Three states have to map onto one bar, and only this process can tell them
 * apart -- the daemon sees SDL traffic not at all (it goes through the kernel)
 * and sees a sparse passthrough session as indistinguishable from idle:
 *
 *   passthrough session owns the pad -> daemon must not paint (the host does)
 *   held open as one of our SDL pads  -> dark red, or the colour the user chose
 *                                       for it (ds5_idle_lb_set_sdl_colour())
 *   connected but unused             -> no selection at all: a clear-selection
 *                                       datagram (ctrl 0x03) restores the
 *                                       daemon's boot-configured default
 *                                       (DS5_IDLE_LIGHTBAR, including "off")
 *                                       instead of overwriting it with a colour
 *
 * We never paint here. The daemon stays the only writer on the pad; this just
 * selects (the two claimed states go out as a ctrl 0x02 colour overlay).
 * Best-effort: with no daemon the datagram goes nowhere.
 *
 * Called from two threads (SDL main loop for the gamepad state, the ctm session
 * thread for ownership), so the state is mutex-guarded and only sent on change.
 *
 * NONE of these calls may ever wait. The SDL main loop is the thread that pumps
 * every keyboard event, so a send that parks there takes the keyboard down with
 * it (1.0.58 regression: an AF_UNIX datagram send is flow controlled and blocks
 * once the daemon's queue is full). The socket is therefore non-blocking, the
 * syscall runs outside the state lock, and a send that cannot go through is
 * dropped and retried on the next change. AURORA_DS5_IDLE_LB=0 disables the
 * signal entirely.
 */

/* A passthrough session has taken (true) / released (false) a pad. Counted,
 * not a flag: sessions run one per pad, so each session must call this exactly
 * once with true and once with false; the bar counts as owned while ANY
 * session still holds a claim. */
void ds5_idle_lb_set_owned(bool owned);

/* We hold at least one DualSense open as an SDL gamepad. */
void ds5_idle_lb_set_sdl_open(bool open);

/* What the painter paints while we hold a DualSense over SDL. */
typedef enum {
    /* The dark red of old: no colour was chosen (Automatic). */
    DS5_IDLE_LB_SDL_DEFAULT = 0,
    /* @c rgb, the one the user chose (or the game's, while it owns the bar) --
     * the colour SDL writes too, so the two writers agree. 0 is a dark bar,
     * sent as "paint black": a plain 000000 would mean "paint nothing". */
    DS5_IDLE_LB_SDL_COLOUR,
    /* Nothing: the open pads want different colours, and one painter colour
     * for every link would fight all but one of them. */
    DS5_IDLE_LB_SDL_NONE,
} ds5_idle_lb_sdl_t;

void ds5_idle_lb_set_sdl_colour(ds5_idle_lb_sdl_t how, uint32_t rgb);

/* Shutdown: drop every claim (the owned count resets to 0) and hand the bar
 * back to the daemon's own default. */
void ds5_idle_lb_release(void);
