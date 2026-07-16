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

/* True if the gamepad should be auto-bridged (and thus kept off the Moonlight
 * gamepad path). Robust against a transiently-unreadable SDL serial during
 * stream churn / mid-session re-enumeration: falls back from the serial-keyed
 * pref to a VID:PID + name match against a known auto-plug logical device. */
bool hid_pt_gamepad_is_autoplug(app_input_t *input, const app_gamepad_state_t *gamepad);

uint16_t hid_pt_moonlight_excluded_mask_at_start(app_input_t *input);

void hid_pt_moonlight_exclude(struct stream_input_t *input, logical_device_t *item);
void hid_pt_moonlight_restore(struct stream_input_t *input, logical_device_t *item);

bool hid_pt_gamepad_is_moonlight_excluded(const struct stream_input_t *input,
                                          const app_gamepad_state_t *gamepad);

#endif

#endif /* HID_PT_GAMEPAD_MATCH_H */
