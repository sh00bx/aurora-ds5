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
 * and an SDL pad on Automatic is left exactly as it always was.
 *
 * Per Moonlight slot (gs_id) this keeps the user's choice, resolved on the main
 * thread (hid_pt_gamepad_lightbar()), and the game's own colour, which arrives
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
 * painted anything yet, so the user's colour goes on. Main thread. */
void hid_pt_lightbar_pad_arrived(app_input_t *input, app_gamepad_state_t *gamepad);

/* A host LED event for slot @p gs_id (connection thread). Returns false when
 * this has no say over the pad -- Automatic, or no pad in the slot -- and the
 * caller passes the colour on as it always did; true when handled, which
 * includes ignoring it for a colour the game may not change. */
bool hid_pt_lightbar_host_led(app_input_t *input, int gs_id, uint8_t r, uint8_t g, uint8_t b);

/* The choices changed on the Controllers page: re-resolve and repaint every
 * open pad, except the slots in @p skip_mask (bridged: an SDL write there
 * would be a second writer next to the bridge). Main thread. */
void hid_pt_lightbar_refresh(app_input_t *input, uint16_t skip_mask);

#endif

#endif /* HID_PT_LIGHTBAR_H */
