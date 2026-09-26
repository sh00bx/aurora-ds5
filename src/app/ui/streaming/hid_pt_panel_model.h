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

typedef struct session_t session_t;

/* Match logical_device_t::key and ::name; hid_pt_panel_model.c asserts both
 * statically, so a CTM-side change breaks the build rather than truncating. */
#define HID_PT_PANEL_KEY_LEN 96
#define HID_PT_PANEL_NAME_LEN 256

/** One device as the list needs to draw it. */
typedef struct {
    char key[HID_PT_PANEL_KEY_LEN];
    /* Name, plus the Flydigi mode suffix and the " [A]" auto-plug marker. */
    char label[128];
    bool plugged;
    /* An SDL pad of the session is this row. Always true for an SDL-only row. */
    bool has_sdl_pad;
    /* The type this row's pad is (or will be) announced with over SDL. */
    gamepad_type_pref_t sdl_type;
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
    bool auto_plugin;
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
 * row's key, plugged state, auto-plug flag, SDL presence and SDL type. The
 * panel re-renders when it changes.
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
 * A controller whose SDL type means something: an SDL pad is this row, or the
 * device is a bridgeable pad kind (it runs over SDL whenever it is not mounted).
 */
bool hid_pt_model_selected_is_gamepad(const hid_pt_model_t *model);

/**
 * The family SDL detects for the selection's pad, translated ("PlayStation",
 * "Xbox", "Nintendo", "Generic"), or NULL when no SDL pad is this row.
 */
const char *hid_pt_model_selected_detected_family(const hid_pt_model_t *model);

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

/** Write the auto-plug flag and persist it to the pref store. */
bool hid_pt_model_set_auto_plugin(const hid_pt_model_t *model, bool on);

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
