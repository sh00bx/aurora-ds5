#ifndef HID_PT_DEVICE_PREFS_H
#define HID_PT_DEVICE_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#if defined(TARGET_WEBOS)

#include "ctm/ctm_state.h"
#include "input/app_input.h"

/* Every buffer that holds a stable id is this long. */
#define HID_PT_STABLE_ID_LEN 96

void hid_pt_prefs_init(void);

/* The ONE normalisation every device identity passes through before it is used
 * as a pref key or compared against another identity: lowercase, the MAC
 * separators ':' '-' ' ' removed, everything else outside [0-9a-z_.] replaced by
 * '_'. Two properties the pref store depends on: it is idempotent (normalising
 * an already-normalised id is a no-op), and it cannot emit a character that
 * would change how the ini parser splits a line. */
void hid_pt_stable_id(const char *raw, char *out, size_t out_len);

/* Identity of a logical (bridge-side) device: its MAC if it has one, else its
 * enumeration key — both through hid_pt_stable_id(). */
void hid_pt_stable_id_for_logical(const logical_device_t *item, char *out, size_t out_len);

/* Identity of an SDL gamepad: its serial through hid_pt_stable_id(), so a pad
 * whose serial IS its BT MAC lands on exactly the string the logical form above
 * produces. A pad with no usable serial gets the synthetic
 * "sdl_<vid><pid>_<guid>" form instead, which hid_pt_stable_id_is_synthetic()
 * recognises. */
void hid_pt_stable_id_for_gamepad(const app_gamepad_state_t *gamepad, char *out, size_t out_len);

/* True for the synthetic no-serial form. It identifies a pad MODEL plus SDL's
 * GUID, not a physical unit, so callers that need per-unit certainty must not
 * treat a match on it as proof of identity. */
bool hid_pt_stable_id_is_synthetic(const char *stable_id);

bool hid_pt_prefs_get_auto_plugin(const char *stable_id);
void hid_pt_prefs_set_auto_plugin(const char *stable_id, bool enabled);
void hid_pt_prefs_flush(void);

/* Emit the [hid_pt_devices] section into an already-open ini writer. Used by
 * settings_save() so a full-config rewrite preserves the per-device prefs
 * instead of truncating them. */
void hid_pt_prefs_write_section(FILE *fp);

bool hid_pt_prefs_auto_plugin_for_logical(const logical_device_t *item);
bool hid_pt_prefs_auto_plugin_for_gamepad(const app_gamepad_state_t *gamepad);

/* INI parse hook: return 1 on handled entry. */
int hid_pt_prefs_ini_handler(const char *section, const char *name, const char *value);

#endif

#endif /* HID_PT_DEVICE_PREFS_H */
