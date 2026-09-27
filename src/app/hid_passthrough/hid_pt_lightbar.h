#ifndef HID_PT_LIGHTBAR_H
#define HID_PT_LIGHTBAR_H

#include <stdbool.h>
#include <stdint.h>

#if defined(TARGET_WEBOS)

#include "input/app_input.h"

/* The lightbar colour chosen on the Controllers page, on the pads aurora drives
 * itself over SDL -- in the menus and in a stream alike -- and in ds5_txd's idle
 * painter, the second writer of a DualSense's bar in that state. A bridged pad
 * gets its colour from the bridge instead (controller_ds4.c, controller_ds5.c),
 * and an SDL pad on Automatic is left as it always was.
 *
 * A controller has a colour per mode. Per Moonlight slot (gs_id) this keeps the
 * one that applies, resolved on the main thread: HID while the pad is bridged,
 * the mode the host builds while the host has a pad for it (from its arrival
 * to its removal), else the mode it comes back in -- the menus, where the idle
 * painter is handed that same colour. And the game's own colour, which arrives
 * on the connection thread: a non-black host LED event means the game owns the
 * bar, a black one hands it back. The bar shows the game's colour while it owns
 * it and the user lets it, else the user's; the painter is handed the same, so
 * a repaint after five quiet seconds cannot undo what SDL wrote. The state is
 * spinlocked for that one cross-thread writer, and nothing here blocks. */

/* SDL opened @p gamepad (app_input_init_gamepad()): paint its colour. Main
 * thread. */
void hid_pt_lightbar_pad_opened(app_input_t *input, app_gamepad_state_t *gamepad);

/* SDL is closing @p gamepad. Main thread. */
void hid_pt_lightbar_pad_closed(app_gamepad_state_t *gamepad);

/* The host just got a new pad for @p gamepad (an arrival): its game has not
 * painted anything yet, so the colour of the mode the host builds goes on.
 * Main thread. */
void hid_pt_lightbar_pad_arrived(app_input_t *input, app_gamepad_state_t *gamepad);

/* The host's pad for @p gamepad is gone (a removal): nothing is painted -- a
 * re-announce's arrival follows at once, a mount goes on with
 * hid_pt_lightbar_pad_bridged(). Main thread. */
void hid_pt_lightbar_pad_removed(app_gamepad_state_t *gamepad);

/* The bridge took @p gamepad's slot: its record follows the HID colour the
 * bridge paints, with nothing written over SDL. Main thread. */
void hid_pt_lightbar_pad_bridged(app_input_t *input, app_gamepad_state_t *gamepad);

/* A host LED event for slot @p gs_id (connection thread). Returns false when
 * this has no say over the pad -- Automatic, or no pad in the slot -- and the
 * caller passes the colour on as it always did; true when handled, which
 * includes ignoring it for a colour the game may not change. */
bool hid_pt_lightbar_host_led(app_input_t *input, int gs_id, uint8_t r, uint8_t g, uint8_t b);

/* The choices changed on the Controllers page, or a pad's mode did: re-resolve
 * every open pad and repaint it, except the slots in @p skip_mask (bridged: an
 * SDL write there would be a second writer next to the bridge), whose choice
 * is only recorded for the painter. A pad whose colour went back to Automatic
 * gets the host's last colour, else SDL's own. Main thread. */
void hid_pt_lightbar_refresh(app_input_t *input, uint16_t skip_mask);

/* The colour picker's live preview: paint @p rgb on @p gamepad (not bridged)
 * and hand it to the painter, storing nothing; until
 * hid_pt_lightbar_preview_end(), which puts every pad's own colour back (the
 * slots in @p skip_mask only in the record). Main thread, and non-blocking like
 * everything here -- the painter's datagram is dropped rather than waited
 * for. */
void hid_pt_lightbar_preview(app_input_t *input, app_gamepad_state_t *gamepad, uint32_t rgb);
void hid_pt_lightbar_preview_end(app_input_t *input, uint16_t skip_mask);

/* The stream ended (its bridges are down): forget every game's colour, the
 * host's pads and any preview, and put each pad's own colour -- the mode it
 * comes back in -- back on every open pad and into the painter. Automatic pads
 * are left as they are, unless a colour of ours was on them. Main thread. */
void hid_pt_lightbar_stream_ended(app_input_t *input);

#endif

#endif /* HID_PT_LIGHTBAR_H */
