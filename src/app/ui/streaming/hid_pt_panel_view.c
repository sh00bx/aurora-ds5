#if defined(TARGET_WEBOS)

#include "hid_pt_panel_view.h"
#include "overlay_style.h"

#include "util/i18n.h"

#include <stdint.h>
#include <string.h>

/* The sheet's grid. Every actionable row is the same height and puts its control
 * in the same gutter on the right, so from the couch the column of values reads
 * as one vertical line instead of eight differently-shaped rows. */
#define SHEET_W        LV_DPX(880)
#define HEADER_H       LV_DPX(52)
#define FOOTER_H       LV_DPX(38)
#define BODY_PAD       LV_DPX(12)
#define DEV_COL_W      LV_DPX(300)
/* What the sheet's 92 % cap leaves for a body pane once the header, the footer
 * and the body padding are taken off. Both panes stop growing here and scroll. */
#define PANE_MAX_H     LV_DPX(390)
#define DEV_ROW_H      LV_DPX(54)
#define OPT_ROW_H      LV_DPX(36)
#define ROW_GAP        LV_DPX(6)
#define SLIDER_W       LV_DPX(220)
#define VALUE_W        LV_DPX(80)
/* The dropdown spans the track and the number together, so its left edge lands
 * on the same axis every slider starts at. LV_DPX(6) is slab_body()'s gap. */
#define GUTTER_W       (SLIDER_W + LV_DPX(6) + VALUE_W)

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

/* Registered on the sheet, which sits above every control in the tree. It acts
 * only on a key event the sheet itself was the target of, so a key delivered to
 * a control is handled by that control's own registration and not twice. */
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

static void row_focused_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (!view || lv_event_get_code(event) != LV_EVENT_FOCUSED || !view->cbs.row_focused) {
        return;
    }
    view->cbs.row_focused(view->cbs.userdata, row_of(lv_event_get_target(event)));
}

static void deleted_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.deleted) {
        view->cbs.deleted(view->cbs.userdata);
    }
}

/* ---- the shared slab ----------------------------------------------------
 *
 * One shape for everything the cursor can land on, in the list and in the
 * settings column alike: a dark plate, a hairline, and a rail down its leading
 * edge. The rail carries state (teal once a device is bridged); focus is the
 * plate lifting behind a chalk border. See overlay_style.h.
 */

static lv_obj_t *slab_rail(lv_obj_t *slab)
{
    lv_obj_t *rail = lv_obj_create(slab);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, OVERLAY_RAIL_W, LV_PCT(100));
    lv_obj_set_style_bg_color(rail, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    return rail;
}

static void slab_style(lv_obj_t *slab, lv_coord_t height)
{
    lv_obj_remove_style_all(slab);
    lv_obj_set_size(slab, LV_PCT(100), height);
    lv_obj_set_style_bg_color(slab, lv_color_hex(OVERLAY_SLAB), 0);
    lv_obj_set_style_bg_opa(slab, OVERLAY_OPA_SLAB, 0);
    lv_obj_set_style_border_width(slab, LV_DPX(1), 0);
    lv_obj_set_style_border_color(slab, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_border_opa(slab, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(slab, OVERLAY_RADIUS, 0);
    lv_obj_set_style_clip_corner(slab, true, 0);
    lv_obj_set_style_pad_all(slab, 0, 0);
    lv_obj_set_style_pad_gap(slab, 0, 0);
    lv_obj_set_flex_flow(slab, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(slab, LV_OBJ_FLAG_SCROLLABLE);
    /* Focus: the plate lifts and takes a white edge with a bloom behind it. No
     * hue moves, because hue already means something else on this rail.
     *
     * Two states, one look. A device row is focusable itself and gets
     * FOCUS_KEY; a settings row is a plate around a slider or a switch, and the
     * focus is on that child — bind_slab_focus() below mirrors it here as
     * USER_1, so both kinds of row light up the same way. */
    for (int i = 0; i < 2; i++) {
        lv_state_t state = i == 0 ? LV_STATE_FOCUS_KEY : LV_STATE_USER_1;
        lv_obj_set_style_bg_color(slab, lv_color_hex(OVERLAY_SLAB_HI), state);
        lv_obj_set_style_bg_opa(slab, OVERLAY_OPA_SLAB_FOCUS, state);
        lv_obj_set_style_border_color(slab, lv_color_hex(OVERLAY_CHALK), state);
        lv_obj_set_style_border_opa(slab, LV_OPA_COVER, state);
        lv_obj_set_style_shadow_width(slab, LV_DPX(20), state);
        lv_obj_set_style_shadow_color(slab, lv_color_hex(OVERLAY_CHALK), state);
        lv_obj_set_style_shadow_opa(slab, OVERLAY_OPA_BLOOM, state);
    }
    lv_obj_set_style_bg_color(slab, lv_color_hex(OVERLAY_SLAB_HI), LV_STATE_PRESSED);
}

/** Light @p slab up while @p control has the cursor. */
static void slab_focus_cb(lv_event_t *event)
{
    lv_obj_t *slab = lv_event_get_user_data(event);
    if (slab == NULL) {
        return;
    }
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED) {
        lv_obj_add_state(slab, LV_STATE_USER_1);
    } else {
        lv_obj_clear_state(slab, LV_STATE_USER_1);
    }
}

static void bind_slab_focus(lv_obj_t *control, lv_obj_t *slab)
{
    lv_obj_add_event_cb(control, slab_focus_cb, LV_EVENT_FOCUSED, slab);
    lv_obj_add_event_cb(control, slab_focus_cb, LV_EVENT_DEFOCUSED, slab);
}

/** The body of a slab: everything but the rail, inset and vertically centred. */
static lv_obj_t *slab_body(lv_obj_t *slab)
{
    lv_obj_t *body = lv_obj_create(slab);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_left(body, LV_DPX(13), 0);
    lv_obj_set_style_pad_right(body, LV_DPX(15), 0);
    lv_obj_set_style_pad_gap(body, LV_DPX(6), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    return body;
}

/** An all-caps, tracked line. The panel's only second voice. */
static lv_obj_t *eyebrow(lv_obj_t *parent, const char *text, uint32_t colour, lv_opa_t opa)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(parent), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_set_style_text_letter_space(label, LV_DPX(2), 0);
    if (text) {
        lv_label_set_text(label, text);
    }
    return label;
}

static lv_obj_t *body_text(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(OVERLAY_CHALK), 0);
    if (text) {
        lv_label_set_text(label, text);
    }
    return label;
}

/* ---- the device list ---------------------------------------------------- */

void hid_pt_view_list_clear(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_clean(view->list);
    memset(view->row_buttons, 0, sizeof(view->row_buttons));
    memset(view->row_state_labels, 0, sizeof(view->row_state_labels));
    memset(view->row_rails, 0, sizeof(view->row_rails));
}

void hid_pt_view_list_show_empty(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_t *empty = lv_label_create(view->list);
    lv_label_set_text(empty, locstr("No HID devices visible to the native app"));
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(empty, LV_PCT(100));
    lv_obj_set_style_text_color(empty, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_text_opa(empty, OVERLAY_OPA_MUTED, 0);
    lv_obj_set_style_pad_all(empty, LV_DPX(9), 0);
}

void hid_pt_view_list_prepare(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_set_flex_flow(view->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(view->list, ROW_GAP, 0);
}

void hid_pt_view_set_row_state(hid_pt_view_t *view, int row, bool plugged)
{
    if (!view || row < 0 || row >= HID_PT_MAX_ROWS || !view->row_state_labels[row]) {
        return;
    }
    lv_obj_t *state = view->row_state_labels[row];
    lv_label_set_text(state, plugged ? locstr("BRIDGED") : locstr("IDLE"));
    lv_obj_set_style_text_color(state, lv_color_hex(plugged ? OVERLAY_LIVE : OVERLAY_CHALK), 0);
    lv_obj_set_style_text_opa(state, plugged ? LV_OPA_COVER : OVERLAY_OPA_FAINT, 0);
    if (view->row_rails[row]) {
        lv_obj_set_style_bg_color(view->row_rails[row],
                                  lv_color_hex(plugged ? OVERLAY_LIVE : OVERLAY_SEAM), 0);
    }
}

void hid_pt_view_add_row(hid_pt_view_t *view, int i, const char *label, bool plugged, bool selected)
{
    if (!view || !view->list || i < 0 || i >= HID_PT_MAX_ROWS) {
        return;
    }
    /* The row is the control. There is no separate plug button any more: one
     * device, one focus stop, and OK on it does the one thing a device row is
     * for. It halves the number of places the cursor can be. */
    lv_obj_t *row = lv_btn_create(view->list);
    view->row_buttons[i] = row;
    slab_style(row, DEV_ROW_H);
    lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(row, row_focused_cb, LV_EVENT_FOCUSED, view);
    lv_obj_add_event_cb(row, key_cb, LV_EVENT_KEY, view);
    lv_obj_set_user_data(row, (void *) (intptr_t) i);

    view->row_rails[i] = slab_rail(row);

    lv_obj_t *body = slab_body(row);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, LV_DPX(3), 0);

    lv_obj_t *name = body_text(body, label);
    /* One line, cut with an ellipsis. LONG_DOT wraps first and only dots once it
     * runs out of HEIGHT, so a two-word-too-long name ("Logitech USB Receiver
     * System Control") took a second line and shoved the state line out of the
     * row. Pinning the height to one line is what makes it truncate instead. */
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_height(name, lv_font_get_line_height(lv_obj_get_style_text_font(name, LV_PART_MAIN)));

    view->row_state_labels[i] = eyebrow(body, NULL, OVERLAY_CHALK, OVERLAY_OPA_FAINT);
    hid_pt_view_set_row_state(view, i, plugged);
    hid_pt_view_set_row_selected(view, i, selected);
}

bool hid_pt_view_has_row(const hid_pt_view_t *view, int row)
{
    return view && row >= 0 && row < HID_PT_MAX_ROWS && view->row_buttons[row] != NULL;
}

/**
 * Mark @p row as the device the settings column is showing.
 *
 * Distinct from focus on purpose: the cursor can be off in the settings column
 * while this row stays the one being edited, and then it is the only thing on
 * the left saying which device that is.
 */
void hid_pt_view_set_row_selected(hid_pt_view_t *view, int row, bool selected)
{
    if (!hid_pt_view_has_row(view, row)) {
        return;
    }
    lv_obj_t *slab = view->row_buttons[row];
    lv_obj_set_style_bg_color(slab, lv_color_hex(selected ? OVERLAY_SLAB_SEL : OVERLAY_SLAB), 0);
    lv_obj_set_style_border_opa(slab, selected ? LV_OPA_COVER : 160, 0);
}

/* ---- focus -------------------------------------------------------------- */

/**
 * Bring the focused control into view.
 *
 * The settings column is built to fit without scrolling — that is the point of
 * the single-line rows — but a small display, a long translation or a device
 * with more controls than a DualSense can still overflow it, and the device list
 * scrolls by nature. One handler on one scroll container each: the old panel had
 * a pane inside a pane, both scrollable, and the two took turns moving.
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

#define OPTION_CHAIN_LEN 8

/**
 * The option column, top to bottom, into @p out.
 *
 * One list, used for the focus group's order and for stepping the cursor, so the
 * two can't disagree. Entries that are hidden for the selected device are
 * skipped by the stepper, not removed from here.
 */
static void option_chain(const hid_pt_view_t *view, lv_obj_t *out[OPTION_CHAIN_LEN])
{
    out[0] = view->auto_plugin_cb;
    out[1] = view->composite_cb;
    out[2] = view->audio_dropdown;
    out[3] = view->speaker_slider;
    out[4] = view->headset_slider;
    out[5] = view->haptics_slider;
    out[6] = view->latency_slider;
    out[7] = view->reset_settings_btn;
}

/**
 * Rebuild the focus group: device rows first, then the option column.
 *
 * lv_group_remove_all_objs() clears obj_focus, and the very next
 * lv_group_add_obj() sees head == tail and calls lv_group_refocus(), which sends
 * LV_EVENT_FOCUSED to row 0. Without the guard that reaches the panel's
 * row-focus handler and overwrites the selection, so the key-based restore the
 * panel does after a re-render has nothing left to restore. The flag only
 * suppresses that *bookkeeping*; LVGL still parks its focus on row 0, which is
 * why the panel puts focus back explicitly afterwards.
 */
void hid_pt_view_rebuild_focus_order(hid_pt_view_t *view)
{
    if (!view || !view->group) {
        return;
    }
    view->rebuilding = true;
    lv_group_remove_all_objs(view->group);
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (view->row_buttons[i]) {
            /* Rows scroll the device list from their own FOCUSED handler, so
             * they do not get the generic one. */
            lv_group_add_obj(view->group, view->row_buttons[i]);
        }
    }
    lv_obj_t *chain[OPTION_CHAIN_LEN];
    option_chain(view, chain);
    for (size_t i = 0; i < OPTION_CHAIN_LEN; ++i) {
        group_add(view, chain[i]);
    }
    group_add(view, view->refresh_btn);
    group_add(view, view->close_btn);
    view->rebuilding = false;
}

bool hid_pt_view_is_rebuilding(const hid_pt_view_t *view)
{
    return view && view->rebuilding;
}

hid_pt_widget_kind_t hid_pt_view_kind_of(const hid_pt_view_t *view, lv_obj_t *obj)
{
    if (!view || !obj) {
        return HID_PT_WK_NONE;
    }
    if (hid_pt_view_row_of(view, obj) >= 0) {
        return HID_PT_WK_ROW;
    }
    const struct {
        lv_obj_t *const *slot;
        hid_pt_widget_kind_t kind;
    } table[] = {
            {&view->composite_cb,       HID_PT_WK_SWITCH},
            {&view->auto_plugin_cb,     HID_PT_WK_SWITCH},
            {&view->latency_slider,     HID_PT_WK_SLIDER},
            {&view->speaker_slider,     HID_PT_WK_SLIDER},
            {&view->headset_slider,     HID_PT_WK_SLIDER},
            {&view->haptics_slider,     HID_PT_WK_SLIDER},
            {&view->audio_dropdown,     HID_PT_WK_DROPDOWN},
            {&view->reset_settings_btn, HID_PT_WK_OPTION_BTN},
            {&view->refresh_btn,        HID_PT_WK_HEADER_BTN},
            {&view->close_btn,          HID_PT_WK_HEADER_BTN},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (*table[i].slot == obj) {
            return table[i].kind;
        }
    }
    return HID_PT_WK_NONE;
}

hid_pt_zone_t hid_pt_view_zone_of(const hid_pt_view_t *view, lv_obj_t *obj)
{
    hid_pt_widget_kind_t kind = hid_pt_view_kind_of(view, obj);
    if (kind == HID_PT_WK_HEADER_BTN) {
        return HID_PT_ZONE_HEADER;
    }
    if (hid_pt_view_kind_is_option(kind)) {
        return HID_PT_ZONE_OPTIONS;
    }
    return HID_PT_ZONE_LIST;
}

int hid_pt_view_row_of(const hid_pt_view_t *view, lv_obj_t *obj)
{
    if (!view || !obj) {
        return -1;
    }
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (view->row_buttons[i] == obj) {
            return i;
        }
    }
    return -1;
}

void hid_pt_view_focus_row(hid_pt_view_t *view, int row)
{
    if (!view || row < 0 || row >= HID_PT_MAX_ROWS || !view->row_buttons[row]) {
        return;
    }
    lv_group_focus_obj(view->row_buttons[row]);
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

lv_obj_t *hid_pt_view_first_option(const hid_pt_view_t *view)
{
    if (!view) {
        return NULL;
    }
    lv_obj_t *chain[OPTION_CHAIN_LEN];
    option_chain(view, chain);
    for (size_t i = 0; i < OPTION_CHAIN_LEN; ++i) {
        if (chain[i] && !hid_pt_view_obj_is_hidden(view, chain[i])) {
            return chain[i];
        }
    }
    return NULL;
}

lv_obj_t *hid_pt_view_step_option(const hid_pt_view_t *view, lv_obj_t *from, int step)
{
    if (!view || !from || step == 0) {
        return NULL;
    }
    lv_obj_t *chain[OPTION_CHAIN_LEN];
    option_chain(view, chain);
    int at = -1;
    for (size_t i = 0; i < OPTION_CHAIN_LEN; ++i) {
        if (chain[i] == from) {
            at = (int) i;
            break;
        }
    }
    if (at < 0) {
        return NULL;
    }
    for (int i = at + step; i >= 0 && i < OPTION_CHAIN_LEN; i += step) {
        if (chain[i] && !hid_pt_view_obj_is_hidden(view, chain[i])) {
            return chain[i];
        }
    }
    return NULL;
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

/** The number in a percentage row's gutter. */
static void set_percent(lv_obj_t *value, lv_obj_t *slider)
{
    if (!value || !slider) {
        return;
    }
    lv_label_set_text_fmt(value, "%d %%", (int) lv_slider_get_value(slider));
}

void hid_pt_view_update_latency_label(hid_pt_view_t *view, int default_ms)
{
    if (!view || !view->latency_value) {
        return;
    }
    lv_label_set_text_fmt(view->latency_value, locstr("%d ms"),
                          (int) lv_slider_get_value(view->latency_slider));
    /* The default is named in the row's label rather than hardcoded in the
     * string: a literal here has already gone stale once — the pt-BR catalogue
     * still carries a msgid claiming 48 ms — and the model's default is free to
     * vary per controller.
     *
     * It only moves with the selected device, while this runs on every step of
     * the slider and on the panel's 2 s refresh, so the caption is written only
     * when it actually changes. */
    if (view->latency_label &&
        (!view->latency_default_valid || view->latency_default_ms != default_ms)) {
        lv_label_set_text_fmt(view->latency_label, locstr("Latency · default %d ms"), default_ms);
        view->latency_default_ms = default_ms;
        view->latency_default_valid = true;
    }
}

void hid_pt_view_update_speaker_label(hid_pt_view_t *view)
{
    if (view) {
        set_percent(view->speaker_value, view->speaker_slider);
    }
}

void hid_pt_view_update_headset_label(hid_pt_view_t *view)
{
    if (view) {
        set_percent(view->headset_value, view->headset_slider);
    }
}

void hid_pt_view_update_haptics_label(hid_pt_view_t *view)
{
    if (view) {
        set_percent(view->haptics_value, view->haptics_slider);
    }
}

bool hid_pt_view_nudge_slider(hid_pt_view_t *view, lv_obj_t *obj, int dir)
{
    if (!view || hid_pt_view_kind_of(view, obj) != HID_PT_WK_SLIDER) {
        return false;
    }
    /* One press moves a fortieth of the range: five points on a percentage, five
     * milliseconds on the latency. Held down, LVGL's key repeat walks the whole
     * range in about a second. */
    int32_t min = lv_slider_get_min_value(obj);
    int32_t max = lv_slider_get_max_value(obj);
    int32_t step = (max - min) / 40;
    if (step < 1) {
        step = 1;
    }
    int32_t next = lv_slider_get_value(obj) + (int32_t) dir * step;
    if (next < min) {
        next = min;
    } else if (next > max) {
        next = max;
    }
    if (next == lv_slider_get_value(obj)) {
        return true;
    }
    lv_slider_set_value(obj, next, LV_ANIM_OFF);
    lv_event_send(obj, LV_EVENT_VALUE_CHANGED, NULL);
    return true;
}

void hid_pt_view_set_hints(hid_pt_view_t *view, hid_pt_zone_t zone, bool plugged)
{
    if (!view || !view->hint_label) {
        return;
    }
    /* Every arrow key asks for the hints again, but only a move between zones —
     * or plugging the selected device in or out — can change them. Rewriting the
     * label re-measures the whole line and dirties the layout, so the common case
     * (stepping to the next row, the next slider) stops here. */
    if (view->hint_valid && view->hint_zone == zone && view->hint_plugged == plugged) {
        return;
    }
    view->hint_zone = zone;
    view->hint_plugged = plugged;
    view->hint_valid = true;
    /* One whole sentence per case rather than assembled fragments: a translator
     * gets to see the line they are translating. */
    const char *text;
    switch (zone) {
        case HID_PT_ZONE_OPTIONS:
            text = locstr("UP/DOWN  setting        LEFT/RIGHT  adjust        BACK  devices");
            break;
        case HID_PT_ZONE_HEADER:
            text = locstr("LEFT/RIGHT  choose        OK  run        BACK  close");
            break;
        case HID_PT_ZONE_LIST:
        default:
            text = plugged
                   ? locstr("UP/DOWN  device        OK  unplug        RIGHT  settings        BACK  close")
                   : locstr("UP/DOWN  device        OK  plug in        RIGHT  settings        BACK  close");
            break;
    }
    lv_label_set_text(view->hint_label, text);
}

/* ---- construction ------------------------------------------------------- */

/** Register the shared VALUE_CHANGED + KEY handlers on an option control. */
static void bind_control(hid_pt_view_t *view, lv_obj_t *obj, hid_pt_ctl_t id)
{
    lv_obj_set_user_data(obj, (void *) (intptr_t) id);
    lv_obj_add_event_cb(obj, value_changed_cb, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, view);
}

/** A quiet outlined button, for the header and for Reset. */
static lv_obj_t *ghost_button(hid_pt_view_t *view, lv_obj_t *parent, const char *text, hid_pt_ctl_t id)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_DPX(30));
    lv_obj_set_style_radius(btn, LV_DPX(5), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, LV_DPX(1), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(btn, LV_DPX(13), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(OVERLAY_SLAB_HI), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(btn, lv_color_hex(OVERLAY_CHALK), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_user_data(btn, (void *) (intptr_t) id);
    lv_obj_add_event_cb(btn, clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(btn, key_cb, LV_EVENT_KEY, view);

    lv_obj_t *label = eyebrow(btn, text, OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_center(label);
    return btn;
}

/**
 * A settings row: label on the left, its control in the shared right gutter.
 *
 * Returns the slab, which is what the view stores and what focus lights up;
 * @p body_out takes the inset strip inside it that the control is added to.
 */
static lv_obj_t *option_row(lv_obj_t *parent, const char *label, lv_obj_t **body_out,
                            lv_obj_t **label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    slab_style(row, OPT_ROW_H);
    slab_rail(row);
    lv_obj_t *body = slab_body(row);

    lv_obj_t *name = body_text(body, label);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
    if (label_out) {
        *label_out = name;
    }
    if (body_out) {
        *body_out = body;
    }
    return row;
}

/**
 * A settings row whose control is a switch, not a checkbox — it lands in the
 * same right-hand gutter as every other control, and it is a bigger target from
 * the couch. Starts hidden; the panel shows it for a device that has the setting.
 */
static lv_obj_t *switch_row(hid_pt_view_t *view, lv_obj_t *parent, const char *label,
                            hid_pt_ctl_t id, lv_obj_t **switch_out)
{
    lv_obj_t *body;
    lv_obj_t *row = option_row(parent, label, &body, NULL);

    lv_obj_t *sw = lv_switch_create(body);
    lv_obj_set_size(sw, LV_DPX(44), LV_DPX(22));
    lv_obj_set_style_bg_color(sw, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_bg_opa(sw, 40, 0);
    /* The filled half of a switch is its INDICATOR, not its background — leaving
     * that part unstyled is why the toggle came out in the theme's blue while
     * every other "this is on" mark in the sheet is teal. */
    lv_obj_set_style_bg_color(sw, lv_color_hex(OVERLAY_LIVE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, 110, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(OVERLAY_CHALK), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, 190, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(OVERLAY_LIVE), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    bind_slab_focus(sw, row);
    bind_control(view, sw, id);
    if (switch_out) {
        *switch_out = sw;
    }
    return row;
}

/** A slider row: label, then the track and the number, both on the gutter. */
static lv_obj_t *slider_row(hid_pt_view_t *view, lv_obj_t *parent, const char *label, int32_t min,
                            int32_t max, hid_pt_ctl_t id, lv_obj_t **slider_out, lv_obj_t **value_out,
                            lv_obj_t **label_out)
{
    lv_obj_t *body;
    lv_obj_t *row = option_row(parent, label, &body, label_out);

    lv_obj_t *slider = lv_slider_create(body);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_size(slider, SLIDER_W, LV_DPX(6));
    lv_obj_set_style_bg_color(slider, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_bg_opa(slider, 40, 0);
    lv_obj_set_style_radius(slider, LV_DPX(3), 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(OVERLAY_LIVE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_DPX(3), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(OVERLAY_CHALK), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, 190, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, LV_DPX(5), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    /* The knob only grows once the row owns the arrow keys, so the row that is
     * being adjusted is obvious even out of the corner of the eye. */
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_all(slider, LV_DPX(8), LV_PART_KNOB | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_width(slider, LV_DPX(12), LV_PART_KNOB | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_color(slider, lv_color_hex(OVERLAY_CHALK), LV_PART_KNOB | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_opa(slider, 120, LV_PART_KNOB | LV_STATE_FOCUS_KEY);
    bind_slab_focus(slider, row);
    bind_control(view, slider, id);
    if (slider_out) {
        *slider_out = slider;
    }

    lv_obj_t *value = body_text(body, "-");
    lv_obj_set_width(value, VALUE_W);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    if (value_out) {
        *value_out = value;
    }
    return row;
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
    lv_obj_set_style_bg_color(cont, lv_color_hex(OVERLAY_INK), 0);
    lv_obj_set_style_bg_opa(cont, OVERLAY_OPA_VEIL, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sheet = lv_obj_create(cont);
    view->sheet = sheet;
    /* As tall as it needs to be, not as tall as it is allowed to be. A fixed 92 %
     * left a third of the sheet empty under the last setting, which is most of
     * what made a five-row panel feel like a takeover of the screen. */
    lv_obj_set_size(sheet, SHEET_W, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(sheet, LV_DPX(300), 0);
    lv_obj_center(sheet);
    lv_obj_set_style_max_width(sheet, LV_PCT(96), 0);
    lv_obj_set_style_max_height(sheet, LV_PCT(92), 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(OVERLAY_INK), 0);
    lv_obj_set_style_bg_opa(sheet, OVERLAY_OPA_SHEET, 0);
    lv_obj_set_style_radius(sheet, LV_DPX(10), 0);
    lv_obj_set_style_clip_corner(sheet, true, 0);
    lv_obj_set_style_border_width(sheet, LV_DPX(1), 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_shadow_width(sheet, LV_DPX(30), 0);
    lv_obj_set_style_shadow_opa(sheet, LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(sheet, lv_color_black(), 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_set_style_pad_gap(sheet, 0, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(sheet, sheet_key_cb, LV_EVENT_KEY, view);

    /* ---- header ---- */
    lv_obj_t *header = lv_obj_create(sheet);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_bg_opa(header, OVERLAY_OPA_BAR, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, LV_DPX(1), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_border_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(header, LV_DPX(16), 0);
    lv_obj_set_style_pad_gap(header, LV_DPX(10), 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_block = lv_obj_create(header);
    lv_obj_remove_style_all(title_block);
    lv_obj_set_size(title_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(title_block, LV_DPX(2), 0);
    lv_obj_clear_flag(title_block, LV_OBJ_FLAG_SCROLLABLE);
    eyebrow(title_block, locstr("HID PASSTHROUGH"), OVERLAY_CHALK, OVERLAY_OPA_FAINT);
    lv_obj_t *title = body_text(title_block, locstr("Controllers"));
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title_block), 0);

    /* The bridge's own words, kept where they belong to the sheet as a whole
     * rather than to any one device. */
    view->status_label = lv_label_create(header);
    lv_label_set_text(view->status_label, locstr("starting"));
    lv_obj_set_style_text_color(view->status_label, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_text_opa(view->status_label, OVERLAY_OPA_MUTED, 0);
    lv_obj_set_style_text_font(view->status_label, lv_theme_get_font_small(header), 0);
    lv_obj_set_style_text_align(view->status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(view->status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(view->status_label, 1);

    view->refresh_btn = ghost_button(view, header, locstr("RESCAN"), HID_PT_CTL_REFRESH);
    view->close_btn = ghost_button(view, header, locstr("CLOSE"), HID_PT_CTL_CLOSE);

    /* ---- error bar: only there when the bridge has something to say ---- */
    view->error_row = lv_obj_create(sheet);
    lv_obj_remove_style_all(view->error_row);
    lv_obj_set_size(view->error_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(view->error_row, LV_DPX(16), 0);
    lv_obj_set_style_pad_ver(view->error_row, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(view->error_row, lv_color_hex(OVERLAY_ALERT), 0);
    lv_obj_set_style_bg_opa(view->error_row, 30, 0);
    lv_obj_add_flag(view->error_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(view->error_row, LV_OBJ_FLAG_SCROLLABLE);
    view->error_label = lv_label_create(view->error_row);
    lv_label_set_text(view->error_label, "");
    lv_obj_set_style_text_color(view->error_label, lv_color_hex(OVERLAY_ALERT), 0);
    lv_obj_set_style_text_font(view->error_label, lv_theme_get_font_small(view->error_row), 0);
    lv_obj_set_width(view->error_label, LV_PCT(100));
    lv_label_set_long_mode(view->error_label, LV_LABEL_LONG_WRAP);

    /* ---- body: devices left, the selected device's settings right ---- */
    lv_obj_t *body_row = lv_obj_create(sheet);
    lv_obj_remove_style_all(body_row);
    lv_obj_set_size(body_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(body_row, BODY_PAD, 0);
    lv_obj_set_style_pad_gap(body_row, LV_DPX(12), 0);
    lv_obj_clear_flag(body_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left_pane = lv_obj_create(body_row);
    lv_obj_remove_style_all(left_pane);
    lv_obj_set_size(left_pane, DEV_COL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(left_pane, LV_DPX(8), 0);
    lv_obj_clear_flag(left_pane, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *devices_title = eyebrow(left_pane, locstr("DEVICES"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(devices_title, LV_DPX(3), 0);

    view->list = lv_obj_create(left_pane);
    lv_obj_remove_style_all(view->list);
    lv_obj_set_size(view->list, LV_PCT(100), LV_SIZE_CONTENT);
    /* The cap the sheet's own 92 % works out to, minus its bars. Past it the list
     * scrolls instead of pushing the sheet off the screen. */
    lv_obj_set_style_max_height(view->list, PANE_MAX_H, 0);
    lv_obj_set_style_pad_right(view->list, LV_DPX(4), 0);
    lv_obj_add_flag(view->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(view->list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(view->list, lv_color_hex(OVERLAY_CHALK), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(view->list, 60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(view->list, LV_DPX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(view->list, LV_DPX(1), LV_PART_SCROLLBAR);

    lv_obj_t *right_pane = lv_obj_create(body_row);
    lv_obj_remove_style_all(right_pane);
    lv_obj_set_height(right_pane, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(right_pane, PANE_MAX_H, 0);
    lv_obj_set_flex_grow(right_pane, 1);
    lv_obj_set_flex_flow(right_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(right_pane, ROW_GAP, 0);
    /* Sized to fit at 1080p — that is what the single-line rows buy. The scroll
     * is the fallback for a smaller panel or a longer translation, and it is the
     * only scroll container on this side now. */
    lv_obj_add_flag(right_pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(right_pane, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(right_pane, lv_color_hex(OVERLAY_CHALK), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(right_pane, 60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(right_pane, LV_DPX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(right_pane, LV_DPX(1), LV_PART_SCROLLBAR);
    view->customize_panel = right_pane;

    /* Device header: who is being edited, and how it is doing. */
    lv_obj_t *head_row = lv_obj_create(right_pane);
    lv_obj_remove_style_all(head_row);
    lv_obj_set_size(head_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(head_row, LV_DPX(3), 0);
    lv_obj_set_style_pad_bottom(head_row, LV_DPX(2), 0);
    lv_obj_clear_flag(head_row, LV_OBJ_FLAG_SCROLLABLE);

    view->customize_title = body_text(head_row, locstr("Controller settings"));
    lv_obj_set_style_text_font(view->customize_title, lv_theme_get_font_large(head_row), 0);
    lv_label_set_long_mode(view->customize_title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(view->customize_title, 1);
    view->reset_settings_btn = ghost_button(view, head_row, locstr("RESET"), HID_PT_CTL_RESET);

    /* State and battery on one line: recoloured so "BRIDGED" carries the same
     * teal as the rail on the device's row. */
    view->customize_state = eyebrow(right_pane, "", OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_label_set_recolor(view->customize_state, true);
    lv_obj_set_style_pad_left(view->customize_state, LV_DPX(3), 0);
    lv_obj_set_style_pad_bottom(view->customize_state, LV_DPX(4), 0);

    view->auto_plugin_row = switch_row(view, right_pane, locstr("Auto-plug on next stream"),
                                       HID_PT_CTL_AUTO_PLUGIN, &view->auto_plugin_cb);
    view->composite_row = switch_row(view, right_pane, locstr("Recognize as native Flydigi on PC"),
                                     HID_PT_CTL_COMPOSITE, &view->composite_cb);

    view->audio_heading = eyebrow(right_pane, locstr("AUDIO & HAPTICS"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(view->audio_heading, LV_DPX(3), 0);
    lv_obj_set_style_pad_top(view->audio_heading, LV_DPX(4), 0);

    lv_obj_t *audio_body;
    view->audio_row = option_row(right_pane, locstr("Audio output"), &audio_body, NULL);
    view->audio_dropdown = lv_dropdown_create(audio_body);
    lv_dropdown_set_options(view->audio_dropdown,
                            locstr("Auto (game decides)\nOff\nController speaker\nHeadphone jack\nSpeaker + jack"));
    /* No box of its own: the row already is the box. The theme gives a dropdown a
     * filled plate, a border and a blue focus outline, which next to four bare
     * slider rows made this one row look like a different design — and the plate
     * clipped its own text, because the theme's vertical padding is written for a
     * content-sized dropdown, not one that has to fit a fixed row.
     *
     * So: transparent, borderless, its own padding, and the value right-aligned
     * on the same axis every slider's number sits on. */
    lv_obj_set_size(view->audio_dropdown, GUTTER_W, LV_DPX(26));
    lv_obj_set_style_bg_opa(view->audio_dropdown, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->audio_dropdown, 0, 0);
    lv_obj_set_style_outline_width(view->audio_dropdown, 0, 0);
    lv_obj_set_style_outline_width(view->audio_dropdown, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_width(view->audio_dropdown, 0, 0);
    lv_obj_set_style_text_color(view->audio_dropdown, lv_color_hex(OVERLAY_CHALK), 0);
    /* lv_dropdown draws its text at pad_top and its symbol against the right
     * edge, and ignores text_align entirely — so the padding IS the layout: the
     * value starts where every slider's track starts, the chevron ends where
     * every number ends. */
    lv_obj_set_style_pad_hor(view->audio_dropdown, 0, 0);
    lv_obj_set_style_pad_ver(view->audio_dropdown, LV_DPX(5), 0);
    lv_obj_set_style_text_color(view->audio_dropdown, lv_color_hex(OVERLAY_CHALK), LV_PART_INDICATOR);
    lv_obj_set_style_text_opa(view->audio_dropdown, OVERLAY_OPA_MUTED, LV_PART_INDICATOR);
    bind_slab_focus(view->audio_dropdown, view->audio_row);
    /* Not bind_control(): the dropdown wants its KEY handler WITHOUT
     * LV_EVENT_PREPROCESS, so LVGL's own list handling runs first, plus a second
     * preprocess handler that turns the arrow keys into panel navigation while
     * the list is closed. */
    lv_obj_set_user_data(view->audio_dropdown, (void *) (intptr_t) HID_PT_CTL_AUDIO_MODE);
    lv_obj_add_event_cb(view->audio_dropdown, value_changed_cb, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(view->audio_dropdown, key_cb, LV_EVENT_KEY, view);
    lv_obj_add_event_cb(view->audio_dropdown, dropdown_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, view);

    view->speaker_row = slider_row(view, right_pane, locstr("Speaker volume"), 0, DS_VOLUME_MAX,
                                   HID_PT_CTL_SPEAKER, &view->speaker_slider, &view->speaker_value, NULL);
    view->headset_row = slider_row(view, right_pane, locstr("Headphone volume"), 0, DS_VOLUME_MAX,
                                   HID_PT_CTL_HEADSET, &view->headset_slider, &view->headset_value, NULL);
    view->haptics_row = slider_row(view, right_pane, locstr("Haptics strength"), 0, DS_HAPTICS_MAX,
                                   HID_PT_CTL_HAPTICS, &view->haptics_slider, &view->haptics_value, NULL);
    view->latency_row = slider_row(view, right_pane, locstr("Latency"), DS_LATENCY_MIN, DS_LATENCY_MAX,
                                   HID_PT_CTL_LATENCY, &view->latency_slider, &view->latency_value,
                                   &view->latency_label);

    /* The advisory sits under the settings it is about, one quiet line rather
     * than a coloured block: it is a consequence to know, not an error. */
    view->audio_warning_label = lv_label_create(right_pane);
    lv_label_set_long_mode(view->audio_warning_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(view->audio_warning_label, LV_PCT(100));
    lv_obj_set_style_text_font(view->audio_warning_label, lv_theme_get_font_small(right_pane), 0);
    lv_obj_set_style_text_color(view->audio_warning_label, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_text_opa(view->audio_warning_label, OVERLAY_OPA_MUTED, 0);
    lv_obj_set_style_pad_left(view->audio_warning_label, LV_DPX(3), 0);
    lv_obj_set_style_pad_top(view->audio_warning_label, LV_DPX(4), 0);
    lv_label_set_recolor(view->audio_warning_label, true);
    lv_obj_add_flag(view->audio_warning_label, LV_OBJ_FLAG_HIDDEN);

    /* ---- footer: what the four keys do, right here, right now ---- */
    lv_obj_t *footer = lv_obj_create(sheet);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), FOOTER_H);
    lv_obj_set_style_bg_color(footer, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_bg_opa(footer, OVERLAY_OPA_BAR, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, LV_DPX(1), 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_border_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(footer, LV_DPX(16), 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    view->hint_label = eyebrow(footer, NULL, OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_center(view->hint_label);
    hid_pt_view_set_hints(view, HID_PT_ZONE_LIST, false);

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
