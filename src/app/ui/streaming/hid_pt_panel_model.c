#if defined(TARGET_WEBOS)

#include "hid_pt_panel_model.h"

#include "ctm/ctm_state.h"
#include "ctm/ctm_settings.h"
#include "hid_passthrough/hid_passthrough_manager.h"
#include "hid_passthrough/hid_pt_device_prefs.h"
#include "hid_passthrough/hid_pt_gamepad_match.h"
#include "hid_passthrough/hid_pt_lightbar.h"
#include "input/input_gamepad.h"
#include "stream/session.h"
#include "stream/input/session_input.h"

#include "logging.h"
#include "util/i18n.h"

#include <SDL.h>

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(((logical_device_t *) 0)->key) == HID_PT_PANEL_KEY_LEN,
               "hid_pt_row_info_t::key must hold a whole logical_device_t key");
_Static_assert(sizeof(((logical_device_t *) 0)->name) == HID_PT_PANEL_NAME_LEN,
               "HID_PT_PANEL_NAME_LEN must hold a whole logical_device_t name");
_Static_assert((unsigned) TV_BRIDGE_AUDIO_AUTO == HID_PT_AUDIO_MODE_AUTO,
               "the panel's audio-mode indices are the bridge's enum values");

/* Keys of the SDL-only rows. No CTM key has this form (they are "hid:",
 * "usb:", "input:", "flydigi:" and "steam:"), so logical_device_by_key() never
 * resolves one, and every CTM-only operation below is a no-op on such a row. */
#define SDL_ROW_KEY_PREFIX "sdl:"

/* As many rows as the CTM table has devices: the view draws no more than that
 * either (HID_PT_MAX_ROWS), so a pad past it could not be shown anyway. */
#define HID_PT_MAX_LIST_ROWS MAX_DEVICES

static logical_device_t *selected_item(const hid_pt_model_t *model)
{
    return model ? logical_device_by_key(model->selected_key) : NULL;
}

/* ---- rows ----------------------------------------------------------------
 *
 * What one row of the list stands for: a CTM device, an SDL pad, or both. The
 * list is re-derived on every question rather than cached: g_devices is rebuilt
 * on the manager's poll and SDL pads come and go on hotplug, both on their own
 * cadence, and a 64-entry walk is nothing next to a render.
 */
typedef struct {
    logical_device_t *item;     /* NULL for an SDL-only row */
    app_gamepad_state_t *pad;   /* NULL when no SDL pad of the session is this row */
} row_ref_t;

static app_input_t *model_app_input(const hid_pt_model_t *model)
{
    stream_input_t *input = (model && model->session) ? session_get_input(model->session) : NULL;
    return input ? input->input : NULL;
}

/* Whether the CTM devices are listed at all. Only while this session runs HID
 * passthrough: otherwise nothing can be mounted, and g_devices is whatever an
 * earlier stream in this process left behind. The page then lists the SDL pads
 * alone, for their SDL type. Without a session the devices are all there is. */
static bool model_lists_devices(const hid_pt_model_t *model)
{
    if (!model || !model->session) {
        return true;
    }
    return hid_passthrough_manager_active(session_get_hid_passthrough(model->session));
}

static void sdl_row_key(const app_gamepad_state_t *pad, char *out, size_t len)
{
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_gamepad(pad, id, sizeof(id));
    snprintf(out, len, SDL_ROW_KEY_PREFIX "%s", id);
}

static void row_key(const row_ref_t *ref, char *out, size_t len)
{
    if (ref->item) {
        snprintf(out, len, "%s", ref->item->key);
    } else {
        sdl_row_key(ref->pad, out, len);
    }
}

/**
 * Every CTM device, then every SDL pad no CTM device answers for.
 *
 * A pad is paired with the device hid_pt_gamepad_panel_peer() names. That
 * pairing is one pad per device and is the same one the arrival reads the type
 * through, so a second pad that names a device already paired gets a row of its
 * own -- and its own type -- rather than vanishing or showing the first pad's:
 * two same-model pads next to one listed device must both stay reachable.
 * SDL-only rows are one per stable id -- two pads with the same (synthetic) id
 * share one pref, so one row edits both.
 */
static int collect_rows(const hid_pt_model_t *model, row_ref_t *out, int max)
{
    int n = 0;
    for (int i = 0; model_lists_devices(model) && i < g_devices.count && n < max; ++i) {
        out[n].item = &g_devices.items[i];
        out[n].pad = NULL;
        n++;
    }
    const int device_rows = n;
    app_input_t *input = model_app_input(model);
    if (!input) {
        return n;
    }
    for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
        if (!gp || !gp->controller) {
            continue;
        }
        const logical_device_t *peer = hid_pt_gamepad_panel_peer(input, gp);
        if (peer) {
            const int r = (int) (peer - g_devices.items);
            if (r >= 0 && r < device_rows && !out[r].pad) {
                out[r].pad = gp;
                continue;
            }
        }
        if (n >= max) {
            continue;
        }
        char key[HID_PT_PANEL_KEY_LEN];
        sdl_row_key(gp, key, sizeof(key));
        bool dup = false;
        for (int r = device_rows; r < n && !dup; ++r) {
            char other[HID_PT_PANEL_KEY_LEN];
            sdl_row_key(out[r].pad, other, sizeof(other));
            dup = strcmp(key, other) == 0;
        }
        if (dup) {
            continue;
        }
        out[n].item = NULL;
        out[n].pad = gp;
        n++;
    }
    return n;
}

static bool row_for_key(const hid_pt_model_t *model, const char *key, row_ref_t *out)
{
    if (!key || !key[0]) {
        return false;
    }
    row_ref_t rows[HID_PT_MAX_LIST_ROWS];
    const int count = collect_rows(model, rows, HID_PT_MAX_LIST_ROWS);
    for (int i = 0; i < count; ++i) {
        char k[HID_PT_PANEL_KEY_LEN];
        row_key(&rows[i], k, sizeof(k));
        if (strcmp(k, key) == 0) {
            *out = rows[i];
            return true;
        }
    }
    return false;
}

static bool selected_row(const hid_pt_model_t *model, row_ref_t *out)
{
    return model && row_for_key(model, model->selected_key, out);
}

/* The type the row's pad is announced with (hid_pt_gamepad_sdl_type(), which
 * is what the arrival reads), or -- no pad right now -- what the CTM device's
 * own id stores, which is what the next arrival reads first. Either way under
 * the current game's lock, if it has one. */
static gamepad_type_pref_t row_sdl_type(const hid_pt_model_t *model, const row_ref_t *ref)
{
    if (ref->pad) {
        return hid_pt_gamepad_sdl_type(model_app_input(model), ref->pad);
    }
    return hid_pt_prefs_effective_sdl_type(ref->item ? hid_pt_prefs_sdl_type_for_logical(ref->item)
                                                     : GAMEPAD_TYPE_PREF_AUTO);
}

/* Not the plain "hid" fallback kind: a pad the bridge can mount. */
static bool item_is_bridgeable(const logical_device_t *item)
{
    const char *kind = item ? bridge_kind_for_item(item) : NULL;
    return kind && strcmp(kind, "hid") != 0;
}

/* Whether the host builds the row's pad as a DualShock 4 when the type is left
 * on AUTO. For a pad of the session, the host's own rule on what the arrival
 * path reports (a PlayStation pad, or a Nintendo one with motion); else whether
 * CTM classified the device as a PlayStation pad. */
static bool row_detected_playstation(const row_ref_t *ref)
{
    if (ref->pad) {
        return stream_input_gamepad_auto_builds_ds4(ref->pad->controller);
    }
    const char *kind = ref->item ? bridge_kind_for_item(ref->item) : NULL;
    return kind && (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds4") == 0);
}

static void fill_row_info(const hid_pt_model_t *model, const row_ref_t *ref, hid_pt_row_info_t *out)
{
    row_key(ref, out->key, sizeof(out->key));
    out->has_sdl_pad = ref->pad != NULL;
    out->is_gamepad = ref->pad != NULL || item_is_bridgeable(ref->item);
    out->effective_type = gamepad_type_pref_effective(row_sdl_type(model, ref), row_detected_playstation(ref));
    if (!ref->item) {
        const char *name = SDL_GameControllerName(ref->pad->controller);
        snprintf(out->label, sizeof(out->label), "%s", name ? name : locstr("Controller"));
        out->plugged = false;
        return;
    }
    const logical_device_t *item = ref->item;
    out->plugged = item->plugged;
    snprintf(out->label, sizeof(out->label), "%s", item->name);
    if (is_flydigi_logical_device(item)) {
        snprintf(out->label, sizeof(out->label), "%s (%s)", item->name,
                 flydigi_is_xinput_evdev_only(item) ? "XInput" :
                 flydigi_is_xinput_mode(item) ? "XInput" : "D-Input");
    }
}

static const char *selected_kind(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    return item ? bridge_kind_for_item(item) : NULL;
}

/* ---- selection ---------------------------------------------------------- */

void hid_pt_model_set_selected_key(hid_pt_model_t *model, const char *key)
{
    if (!model) {
        return;
    }
    snprintf(model->selected_key, sizeof(model->selected_key), "%s", key ? key : "");
}

const char *hid_pt_model_selected_key(const hid_pt_model_t *model)
{
    return model ? model->selected_key : "";
}

void hid_pt_model_resolve_selection(hid_pt_model_t *model)
{
    if (!model) {
        return;
    }
    row_ref_t rows[HID_PT_MAX_LIST_ROWS];
    const int count = collect_rows(model, rows, HID_PT_MAX_LIST_ROWS);
    for (int i = 0; i < count; ++i) {
        char k[HID_PT_PANEL_KEY_LEN];
        row_key(&rows[i], k, sizeof(k));
        if (strcmp(k, model->selected_key) == 0) {
            return;
        }
    }
    if (count > 0) {
        char first[HID_PT_PANEL_KEY_LEN];
        row_key(&rows[0], first, sizeof(first));
        hid_pt_model_set_selected_key(model, first);
    }
}

/* ---- the device list ---------------------------------------------------- */

int hid_pt_model_row_count(const hid_pt_model_t *model)
{
    row_ref_t rows[HID_PT_MAX_LIST_ROWS];
    return collect_rows(model, rows, HID_PT_MAX_LIST_ROWS);
}

bool hid_pt_model_row_info(const hid_pt_model_t *model, int index, hid_pt_row_info_t *out)
{
    row_ref_t rows[HID_PT_MAX_LIST_ROWS];
    const int count = collect_rows(model, rows, HID_PT_MAX_LIST_ROWS);
    if (!out || index < 0 || index >= count) {
        return false;
    }
    fill_row_info(model, &rows[index], out);
    return true;
}

uint64_t hid_pt_model_signature(const hid_pt_model_t *model)
{
    uint64_t sig = 1469598103934665603ULL;
#define SIG_MIX(p, n) do {                                              \
        const unsigned char *_b = (const unsigned char *) (p);           \
        for (size_t _i = 0; _i < (size_t) (n); ++_i) {                  \
            sig ^= _b[_i];                                              \
            sig *= 1099511628211ULL;                                    \
        }                                                               \
    } while (0)
    row_ref_t rows[HID_PT_MAX_LIST_ROWS];
    const int count = collect_rows(model, rows, HID_PT_MAX_LIST_ROWS);
    SIG_MIX(&count, sizeof(count));
    for (int i = 0; i < count; ++i) {
        /* Everything the row draws: its key, and the state line built from
         * plugged, SDL presence and the type the host builds -- a type change
         * has to repaint "X360" as "DS4" without waiting for an unrelated
         * device event. */
        hid_pt_row_info_t info;
        fill_row_info(model, &rows[i], &info);
        unsigned char st = (unsigned char) (info.plugged ? 1 : 0);
        unsigned char pad = (unsigned char) ((info.has_sdl_pad ? 1 : 0) | (info.is_gamepad ? 2 : 0));
        unsigned char type = (unsigned char) info.effective_type;
        /* The label is mixed whole: a Flydigi's mode suffix is part of it. */
        SIG_MIX(info.key, strlen(info.key));
        SIG_MIX(info.label, strlen(info.label));
        SIG_MIX(&st, 1);
        SIG_MIX(&pad, 1);
        SIG_MIX(&type, 1);
    }
#undef SIG_MIX
    return sig;
}

/* ---- status line -------------------------------------------------------- */

void hid_pt_model_status_text(const hid_pt_model_t *model, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    if (!model_lists_devices(model)) {
        const int rows = hid_pt_model_row_count(model);
        snprintf(buf, len, "%d controller%s | HID passthrough off", rows, rows == 1 ? "" : "s");
        return;
    }
    snprintf(buf, len, "%d device%s | Windows %s",
             g_devices.count,
             g_devices.count == 1 ? "" : "s",
             ctm_agent_reachable() ? g_agent_host : "not found");
}

const char *hid_pt_model_plug_error(void)
{
    return ctm_last_plug_error();
}

bool hid_pt_model_battery_text(const hid_pt_model_t *model, char *buf, size_t len)
{
    if (!buf || len == 0 || !hid_pt_model_selected_has_battery(model)) {
        return false;
    }
    /* Non-NULL: hid_pt_model_selected_has_battery() just resolved the same key. */
    const logical_device_t *item = selected_item(model);
    int session_index = session_index_for_key(item->key);
    ctm_controller_status_t st;
    memset(&st, 0, sizeof(st));
    if (session_index >= 0 && g_sessions[session_index].controller) {
        ctm_controller_get_status(g_sessions[session_index].controller, &st);
    }
    if (st.battery_valid) {
        int pct = (int) (st.battery_level * 10);
        if (st.battery_status == 2) {
            pct = 100;
        }
        const char *stat = st.battery_status == 2 ? locstr(" (full)") :
                           st.battery_status == 1 ? locstr(" (charging)") : "";
        snprintf(buf, len, locstr("Battery: %d%%%s"), pct, stat);
    } else {
        snprintf(buf, len, "%s", locstr("Battery: --"));
    }
    return true;
}

/* ---- what the selection is --------------------------------------------- */

bool hid_pt_model_selected_is_ds5(const hid_pt_model_t *model)
{
    const char *kind = selected_kind(model);
    return kind && strcmp(kind, "ds5") == 0;
}

/* Deliberately wider than is_ds5(): both Sony pads report a battery through
 * their controller's on_input_report hook, while the haptics row above stays
 * DS5-only because the DS4 has motors, not coils. Gating the battery line on
 * is_ds5() is what left a bridged DS4 with no charge anywhere in the UI. */
bool hid_pt_model_selected_has_battery(const hid_pt_model_t *model)
{
    const char *kind = selected_kind(model);
    return kind && (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds4") == 0);
}

bool hid_pt_model_selected_has_audio(const hid_pt_model_t *model)
{
    const char *kind = selected_kind(model);
    return kind && (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds4") == 0);
}

bool hid_pt_model_selected_is_plugged(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    return item && item->plugged;
}

bool hid_pt_model_selected_is_bridgeable(const hid_pt_model_t *model)
{
    return item_is_bridgeable(selected_item(model));
}

bool hid_pt_model_selected_is_flydigi(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    if (!item) {
        return false;
    }
    return is_flydigi_logical_device(item) ||
           (item->usb_busid[0] && is_flydigi_usb_busid(item->usb_busid)) ||
           (strcmp(item->vid, "04b4") == 0 && strcmp(item->pid, "2412") == 0) ||
           contains_ci(item->name, "flydigi") || contains_ci(item->name, "vader") ||
           contains_ci(item->name, "apex");
}

bool hid_pt_model_selected_name(const hid_pt_model_t *model, char *buf, size_t len)
{
    row_ref_t ref;
    if (!buf || len == 0 || !selected_row(model, &ref)) {
        return false;
    }
    if (ref.item) {
        snprintf(buf, len, "%s", ref.item->name);
    } else {
        const char *name = SDL_GameControllerName(ref.pad->controller);
        snprintf(buf, len, "%s", name ? name : locstr("Controller"));
    }
    return true;
}

bool hid_pt_model_selected_row_info(const hid_pt_model_t *model, hid_pt_row_info_t *out)
{
    row_ref_t ref;
    if (!out || !selected_row(model, &ref)) {
        return false;
    }
    fill_row_info(model, &ref, out);
    return true;
}

bool hid_pt_model_selected_is_sdl_only(const hid_pt_model_t *model)
{
    row_ref_t ref;
    return selected_row(model, &ref) && !ref.item;
}

bool hid_pt_model_selected_has_lightbar(const hid_pt_model_t *model)
{
    row_ref_t ref;
    if (!selected_row(model, &ref)) {
        return false;
    }
    const char *kind = ref.item ? bridge_kind_for_item(ref.item) : NULL;
    if (kind && (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds4") == 0)) {
        return true;
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    return ref.pad && SDL_GameControllerHasLED(ref.pad->controller);
#else
    return false;
#endif
}

bool hid_pt_model_selected_lightbar(const hid_pt_model_t *model, lightbar_pref_t *out)
{
    row_ref_t ref;
    if (!out || !selected_row(model, &ref)) {
        return false;
    }
    *out = ref.pad ? hid_pt_gamepad_lightbar(model_app_input(model), ref.pad)
                   : ref.item ? hid_pt_prefs_lightbar_for_logical(ref.item) : lightbar_pref_automatic();
    return true;
}

int hid_pt_model_default_latency_ms(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    return item ? (int) default_settings_for_item(item).latency_ms : 60;
}

/* ---- settings ----------------------------------------------------------- */

/* The DS4 has no speaker-only route: "Controller speaker" and "Speaker + jack"
 * would both be the 0xDF split route (probed bitmask, see controller_ds4.c
 * ds4_route_for_mode), so the two dropdown items would be indistinguishable —
 * and "Controller speaker" would not keep a plugged headset quiet. Collapsing
 * SPEAKER onto BOTH at this boundary makes the dropdown settle on the label
 * that says what actually happens. The DS5 keeps the distinction: its 0x93 is
 * a real speaker-only route. */
static unsigned collapse_ds4_audio_mode(const hid_pt_model_t *model, unsigned mode)
{
    const char *kind = selected_kind(model);
    if (mode == (unsigned) TV_BRIDGE_AUDIO_SPEAKER && kind && strcmp(kind, "ds4") == 0) {
        return (unsigned) TV_BRIDGE_AUDIO_BOTH;
    }
    return mode;
}

bool hid_pt_model_read_controls(const hid_pt_model_t *model, hid_pt_controls_t *out)
{
    const logical_device_t *item = selected_item(model);
    const tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!out || !settings) {
        return false;
    }
    out->latency_ms = settings->latency_ms;
    out->audio_mode = collapse_ds4_audio_mode(model, (unsigned) settings->audio_mode);
    out->speaker_volume_percent = settings->speaker_volume_percent;
    out->headset_volume_percent = settings->headset_volume_percent;
    out->haptics_gain_centi = settings->haptics_gain_centi;
    out->trigger_reduce = settings->ds5_trigger_reduce;
    out->composite_passthrough = settings->composite_passthrough;
    return true;
}

bool hid_pt_model_write_controls(const hid_pt_model_t *model, const hid_pt_controls_t *in)
{
    const logical_device_t *item = selected_item(model);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!in || !item || !settings) {
        return false;
    }
    settings->latency_ms = in->latency_ms;
    settings->audio_mode = (tv_bridge_audio_mode_t) collapse_ds4_audio_mode(model, in->audio_mode);
    settings->speaker_volume_percent = in->speaker_volume_percent;
    settings->headset_volume_percent = in->headset_volume_percent;
    if (hid_pt_model_selected_is_ds5(model)) {
        settings->haptics_gain_centi = in->haptics_gain_centi;
        settings->ds5_trigger_reduce = in->trigger_reduce;
    }
    apply_settings_to_session(item);
    return true;
}

bool hid_pt_model_set_composite(const hid_pt_model_t *model, bool on)
{
    const logical_device_t *item = selected_item(model);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) {
        return false;
    }
    settings->composite_passthrough = on;
    apply_settings_to_session(item);
    return true;
}

/* The ids a controller's own choices are written under, in the order
 * hid_pt_gamepad_sdl_type() and hid_pt_gamepad_lightbar() read them: the
 * listed device's (for an SDL-only row, the device its pad pairs with), then
 * the pad's own -- except a synthetic per-model id next to a device, which is
 * shared by every serial-less pad of that model, and one equal to the
 * device's. Either may come back empty. */
static const logical_device_t *choice_ids(const hid_pt_model_t *model, const row_ref_t *ref,
                                          char device_id[HID_PT_STABLE_ID_LEN], char pad_id[HID_PT_STABLE_ID_LEN])
{
    const logical_device_t *device = ref->item;
    if (!device && ref->pad) {
        device = hid_pt_gamepad_panel_peer(model_app_input(model), ref->pad);
    }
    device_id[0] = '\0';
    pad_id[0] = '\0';
    if (device) {
        hid_pt_stable_id_for_logical(device, device_id, HID_PT_STABLE_ID_LEN);
    }
    if (ref->pad) {
        hid_pt_stable_id_for_gamepad(ref->pad, pad_id, HID_PT_STABLE_ID_LEN);
        if ((device && hid_pt_stable_id_is_synthetic(pad_id)) || strcmp(pad_id, device_id) == 0) {
            pad_id[0] = '\0';
        }
    }
    return device;
}

static const char *sdl_type_log_name(gamepad_type_pref_t type)
{
    switch (type) {
        case GAMEPAD_TYPE_PREF_XBOX:
            return "xbox";
        case GAMEPAD_TYPE_PREF_PLAYSTATION:
            return "playstation";
        case GAMEPAD_TYPE_PREF_AUTO:
        default:
            return "auto";
    }
}

/* Bounds the before/after snapshot below; gs_ids are 0..15 anyway. */
#define HID_PT_MAX_PADS 16

bool hid_pt_model_set_sdl_type(const hid_pt_model_t *model, gamepad_type_pref_t type)
{
    row_ref_t ref;
    if (!selected_row(model, &ref)) {
        return false;
    }
    app_input_t *app_input = model_app_input(model);
    short pad_count = app_input ? app_input_get_max_gamepads(app_input) : 0;
    if (pad_count > HID_PT_MAX_PADS) {
        pad_count = HID_PT_MAX_PADS;
    }
    /* What every pad is announced with now, to re-announce exactly the ones
     * whose type this write changes -- normally just the row's pad, but pads
     * that share a synthetic id share its pref too. */
    gamepad_type_pref_t before[HID_PT_MAX_PADS];
    for (short i = 0; i < pad_count; ++i) {
        const app_gamepad_state_t *gp = app_input_gamepad_state_by_index(app_input, i);
        before[i] = (gp && gp->controller) ? hid_pt_gamepad_sdl_type(app_input, gp) : GAMEPAD_TYPE_PREF_AUTO;
    }

    /* The ids hid_pt_gamepad_sdl_type() reads, in its order (choice_ids()).
     * The listed device's first, "Automatic" kept as a choice of its own: that
     * id is read before the pad's, and while the controller is mounted it is
     * the only one this row has, so it must be able to outvote an older type
     * under the pad's SDL serial (a USB DualShock's serial is not its hidraw
     * id). Then the pad's own, kept in step so the choice also holds where the
     * pad pairs with no device; last in line, so "Automatic" there just
     * erases. */
    char device_id[HID_PT_STABLE_ID_LEN];
    char pad_id[HID_PT_STABLE_ID_LEN];
    choice_ids(model, &ref, device_id, pad_id);

    char name[HID_PT_PANEL_NAME_LEN];
    if (!hid_pt_model_selected_name(model, name, sizeof(name))) {
        name[0] = '\0';
    }
    bool stored = device_id[0] || pad_id[0];
    if (device_id[0]) {
        stored = hid_pt_prefs_set_sdl_type(device_id, type, true) && stored;
    }
    if (pad_id[0]) {
        stored = hid_pt_prefs_set_sdl_type(pad_id, type, false) && stored;
    }
    if (stored) {
        commons_log_info("HID-PT", "SDL controller type for %s set to %s", name, sdl_type_log_name(type));
    } else {
        /* Same place and wording pattern as a failed auto-plug save: the
         * mode row lights what the store holds, so without this line the
         * press would just silently not take. */
        ctm_set_plug_error("SDL controller type for %s could not be saved", name);
    }

    /* Even after a partial failure: whatever did change must reach the host,
     * or the badge would name a type the host was not given.
     * stream_input_reannounce_gamepad() itself leaves bridged, unannounced and
     * view-only pads alone. */
    stream_input_t *input = model->session ? session_get_input(model->session) : NULL;
    for (short i = 0; input && i < pad_count; ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(app_input, i);
        if (gp && gp->controller && hid_pt_gamepad_sdl_type(app_input, gp) != before[i]) {
            stream_input_reannounce_gamepad(input, gp);
        }
    }
    return stored;
}

bool hid_pt_model_set_lightbar(const hid_pt_model_t *model, const lightbar_pref_t *lb)
{
    row_ref_t ref;
    if (!lb || !selected_row(model, &ref)) {
        return false;
    }
    char device_id[HID_PT_STABLE_ID_LEN];
    char pad_id[HID_PT_STABLE_ID_LEN];
    const logical_device_t *device = choice_ids(model, &ref, device_id, pad_id);
    bool stored = device_id[0] || pad_id[0];
    if (device_id[0]) {
        stored = hid_pt_prefs_set_lightbar(device_id, lb, true) && stored;
    }
    if (pad_id[0]) {
        stored = hid_pt_prefs_set_lightbar(pad_id, lb, false) && stored;
    }
    char name[HID_PT_PANEL_NAME_LEN];
    if (!hid_pt_model_selected_name(model, name, sizeof(name))) {
        name[0] = '\0';
    }
    if (stored) {
        if (lb->automatic) {
            commons_log_info("HID-PT", "lightbar for %s set to Automatic", name);
        } else {
            commons_log_info("HID-PT", "lightbar for %s set to %06x, game may change it: %s", name,
                             (unsigned) lb->rgb, lb->game ? "yes" : "no");
        }
    } else {
        ctm_set_plug_error("Lightbar colour for %s could not be saved", name);
    }
    /* Mounted: the record is what the bridge paints from, and setting it
     * pushes a report at once. Not mounted, the record is what the next mount
     * starts with. */
    tv_bridge_worker_settings_t *settings = device ? settings_for_item(device) : NULL;
    if (settings) {
        settings->lightbar_user = !lb->automatic;
        settings->lightbar_rgb = lb->automatic ? 0 : lb->rgb;
        settings->lightbar_game = lb->automatic || lb->game;
        apply_settings_to_session(device);
    }
    /* Over SDL: every pad, since pads sharing an id share the choice; a
     * bridged slot is left to the bridge. */
    stream_input_t *input = model->session ? session_get_input(model->session) : NULL;
    hid_pt_lightbar_refresh(model_app_input(model), input ? input->moonlightExcludedMask : 0);
    return stored;
}

bool hid_pt_model_persist_mode(const hid_pt_model_t *model, bool hid, gamepad_type_pref_t type)
{
    row_ref_t ref;
    if (!selected_row(model, &ref) || (hid && !ref.item)) {
        return false;
    }
    if (!hid && !hid_pt_model_set_sdl_type(model, type)) {
        return false;
    }
    bool stored = true;
    if (ref.item) {
        tv_bridge_worker_settings_t *settings = settings_for_item(ref.item);
        if (!settings) {
            return false;
        }
        /* The record first, then the store, exactly as the auto-plug switch
         * did: the record is what the reconcile reads, the store what the next
         * launch reads, and the sync reports a refused write in the error line
         * itself. */
        settings->auto_plugin = hid;
        hid_pt_sync_auto_plugin_pref(ref.item);
        stored = hid_pt_prefs_auto_plugin_for_logical(ref.item) == hid;
    }
    if (!hid && ref.pad) {
        /* A pad whose serial is not the device's id (a USB DualShock) may still
         * carry an auto-plug flag under that serial from an older build, and
         * hid_pt_gamepad_is_autoplug() reads it first: it would keep taking the
         * pad off SDL at every stream start. Clearing needs no slot. */
        char pad_id[HID_PT_STABLE_ID_LEN];
        hid_pt_stable_id_for_gamepad(ref.pad, pad_id, sizeof(pad_id));
        hid_pt_prefs_set_auto_plugin(pad_id, false);
    }
    return stored;
}

/* ---- the game's fixed mode ---------------------------------------------- */

bool hid_pt_model_app_name(const hid_pt_model_t *model, char *buf, size_t len)
{
    return model && model->session && hid_pt_prefs_current_app_name(buf, len);
}

gamepad_mode_t hid_pt_model_app_mode(const hid_pt_model_t *model)
{
    return hid_pt_model_app_name(model, NULL, 0) ? hid_pt_prefs_current_app_mode() : GAMEPAD_MODE_NONE;
}

/* Mount or unmount every listed controller whose effective mode now says
 * otherwise. Nothing but the page's own plug toggle, so a lock moves a pad
 * exactly as its row's OK would; the reconcile then keeps it there, since it
 * reads the same effective mode. */
static void apply_effective_mounts(hid_pt_model_t *model)
{
    if (!model_lists_devices(model)) {
        return;
    }
    char keys[MAX_DEVICES][HID_PT_PANEL_KEY_LEN];
    int count = 0;
    for (int i = 0; i < g_devices.count && count < MAX_DEVICES; ++i) {
        logical_device_t *item = &g_devices.items[i];
        const tv_bridge_worker_settings_t *settings = settings_for_item(item);
        if (!item_is_bridgeable(item) || !settings ||
            hid_pt_prefs_effective_hid(settings->auto_plugin) == item->plugged) {
            continue;
        }
        /* Keys, not pointers: a plug-out can reap the session behind one. */
        snprintf(keys[count++], sizeof(keys[0]), "%s", item->key);
    }
    char selected[HID_PT_PANEL_KEY_LEN];
    snprintf(selected, sizeof(selected), "%s", model->selected_key);
    for (int i = 0; i < count; ++i) {
        hid_pt_model_toggle_plug(model, keys[i], model->session, NULL);
    }
    /* The toggle selects what it plugged; the page stays on its controller. */
    hid_pt_model_set_selected_key(model, selected);
}

bool hid_pt_model_set_app_mode(hid_pt_model_t *model, gamepad_mode_t mode)
{
    char name[128];
    if (!hid_pt_model_app_name(model, name, sizeof(name))) {
        return false;
    }
    stream_input_t *input = session_get_input(model->session);
    app_input_t *app_input = model_app_input(model);
    short pad_count = app_input ? app_input_get_max_gamepads(app_input) : 0;
    if (pad_count > HID_PT_MAX_PADS) {
        pad_count = HID_PT_MAX_PADS;
    }
    /* Which pads the host has over SDL now, and as what: only those can need a
     * re-announce. A pad the plug-out below hands back to SDL is announced by
     * its slot restore with the new type already. */
    gamepad_type_pref_t before[HID_PT_MAX_PADS];
    bool on_sdl[HID_PT_MAX_PADS];
    for (short i = 0; i < pad_count; ++i) {
        const app_gamepad_state_t *gp = app_input_gamepad_state_by_index(app_input, i);
        const bool live = gp && gp->controller && gp->gs_id >= 0;
        before[i] = live ? hid_pt_gamepad_sdl_type(app_input, gp) : GAMEPAD_TYPE_PREF_AUTO;
        on_sdl[i] = live && input && !hid_pt_gamepad_is_moonlight_excluded(input, gp) &&
                    (input->announcedGamepadMask & (1u << (unsigned) gp->gs_id)) != 0;
    }
    if (!hid_pt_prefs_set_current_app_mode(mode)) {
        ctm_set_plug_error("Controller mode for %s could not be saved", name);
        return false;
    }
    apply_effective_mounts(model);
    for (short i = 0; input && i < pad_count; ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(app_input, i);
        if (on_sdl[i] && gp && gp->controller && hid_pt_gamepad_sdl_type(app_input, gp) != before[i]) {
            stream_input_reannounce_gamepad(input, gp);
        }
    }
    return true;
}

bool hid_pt_model_reset_selected(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) {
        return false;
    }
    *settings = default_settings_for_item(item);
    return true;
}

void hid_pt_model_commit_selected(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    if (!item) {
        return;
    }
    apply_settings_to_session(item);
    hid_pt_sync_auto_plugin_pref(item);
}

/* ---- plugging ----------------------------------------------------------- */

hid_pt_plug_result_t hid_pt_model_toggle_plug(hid_pt_model_t *model, const char *key,
                                              session_t *session, bool *out_plugged)
{
    /* Resolve the key the row is LABELLED with. The bare index the plug button
     * used to carry addressed g_devices, which the manager's poll rebuilds in
     * readdir order on a different cadence than the panel re-renders on, so a
     * press could bridge the pad in the next row instead. NULL means the device
     * left between the last render and the press: do nothing, and say so. */
    logical_device_t *item = key ? logical_device_by_key(key) : NULL;
    if (!item) {
        ctm_set_plug_error("%s is no longer connected", key ? key : "");
        return HID_PT_PLUG_GONE;
    }
    bool requested_state = !item->plugged;
    if (requested_state) {
        ctm_clear_plug_error();
        if (!plug_in_item(item)) {
            return HID_PT_PLUG_FAILED;
        }
        if (session) {
            stream_input_t *input = session_get_input(session);
            if (input) {
                hid_pt_moonlight_exclude(input, item);
                /* Keep the resolved slot in the session record too: it is the only
                 * copy that survives the device disappearing from g_devices, and
                 * the reconcile's reap path needs it to give the slot back. */
                int session_index = session_index_for_key(item->key);
                if (session_index >= 0) {
                    g_sessions[session_index].moonlight_gs_id = item->moonlight_gs_id;
                }
            }
        }
    } else {
        /* Third teardown path, alongside the reconcile's vanish-reap and
         * zombie-session branches, and it needs the same care. Take the slot out
         * of the session record before stop_session destroys it: matching the pad
         * again afterwards only works while it is still enumerated in SDL, and a
         * bridged pad usually is not — the exclusion bit would then stay set for
         * the rest of the stream and kill whichever controller next inherits that
         * gs_id. Clear item->plugged before the restore, or the owner guard
         * refuses to release the slot this very device still claims. */
        int session_index = session_index_for_key(item->key);
        int gs_id = (session_index >= 0) ? g_sessions[session_index].moonlight_gs_id : -1;
        stop_session(item->key);
        ctm_clear_plug_error();
        item->plugged = false;
        if (session) {
            stream_input_t *input = session_get_input(session);
            if (input) {
                hid_pt_moonlight_restore_slot(input, gs_id);
            }
        }
    }

    item->plugged = requested_state;
    set_plug_key(item->key, item->plugged);
    /* The user took manual control of this device: stop auto-managing it (so a
     * deliberate plug-out is not re-plugged by the reconcile poll) until it
     * physically reconnects. */
    autoplug_mark_done(item->key);
    hid_pt_model_set_selected_key(model, item->key);
    if (out_plugged) {
        *out_plugged = item->plugged;
    }
    return HID_PT_PLUG_DONE;
}

#endif
