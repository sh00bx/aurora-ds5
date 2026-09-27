#ifndef HID_PT_DEVICE_PREFS_H
#define HID_PT_DEVICE_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#if defined(TARGET_WEBOS)

#include "ctm/ctm_state.h"
#include "input/app_input.h"
#include "stream/input/gamepad_type_pref.h"
#include "stream/input/lightbar_pref.h"

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

/* Store (and persist) the auto-plug choice for one device. Returns false when
 * the choice could NOT be stored -- an empty id, or a full table in which every
 * slot belongs to a device that opted in. Callers must surface that: the UI
 * checkbox reads its state from a different store, so a dropped write leaves the
 * box ticked and the setting is simply gone after the next launch. */
bool hid_pt_prefs_set_auto_plugin(const char *stable_id, bool enabled);
void hid_pt_prefs_flush(void);

/* Emit the [hid_pt_devices] and [controller_app_modes] sections into an
 * already-open ini writer. Used by settings_save() so a full-config rewrite
 * preserves the per-device prefs instead of truncating them. Only non-default
 * prefs are written: an opted-in auto-plug as `<id> = true`, a chosen SDL type
 * as `<id>.sdl_type = xbox|playstation|auto`, a chosen lightbar as
 * `<id>.lightbar = auto|off|rrggbb` (+ `<id>.lightbar_game = 0`), a game's fixed
 * mode as `<app> = hid|x360|ds4`. An absent key reads as the default. */
void hid_pt_prefs_write_section(FILE *fp);

bool hid_pt_prefs_auto_plugin_for_logical(const logical_device_t *item);
bool hid_pt_prefs_auto_plugin_for_gamepad(const app_gamepad_state_t *gamepad);

/* The controller type the host should emulate for this device while it runs
 * over SDL; GAMEPAD_TYPE_PREF_AUTO when nothing is stored. Shares the entry,
 * the table and the flush with the auto-plug pref above. */
gamepad_type_pref_t hid_pt_prefs_get_sdl_type(const char *stable_id);

/* True when an SDL type was chosen for this id -- AUTO included, see
 * hid_pt_prefs_set_sdl_type() -- and then *out is that choice. */
bool hid_pt_prefs_lookup_sdl_type(const char *stable_id, gamepad_type_pref_t *out);

/* Store (and persist) the SDL type for one device. Same contract as
 * hid_pt_prefs_set_auto_plugin(): false when the choice could NOT be stored (an
 * empty id, or a full table in which every slot holds a non-default pref), and
 * the caller must say so.
 *
 * AUTO with @p keep_auto false erases the choice and never needs a slot. With
 * @p keep_auto true it is stored as a choice of its own, which a lookup that
 * falls back to another id when this one has none needs: there "Automatic" must
 * stop the fallback rather than reveal what the other id holds. */
bool hid_pt_prefs_set_sdl_type(const char *stable_id, gamepad_type_pref_t type, bool keep_auto);

gamepad_type_pref_t hid_pt_prefs_sdl_type_for_logical(const logical_device_t *item);
gamepad_type_pref_t hid_pt_prefs_sdl_type_for_gamepad(const app_gamepad_state_t *gamepad);

/* The lightbar colour chosen for this id, Automatic included: true when one
 * was stored, and then *out is it. Shares the entry, the table, the flush and
 * the explicit-Automatic rule with the SDL type above:
 * `<id>.lightbar = auto|off|rrggbb`, `<id>.lightbar_game = 0` for a colour the
 * game may not change. */
bool hid_pt_prefs_lookup_lightbar(const char *stable_id, lightbar_pref_t *out);

/* Store (and persist) the lightbar colour for one device. Same contract as
 * hid_pt_prefs_set_sdl_type(), @p keep_auto included. */
bool hid_pt_prefs_set_lightbar(const char *stable_id, const lightbar_pref_t *lb, bool keep_auto);

/* What is stored under the device's own id, Automatic when nothing is. */
lightbar_pref_t hid_pt_prefs_lightbar_for_logical(const logical_device_t *item);

/* ---- per-game mode ([controller_app_modes]) --------------------------------
 *
 * A game can fix the mode of every controller (Forza only takes an Xbox pad).
 * Stored per host app as `<app> = hid|x360|ds4`, keyed by the app's NAME passed
 * through hid_pt_stable_id(): app ids change whenever the host re-syncs its
 * library, the name does not. Only the app the running session launched is ever
 * looked up, so the table is read through "the current app" alone. Main thread
 * only, like the rest of this store. */

/* The app the session streams, set at session create and cleared (NULL) at
 * destroy. Its name is kept as given for the Controllers page. */
void hid_pt_prefs_set_current_app(const char *app_name);

/* The current app's name as the host lists it. False with no session, or a
 * name that normalises to nothing (no key to store a lock under). */
bool hid_pt_prefs_current_app_name(char *buf, size_t len);

/* The current app's fixed mode; GAMEPAD_MODE_NONE when it has none, or when no
 * app is current. */
gamepad_mode_t hid_pt_prefs_current_app_mode(void);

/* Fix (and persist) the current app's mode; GAMEPAD_MODE_NONE removes it. False
 * when it could not be stored: no current app, or a full table (removal never
 * fails). The caller must say so. */
bool hid_pt_prefs_set_current_app_mode(gamepad_mode_t mode);

/* A device's own "mode HID" (its auto-plug flag) as the current app's lock has
 * it: true under a HID lock, false under an SDL one, @p own_hid without. The
 * one rule every auto-plug decision goes through. */
bool hid_pt_prefs_effective_hid(bool own_hid);

/* A pad's own SDL type as the current app's lock has it: the lock's type under
 * X360/DS4, @p own otherwise (also under a HID lock, for a pad that cannot be
 * mounted and so stays on SDL). */
gamepad_type_pref_t hid_pt_prefs_effective_sdl_type(gamepad_type_pref_t own);

/* INI parse hook for [hid_pt_devices] and [controller_app_modes]: return 1 on
 * handled entry. */
int hid_pt_prefs_ini_handler(const char *section, const char *name, const char *value);

#endif

#endif /* HID_PT_DEVICE_PREFS_H */
