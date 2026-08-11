#if defined(TARGET_WEBOS)

/**
 * The HID passthrough panel's wiring.
 *
 * Three files, one job each: hid_pt_panel_view.c builds and drives the widgets
 * and knows nothing about devices; hid_pt_panel_model.c owns the selection and
 * is the only place that touches the CTM globals; this file is what connects
 * them, and the only place where a widget event meets a device.
 *
 * The two directions are separate on purpose:
 *   - sync_customize_ui_from_settings() and update_device_options() push the
 *     model into the widgets. They run on the 2 s refresh as well as on a
 *     selection change, so they must not overwrite a control the user currently
 *     owns -- see the comment on sync_customize_ui_from_settings().
 *   - customize_setting_changed() and the two checkbox handlers push the
 *     widgets into the model, and are only ever reached from a widget's
 *     LV_EVENT_VALUE_CHANGED, i.e. from a change the user just made.
 */

#include "hid_passthrough_panel.h"
#include "hid_pt_panel_model.h"
#include "hid_pt_panel_view.h"

#include "hid_passthrough/hid_passthrough_manager.h"
#include "stream/session.h"

#include "util/bus.h"
#include "util/i18n.h"
#include "util/user_event.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    hid_pt_view_t view;
    hid_pt_model_t model;
    session_t *session;
    hid_passthrough_panel_close_cb on_close;
    void *on_close_userdata;
    lv_timer_t *refresh_timer;
    /* The device key each rendered row is LABELLED with, stamped by
     * render_device_list(). The device model is rebuilt from scratch on the
     * manager's 1 Hz poll (and synchronously from the SDL hotplug handlers)
     * while this panel re-renders on its own 2 s timer, so between the two a
     * model index no longer names the device the user is looking at. Every
     * click and every settings write resolves this key instead. */
    char row_keys[HID_PT_MAX_ROWS][HID_PT_PANEL_KEY_LEN];
    /* A ROW index into the view's rows and into row_keys — never an index into
     * the device model. The selected DEVICE is the model's key. */
    int selected_index;
    uint64_t last_sig;
    bool have_rendered;
} hid_pt_panel_t;

/* The row currently showing @p key, or -1. */
static int panel_row_for_key(const hid_pt_panel_t *panel, const char *key)
{
    if (!panel || !key || !key[0]) {
        return -1;
    }
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (hid_pt_view_has_row(&panel->view, i) && strcmp(panel->row_keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

/* Styled by key, not by index: this also runs on refreshes that did NOT
 * re-render, where the rows still show the previous rebuild's order. */
static void update_row_styles(hid_pt_panel_t *panel) {
    const char *sel = hid_pt_model_selected_key(&panel->model);
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        hid_pt_view_set_row_selected(&panel->view, i,
                                     sel[0] && strcmp(panel->row_keys[i], sel) == 0);
    }
}

static void panel_update_status(hid_pt_panel_t *panel);
static void update_device_options(hid_pt_panel_t *panel);
static void panel_select_device(hid_pt_panel_t *panel, int row);
static lv_obj_t *panel_first_option_target(hid_pt_panel_t *panel);
static void panel_focus_current_plug(hid_pt_panel_t *panel);
static bool panel_is_plug_button(const hid_pt_panel_t *panel, lv_obj_t *target);
static bool panel_is_option_control(const hid_pt_panel_t *panel, lv_obj_t *target);

static void panel_request_close(hid_pt_panel_t *panel) {
    (void) panel;
    bus_pushevent(USER_CLOSE_HID_PANEL, NULL, NULL);
}

static bool panel_item_needs_lrkey(lv_obj_t *obj)
{
    return lv_obj_has_class(obj, &lv_slider_class);
}

/* @p row is a ROW index. The selection is stored as the key that row shows, so
 * it keeps naming the same controller after the device model is rebuilt
 * underneath. */
static void panel_select_device(hid_pt_panel_t *panel, int row)
{
    if (!panel || !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }
    panel->selected_index = row;
    hid_pt_model_set_selected_key(&panel->model, panel->row_keys[row]);
    update_row_styles(panel);
    update_device_options(panel);
}

static lv_obj_t *panel_first_option_target(hid_pt_panel_t *panel)
{
    if (!panel) {
        return NULL;
    }
    if (hid_pt_model_selected_is_flydigi(&panel->model) && panel->view.composite_row &&
        !lv_obj_has_flag(panel->view.composite_row, LV_OBJ_FLAG_HIDDEN) && panel->view.composite_cb) {
        return panel->view.composite_cb;
    }
    if (panel->view.customize_panel && !lv_obj_has_flag(panel->view.customize_panel, LV_OBJ_FLAG_HIDDEN)) {
        if (panel->view.latency_slider) {
            return panel->view.latency_slider;
        }
    }
    return NULL;
}

static void panel_focus_current_plug(hid_pt_panel_t *panel)
{
    if (!panel) {
        return;
    }
    hid_pt_view_focus_plug(&panel->view, panel->selected_index);
}

static void panel_focus_plug_at(hid_pt_panel_t *panel, int index)
{
    if (!panel || index < 0 || index >= HID_PT_MAX_ROWS ||
        !hid_pt_view_has_row(&panel->view, index)) {
        return;
    }
    panel_select_device(panel, index);
    hid_pt_view_focus_plug(&panel->view, index);
}

static bool panel_is_plug_button(const hid_pt_panel_t *panel, lv_obj_t *target)
{
    return hid_pt_view_plug_row_of(&panel->view, target) >= 0;
}

static bool panel_is_option_control(const hid_pt_panel_t *panel, lv_obj_t *target)
{
    if (!panel || !target) {
        return false;
    }
    const hid_pt_view_t *v = &panel->view;
    return target == v->composite_cb ||
           target == v->auto_plugin_cb ||
           target == v->latency_slider ||
           target == v->audio_dropdown ||
           target == v->speaker_slider ||
           target == v->headset_slider ||
           target == v->haptics_slider ||
           (v->haptics_row && lv_obj_get_parent(target) == v->haptics_row) ||
           target == v->reset_settings_btn;
}

static void panel_plug_focused(void *userdata, int row)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel || hid_pt_view_is_rebuilding(&panel->view) ||
        !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }
    panel_select_device(panel, row);
    hid_pt_view_scroll_row_into_view(&panel->view, row);
}

static void panel_dropdown_key(void *userdata, lv_event_t *event)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel || !panel->view.group || lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    if (!lv_obj_has_class(target, &lv_dropdown_class) ||
        hid_pt_view_dropdown_is_open(&panel->view, target)) {
        return;
    }
    const uint32_t key = lv_event_get_key(event);
    switch (key) {
        case LV_KEY_UP:
            lv_group_focus_prev(panel->view.group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_DOWN:
            lv_group_focus_next(panel->view.group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_LEFT:
            panel_focus_current_plug(panel);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_RIGHT:
            lv_group_focus_next(panel->view.group);
            lv_event_stop_processing(event);
            return;
        default:
            break;
    }
}

static void panel_control_key(void *userdata, lv_event_t *event)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel || !panel->view.group || lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    uint32_t key = lv_event_get_key(event);
    bool editing = lv_group_get_editing(panel->view.group);

    switch (key) {
        case LV_KEY_ESC:
            if (editing && panel_item_needs_lrkey(target)) {
                lv_group_set_editing(panel->view.group, false);
                lv_event_stop_processing(event);
                return;
            }
            if (lv_obj_has_class(target, &lv_dropdown_class)) {
                if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
                    hid_pt_view_close_dropdown(&panel->view, target);
                    lv_event_stop_processing(event);
                    return;
                }
            }
            panel_request_close(panel);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_ENTER:
            if (lv_obj_has_class(target, &lv_slider_class)) {
                /* Toggle edit mode: first OK enters (LEFT/RIGHT adjust value),
                 * a second OK leaves it again (like ESC). */
                lv_group_set_editing(panel->view.group, !editing);
                lv_event_stop_processing(event);
                return;
            }
            if (lv_obj_has_class(target, &lv_dropdown_class) &&
                !hid_pt_view_dropdown_is_open(&panel->view, target)) {
                hid_pt_view_open_dropdown(&panel->view, target);
                lv_event_stop_processing(event);
                return;
            }
            return;
        case LV_KEY_LEFT:
            if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
                return;
            }
            if (panel_item_needs_lrkey(target) && editing) {
                return;
            }
            if (panel_is_option_control(panel, target)) {
                panel_focus_current_plug(panel);
                lv_event_stop_processing(event);
                return;
            }
            lv_group_focus_prev(panel->view.group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_RIGHT:
            if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
                return;
            }
            if (panel_item_needs_lrkey(target) && editing) {
                return;
            }
            if (panel_is_plug_button(panel, target)) {
                lv_obj_t *opt = panel_first_option_target(panel);
                if (opt) {
                    lv_group_focus_obj(opt);
                }
                lv_event_stop_processing(event);
                return;
            }
            lv_group_focus_next(panel->view.group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_UP:
            if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
                return;
            }
            if (panel_item_needs_lrkey(target) && editing) {
                return;   /* edit mode: UP adjusts the slider value, don't navigate */
            }
            if (panel_is_plug_button(panel, target)) {
                int idx = hid_pt_view_plug_row_of(&panel->view, target);
                for (int j = idx - 1; j >= 0; --j) {
                    if (hid_pt_view_has_row(&panel->view, j)) {
                        panel_focus_plug_at(panel, j);
                        break;
                    }
                }
                lv_event_stop_processing(event);
                return;
            }
            lv_group_focus_prev(panel->view.group);
            lv_event_stop_processing(event);
            return;
        case LV_KEY_DOWN:
            if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
                return;
            }
            if (panel_item_needs_lrkey(target) && editing) {
                return;   /* edit mode: DOWN adjusts the slider value, don't navigate */
            }
            if (panel_is_plug_button(panel, target)) {
                int idx = hid_pt_view_plug_row_of(&panel->view, target);
                for (int j = idx + 1; j < hid_pt_model_device_count() && j < HID_PT_MAX_ROWS; ++j) {
                    if (hid_pt_view_has_row(&panel->view, j)) {
                        panel_focus_plug_at(panel, j);
                        break;
                    }
                }
                lv_event_stop_processing(event);
                return;
            }
            lv_group_focus_next(panel->view.group);
            lv_event_stop_processing(event);
            return;
        default:
            break;
    }
}

/* @p controls is NULL when the selection has no settings record, which is the
 * same case the hand-written version treated as "nothing to warn about". */
static void update_audio_warning(hid_pt_panel_t *panel, const hid_pt_controls_t *controls)
{
    if (!panel || !panel->view.audio_warning_label) {
        return;
    }
    if (!controls || controls->audio_mode == HID_PT_AUDIO_MODE_AUTO) {
        lv_obj_add_flag(panel->view.audio_warning_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(panel->view.audio_warning_label,
                          locstr("Enabling the controller speaker may route game audio over Bluetooth (SBC). "
                                 "Use Auto to keep game audio on HDMI."));
        lv_obj_clear_flag(panel->view.audio_warning_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_battery_label(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.battery_label) {
        return;
    }
    char text[64];
    if (!hid_pt_model_battery_text(&panel->model, text, sizeof(text))) {
        lv_obj_add_flag(panel->view.battery_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(panel->view.battery_label, text);
    lv_obj_clear_flag(panel->view.battery_label, LV_OBJ_FLAG_HIDDEN);
}

/**
 * Push the stored settings back into the widgets.
 *
 * Runs on every 2 s refresh, not just on a selection change, so it has to keep
 * its hands off a control the user currently owns. The audio dropdown is the one
 * that loses data: while its list is open LVGL's key handler only moves
 * sel_opt_id and fires no LV_EVENT_VALUE_CHANGED, so the model still holds the
 * old value -- writing it back with lv_dropdown_set_selected() resets sel_opt_id
 * *and* sel_opt_id_orig and re-scrolls the open list, and the user's eventual OK
 * then commits the value they had already moved away from.
 * The sliders deliberately get no such guard: they emit VALUE_CHANGED on every
 * step, so the model is always current and the write-back is a no-op.
 */
static void sync_customize_ui_from_settings(hid_pt_panel_t *panel)
{
    hid_pt_controls_t c;
    if (!panel || !hid_pt_model_read_controls(&panel->model, &c)) {
        return;
    }
    hid_pt_view_t *v = &panel->view;
    if (v->latency_slider) {
        int latency = (int) c.latency_ms;
        if (latency < DS_LATENCY_MIN) {
            latency = DS_LATENCY_MIN;
        } else if (latency > DS_LATENCY_MAX) {
            latency = DS_LATENCY_MAX;
        }
        lv_slider_set_value(v->latency_slider, latency, LV_ANIM_OFF);
        hid_pt_view_update_latency_label(v, hid_pt_model_default_latency_ms(&panel->model));
    }
    /* Asked about this dropdown by name, not via the view's active_dropdown:
     * that field is only written when the list is opened with OK, so a list the
     * pointer indev (magic remote) opened would answer "closed" and get reset. */
    if (v->audio_dropdown && !hid_pt_view_dropdown_is_open(v, v->audio_dropdown)) {
        lv_dropdown_set_selected(v->audio_dropdown, (uint16_t) c.audio_mode);
    }
    if (v->speaker_slider) {
        int spk = (int) c.speaker_volume_percent;
        if (spk > DS_VOLUME_MAX) {
            spk = DS_VOLUME_MAX;
        }
        lv_slider_set_value(v->speaker_slider, spk, LV_ANIM_OFF);
        hid_pt_view_update_speaker_label(v);
    }
    if (v->headset_slider) {
        int hs = (int) c.headset_volume_percent;
        if (hs > DS_VOLUME_MAX) {
            hs = DS_VOLUME_MAX;
        }
        lv_slider_set_value(v->headset_slider, hs, LV_ANIM_OFF);
        hid_pt_view_update_headset_label(v);
    }
    if (v->haptics_slider) {
        int hap = (int) c.haptics_gain_centi;
        if (hap > DS_HAPTICS_MAX) {
            hap = DS_HAPTICS_MAX;
        }
        lv_slider_set_value(v->haptics_slider, hap, LV_ANIM_OFF);
        hid_pt_view_update_haptics_label(v);
    }
    if (v->haptics_row) {
        if (hid_pt_model_selected_is_ds5(&panel->model)) {
            lv_obj_clear_flag(v->haptics_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(v->haptics_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (v->auto_plugin_cb) {
        if (c.auto_plugin) {
            lv_obj_add_state(v->auto_plugin_cb, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(v->auto_plugin_cb, LV_STATE_CHECKED);
        }
    }
    update_audio_warning(panel, &c);
}

/**
 * Push what the widgets hold back into the model.
 *
 * Only ever called from a widget's LV_EVENT_VALUE_CHANGED, i.e. from a change
 * the user just made. The reverse direction lives in
 * sync_customize_ui_from_settings().
 */
static void customize_setting_changed(hid_pt_panel_t *panel)
{
    hid_pt_controls_t c;
    if (!panel || !hid_pt_model_read_controls(&panel->model, &c)) {
        return;
    }
    hid_pt_view_t *v = &panel->view;
    if (v->latency_slider) {
        c.latency_ms = (unsigned) lv_slider_get_value(v->latency_slider);
    }
    if (v->audio_dropdown) {
        c.audio_mode = lv_dropdown_get_selected(v->audio_dropdown);
    }
    if (v->speaker_slider) {
        c.speaker_volume_percent = (unsigned) lv_slider_get_value(v->speaker_slider);
    }
    if (v->headset_slider) {
        c.headset_volume_percent = (unsigned) lv_slider_get_value(v->headset_slider);
    }
    if (v->haptics_slider) {
        /* The model drops this again unless the selection is a DualSense. */
        c.haptics_gain_centi = (unsigned) lv_slider_get_value(v->haptics_slider);
    }
    if (!hid_pt_model_write_controls(&panel->model, &c)) {
        return;
    }
    update_audio_warning(panel, &c);
}

static void update_auto_plugin_row(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.auto_plugin_row || !panel->view.auto_plugin_cb) {
        return;
    }
    if (!hid_pt_model_selected_is_bridgeable(&panel->model)) {
        lv_obj_add_flag(panel->view.auto_plugin_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    hid_pt_controls_t c;
    if (hid_pt_model_read_controls(&panel->model, &c)) {
        if (c.auto_plugin) {
            lv_obj_add_state(panel->view.auto_plugin_cb, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(panel->view.auto_plugin_cb, LV_STATE_CHECKED);
        }
    }
    lv_obj_clear_flag(panel->view.auto_plugin_row, LV_OBJ_FLAG_HIDDEN);
}

static void update_customize_panel(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.customize_panel) {
        return;
    }
    if (hid_pt_model_selected_has_audio(&panel->model)) {
        sync_customize_ui_from_settings(panel);
        char name[HID_PT_PANEL_NAME_LEN];
        if (panel->view.customize_title &&
            hid_pt_model_selected_name(&panel->model, name, sizeof(name))) {
            lv_label_set_text_fmt(panel->view.customize_title, "%s", name);
        }
        lv_obj_clear_flag(panel->view.customize_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel->view.customize_panel, LV_OBJ_FLAG_HIDDEN);
    }
    update_battery_label(panel);
}

static void update_composite_row(hid_pt_panel_t *panel) {
    if (!panel || !panel->view.composite_row || !panel->view.composite_cb) {
        return;
    }
    bool show = hid_pt_model_selected_is_flydigi(&panel->model);
    if (show) {
        hid_pt_controls_t c;
        if (hid_pt_model_read_controls(&panel->model, &c)) {
            if (c.composite_passthrough) {
                lv_obj_add_state(panel->view.composite_cb, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(panel->view.composite_cb, LV_STATE_CHECKED);
            }
        }
    }
    if (show) {
        lv_obj_clear_flag(panel->view.composite_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel->view.composite_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_device_options(hid_pt_panel_t *panel)
{
    update_auto_plugin_row(panel);
    update_composite_row(panel);
    update_customize_panel(panel);
}

static void panel_row_clicked(void *userdata, int row) {
    hid_pt_panel_t *panel = userdata;
    if (!panel || !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }
    panel_select_device(panel, row);
    hid_pt_view_focus_plug(&panel->view, row);
}

static void panel_plug_clicked(void *userdata, int row) {
    hid_pt_panel_t *panel = userdata;
    if (!panel || !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }

    /* Act on the key the row is LABELLED with, not on its index: the model is
     * rebuilt in readdir order on a different cadence than this panel
     * re-renders on, so an index could bridge the pad in the next row instead.
     * The model also owns the failure cases -- the device having left since the
     * row was drawn, and the plug-in refusing -- and leaves the reason in the
     * plug error the status line shows. */
    bool plugged = false;
    if (hid_pt_model_toggle_plug(&panel->model, panel->row_keys[row], panel->session, &plugged) !=
        HID_PT_PLUG_DONE) {
        panel_update_status(panel);
        return;
    }
    panel->selected_index = row;

    hid_pt_view_set_plug_label(&panel->view, row, plugged);
    update_row_styles(panel);
    update_device_options(panel);
    panel_update_status(panel);
}

static void panel_value_changed(void *userdata, hid_pt_ctl_t id)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel) {
        return;
    }
    switch (id) {
        case HID_PT_CTL_COMPOSITE:
            if (panel->view.composite_cb) {
                hid_pt_model_set_composite(&panel->model,
                                           lv_obj_has_state(panel->view.composite_cb, LV_STATE_CHECKED));
            }
            return;
        case HID_PT_CTL_AUTO_PLUGIN:
            if (panel->view.auto_plugin_cb) {
                hid_pt_model_set_auto_plugin(&panel->model,
                                             lv_obj_has_state(panel->view.auto_plugin_cb, LV_STATE_CHECKED));
            }
            return;
        case HID_PT_CTL_LATENCY:
            hid_pt_view_update_latency_label(&panel->view, hid_pt_model_default_latency_ms(&panel->model));
            break;
        case HID_PT_CTL_SPEAKER:
            hid_pt_view_update_speaker_label(&panel->view);
            break;
        case HID_PT_CTL_HEADSET:
            hid_pt_view_update_headset_label(&panel->view);
            break;
        case HID_PT_CTL_HAPTICS:
            hid_pt_view_update_haptics_label(&panel->view);
            break;
        case HID_PT_CTL_AUDIO_MODE:
            break;
        default:
            return;
    }
    customize_setting_changed(panel);
}

static void refresh_devices(hid_pt_panel_t *panel, bool rescan);

static void panel_clicked(void *userdata, hid_pt_ctl_t id)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel) {
        return;
    }
    switch (id) {
        case HID_PT_CTL_RESET:
            /* Defaults into the model, then into the widgets, then out to the
             * bridge and the pref store — the order the panel has always used. */
            if (hid_pt_model_reset_selected(&panel->model)) {
                sync_customize_ui_from_settings(panel);
                hid_pt_model_commit_selected(&panel->model);
            }
            return;
        case HID_PT_CTL_REFRESH:
            refresh_devices(panel, true);
            return;
        case HID_PT_CTL_CLOSE:
            panel_request_close(panel);
            return;
        default:
            return;
    }
}

static void render_device_list(hid_pt_panel_t *panel) {
    hid_pt_view_list_clear(&panel->view);
    memset(panel->row_keys, 0, sizeof(panel->row_keys));

    const int device_count = hid_pt_model_device_count();
    if (device_count == 0) {
        hid_pt_view_list_show_empty(&panel->view);
        return;
    }

    hid_pt_view_list_prepare(&panel->view);

    const char *sel = hid_pt_model_selected_key(&panel->model);
    for (int i = 0; i < device_count && i < HID_PT_MAX_ROWS; ++i) {
        hid_pt_row_info_t info;
        if (!hid_pt_model_row_info(i, &info)) {
            /* Only out-of-range fails, and the loop bound is the count the model
             * validates against; stop rather than leave a hole in the arrays. */
            break;
        }
        /* What this row stands for, for as long as it exists. */
        snprintf(panel->row_keys[i], sizeof(panel->row_keys[0]), "%s", info.key);
        hid_pt_view_add_row(&panel->view, i, info.label, info.plugged,
                            sel[0] && strcmp(info.key, sel) == 0);
    }
    hid_pt_view_rebuild_focus_order(&panel->view);
}

static void panel_update_status(hid_pt_panel_t *panel) {
    if (!panel || !panel->view.status_label) {
        return;
    }
    if (panel->view.error_label) {
        const char *err = hid_pt_model_plug_error();
        if (err) {
            lv_label_set_text(panel->view.error_label, err);
            lv_obj_clear_flag(panel->view.error_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panel->view.error_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    char status[128];
    hid_pt_model_status_text(status, sizeof(status));
    lv_label_set_text(panel->view.status_label, status);
}

static void focus_initial_target(hid_pt_panel_t *panel) {
    if (!panel || !panel->view.group) {
        return;
    }
    if (panel->selected_index >= 0 && panel->selected_index < HID_PT_MAX_ROWS &&
        hid_pt_view_has_row(&panel->view, panel->selected_index)) {
        hid_pt_view_focus_plug(&panel->view, panel->selected_index);
        return;
    }
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (hid_pt_view_has_row(&panel->view, i)) {
            panel_select_device(panel, i);
            hid_pt_view_focus_plug(&panel->view, i);
            return;
        }
    }
    if (panel->view.close_btn) {
        lv_group_focus_obj(panel->view.close_btn);
    }
}

/* @p rescan: ask the manager to rebuild the model first. Only the Refresh
 * button and the first render do — the periodic refresh just re-reads what the
 * manager's own 1000 ms poll already keeps current, and running a second full
 * sysfs enumeration on this panel's 2000 ms timer only duplicated it. */
static void refresh_devices(hid_pt_panel_t *panel, bool rescan) {
    if (!panel || !panel->session) {
        return;
    }
    /* Only while the stream still owns its input. The panel is destroyed on
     * USER_STREAM_FINISHED but hid_passthrough_manager_stop() already ran back at
     * USER_STREAM_CLOSE (root.c), and the gap between the two spans
     * LiStopConnection + gs_quit_app + pcmanager_update_by_host -- easily more
     * than one 2 s tick. Calling ensure() in that window restarted the whole
     * subsystem against a host that is already gone. session_has_input() is the
     * same predicate root.c tests before it calls session_stop_input(). */
    if (session_has_input(panel->session)) {
        session_ensure_hid_passthrough(panel->session);
    }
    if (rescan) {
        hid_passthrough_manager_request_rescan(session_get_hid_passthrough(panel->session),
                                               session_get_input(panel->session));
    }

    /* The device the user was on, captured before the resolve below is allowed to
     * silently re-point the selection at row 0. The focus restore further down
     * compares against it: a control kept across a *device* change would hand the
     * user's arrow keys to a different controller's settings. */
    char prev_key[HID_PT_PANEL_KEY_LEN];
    snprintf(prev_key, sizeof(prev_key), "%s", hid_pt_model_selected_key(&panel->model));

    /* The selection is a key; only its presence in the current model is decided
     * here. Its ROW is resolved after the render decision below, because on a
     * refresh that does not re-render the rows still carry the previous
     * rebuild's order and a model index would point at the wrong widget. */
    hid_pt_model_resolve_selection(&panel->model);
    panel_update_status(panel);

    /* Where the cursor was before the re-render. render_device_list() empties and
     * refills the focus group, which parks LVGL's focus on plug button 0 and
     * clears edit mode, so a user half-way through nudging a slider was thrown
     * back into the device list with their arrow keys silently meaning something
     * else. The option controls are created once by the view and only
     * shown/hidden, so they survive the rebuild and can be focused again by
     * pointer. Not done on the very first render: the group is then focused on
     * whatever the view added first, which is not a place the user chose.
     * Only for the same device: the option controls are shared by every row, so
     * without the key test a pad disappearing under the cursor would leave focus
     * (and edit mode) sitting on a slider that now writes the *next* pad's
     * settings. When the device changed, fall through to focus_initial_target(),
     * which moves the cursor visibly back to the list. */
    lv_obj_t *prev_focus = lv_group_get_focused(panel->view.group);
    bool same_device = strcmp(prev_key, hid_pt_model_selected_key(&panel->model)) == 0;
    bool keep_focus = panel->have_rendered && same_device && panel_is_option_control(panel, prev_focus);
    bool prev_editing = keep_focus && lv_group_get_editing(panel->view.group);

    uint64_t sig = hid_pt_model_signature();
    bool rerendered = false;
    if (!panel->have_rendered || sig != panel->last_sig) {
        panel->last_sig = sig;
        panel->have_rendered = true;
        render_device_list(panel);
        rerendered = true;
    } else {
        update_row_styles(panel);
    }
    /* Rows exist and are current as of here, so the selected key has a row. */
    panel->selected_index = panel_row_for_key(panel, hid_pt_model_selected_key(&panel->model));
    /* Before restoring focus, not after: this is what decides whether the control
     * the user was on is still on screen for the selected device (the hidden test
     * below reads the flags it sets). */
    update_device_options(panel);
    if (rerendered) {
        if (keep_focus && !hid_pt_view_obj_is_hidden(&panel->view, prev_focus)) {
            lv_group_focus_obj(prev_focus);
            if (prev_editing) {
                /* lv_group_focus_obj() leaves edit mode on its way in. */
                lv_group_set_editing(panel->view.group, true);
            }
        } else {
            focus_initial_target(panel);
        }
    }
}

static void refresh_timer_cb(lv_timer_t *timer) {
    hid_pt_panel_t *panel = timer->user_data;
    if (panel && panel->view.container && lv_obj_is_valid(panel->view.container)) {
        refresh_devices(panel, false);
    }
}

static void panel_deleted(void *userdata) {
    hid_pt_panel_t *panel = userdata;
    if (!panel) {
        return;
    }
    if (panel->refresh_timer) {
        lv_timer_del(panel->refresh_timer);
        panel->refresh_timer = NULL;
    }
    hid_pt_view_destroy(&panel->view);
    free(panel);
}

void hid_passthrough_panel_refresh(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (panel) {
        refresh_devices(panel, true);
    }
}

void hid_passthrough_panel_focus_initial(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (panel) {
        focus_initial_target(panel);
    }
}

lv_group_t *hid_passthrough_panel_get_group(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    return panel ? panel->view.group : NULL;
}

lv_obj_t *hid_passthrough_panel_create(lv_obj_t *parent, session_t *session,
                                       hid_passthrough_panel_close_cb on_close, void *userdata) {
    hid_pt_panel_t *panel = calloc(1, sizeof(*panel));
    if (!panel) {
        return NULL;
    }
    panel->session = session;
    panel->on_close = on_close;
    panel->on_close_userdata = userdata;
    panel->selected_index = -1;

    const hid_pt_view_cbs_t cbs = {
            .userdata = panel,
            .value_changed = panel_value_changed,
            .clicked = panel_clicked,
            .row_clicked = panel_row_clicked,
            .plug_clicked = panel_plug_clicked,
            .plug_focused = panel_plug_focused,
            .key = panel_control_key,
            .dropdown_key = panel_dropdown_key,
            .deleted = panel_deleted,
    };
    lv_obj_t *cont = hid_pt_view_create(&panel->view, parent, &cbs);
    if (!cont) {
        free(panel);
        return NULL;
    }
    lv_obj_set_user_data(cont, panel);
    panel->refresh_timer = lv_timer_create(refresh_timer_cb, 2000, panel);

    hid_passthrough_panel_refresh(cont);
    return cont;
}

#endif
