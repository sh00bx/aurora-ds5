#ifndef HID_PT_GAMEPAD_MATCH_H
#define HID_PT_GAMEPAD_MATCH_H

#include <stdint.h>

#if defined(TARGET_WEBOS)

#include "ctm/ctm_state.h"
#include "input/app_input.h"

struct stream_input_t;

/* Pad<->bridged-device identity lives entirely inside hid_pt_gamepad_match.c:
 * one ordered tier table plus the two resolvers over it. Nothing outside needs
 * a raw match, and exporting one invited callers to pick their own confidence
 * floor by accident. */

/* True if the gamepad should be auto-bridged (and thus kept off the Moonlight
 * gamepad path). Robust against a transiently-unreadable SDL serial during
 * stream churn / mid-session re-enumeration: a pad with no usable serial may
 * reach its logical device through the VID:PID tiers, while a pad WITH one is
 * only ever matched by exact identity — a weaker match would hand a
 * genuinely-new controller another same-model pad's pref. */
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
