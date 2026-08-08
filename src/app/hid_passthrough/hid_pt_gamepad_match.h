#ifndef HID_PT_GAMEPAD_MATCH_H
#define HID_PT_GAMEPAD_MATCH_H

#include <stdint.h>

#if defined(TARGET_WEBOS)

#include "ctm/ctm_state.h"
#include "input/app_input.h"

struct stream_input_t;

bool hid_pt_match_gamepad_to_logical(const app_gamepad_state_t *gamepad,
                                     const logical_device_t *item);

app_gamepad_state_t *hid_pt_find_gamepad_for_logical(app_input_t *input,
                                                     logical_device_t *item);

logical_device_t *hid_pt_find_logical_for_gamepad(app_input_t *input,
                                                  const app_gamepad_state_t *gamepad);

/* Same lookup WITHOUT the moonlight_gs_id binding side effect — safe for
 * predicates that run against every pad (binding a mere match corrupts the
 * multi-pad slot/rumble routing). */
logical_device_t *hid_pt_peek_logical_for_gamepad(const app_gamepad_state_t *gamepad);

/* True if the gamepad should be auto-bridged (and thus kept off the Moonlight
 * gamepad path). Robust against a transiently-unreadable SDL serial during
 * stream churn / mid-session re-enumeration: for genuinely SERIAL-LESS pads it
 * falls back from the serial-keyed pref to a VID:PID + name match against a
 * known auto-plug logical device (with a readable serial the serial-keyed pref
 * is authoritative — the fuzzy fallback would adopt another same-model pad's
 * pref for a genuinely-new controller). */
bool hid_pt_gamepad_is_autoplug(app_input_t *input, const app_gamepad_state_t *gamepad);

uint16_t hid_pt_moonlight_excluded_mask_at_start(app_input_t *input);

void hid_pt_moonlight_exclude(struct stream_input_t *input, logical_device_t *item);

/// Give a Moonlight slot back by gs_id alone. This is the ONLY restore entry
/// point, deliberately: every teardown path runs after the bridge is gone, and
/// by then the pad is usually no longer enumerated in SDL, so re-deriving the
/// slot from the logical device silently resolves nothing and leaves the
/// exclusion bit set for the rest of the stream. Read the slot out of the
/// session record before stop_session() destroys it, and call this afterwards —
/// after clearing item->plugged, or the owner guard keeps the slot excluded on
/// behalf of the very device being unplugged.
void hid_pt_moonlight_restore_slot(struct stream_input_t *input, int gs_id);

/// Convergent exclusion sweep (1 Hz auto-plug poll): excludes the Moonlight
/// slot of every bridged pad that slipped past the point-in-time guards, so a
/// leaked parallel host pad heals within one tick instead of an app restart.
void hid_pt_moonlight_reconcile_exclusions(struct stream_input_t *input);

bool hid_pt_gamepad_is_moonlight_excluded(const struct stream_input_t *input,
                                          const app_gamepad_state_t *gamepad);

#endif

#endif /* HID_PT_GAMEPAD_MATCH_H */
