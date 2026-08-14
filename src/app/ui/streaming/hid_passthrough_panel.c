#if defined(TARGET_WEBOS)

/**
 * The HID passthrough panel's wiring.
 *
 * Three files, one job each. hid_pt_panel_view.c builds and drives the widgets;
 * its TU has no include path to the model, so it cannot name a device.
 * hid_pt_panel_model.c owns the selection and is the panel's only door to the
 * CTM globals; this TU in turn has no include path to ctm_state.h. That leaves
 * this file as the one place where a widget event meets a device.
 *
 * The two directions are kept apart:
 *   - sync_customize_ui_from_settings() and update_device_options() push the
 *     model into the widgets. They run on the 2 s refresh as well as on a
 *     selection change, so they must not overwrite a control the user currently
 *     owns -- see the comment on sync_customize_ui_from_settings() for the one
 *     control where that is enforced, and why the sliders need no such guard.
 *   - customize_setting_changed(), and the composite and auto-plug branches of
 *     panel_value_changed(), push the widgets into the model. Every caller is a
 *     widget's LV_EVENT_VALUE_CHANGED, i.e. a change the user just made.
 */

#include "hid_passthrough_panel.h"
#include "hid_pt_panel_model.h"
#include "hid_pt_panel_view.h"
#include "overlay_style.h"

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
    /* Stored, and nothing in this file calls them. The panel asks to be closed
     * by pushing USER_CLOSE_HID_PANEL (panel_request_close()); the one caller,
     * streaming.controller.c, handles that event and invokes its own close
     * callback there, so nothing is lost today. A different caller that passed
     * a callback here would simply never see it run. */
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
static void panel_focus_selected_row(hid_pt_panel_t *panel);
static void panel_update_hints(hid_pt_panel_t *panel, lv_obj_t *focused);
static void panel_focus(hid_pt_panel_t *panel, lv_obj_t *obj);

static void panel_request_close(hid_pt_panel_t *panel) {
    (void) panel;
    bus_pushevent(USER_CLOSE_HID_PANEL, NULL, NULL);
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

static void panel_focus_selected_row(hid_pt_panel_t *panel)
{
    if (!panel) {
        return;
    }
    hid_pt_view_focus_row(&panel->view, panel->selected_index);
}

/**
 * Move the cursor to the nearest row in direction @p step, or nowhere.
 *
 * The downward bound is the model's *current* device count, not the number of
 * rows on screen. Those differ when the model changed after the last render, and
 * then the trailing rows are not reachable with DOWN until the next re-render.
 * Carried over unchanged from the previous key handling.
 */
static bool panel_step_row(hid_pt_panel_t *panel, int from, int step)
{
    if (step < 0) {
        for (int j = from - 1; j >= 0; --j) {
            if (hid_pt_view_has_row(&panel->view, j)) {
                hid_pt_view_focus_row(&panel->view, j);
                return true;
            }
        }
        return false;
    }
    for (int j = from + 1; j < hid_pt_model_device_count() && j < HID_PT_MAX_ROWS; ++j) {
        if (hid_pt_view_has_row(&panel->view, j)) {
            hid_pt_view_focus_row(&panel->view, j);
            return true;
        }
    }
    return false;
}

/* Arriving on a row IS selecting the device: the settings column follows the
 * cursor, so there is nothing extra to press to see a controller's settings. */
static void panel_row_focused(void *userdata, int row)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel || hid_pt_view_is_rebuilding(&panel->view) ||
        !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }
    panel_select_device(panel, row);
    hid_pt_view_scroll_row_into_view(&panel->view, row);
    panel_update_hints(panel, panel->view.row_buttons[row]);
}

/* The footer names the keys for where the cursor actually is — OK plugs a
 * device in and does nothing at all to a slider, and saying so once at the
 * bottom is cheaper than a legend on every row. */
static void panel_update_hints(hid_pt_panel_t *panel, lv_obj_t *focused)
{
    if (!panel) {
        return;
    }
    hid_pt_zone_t zone = hid_pt_view_zone_of(&panel->view, focused);
    hid_pt_view_set_hints(&panel->view, zone, hid_pt_model_selected_is_plugged(&panel->model));
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
        case LV_KEY_DOWN: {
            panel_focus(panel, hid_pt_view_step_option(&panel->view, target, key == LV_KEY_UP ? -1 : 1));
            lv_event_stop_processing(event);
            return;
        }
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT: {
            /* LEFT/RIGHT steps the audio mode where it steps a slider's value, so
             * the whole settings column answers to one pair of keys. OK still
             * opens the full list for anyone who wants to see all five at once. */
            uint16_t count = lv_dropdown_get_option_cnt(target);
            int32_t sel = (int32_t) lv_dropdown_get_selected(target) + (key == LV_KEY_RIGHT ? 1 : -1);
            if (count > 0 && sel >= 0 && sel < (int32_t) count) {
                lv_dropdown_set_selected(target, (uint16_t) sel);
                lv_event_send(target, LV_EVENT_VALUE_CHANGED, NULL);
            }
            lv_event_stop_processing(event);
            return;
        }
        default:
            break;
    }
}

/** Move the cursor and tell the footer about it. */
static void panel_focus(hid_pt_panel_t *panel, lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_group_focus_obj(obj);
    panel_update_hints(panel, obj);
}

/**
 * The panel's key handling, for every control and for the sheet itself.
 *
 * The sheet is three zones laid out the way the eye sees them — the header
 * above, the devices left, that device's settings right — and the arrows move
 * within a zone rather than along one flat chain of every widget on screen:
 *
 *   UP/DOWN     the next thing in this column, and no further
 *   RIGHT       from a device, into its settings
 *   LEFT        from a setting, back to the device — or, on a slider, DOWN a
 *               step, because a slider owns both horizontal keys outright
 *   OK          plugs the focused device in or out; opens the dropdown
 *   BACK        from the settings, back to the devices; from there, closes
 *
 * The sliders take LEFT/RIGHT with nothing in between. They used to want OK to
 * enter an edit mode, arrows to move, then OK or BACK to leave — three keys and
 * a mode to remember for what is one continuous gesture, and the mode was
 * invisible except for LVGL's own focus tint.
 */
static void panel_control_key(void *userdata, lv_event_t *event)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel || !panel->view.group || lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    const uint32_t key = lv_event_get_key(event);
    const hid_pt_widget_kind_t kind = hid_pt_view_kind_of(&panel->view, target);
    const hid_pt_zone_t zone = hid_pt_view_zone_of(&panel->view, target);

    switch (key) {
        case LV_KEY_ESC:
            if (kind == HID_PT_WK_DROPDOWN && hid_pt_view_dropdown_is_open(&panel->view, target)) {
                hid_pt_view_close_dropdown(&panel->view, target);
                break;
            }
            /* One step back out of the settings, then out of the sheet. Without
             * this the only way off a slider would be UP or DOWN, since it has
             * taken LEFT for its own. */
            if (zone == HID_PT_ZONE_OPTIONS && hid_pt_view_has_row(&panel->view, panel->selected_index)) {
                panel_focus_selected_row(panel);
                break;
            }
            panel_request_close(panel);
            break;
        case LV_KEY_ENTER:
            if (kind == HID_PT_WK_SLIDER) {
                /* Nothing to confirm: the value is already what it looks like. */
                break;
            }
            if (kind == HID_PT_WK_DROPDOWN && !hid_pt_view_dropdown_is_open(&panel->view, target)) {
                hid_pt_view_open_dropdown(&panel->view, target);
                break;
            }
            /* Anything else -- a device row, a switch, a button -- has its own
             * OK, and LVGL turns the key into a click on it. */
            return;
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
        case LV_KEY_UP:
        case LV_KEY_DOWN: {
            /* An open dropdown list owns all four arrows: there they pick an
             * entry rather than move the cursor. */
            if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
                return;
            }
            const int dir = (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) ? 1 : -1;
            if ((key == LV_KEY_LEFT || key == LV_KEY_RIGHT) &&
                hid_pt_view_nudge_slider(&panel->view, target, dir)) {
                break;
            }
            if (zone == HID_PT_ZONE_LIST) {
                if (key == LV_KEY_RIGHT) {
                    panel_focus(panel, hid_pt_view_first_option(&panel->view));
                } else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
                    int from = hid_pt_view_row_of(&panel->view, target);
                    if (!panel_step_row(panel, from, dir) && key == LV_KEY_UP) {
                        /* Off the top of the list is the header, which is where
                         * it sits on screen. */
                        panel_focus(panel, panel->view.refresh_btn);
                    }
                }
            } else if (zone == HID_PT_ZONE_OPTIONS) {
                if (key == LV_KEY_LEFT) {
                    panel_focus_selected_row(panel);
                } else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
                    lv_obj_t *next = hid_pt_view_step_option(&panel->view, target, dir);
                    if (next) {
                        panel_focus(panel, next);
                    } else if (key == LV_KEY_UP) {
                        panel_focus(panel, panel->view.close_btn);
                    }
                }
            } else { /* the header */
                if (key == LV_KEY_LEFT) {
                    panel_focus(panel, panel->view.refresh_btn);
                } else if (key == LV_KEY_RIGHT) {
                    panel_focus(panel, panel->view.close_btn);
                } else if (key == LV_KEY_DOWN) {
                    if (hid_pt_view_has_row(&panel->view, panel->selected_index)) {
                        panel_focus_selected_row(panel);
                    } else {
                        panel_focus(panel, hid_pt_view_first_option(&panel->view));
                    }
                }
            }
            break;
        }
        default:
            return;
    }
    lv_event_stop_processing(event);
}

/* Every one of these is rewritten on the 2 s refresh with the value it already
 * has. LVGL treats a flag write as a change regardless: it invalidates the row
 * and marks both it and its parent's layout dirty, which costs a full-screen
 * layout walk and a repaint of the sheet — over a decoding game — for nothing.
 * So the no-op case stops here. */
static void show_row(lv_obj_t *row, bool show)
{
    if (!row || show != lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
        return; /* already in the state being asked for */
    }
    if (show) {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_switch(lv_obj_t *sw, bool on)
{
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
}

/* @p controls is NULL when the selection has no settings record, which is the
 * same case the hand-written version treated as "nothing to warn about". */
static void update_audio_warning(hid_pt_panel_t *panel, const hid_pt_controls_t *controls)
{
    if (!panel || !panel->view.audio_warning_label) {
        return;
    }
    const bool warn = controls && controls->audio_mode != HID_PT_AUDIO_MODE_AUTO;
    /* The wording never varies, so it is written once, when the label comes up —
     * not again on every one of the 2 s refreshes it stays up for. */
    if (warn && lv_obj_has_flag(panel->view.audio_warning_label, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(panel->view.audio_warning_label,
                          locstr("Enabling the controller speaker may route game audio over Bluetooth (SBC). "
                                 "Use Auto to keep game audio on HDMI."));
    }
    show_row(panel->view.audio_warning_label, warn);
}

/**
 * The one line under the device's name: whether it is bridged, and how it is
 * doing. "BRIDGED" is tinted with the same teal the device's rail wears in the
 * list, so the two say the same thing in the same colour.
 */
static void update_state_line(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.customize_state) {
        return;
    }
    char battery[64];
    const bool has_battery = hid_pt_model_battery_text(&panel->model, battery, sizeof(battery));
    const bool plugged = hid_pt_model_selected_is_plugged(&panel->model);
    const char *state = plugged ? locstr("BRIDGED") : locstr("IDLE");
    char line[128];
    if (plugged) {
        /* LVGL's inline recolour: #rrggbb marks the run, # ends it. The teal is
         * the palette's, so it cannot drift from the rail it is matching. */
        snprintf(line, sizeof(line), has_battery ? "#%06x %s#   %s" : "#%06x %s#", OVERLAY_LIVE, state,
                 has_battery ? battery : "");
    } else {
        snprintf(line, sizeof(line), has_battery ? "%s   %s" : "%s", state, has_battery ? battery : "");
    }
    /* This runs on the 2 s refresh and on every focus move, almost always with
     * the line already on screen; setting it again would re-measure the whole
     * recoloured string and dirty the sheet's layout for nothing. */
    if (strcmp(lv_label_get_text(panel->view.customize_state), line) != 0) {
        lv_label_set_text(panel->view.customize_state, line);
    }
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
    show_row(v->haptics_row, hid_pt_model_selected_is_ds5(&panel->model));
    if (v->auto_plugin_cb) {
        set_switch(v->auto_plugin_cb, c.auto_plugin);
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

/* Both switch rows do the same two things: show themselves only for a device the
 * setting applies to, and mirror that device's stored value while they do. The
 * value is written only when the settings record could be read, so a device
 * without one leaves the switch where it was rather than clearing it. */
static void update_auto_plugin_row(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.auto_plugin_row || !panel->view.auto_plugin_cb) {
        return;
    }
    const bool show = hid_pt_model_selected_is_bridgeable(&panel->model);
    hid_pt_controls_t c;
    if (show && hid_pt_model_read_controls(&panel->model, &c)) {
        set_switch(panel->view.auto_plugin_cb, c.auto_plugin);
    }
    show_row(panel->view.auto_plugin_row, show);
}

/**
 * Show the settings the selected device actually has.
 *
 * The whole column used to be hidden at once for anything that is not a
 * PlayStation pad, which meant a bridgeable device with an auto-plug setting had
 * nowhere to show it. Rows are hidden one group at a time now, and the flex
 * layout closes the gap.
 */
static void update_customize_panel(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.customize_panel) {
        return;
    }
    hid_pt_view_t *v = &panel->view;
    char name[HID_PT_PANEL_NAME_LEN];
    const bool have_device = hid_pt_model_selected_name(&panel->model, name, sizeof(name));
    const bool has_audio = have_device && hid_pt_model_selected_has_audio(&panel->model);

    if (v->customize_title) {
        /* Only moves when the selection does, while this runs on every refresh —
         * and a content-sized label re-measures its whole string on every write. */
        const char *title = have_device ? name : locstr("No device selected");
        if (strcmp(lv_label_get_text(v->customize_title), title) != 0) {
            lv_label_set_text(v->customize_title, title);
        }
    }
    if (has_audio) {
        sync_customize_ui_from_settings(panel);
    }
    show_row(v->audio_heading, has_audio);
    show_row(v->audio_row, has_audio);
    show_row(v->speaker_row, has_audio);
    show_row(v->headset_row, has_audio);
    show_row(v->latency_row, has_audio);
    show_row(v->reset_settings_btn, has_audio);
    /* The haptics row is hidden from inside sync_...(), which only runs for a
     * device that has the settings record to read it from. */
    if (!has_audio) {
        show_row(v->haptics_row, false);
        show_row(v->audio_warning_label, false);
    }
    show_row(v->customize_state, have_device);
    update_state_line(panel);
}

static void update_composite_row(hid_pt_panel_t *panel) {
    if (!panel || !panel->view.composite_row || !panel->view.composite_cb) {
        return;
    }
    const bool show = hid_pt_model_selected_is_flydigi(&panel->model);
    hid_pt_controls_t c;
    if (show && hid_pt_model_read_controls(&panel->model, &c)) {
        set_switch(panel->view.composite_cb, c.composite_passthrough);
    }
    show_row(panel->view.composite_row, show);
}

static void update_device_options(hid_pt_panel_t *panel)
{
    update_auto_plugin_row(panel);
    update_composite_row(panel);
    update_customize_panel(panel);
}

/* The row is the button now: OK on a device, or a click anywhere on it, is the
 * plug. There is no second control to aim at. */
static void panel_row_clicked(void *userdata, int row) {
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

    hid_pt_view_set_row_state(&panel->view, row, plugged);
    update_row_styles(panel);
    update_device_options(panel);
    panel_update_status(panel);
    panel_update_hints(panel, panel->view.row_buttons[row]);
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
    if (panel->view.error_label && panel->view.error_row) {
        const char *err = hid_pt_model_plug_error();
        if (err) {
            lv_label_set_text(panel->view.error_label, err);
            lv_obj_clear_flag(panel->view.error_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panel->view.error_row, LV_OBJ_FLAG_HIDDEN);
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
        panel_focus(panel, panel->view.row_buttons[panel->selected_index]);
        return;
    }
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (hid_pt_view_has_row(&panel->view, i)) {
            panel_select_device(panel, i);
            panel_focus(panel, panel->view.row_buttons[i]);
            return;
        }
    }
    if (panel->view.close_btn) {
        panel_focus(panel, panel->view.close_btn);
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
    bool keep_focus = panel->have_rendered && same_device &&
                      hid_pt_view_kind_is_option(hid_pt_view_kind_of(&panel->view, prev_focus));
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
            .row_focused = panel_row_focused,
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
