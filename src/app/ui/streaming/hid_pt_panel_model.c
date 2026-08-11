#if defined(TARGET_WEBOS)

#include "hid_pt_panel_model.h"

#include "ctm/ctm_state.h"
#include "ctm/ctm_settings.h"
#include "hid_passthrough/hid_pt_gamepad_match.h"
#include "stream/session.h"

#include "util/i18n.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(((logical_device_t *) 0)->key) == HID_PT_PANEL_KEY_LEN,
               "hid_pt_row_info_t::key must hold a whole logical_device_t key");
_Static_assert(sizeof(((logical_device_t *) 0)->name) == HID_PT_PANEL_NAME_LEN,
               "HID_PT_PANEL_NAME_LEN must hold a whole logical_device_t name");
_Static_assert((unsigned) TV_BRIDGE_AUDIO_AUTO == HID_PT_AUDIO_MODE_AUTO,
               "the panel's audio-mode indices are the bridge's enum values");

static logical_device_t *selected_item(const hid_pt_model_t *model)
{
    return model ? logical_device_by_key(model->selected_key) : NULL;
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
    if (!logical_device_by_key(model->selected_key) && g_devices.count > 0) {
        hid_pt_model_set_selected_key(model, g_devices.items[0].key);
    }
}

/* ---- the device list ---------------------------------------------------- */

int hid_pt_model_device_count(void)
{
    return g_devices.count;
}

bool hid_pt_model_row_info(int index, hid_pt_row_info_t *out)
{
    if (!out || index < 0 || index >= g_devices.count) {
        return false;
    }
    const logical_device_t *item = &g_devices.items[index];
    snprintf(out->key, sizeof(out->key), "%s", item->key);
    out->plugged = item->plugged;
    snprintf(out->label, sizeof(out->label), "%s", item->name);
    if (is_flydigi_logical_device(item)) {
        snprintf(out->label, sizeof(out->label), "%s (%s)", item->name,
                 flydigi_is_xinput_evdev_only(item) ? "XInput" :
                 flydigi_is_xinput_mode(item) ? "XInput" : "D-Input");
    }
    const tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (settings && settings->auto_plugin) {
        size_t len = strlen(out->label);
        snprintf(out->label + len, sizeof(out->label) - len, " [A]");
    }
    return true;
}

uint64_t hid_pt_model_signature(void)
{
    uint64_t sig = 1469598103934665603ULL;
#define SIG_MIX(p, n) do {                                              \
        const unsigned char *_b = (const unsigned char *) (p);           \
        for (size_t _i = 0; _i < (size_t) (n); ++_i) {                  \
            sig ^= _b[_i];                                              \
            sig *= 1099511628211ULL;                                    \
        }                                                               \
    } while (0)
    SIG_MIX(&g_devices.count, sizeof(g_devices.count));
    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        unsigned char st = (unsigned char) (item->plugged ? 1 : 0);
        /* The row label carries the " [A]" marker, so auto_plugin belongs in the
         * signature: without it, ticking the checkbox left the marker stale until
         * some unrelated device event happened to change the hash.
         * settings_for_item() materialises the record for a device that has none
         * yet -- hid_pt_model_row_info() does the same for every row the panel
         * draws, so this adds no entry the renderer would not have added anyway. */
        const tv_bridge_worker_settings_t *settings = settings_for_item(item);
        unsigned char ap = (unsigned char) ((settings && settings->auto_plugin) ? 1 : 0);
        SIG_MIX(item->key, strlen(item->key));
        SIG_MIX(&st, 1);
        SIG_MIX(&ap, 1);
    }
#undef SIG_MIX
    return sig;
}

/* ---- status line -------------------------------------------------------- */

void hid_pt_model_status_text(char *buf, size_t len)
{
    if (!buf || len == 0) {
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
    if (!buf || len == 0 || !hid_pt_model_selected_is_ds5(model)) {
        return false;
    }
    /* Non-NULL: hid_pt_model_selected_is_ds5() just resolved the same key. */
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

bool hid_pt_model_selected_has_audio(const hid_pt_model_t *model)
{
    const char *kind = selected_kind(model);
    return kind && (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds4") == 0);
}

bool hid_pt_model_selected_is_bridgeable(const hid_pt_model_t *model)
{
    const char *kind = selected_kind(model);
    return kind && strcmp(kind, "hid") != 0;
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
    const logical_device_t *item = selected_item(model);
    if (!item || !buf || len == 0) {
        return false;
    }
    snprintf(buf, len, "%s", item->name);
    return true;
}

int hid_pt_model_default_latency_ms(const hid_pt_model_t *model)
{
    const logical_device_t *item = selected_item(model);
    return item ? (int) default_settings_for_item(item).latency_ms : 60;
}

/* ---- settings ----------------------------------------------------------- */

bool hid_pt_model_read_controls(const hid_pt_model_t *model, hid_pt_controls_t *out)
{
    const logical_device_t *item = selected_item(model);
    const tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!out || !settings) {
        return false;
    }
    out->latency_ms = settings->latency_ms;
    out->audio_mode = (unsigned) settings->audio_mode;
    out->speaker_volume_percent = settings->speaker_volume_percent;
    out->headset_volume_percent = settings->headset_volume_percent;
    out->haptics_gain_centi = settings->haptics_gain_centi;
    out->auto_plugin = settings->auto_plugin;
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
    settings->audio_mode = (tv_bridge_audio_mode_t) in->audio_mode;
    settings->speaker_volume_percent = in->speaker_volume_percent;
    settings->headset_volume_percent = in->headset_volume_percent;
    if (hid_pt_model_selected_is_ds5(model)) {
        settings->haptics_gain_centi = in->haptics_gain_centi;
    }
    apply_settings_to_session(item);
    return true;
}

bool hid_pt_model_set_auto_plugin(const hid_pt_model_t *model, bool on)
{
    const logical_device_t *item = selected_item(model);
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) {
        return false;
    }
    settings->auto_plugin = on;
    hid_pt_sync_auto_plugin_pref(item);
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
