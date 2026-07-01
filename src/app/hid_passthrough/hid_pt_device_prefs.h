#ifndef HID_PT_DEVICE_PREFS_H
#define HID_PT_DEVICE_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#if defined(TARGET_WEBOS)

#include "ctm/ctm_state.h"
#include "input/app_input.h"

void hid_pt_prefs_init(void);
void hid_pt_prefs_clear(void);

/* Stable id: BT MAC without ':' (lowercase), else logical_device_t.key */
void hid_pt_stable_id_for_logical(const logical_device_t *item, char *out, size_t out_len);
void hid_pt_stable_id_for_gamepad(const app_gamepad_state_t *gamepad, char *out, size_t out_len);

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
