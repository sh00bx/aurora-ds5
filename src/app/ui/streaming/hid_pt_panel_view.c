#if defined(TARGET_WEBOS)

#include "hid_pt_panel_view.h"

#include "util/i18n.h"

#include <stdint.h>
#include <string.h>

/* ---- event trampolines --------------------------------------------------
 *
 * Every widget carries the view as its event user data. The option controls
 * additionally carry their hid_pt_ctl_t in lv_obj_set_user_data(), and the list
 * rows carry their row index there, so one trampoline per event kind is enough
 * to tell the panel which control spoke.
 */

static hid_pt_ctl_t ctl_of(lv_obj_t *obj)
{
    return (hid_pt_ctl_t) (intptr_t) lv_obj_get_user_data(obj);
}

static int row_of(lv_obj_t *obj)
{
    return (int) (intptr_t) lv_obj_get_user_data(obj);
}

static void value_changed_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.value_changed) {
        view->cbs.value_changed(view->cbs.userdata, ctl_of(lv_event_get_current_target(event)));
    }
}

static void clicked_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.clicked) {
        view->cbs.clicked(view->cbs.userdata, ctl_of(lv_event_get_current_target(event)));
    }
}

static void key_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.key) {
        view->cbs.key(view->cbs.userdata, event);
    }
}

static void dropdown_key_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.dropdown_key) {
        view->cbs.dropdown_key(view->cbs.userdata, event);
    }
}

/* The sheet is the group's fallback target: it only handles a key when it is
 * itself the object the key was sent to, never one bubbled up from a child. */
static void sheet_key_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (!view || !view->group || lv_event_get_target(event) != lv_event_get_current_target(event)) {
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    if (view->cbs.key) {
        view->cbs.key(view->cbs.userdata, event);
    }
}

static void row_clicked_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.row_clicked) {
        view->cbs.row_clicked(view->cbs.userdata, row_of(lv_event_get_current_target(event)));
    }
}

static void plug_clicked_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.plug_clicked) {
        view->cbs.plug_clicked(view->cbs.userdata, row_of(lv_event_get_current_target(event)));
    }
}

static void plug_focused_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (!view || lv_event_get_code(event) != LV_EVENT_FOCUSED || !view->cbs.plug_focused) {
        return;
    }
    view->cbs.plug_focused(view->cbs.userdata, row_of(lv_event_get_target(event)));
}

static void deleted_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.deleted) {
        view->cbs.deleted(view->cbs.userdata);
    }
}

/* ---- the device list ---------------------------------------------------- */

static void style_row(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_radius(row, LV_DPX(8), 0);
    lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, selected ? lv_color_hex(0x38bdf8) : lv_color_hex(0x263540), 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x12384a) : lv_color_hex(0x18232c), 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x16455d) : lv_color_hex(0x202e38), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
}

void hid_pt_view_list_clear(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_clean(view->list);
    memset(view->row_buttons, 0, sizeof(view->row_buttons));
    memset(view->plug_buttons, 0, sizeof(view->plug_buttons));
    memset(view->plug_labels, 0, sizeof(view->plug_labels));
}

void hid_pt_view_list_show_empty(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_t *empty = lv_label_create(view->list);
    lv_label_set_text(empty, locstr("No HID devices visible to the native app"));
    lv_obj_set_style_text_color(empty, lv_color_hex(0xb6c5cf), 0);
    lv_obj_set_style_pad_all(empty, LV_DPX(18), 0);
}

void hid_pt_view_list_prepare(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_set_flex_flow(view->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(view->list, LV_DPX(8), 0);
}

void hid_pt_view_add_row(hid_pt_view_t *view, int i, const char *label, bool plugged, bool selected)
{
    if (!view || !view->list || i < 0 || i >= HID_PT_MAX_ROWS) {
        return;
    }
    const int row_h = LV_DPX(60);
    const int button_w = LV_DPX(112);

    lv_obj_t *row = lv_obj_create(view->list);
    view->row_buttons[i] = row;
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, row_h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_set_user_data(row, (void *) (intptr_t) i);
    style_row(row, selected);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, LV_DPX(14), 0);
    lv_obj_set_style_pad_gap(row, LV_DPX(8), 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);

    lv_obj_t *plug_btn = lv_btn_create(row);
    view->plug_buttons[i] = plug_btn;
    lv_obj_set_size(plug_btn, button_w, LV_DPX(38));
    lv_obj_set_style_radius(plug_btn, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(plug_btn, plugged ? lv_color_hex(0x7f1d1d) : lv_color_hex(0x0f766e), 0);
    lv_obj_set_style_bg_color(plug_btn, plugged ? lv_color_hex(0x991b1b) : lv_color_hex(0x0d9488),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(plug_btn, plug_clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(plug_btn, plug_focused_cb, LV_EVENT_FOCUSED, view);
    lv_obj_add_event_cb(plug_btn, key_cb, LV_EVENT_KEY, view);
    lv_obj_set_user_data(plug_btn, (void *) (intptr_t) i);

    view->plug_labels[i] = lv_label_create(plug_btn);
    lv_label_set_text(view->plug_labels[i], plugged ? locstr("Plug out") : locstr("Plug in"));
    lv_obj_center(view->plug_labels[i]);
}

bool hid_pt_view_has_row(const hid_pt_view_t *view, int row)
{
    return view && row >= 0 && row < HID_PT_MAX_ROWS && view->row_buttons[row] != NULL;
}

void hid_pt_view_set_row_selected(hid_pt_view_t *view, int row, bool selected)
{
    if (!hid_pt_view_has_row(view, row)) {
        return;
    }
    style_row(view->row_buttons[row], selected);
}

void hid_pt_view_set_plug_label(hid_pt_view_t *view, int row, bool plugged)
{
    if (!view || row < 0 || row >= HID_PT_MAX_ROWS || !view->plug_labels[row]) {
        return;
    }
    lv_label_set_text(view->plug_labels[row], plugged ? locstr("Plug out") : locstr("Plug in"));
}

/* ---- focus -------------------------------------------------------------- */

/**
 * Bring the focused control into view.
 *
 * The settings column is taller than the sheet for a DualSense (battery,
 * latency, audio route + warning, two volume sliders, haptics, reset), so the
 * last entries -- "Reset to defaults" in particular -- sit below the fold. The
 * pane scrolls, but nothing was asking it to: LVGL only auto-scrolls to a
 * focused object that carries LV_OBJ_FLAG_SCROLL_ON_FOCUS, which these controls
 * don't, so arrowing down onto the button moved focus somewhere invisible.
 * Recursive, because both the right pane and the settings box can be the one
 * that has to move.
 */
static void scroll_into_view_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    if (target == NULL) {
        return;
    }
    lv_obj_scroll_to_view_recursive(target, LV_ANIM_ON);
}

/** Add to the focus group and keep exactly one scroll-into-view handler on it. */
static void group_add(hid_pt_view_t *view, lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    lv_group_add_obj(view->group, obj);
    /* This runs again on every refresh; drop any previous registration first so
     * the callbacks don't pile up on the same object. */
    lv_obj_remove_event_cb(obj, scroll_into_view_cb);
    lv_obj_add_event_cb(obj, scroll_into_view_cb, LV_EVENT_FOCUSED, view);
}

/**
 * Rebuild the focus group: device rows first, then the option column.
 *
 * lv_group_remove_all_objs() clears obj_focus, and the very next
 * lv_group_add_obj() sees head == tail and calls lv_group_refocus(), which sends
 * LV_EVENT_FOCUSED to plug button 0. Without the guard that reaches the panel's
 * plug-focus handler and overwrites the selection, so the key-based restore the
 * panel does after a re-render has nothing left to restore. The flag only
 * suppresses that *bookkeeping*; LVGL still parks its focus on plug button 0,
 * which is why the panel puts focus back explicitly afterwards.
 */
void hid_pt_view_rebuild_focus_order(hid_pt_view_t *view)
{
    if (!view || !view->group) {
        return;
    }
    view->rebuilding = true;
    lv_group_remove_all_objs(view->group);
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (view->plug_buttons[i]) {
            /* Plug buttons scroll the device list from their own FOCUSED
             * handler, so they do not get the generic one. */
            lv_group_add_obj(view->group, view->plug_buttons[i]);
        }
    }
    group_add(view, view->composite_cb);
    group_add(view, view->auto_plugin_cb);
    group_add(view, view->latency_slider);
    group_add(view, view->audio_dropdown);
    group_add(view, view->speaker_slider);
    group_add(view, view->headset_slider);
    group_add(view, view->haptics_slider);
    group_add(view, view->reset_settings_btn);
    group_add(view, view->refresh_btn);
    group_add(view, view->close_btn);
    view->rebuilding = false;
}

bool hid_pt_view_is_rebuilding(const hid_pt_view_t *view)
{
    return view && view->rebuilding;
}

int hid_pt_view_plug_row_of(const hid_pt_view_t *view, lv_obj_t *obj)
{
    if (!view || !obj) {
        return -1;
    }
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (view->plug_buttons[i] == obj) {
            return i;
        }
    }
    return -1;
}

void hid_pt_view_focus_plug(hid_pt_view_t *view, int row)
{
    if (!view || row < 0 || row >= HID_PT_MAX_ROWS || !view->plug_buttons[row]) {
        return;
    }
    lv_group_focus_obj(view->plug_buttons[row]);
}

void hid_pt_view_scroll_row_into_view(hid_pt_view_t *view, int row)
{
    if (!hid_pt_view_has_row(view, row)) {
        return;
    }
    lv_obj_scroll_to_view(view->row_buttons[row], LV_ANIM_ON);
}

bool hid_pt_view_obj_is_hidden(const hid_pt_view_t *view, lv_obj_t *obj)
{
    if (!view) {
        return false;
    }
    for (lv_obj_t *o = obj; o != NULL && o != view->sheet; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return true;
        }
    }
    return false;
}

/* ---- the audio dropdown's list ------------------------------------------ */

bool hid_pt_view_dropdown_is_open(const hid_pt_view_t *view, lv_obj_t *target)
{
    if (view && view->active_dropdown && lv_dropdown_is_open(view->active_dropdown)) {
        return true;
    }
    return target != NULL && lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target);
}

void hid_pt_view_open_dropdown(hid_pt_view_t *view, lv_obj_t *dropdown)
{
    if (!view || !dropdown) {
        return;
    }
    lv_dropdown_open(dropdown);
    view->active_dropdown = dropdown;
    lv_group_set_editing(view->group, true);
}

void hid_pt_view_close_dropdown(hid_pt_view_t *view, lv_obj_t *dropdown)
{
    if (!view || !dropdown) {
        return;
    }
    view->active_dropdown = NULL;
    lv_group_set_editing(view->group, false);
    if (lv_dropdown_is_open(dropdown)) {
        lv_dropdown_close(dropdown);
    }
    lv_group_focus_obj(dropdown);
}

/* ---- the option column's labels ----------------------------------------- */

void hid_pt_view_update_latency_label(hid_pt_view_t *view, int default_ms)
{
    if (!view || !view->latency_label) {
        return;
    }
    int ms = (int) lv_slider_get_value(view->latency_slider);
    /* The default is passed in rather than named here. A literal in this string
     * has already gone stale once -- the pt-BR translation still carries a msgid
     * claiming 48 ms -- and the model's default is free to vary per controller. */
    lv_label_set_text_fmt(view->latency_label,
                          locstr("Audio/haptics latency — %d ms (default: %d ms)"), ms, default_ms);
}

void hid_pt_view_update_speaker_label(hid_pt_view_t *view)
{
    if (!view || !view->speaker_label) {
        return;
    }
    int pct = (int) lv_slider_get_value(view->speaker_slider);
    lv_label_set_text_fmt(view->speaker_label, locstr("Speaker volume — %d%%"), pct);
}

void hid_pt_view_update_headset_label(hid_pt_view_t *view)
{
    if (!view || !view->headset_label) {
        return;
    }
    int pct = (int) lv_slider_get_value(view->headset_slider);
    lv_label_set_text_fmt(view->headset_label, locstr("Headphone jack volume — %d%%"), pct);
}

void hid_pt_view_update_haptics_label(hid_pt_view_t *view)
{
    if (!view || !view->haptics_label) {
        return;
    }
    int pct = (int) lv_slider_get_value(view->haptics_slider);
    lv_label_set_text_fmt(view->haptics_label, locstr("Haptics strength — %d%% (default: 100%%)"), pct);
}

/* ---- construction ------------------------------------------------------- */

/** Register the shared VALUE_CHANGED + KEY handlers on an option control. */
static void bind_control(hid_pt_view_t *view, lv_obj_t *obj, hid_pt_ctl_t id)
{
    lv_obj_set_user_data(obj, (void *) (intptr_t) id);
    lv_obj_add_event_cb(obj, value_changed_cb, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, view);
}

lv_obj_t *hid_pt_view_create(hid_pt_view_t *view, lv_obj_t *parent, const hid_pt_view_cbs_t *cbs)
{
    if (!view || !cbs) {
        return NULL;
    }
    view->cbs = *cbs;
    view->group = lv_group_create();
    lv_group_set_wrap(view->group, false);

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_50, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sheet = lv_obj_create(cont);
    view->sheet = sheet;
    /* A DualSense fills the settings column (battery, latency, audio route,
     * speaker, headset, haptics, reset); a couple more percent of height keeps
     * that off the scrollbar in the common case. */
    lv_obj_set_size(sheet, LV_DPX(760), LV_PCT(90));
    lv_obj_set_style_min_height(sheet, LV_DPX(400), 0);
    lv_obj_center(sheet);
    lv_obj_set_style_max_width(sheet, LV_PCT(92), 0);
    lv_obj_set_style_max_height(sheet, LV_PCT(92), 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x121a20), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sheet, LV_DPX(12), 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_style_shadow_width(sheet, LV_DPX(24), 0);
    lv_obj_set_style_shadow_opa(sheet, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(sheet, sheet_key_cb, LV_EVENT_KEY, view);

    lv_obj_t *header = lv_obj_create(sheet);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_DPX(56));
    lv_obj_set_style_bg_color(header, lv_color_hex(0x18232c), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, LV_DPX(12), 0);
    lv_obj_set_style_pad_hor(header, LV_DPX(14), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, locstr("HID Devices"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *refresh_btn = lv_btn_create(header);
    view->refresh_btn = refresh_btn;
    lv_obj_set_size(refresh_btn, LV_DPX(88), LV_DPX(36));
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, LV_DPX(-98), 0);
    lv_obj_set_user_data(refresh_btn, (void *) (intptr_t) HID_PT_CTL_REFRESH);
    lv_obj_add_event_cb(refresh_btn, clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(refresh_btn, key_cb, LV_EVENT_KEY, view);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, locstr("Refresh"));
    lv_obj_center(refresh_lbl);

    lv_obj_t *close_btn = lv_btn_create(header);
    view->close_btn = close_btn;
    lv_obj_set_size(close_btn, LV_DPX(88), LV_DPX(36));
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_user_data(close_btn, (void *) (intptr_t) HID_PT_CTL_CLOSE);
    lv_obj_add_event_cb(close_btn, clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(close_btn, key_cb, LV_EVENT_KEY, view);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, locstr("Close"));
    lv_obj_center(close_lbl);

    lv_obj_t *status_row = lv_obj_create(sheet);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_width(status_row, LV_PCT(100));
    lv_obj_set_height(status_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(status_row, LV_DPX(14), 0);
    lv_obj_set_style_pad_bottom(status_row, LV_DPX(4), 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(status_row, LV_DPX(2), 0);

    view->status_label = lv_label_create(status_row);
    lv_label_set_text(view->status_label, locstr("starting"));
    lv_obj_set_style_text_color(view->status_label, lv_color_hex(0xaab6bf), 0);
    lv_obj_set_style_text_font(view->status_label, lv_theme_get_font_small(view->status_label), 0);

    view->error_label = lv_label_create(status_row);
    lv_label_set_text(view->error_label, "");
    lv_obj_set_style_text_color(view->error_label, lv_color_hex(0xf87171), 0);
    lv_obj_set_style_text_font(view->error_label, lv_theme_get_font_small(view->error_label), 0);
    lv_obj_set_width(view->error_label, LV_PCT(100));
    lv_label_set_long_mode(view->error_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(view->error_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *body_row = lv_obj_create(sheet);
    lv_obj_remove_style_all(body_row);
    lv_obj_set_width(body_row, LV_PCT(100));
    lv_obj_set_flex_grow(body_row, 1);
    lv_obj_set_style_bg_opa(body_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(body_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(body_row, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(body_row, LV_DPX(8), 0);
    lv_obj_clear_flag(body_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left_pane = lv_obj_create(body_row);
    lv_obj_remove_style_all(left_pane);
    lv_obj_set_height(left_pane, LV_PCT(100));
    lv_obj_set_flex_grow(left_pane, 4);
    lv_obj_set_style_bg_opa(left_pane, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(left_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left_pane, 0, 0);

    lv_obj_t *devices_title = lv_label_create(left_pane);
    lv_label_set_text(devices_title, locstr("Devices"));
    lv_obj_set_style_text_color(devices_title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_set_style_pad_left(devices_title, LV_DPX(10), 0);
    lv_obj_set_style_pad_top(devices_title, LV_DPX(4), 0);

    view->list = lv_obj_create(left_pane);
    lv_obj_set_width(view->list, LV_PCT(100));
    lv_obj_set_flex_grow(view->list, 1);
    lv_obj_set_style_bg_opa(view->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(view->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(view->list, LV_DPX(6), 0);
    lv_obj_add_flag(view->list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *right_pane = lv_obj_create(body_row);
    lv_obj_remove_style_all(right_pane);
    lv_obj_set_height(right_pane, LV_PCT(100));
    lv_obj_set_flex_grow(right_pane, 6);
    lv_obj_set_style_bg_color(right_pane, lv_color_hex(0x0e1820), 0);
    lv_obj_set_style_bg_opa(right_pane, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(right_pane, LV_DPX(8), 0);
    lv_obj_set_style_border_width(right_pane, 1, 0);
    lv_obj_set_style_border_color(right_pane, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_flex_flow(right_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right_pane, LV_DPX(12), 0);
    lv_obj_add_flag(right_pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(right_pane, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(right_pane, lv_color_hex(0x64748b), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(right_pane, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(right_pane, LV_DPX(4), LV_PART_SCROLLBAR);

    view->composite_row = lv_obj_create(right_pane);
    lv_obj_remove_style_all(view->composite_row);
    lv_obj_set_width(view->composite_row, LV_PCT(100));
    lv_obj_set_height(view->composite_row, LV_DPX(44));
    lv_obj_add_flag(view->composite_row, LV_OBJ_FLAG_HIDDEN);
    view->composite_cb = lv_checkbox_create(view->composite_row);
    lv_checkbox_set_text(view->composite_cb, locstr("Recognize as native Flydigi on PC"));
    lv_obj_set_style_text_color(view->composite_cb, lv_color_hex(0xdbe4ea), 0);
    lv_obj_align(view->composite_cb, LV_ALIGN_LEFT_MID, 0, 0);
    bind_control(view, view->composite_cb, HID_PT_CTL_COMPOSITE);

    view->auto_plugin_row = lv_obj_create(right_pane);
    lv_obj_remove_style_all(view->auto_plugin_row);
    lv_obj_set_width(view->auto_plugin_row, LV_PCT(100));
    lv_obj_set_height(view->auto_plugin_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(view->auto_plugin_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(view->auto_plugin_row, LV_DPX(4), 0);
    lv_obj_add_flag(view->auto_plugin_row, LV_OBJ_FLAG_HIDDEN);
    view->auto_plugin_cb = lv_checkbox_create(view->auto_plugin_row);
    lv_checkbox_set_text(view->auto_plugin_cb,
                         locstr("Auto-Plugin (connect via HID Passthrough on next stream)"));
    lv_obj_set_style_text_color(view->auto_plugin_cb, lv_color_hex(0xdbe4ea), 0);
    lv_obj_set_width(view->auto_plugin_cb, LV_PCT(100));
    bind_control(view, view->auto_plugin_cb, HID_PT_CTL_AUTO_PLUGIN);
    lv_obj_t *auto_plugin_hint = lv_label_create(view->auto_plugin_row);
    lv_label_set_text(auto_plugin_hint,
                      locstr("When unchecked, the controller uses normal Moonlight emulation until you Plug in."));
    lv_label_set_long_mode(auto_plugin_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(auto_plugin_hint, LV_PCT(100));
    lv_obj_set_style_text_color(auto_plugin_hint, lv_color_hex(0x94a3b8), 0);

    view->customize_panel = lv_obj_create(right_pane);
    lv_obj_remove_style_all(view->customize_panel);
    lv_obj_set_width(view->customize_panel, LV_PCT(100));
    lv_obj_set_flex_grow(view->customize_panel, 1);
    lv_obj_set_style_bg_color(view->customize_panel, lv_color_hex(0x152028), 0);
    lv_obj_set_style_bg_opa(view->customize_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->customize_panel, 1, 0);
    lv_obj_set_style_border_color(view->customize_panel, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_style_radius(view->customize_panel, LV_DPX(6), 0);
    lv_obj_set_style_pad_all(view->customize_panel, LV_DPX(12), 0);
    lv_obj_set_flex_flow(view->customize_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->customize_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(view->customize_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->customize_panel, LV_OBJ_FLAG_HIDDEN);
    /* lv_obj_remove_style_all() above took the scrollbar with it, so a pane that
     * had more below the fold gave no hint of it. Put a slim one back. */
    lv_obj_set_scrollbar_mode(view->customize_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(view->customize_panel, lv_color_hex(0x64748b), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(view->customize_panel, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(view->customize_panel, LV_DPX(4), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(view->customize_panel, LV_DPX(2), LV_PART_SCROLLBAR);
    /* Room under the last control so it doesn't sit flush against the border. */
    lv_obj_set_style_pad_bottom(view->customize_panel, LV_DPX(16), 0);

    view->customize_title = lv_label_create(view->customize_panel);
    lv_label_set_text(view->customize_title, locstr("Controller settings"));
    lv_obj_set_style_text_color(view->customize_title, lv_color_hex(0xf5f8fa), 0);

    view->battery_label = lv_label_create(view->customize_panel);
    lv_obj_set_style_text_color(view->battery_label, lv_color_hex(0x7dd3fc), 0);
    lv_obj_add_flag(view->battery_label, LV_OBJ_FLAG_HIDDEN);

    view->latency_label = lv_label_create(view->customize_panel);
    lv_obj_set_style_text_color(view->latency_label, lv_color_hex(0xdbe4ea), 0);
    view->latency_slider = lv_slider_create(view->customize_panel);
    lv_slider_set_range(view->latency_slider, DS_LATENCY_MIN, DS_LATENCY_MAX);
    lv_obj_set_width(view->latency_slider, LV_PCT(100));
    bind_control(view, view->latency_slider, HID_PT_CTL_LATENCY);

    lv_obj_t *audio_lbl = lv_label_create(view->customize_panel);
    lv_label_set_text(audio_lbl, locstr("Audio output"));
    lv_obj_set_style_text_color(audio_lbl, lv_color_hex(0xdbe4ea), 0);
    view->audio_dropdown = lv_dropdown_create(view->customize_panel);
    lv_dropdown_set_options(view->audio_dropdown,
                            locstr("Auto (game decides)\nOff\nController speaker\nHeadphone jack\nSpeaker + jack"));
    lv_obj_set_width(view->audio_dropdown, LV_PCT(100));
    /* Not bind_control(): the dropdown wants its KEY handler WITHOUT
     * LV_EVENT_PREPROCESS, so LVGL's own list handling runs first, plus a second
     * preprocess handler that turns the arrow keys into panel navigation while
     * the list is closed. */
    lv_obj_set_user_data(view->audio_dropdown, (void *) (intptr_t) HID_PT_CTL_AUDIO_MODE);
    lv_obj_add_event_cb(view->audio_dropdown, value_changed_cb, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(view->audio_dropdown, key_cb, LV_EVENT_KEY, view);
    lv_obj_add_event_cb(view->audio_dropdown, dropdown_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, view);

    view->audio_warning_label = lv_label_create(view->customize_panel);
    lv_label_set_long_mode(view->audio_warning_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(view->audio_warning_label, LV_PCT(100));
    lv_obj_set_style_text_color(view->audio_warning_label, lv_color_hex(0xfbbf24), 0);
    lv_obj_add_flag(view->audio_warning_label, LV_OBJ_FLAG_HIDDEN);

    view->speaker_label = lv_label_create(view->customize_panel);
    lv_obj_set_style_text_color(view->speaker_label, lv_color_hex(0xdbe4ea), 0);
    view->speaker_slider = lv_slider_create(view->customize_panel);
    lv_slider_set_range(view->speaker_slider, 0, DS_VOLUME_MAX);
    lv_obj_set_width(view->speaker_slider, LV_PCT(100));
    bind_control(view, view->speaker_slider, HID_PT_CTL_SPEAKER);

    view->headset_label = lv_label_create(view->customize_panel);
    lv_obj_set_style_text_color(view->headset_label, lv_color_hex(0xdbe4ea), 0);
    view->headset_slider = lv_slider_create(view->customize_panel);
    lv_slider_set_range(view->headset_slider, 0, DS_VOLUME_MAX);
    lv_obj_set_width(view->headset_slider, LV_PCT(100));
    bind_control(view, view->headset_slider, HID_PT_CTL_HEADSET);

    view->haptics_row = lv_obj_create(view->customize_panel);
    lv_obj_remove_style_all(view->haptics_row);
    lv_obj_set_width(view->haptics_row, LV_PCT(100));
    lv_obj_set_height(view->haptics_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(view->haptics_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(view->haptics_row, LV_OBJ_FLAG_SCROLLABLE);
    view->haptics_label = lv_label_create(view->haptics_row);
    lv_obj_set_style_text_color(view->haptics_label, lv_color_hex(0xdbe4ea), 0);
    view->haptics_slider = lv_slider_create(view->haptics_row);
    lv_slider_set_range(view->haptics_slider, 0, DS_HAPTICS_MAX);
    lv_obj_set_width(view->haptics_slider, LV_PCT(100));
    bind_control(view, view->haptics_slider, HID_PT_CTL_HAPTICS);

    view->reset_settings_btn = lv_btn_create(view->customize_panel);
    lv_obj_set_size(view->reset_settings_btn, LV_DPX(180), LV_DPX(40));
    lv_obj_set_user_data(view->reset_settings_btn, (void *) (intptr_t) HID_PT_CTL_RESET);
    lv_obj_add_event_cb(view->reset_settings_btn, clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(view->reset_settings_btn, key_cb, LV_EVENT_KEY, view);
    lv_obj_t *reset_lbl = lv_label_create(view->reset_settings_btn);
    lv_label_set_text(reset_lbl, locstr("Reset to defaults"));
    lv_obj_center(reset_lbl);

    hid_pt_view_rebuild_focus_order(view);

    view->container = cont;
    lv_obj_add_event_cb(cont, deleted_cb, LV_EVENT_DELETE, view);
    return cont;
}

void hid_pt_view_destroy(hid_pt_view_t *view)
{
    if (!view) {
        return;
    }
    if (view->group) {
        lv_group_del(view->group);
        view->group = NULL;
    }
}

#endif
