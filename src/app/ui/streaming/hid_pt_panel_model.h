#pragma once

/**
 * Selection state for the Controllers panel, and the panel's whole view of the
 * CTM bridge and of the session's SDL pads.
 *
 * This header deliberately exposes no CTM or SDL type: the panel's other two
 * translation units (the widget layer in hid_pt_panel_view.c and the wiring in
 * hid_passthrough_panel.c) get devices as plain values -- a key, a label, a
 * plugged flag, a block of control values -- and can therefore not reach
 * g_devices, g_sessions, g_settings, g_agent_host or an SDL pad at all. Of the
 * panel's three translation units, only this module's includes ctm_state.h,
 * so those names are not even declared in the other two.
 *
 * The list is every CTM device, then every SDL pad of the session that no CTM
 * device answers for (the agent is not running, or CTM does not list the pad),
 * so a controller's SDL type can always be changed. Such an SDL-only row is
 * keyed "sdl:<stable id>" and has no HID controls. A session that does not run
 * HID passthrough lists its SDL pads alone.
 *
 * Everything here runs on the LVGL thread. That is not a property this module
 * enforces; it is the same contract root.c states for every other CTM caller,
 * and the panel is created and destroyed from that thread.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stream/input/gamepad_type_pref.h"
#include "stream/input/lightbar_pref.h"

typedef struct session_t session_t;

/* Match logical_device_t::key and ::name; hid_pt_panel_model.c asserts both
 * statically, so a CTM-side change breaks the build rather than truncating. */
#define HID_PT_PANEL_KEY_LEN 96
#define HID_PT_PANEL_NAME_LEN 256

/** One device as the list needs to draw it. */
typedef struct {
    char key[HID_PT_PANEL_KEY_LEN];
    /* Name, plus the Flydigi mode suffix. */
    char label[128];
    bool plugged;
    /* An SDL pad of the session is this row. Always true for an SDL-only row. */
    bool has_sdl_pad;
    /* A controller: an SDL pad is this row, or the device is a pad the bridge
     * can mount. Only these have a mode; anything else is a plain HID device. */
    bool is_gamepad;
    /* The pad the host builds for this row over SDL, now or at its next
     * arrival: the stored type, AUTO resolved (gamepad_type_pref_effective()).
     * XBOX, PLAYSTATION, DUALSENSE or SWITCH, never AUTO. Meaningless when
     * !is_gamepad. */
    gamepad_type_pref_t effective_type;
} hid_pt_row_info_t;

/**
 * "Auto (game decides)" — the audio mode that suppresses the Bluetooth-audio
 * warning. hid_pt_panel_model.c asserts statically that it still equals the
 * bridge's TV_BRIDGE_AUDIO_AUTO, so this stays a definition rather than a
 * duplicated magic number.
 */
#define HID_PT_AUDIO_MODE_AUTO 0u

/** The per-device settings the option column edits, as plain numbers. */
typedef struct {
    unsigned latency_ms;
    unsigned audio_mode;            /* index into the audio dropdown */
    unsigned speaker_volume_percent;
    unsigned headset_volume_percent;
    unsigned haptics_gain_centi;
    unsigned trigger_reduce;        /* DualSense trigger power reduction, 0 = off */
    bool composite_passthrough;
} hid_pt_controls_t;

typedef enum {
    /** The key names no device in the model any more; the error text says so. */
    HID_PT_PLUG_GONE = 0,
    /** plug_in_item() refused; it left its own reason in the plug error. */
    HID_PT_PLUG_FAILED,
    HID_PT_PLUG_DONE,
} hid_pt_plug_result_t;

typedef struct {
    char selected_key[HID_PT_PANEL_KEY_LEN];
    /* Where the SDL pads come from, and where a type change is re-announced.
     * NULL lists the CTM devices only. */
    session_t *session;
    /* A colour-picker preview on a mounted controller's settings record: the
     * device, and the record's lightbar as it was, to put back at the end. */
    struct {
        bool record;
        char key[HID_PT_PANEL_KEY_LEN];
        bool user;
        unsigned rgb;
        bool game;
    } lightbar_preview;
} hid_pt_model_t;

/* ---- selection ---------------------------------------------------------- */

void hid_pt_model_set_selected_key(hid_pt_model_t *model, const char *key);
const char *hid_pt_model_selected_key(const hid_pt_model_t *model);

/**
 * Point the selection at the first row when the selected key has left the
 * list. A selection that still resolves, and an empty list, are both left
 * alone.
 */
void hid_pt_model_resolve_selection(hid_pt_model_t *model);

/* ---- the device list ---------------------------------------------------- */

/** Rows in the list: CTM devices first, then SDL-only pads. */
int hid_pt_model_row_count(const hid_pt_model_t *model);

/** Fill @p out for row @p index. False (and @p out untouched) when out of range. */
bool hid_pt_model_row_info(const hid_pt_model_t *model, int index, hid_pt_row_info_t *out);

/**
 * A hash over the part of the model the device list draws: the count, and each
 * row's key, label, plugged state, SDL presence and the type the host builds
 * for it. The panel re-renders when it changes.
 */
uint64_t hid_pt_model_signature(const hid_pt_model_t *model);

/* ---- status line -------------------------------------------------------- */

/** "N devices | Windows <host>", or -- HID passthrough off for this session,
 * so only SDL pads are listed -- "N controllers | HID passthrough off". */
void hid_pt_model_status_text(const hid_pt_model_t *model, char *buf, size_t len);

/** The last plug error, or NULL. */
const char *hid_pt_model_plug_error(void);

/**
 * The battery line for the selection, e.g. "Battery: 62% (charging)".
 * False when the selection is not a DualSense, in which case the caller hides
 * the label.
 */
bool hid_pt_model_battery_text(const hid_pt_model_t *model, char *buf, size_t len);

/* ---- what the selection is --------------------------------------------- */

bool hid_pt_model_selected_is_ds5(const hid_pt_model_t *model);

/** True for any bridged pad that reports a battery (DS5 family and DS4). */
bool hid_pt_model_selected_has_battery(const hid_pt_model_t *model);
bool hid_pt_model_selected_is_flydigi(const hid_pt_model_t *model);
/** Bridged right now, i.e. the state the device's rail and the footer report. */
bool hid_pt_model_selected_is_plugged(const hid_pt_model_t *model);
/** Bridgeable at all, i.e. not the plain "hid" fallback kind. */
bool hid_pt_model_selected_is_bridgeable(const hid_pt_model_t *model);
/** A PlayStation pad, i.e. the one that has the audio/haptics column. */
bool hid_pt_model_selected_has_audio(const hid_pt_model_t *model);

/** The selected device's name. False when nothing is selected. */
bool hid_pt_model_selected_name(const hid_pt_model_t *model, char *buf, size_t len);

/** The selected row as the list draws it. False when nothing is selected. */
bool hid_pt_model_selected_row_info(const hid_pt_model_t *model, hid_pt_row_info_t *out);

/** An SDL pad with no CTM device behind it: no HID controls at all. */
bool hid_pt_model_selected_is_sdl_only(const hid_pt_model_t *model);

/**
 * A controller whose lightbar aurora can paint: a Bluetooth DS4/DS5 (the
 * bridge paints it mounted, SDL on SDL), or an SDL pad that is not mounted and
 * that SDL can set an LED on. A DS4/DS5 mounted over a cable has none: the
 * bridge drives it with its generic type, which paints nothing.
 */
bool hid_pt_model_selected_has_lightbar(const hid_pt_model_t *model);

/**
 * The selection's lightbar colour in the mode its mode row lights -- HID while
 * mounted, else the pad the host builds for it (the game's lock included) --
 * which is the one the LIGHTBAR row shows and edits; that mode into
 * *@p mode_out (may be NULL). The colour is what its pad resolves to
 * (hid_pt_gamepad_lightbar()), or without a pad what the device's own id
 * stores. Automatic when nothing was chosen; false when nothing is selected.
 */
bool hid_pt_model_selected_lightbar(const hid_pt_model_t *model, lightbar_pref_t *out, gamepad_mode_t *mode_out);

/** default_settings_for_item()'s latency for the selection, or 60 with none. */
int hid_pt_model_default_latency_ms(const hid_pt_model_t *model);

/* ---- settings ----------------------------------------------------------- */

/**
 * Read the selection's stored settings.
 *
 * False when nothing is selected or the settings table could not hand out a
 * record; the caller then leaves its widgets as they are. Materialises the
 * record for a device that has none yet, exactly as the hand-written reads it
 * replaced did.
 */
bool hid_pt_model_read_controls(const hid_pt_model_t *model, hid_pt_controls_t *out);

/**
 * Write the four audio/latency values back and push them at a live bridge.
 * The haptics gain and the trigger reduction are only written for a DualSense. False on the same
 * conditions as hid_pt_model_read_controls().
 */
bool hid_pt_model_write_controls(const hid_pt_model_t *model, const hid_pt_controls_t *in);

/** Write the Flydigi composite flag and push it at a live bridge. */
bool hid_pt_model_set_composite(const hid_pt_model_t *model, bool on);

/**
 * Store the selection's SDL controller type and re-announce every pad whose
 * type that changes, so the host re-creates it with the new one.
 *
 * Written under the ids hid_pt_gamepad_sdl_type() reads: the CTM device's (with
 * "Automatic" kept as an explicit choice, since that id is read first) and the
 * pad's own serial, never a synthetic per-model id next to a device. False when
 * the store refused it; the reason is then in the plug error the status line
 * shows.
 */
bool hid_pt_model_set_sdl_type(const hid_pt_model_t *model, gamepad_type_pref_t type);

/**
 * Remember the selection's mode: the one it comes back in after a Bluetooth
 * reconnect, at the next stream and after an app restart. @p hid sets the
 * device's auto-plug flag, which is what "mode HID" is stored as, and keeps the
 * SDL type for when it leaves HID; otherwise the flag is cleared and @p type is
 * stored through hid_pt_model_set_sdl_type(), re-announce included.
 *
 * Mounts and unmounts nothing: the caller does that through the plug toggle.
 * False when a pref could not be written; the reason is then in the plug error
 * the status line shows.
 */
bool hid_pt_model_persist_mode(const hid_pt_model_t *model, bool hid, gamepad_type_pref_t type);

/**
 * Store the selection's lightbar colour for the mode hid_pt_model_selected_lightbar()
 * names -- @p lb's game switch for every mode of the controller -- and paint it
 * at once wherever the controller is: the bridge's settings record (pushed to a
 * mounted pad through ctm_controller_set_settings()) and every SDL pad of the
 * session (hid_pt_lightbar_refresh()). Written under the ids
 * hid_pt_model_set_sdl_type() writes, by the same rules. False when the store
 * refused it; the reason is then in the plug error, and the colour still
 * applies until the app quits.
 */
bool hid_pt_model_set_lightbar(const hid_pt_model_t *model, const lightbar_pref_t *lb);

/**
 * The colour picker's live preview: show @p rgb on the selection's bar at once
 * through the path a change takes -- the bridge's record for a mounted pad
 * (the game kept off it meanwhile), the SDL pad otherwise -- storing nothing.
 * The caller throttles it. Ends with hid_pt_model_end_lightbar_preview(): with
 * @p keep after the colour was stored (the record already holds it), without
 * for a cancel, which puts back what the bar showed.
 */
void hid_pt_model_preview_lightbar(hid_pt_model_t *model, uint32_t rgb);
void hid_pt_model_end_lightbar_preview(hid_pt_model_t *model, bool keep);

/* ---- the game's fixed mode ---------------------------------------------- */

/**
 * The game this session streams, for the lock's caption. False without a
 * session or with a name there is nothing to key a lock by -- the page offers
 * no lock then.
 */
bool hid_pt_model_app_name(const hid_pt_model_t *model, char *buf, size_t len);

/** The current game's fixed mode; GAMEPAD_MODE_NONE when it has none. */
gamepad_mode_t hid_pt_model_app_mode(const hid_pt_model_t *model);

/**
 * Fix the current game's mode to @p mode, or remove the lock with
 * GAMEPAD_MODE_NONE, and bring every controller of the session to its new
 * effective mode at once -- the lock's, or without one each controller's own:
 * mounted or unmounted through the plain plug toggle, re-announced where the
 * pad the host builds for it changes, and left alone where nothing changes. A
 * mount that fails leaves its reason in the plug error (the first one, if
 * several fail) and the others still go ahead.
 *
 * False when the lock could not be stored (no game, full table); the reason is
 * then in the plug error, and no controller was touched. The selection is kept.
 */
bool hid_pt_model_set_app_mode(hid_pt_model_t *model, gamepad_mode_t mode);

/**
 * Overwrite the selection's settings with the per-device defaults, without
 * publishing them. The caller re-reads them into its widgets and then calls
 * hid_pt_model_commit_selected(), which is the order the panel has always used.
 */
bool hid_pt_model_reset_selected(const hid_pt_model_t *model);

/** Push the selection's current settings at a live bridge and at the pref store. */
void hid_pt_model_commit_selected(const hid_pt_model_t *model);

/* ---- plugging ----------------------------------------------------------- */

/**
 * Toggle the bridge for the device named by @p key -- which is the key the
 * pressed row is labelled with, not an index, because g_devices is rebuilt on a
 * different cadence than the panel renders on.
 *
 * On HID_PT_PLUG_DONE the selection is moved to @p key and *@p out_plugged
 * holds the new state.
 */
hid_pt_plug_result_t hid_pt_model_toggle_plug(hid_pt_model_t *model, const char *key,
                                              session_t *session, bool *out_plugged);
