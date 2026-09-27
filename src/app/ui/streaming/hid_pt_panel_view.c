#if defined(TARGET_WEBOS)

#include "hid_pt_panel_view.h"
#include "lightbar_colour.h"
#include "overlay_style.h"

#include "util/font.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "util/i18n.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The sheet's grid. Every actionable row is the same height and puts its control
 * in the same gutter on the right, so from the couch the column of values reads
 * as one vertical line instead of eight differently-shaped rows. */
#define SHEET_W        LV_DPX(880)
#define HEADER_H       LV_DPX(52)
#define BODY_PAD       LV_DPX(12)
/* Header + padded body at its pane cap: the sheet's one, static height. */
#define SHEET_H        (HEADER_H + 2 * BODY_PAD + PANE_MAX_H)
#define DEV_COL_W      LV_DPX(300)
/* What the sheet's 92 % cap leaves for a body pane once the header and the
 * body padding are taken off (the key-hint footer is gone, its 38dpx returned
 * to the panes). It sizes the sheet; the panes measure themselves against the
 * body box they actually get, which is this minus the error bar when it is up. */
#define PANE_MAX_H     LV_DPX(428)
#define DEV_ROW_H      LV_DPX(54)
#define OPT_ROW_H      LV_DPX(30)
/* Between device rows. The settings column is tighter (OPT_GAP): its tallest
 * case has to fit the pane without scrolling, see below. */
#define ROW_GAP        LV_DPX(6)
#define OPT_GAP        LV_DPX(4)
#define SLIDER_W       LV_DPX(220)
#define VALUE_W        LV_DPX(80)
/* The dropdown spans the track and the number together, so its left edge lands
 * on the same axis every slider starts at. LV_DPX(6) is slab_body()'s gap. */
#define GUTTER_W       (SLIDER_W + LV_DPX(6) + VALUE_W)
/* A mode button: the large icon (19dpx, a Material em is its line) over its
 * one-word name (a small line, 14dpx * 1.2 in Museo Sans) -- 36.8dpx of text.
 * Six share the row, flex-grown over the settings column's 1084 px at
 * 1920x1080 (sheet 1760, less its border 4, the device column 600, the body
 * padding 48 and the gap 24): (1084 - 5 x OPT_GAP 8) / 6 = 174 px each. The
 * longest name, "SWITCH", is six small capitals at 28 px with 4 px tracking,
 * about 125 px, and the icon 38: both fit with room, so the buttons keep their
 * padding and the label its size. Without a game (no GAME) five take 210. */
#define MODE_BTN_H     LV_DPX(42)
/* A slider in a paired row (two side by side): half a row leaves the track
 * and the number this much, the label the rest. The number has to hold
 * "200 %" on one line: ~90 px in the page's 32 px Museo Sans. */
#define PAIR_SLIDER_W  LV_DPX(62)
#define PAIR_VALUE_W   LV_DPX(48)
/* A lightbar swatch: a disc smaller than a switch is tall, with its ring
 * (SWATCH_RING + SWATCH_RING_PAD, 8 px a side) around it. The LIGHTBAR slab
 * holds two lines of them, SWATCH_LINE_GAP apart: 36 + 16 + 36 of its 108 px,
 * which leaves 10 px above and below -- an outer ring ends on the slab's
 * border, as it did in 1.7.30's one-line row of 40 px discs -- and 8 px between
 * a ring and the disc in the other line. */
#define SWATCH_D       LV_DPX(18)
#define SWATCH_GAP     LV_DPX(6)
#define SWATCH_RING    LV_DPX(2)
#define SWATCH_RING_PAD LV_DPX(2)
#define SWATCH_LINE_GAP LV_DPX(8)
#define LIGHTBAR_ROW_H LV_DPX(54)

/* The settings column fits its pane, so it never scrolls and no scrollbar ever
 * shows. The TV draws the UI at 1920x1080, dpi = 1920 / 6 = 320, so one dpx is
 * 2 px. The sheet is min(SHEET_H 1008, 92 % of 1080 = 993) = 993 px; less its
 * border (2 x 2), the header (104) and the body padding (2 x 24), a pane gets
 * 837 px, or 779 with the error bar up (58). Its tallest case, a DualSense
 * mounted in a game with a fixed mode, is eleven children with ten OPT_GAPs
 * of 8:
 *
 *   head (title 46 + 4 + state line 34, pad 4)   88
 *   MODE eyebrow                                  34
 *   mode row                                      84
 *   "Locked for <game>" (one small line)          34
 *   LIGHTBAR · <mode> eyebrow (pad 8)             42
 *   two lines of swatches + "Game may change
 *   colour" (LIGHTBAR_ROW_H)                     108
 *   AUDIO & HAPTICS eyebrow (pad 8)               42
 *   audio, speaker | headphones,
 *   haptics | soften triggers, latency: 4 x 60   240
 *   gaps: 10 x 8                                  80
 *                                                ---
 *                                                752
 *
 * which leaves 85 px (27 under the error bar). The audio advisory (two small
 * lines, pad 8, + a gap: 84) fits without the error bar: 836 of 837. A
 * DualShock 4 is 684 (no haptics pair), a Flydigi with its composite switch
 * 290, a DualSense on SDL with no bridge behind it 430.
 *
 * 1.7.31 gives the palette a second line of swatches: sixteen do not fit one
 * line next to the switch (16 x 40 + 15 x 12 = 820 of the row's ~1020, and the
 * switch and its label want ~440). Two lines of eight in one slab: 8 x 36 +
 * 7 x 12 = 372 px wide, 108 high -- 48 more than the one line, where two slabs
 * of 60 and a gap would have been 68 and pushed the advisory case to 856. The
 * discs are 36 px instead of 40 for the same reason: at 40 the slab is 116 and
 * the advisory case 844, over the pane.
 *
 * 1.7.30 took the auto-plug switch out (60 + a gap) and put in the lock line,
 * the LIGHTBAR heading and row (+160), which would have been 840 -- over the
 * error-bar pane. The sliders pair up instead: two to a row, halves of
 * (1084 - 8) / 2 = 538 px, of which the rail, padding and border take 68, a
 * PAIR_SLIDER_W track 124, a PAIR_VALUE_W number 96 ("100 %" is 87-90 px,
 * "200 %" 90) and the gaps 24, leaving the label 226 -- hence
 * "Speaker"/"Headphones"/"Haptics" there ("Soften triggers", the longest, is
 * 218-223), the section heading already says what they adjust. The LIGHTBAR
 * row was one row for the same reason, its switch beside the swatches; in
 * 1.7.31 the switch ends the first of its two lines, where eight 36 px discs
 * and their gaps take 372 of ~1020, the switch 88 and the gaps 24, and the
 * label keeps ~536 for its ~340.
 * 1.7.29 measured 748 for the same DualSense (with the auto-plug switch); in
 * 1.7.28 it was 1020 mounted and 974 on SDL, and a DualShock 4 852 mounted
 * against 806 on SDL -- the 46 px of the "applies over SDL" caption were what
 * tipped it into scrolling, which is the scrollbar a mounted pad brought up. */

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

static void mode_clicked_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.mode_clicked) {
        view->cbs.mode_clicked(view->cbs.userdata, row_of(lv_event_get_current_target(event)));
    }
}

static void swatch_clicked_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    if (view && view->cbs.swatch_clicked) {
        view->cbs.swatch_clicked(view->cbs.userdata, row_of(lv_event_get_current_target(event)));
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
    /* lv_obj_clean() frees the rows front to back, and deleting the focused
     * row makes the group refocus its neighbour mid-clean. Raise the rebuild
     * guard so that FOCUSED does not reach the panel's row bookkeeping while
     * row_buttons[] still points at rows that are already freed. */
    view->rebuilding = true;
    lv_obj_clean(view->list);
    memset(view->row_buttons, 0, sizeof(view->row_buttons));
    memset(view->row_state_labels, 0, sizeof(view->row_state_labels));
    memset(view->row_rails, 0, sizeof(view->row_rails));
    view->rebuilding = false;
}

void hid_pt_view_list_show_empty(hid_pt_view_t *view)
{
    if (!view || !view->list) {
        return;
    }
    lv_obj_t *empty = lv_label_create(view->list);
    lv_label_set_text(empty, locstr("No controllers found"));
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

void hid_pt_view_set_row_state(hid_pt_view_t *view, int row, const char *text, bool live)
{
    if (!view || row < 0 || row >= HID_PT_MAX_ROWS || !view->row_state_labels[row]) {
        return;
    }
    lv_obj_t *state = view->row_state_labels[row];
    lv_label_set_text(state, text ? text : "");
    lv_obj_set_style_text_color(state, lv_color_hex(live ? OVERLAY_LIVE : OVERLAY_CHALK), 0);
    lv_obj_set_style_text_opa(state, live ? LV_OPA_COVER : OVERLAY_OPA_FAINT, 0);
    if (view->row_rails[row]) {
        lv_obj_set_style_bg_color(view->row_rails[row],
                                  lv_color_hex(live ? OVERLAY_LIVE : OVERLAY_SEAM), 0);
    }
}

void hid_pt_view_add_row(hid_pt_view_t *view, int i, const char *label, const char *state, bool live,
                         bool selected)
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
    hid_pt_view_set_row_state(view, i, state, live);
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

#define OPTION_CHAIN_LEN 10
/* Where the two multi-control rows stand in option_chain(). */
#define CHAIN_MODE     0
#define CHAIN_LIGHTBAR 2

/**
 * Where the cursor enters the mode row: the lit button, else the first enabled
 * one, else NULL. The row is one stop of the column for UP/DOWN; LEFT/RIGHT
 * walk it (hid_pt_view_step_mode()).
 */
static lv_obj_t *mode_entry(const hid_pt_view_t *view)
{
    lv_obj_t *first = NULL;
    for (int i = 0; i < view->mode_count; ++i) {
        lv_obj_t *btn = view->mode_btns[i];
        if (!btn || lv_obj_has_state(btn, LV_STATE_DISABLED)) {
            continue;
        }
        if (lv_obj_has_state(btn, LV_STATE_CHECKED)) {
            return btn;
        }
        if (!first) {
            first = btn;
        }
    }
    return first;
}

/**
 * Where the cursor enters the LIGHTBAR row: the ringed swatch, else the first
 * one it may rest on, else NULL.
 */
static lv_obj_t *lightbar_entry(const hid_pt_view_t *view)
{
    lv_obj_t *first = NULL;
    for (int i = 0; i < view->swatch_count; ++i) {
        lv_obj_t *swatch = view->swatches[i];
        if (!swatch || lv_obj_has_state(swatch, LV_STATE_DISABLED)) {
            continue;
        }
        if (lv_obj_has_state(swatch, LV_STATE_CHECKED)) {
            return swatch;
        }
        if (!first) {
            first = swatch;
        }
    }
    return first;
}

/**
 * The option column, top to bottom, into @p out.
 *
 * One list, used for the focus group's order and for stepping the cursor, so the
 * two can't disagree. Entries that are hidden for the selected device are
 * skipped by the stepper, not removed from here. The mode row and the LIGHTBAR
 * row are one entry each, the control the cursor enters it on; the two sliders
 * of a pair are two, left before right, as they read.
 */
static void option_chain(const hid_pt_view_t *view, lv_obj_t *out[OPTION_CHAIN_LEN])
{
    out[CHAIN_MODE] = mode_entry(view);
    out[1] = view->composite_cb;
    out[CHAIN_LIGHTBAR] = lightbar_entry(view);
    out[3] = view->audio_dropdown;
    out[4] = view->speaker_slider;
    out[5] = view->headset_slider;
    out[6] = view->haptics_slider;
    out[7] = view->trigger_slider;
    out[8] = view->latency_slider;
    out[9] = view->reset_settings_btn;
}

/* The chain index @p obj stands at: a row's every control at its row's. */
static int chain_index_of(const hid_pt_view_t *view, lv_obj_t *obj, lv_obj_t *const chain[OPTION_CHAIN_LEN])
{
    if (hid_pt_view_mode_of(view, obj) >= 0) {
        return CHAIN_MODE;
    }
    if (hid_pt_view_swatch_of(view, obj) >= 0 || (obj && obj == view->lightbar_game_cb)) {
        return CHAIN_LIGHTBAR;
    }
    for (int i = 0; i < OPTION_CHAIN_LEN; ++i) {
        if (chain[i] == obj) {
            return i;
        }
    }
    return -1;
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
    if (view->picker.veil) {
        /* The picker is modal: nothing behind it can take the cursor. */
        for (int i = 0; i < 3; ++i) {
            lv_group_add_obj(view->group, view->picker.sliders[i]);
        }
        lv_group_add_obj(view->group, view->picker.ok_btn);
        lv_group_add_obj(view->group, view->picker.cancel_btn);
        view->rebuilding = false;
        return;
    }
    for (int i = 0; i < HID_PT_MAX_ROWS; ++i) {
        if (view->row_buttons[i]) {
            /* Rows scroll the device list from their own FOCUSED handler, so
             * they do not get the generic one. */
            lv_group_add_obj(view->group, view->row_buttons[i]);
        }
    }
    /* The mode row and the LIGHTBAR row whole, in place of the one control
     * the chain names for each. */
    lv_obj_t *chain[OPTION_CHAIN_LEN];
    option_chain(view, chain);
    for (int i = 0; i < OPTION_CHAIN_LEN; ++i) {
        if (i == CHAIN_MODE) {
            for (int m = 0; m < view->mode_count; ++m) {
                group_add(view, view->mode_btns[m]);
            }
        } else if (i == CHAIN_LIGHTBAR) {
            for (int w = 0; w < view->swatch_count; ++w) {
                group_add(view, view->swatches[w]);
            }
            group_add(view, view->lightbar_game_cb);
        } else {
            group_add(view, chain[i]);
        }
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
    if (hid_pt_view_mode_of(view, obj) >= 0) {
        return HID_PT_WK_MODE_BTN;
    }
    if (hid_pt_view_swatch_of(view, obj) >= 0) {
        return HID_PT_WK_SWATCH;
    }
    const struct {
        lv_obj_t *const *slot;
        hid_pt_widget_kind_t kind;
    } table[] = {
            {&view->picker.sliders[0],  HID_PT_WK_PICKER_SLIDER},
            {&view->picker.sliders[1],  HID_PT_WK_PICKER_SLIDER},
            {&view->picker.sliders[2],  HID_PT_WK_PICKER_SLIDER},
            {&view->picker.ok_btn,      HID_PT_WK_PICKER_BTN},
            {&view->picker.cancel_btn,  HID_PT_WK_PICKER_BTN},
            {&view->composite_cb,       HID_PT_WK_SWITCH},
            {&view->lightbar_game_cb,   HID_PT_WK_SWITCH},
            {&view->latency_slider,     HID_PT_WK_SLIDER},
            {&view->speaker_slider,     HID_PT_WK_SLIDER},
            {&view->headset_slider,     HID_PT_WK_SLIDER},
            {&view->haptics_slider,     HID_PT_WK_SLIDER},
            {&view->trigger_slider,     HID_PT_WK_SLIDER},
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
    if (kind == HID_PT_WK_PICKER_SLIDER || kind == HID_PT_WK_PICKER_BTN) {
        return HID_PT_ZONE_PICKER;
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
        if (chain[i] && hid_pt_view_obj_is_focusable(view, chain[i])) {
            return chain[i];
        }
    }
    return NULL;
}

bool hid_pt_view_obj_is_focusable(const hid_pt_view_t *view, lv_obj_t *obj)
{
    return obj && !hid_pt_view_obj_is_hidden(view, obj) && !lv_obj_has_state(obj, LV_STATE_DISABLED);
}

lv_obj_t *hid_pt_view_step_option(const hid_pt_view_t *view, lv_obj_t *from, int step)
{
    if (!view || !from || step == 0) {
        return NULL;
    }
    lv_obj_t *chain[OPTION_CHAIN_LEN];
    option_chain(view, chain);
    /* Any control of the mode or LIGHTBAR row stands where its row's entry
     * does. */
    const int at = chain_index_of(view, from, chain);
    if (at < 0) {
        return NULL;
    }
    for (int i = at + step; i >= 0 && i < OPTION_CHAIN_LEN; i += step) {
        if (chain[i] && hid_pt_view_obj_is_focusable(view, chain[i])) {
            return chain[i];
        }
    }
    return NULL;
}

/* ---- the dropdowns' lists ------------------------------------------------ */

bool hid_pt_view_dropdown_is_open(const hid_pt_view_t *view, lv_obj_t *target)
{
    if (view && view->active_dropdown && lv_dropdown_is_open(view->active_dropdown)) {
        return true;
    }
    return target != NULL && lv_obj_has_class(target, &lv_dropdown_class) && lv_dropdown_is_open(target);
}

void hid_pt_view_forget_dropdown(hid_pt_view_t *view)
{
    if (!view) {
        return;
    }
    view->active_dropdown = NULL;
    lv_group_set_editing(view->group, false);
}

/**
 * Keep the bookkeeping in step with what the widget just did on its own.
 *
 * The list is opened and closed by LVGL's press/release handling — a short OK
 * toggles it open on the key's RELEASE, the next OK commits the highlighted
 * option and closes it, a pointer click does either. The view no longer opens
 * the list itself: it used to, on the KEY event (which arrives at key DOWN),
 * and LVGL's own release toggle then promptly shut it again — which is why the
 * list only stayed open for as long as OK was held.
 *
 * Registered without PREPROCESS, so it runs after the widget has settled, and
 * on every event that can change the list's openness from inside the widget:
 * RELEASED (both toggle directions), DEFOCUSED (LVGL closes on focus leaving),
 * and VALUE_CHANGED (a pointer click on a list row commits and closes without
 * a RELEASED on the dropdown itself).
 */
static void dropdown_state_sync_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    lv_obj_t *dropdown = lv_event_get_current_target(event);
    if (!view || !view->group) {
        return;
    }
    const bool open = lv_dropdown_is_open(dropdown);
    const bool was_open = view->active_dropdown == dropdown;
    if (open == was_open) {
        return;
    }
    view->active_dropdown = open ? dropdown : NULL;
    lv_group_set_editing(view->group, open);
    if (view->cbs.dropdown_toggled) {
        view->cbs.dropdown_toggled(view->cbs.userdata, dropdown, open);
    }
}

/* ---- the mode row -------------------------------------------------------- */

/* Flag writes invalidate and re-lay the column even when nothing changes, and
 * this runs on the panel's 2 s refresh -- see show_row() in the panel. */
static void show_obj(lv_obj_t *obj, bool show)
{
    if (!obj || show != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_obj_state(lv_obj_t *obj, lv_state_t state, bool on)
{
    if (on) {
        lv_obj_add_state(obj, state);
    } else {
        lv_obj_clear_state(obj, state);
    }
}

/**
 * The slab every other control is, split across the column instead of spanning
 * it, so the cursor lights it exactly the same way. Lit is a fill, not a hue on
 * the rail: the bridge's teal for HID, the plain white wash for an SDL type --
 * the two looks the overlay's pad badge wears for the same two cases.
 *
 * Not LV_OBJ_FLAG_CHECKABLE: LVGL would toggle that state on its own on the
 * arrow keys, and a mode is only ever chosen by OK.
 */
lv_obj_t *hid_pt_view_add_mode(hid_pt_view_t *view, const char *glyph, const char *label, bool live)
{
    if (!view || !view->mode_row || view->mode_count >= HID_PT_MAX_MODES) {
        return NULL;
    }
    const int index = view->mode_count;
    lv_obj_t *btn = lv_btn_create(view->mode_row);
    slab_style(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_color_t fill = lv_color_hex(live ? OVERLAY_LIVE : OVERLAY_CHALK);
    const lv_opa_t fill_opa = live ? LV_OPA_60 : LV_OPA_20;
    /* Twice: with the cursor on it too, or the focus plate (FOCUS_KEY outranks
     * CHECKED) would hide which mode is lit. The border and the bloom stay the
     * focus look's, and are the ONLY thing the cursor changes on a lit button --
     * so lit must not wear the focus edge itself: HID keeps the teal rim, an SDL
     * type the plain seam of an idle slab. Chalk edge = cursor, fill = lit. */
    lv_obj_set_style_bg_color(btn, fill, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, fill_opa, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, fill, LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btn, fill_opa, LV_STATE_CHECKED | LV_STATE_FOCUS_KEY);
    if (live) {
        lv_obj_set_style_border_color(btn, fill, LV_STATE_CHECKED);
    }
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);

    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_set_style_text_font(icon, lv_theme_moonlight_get_iconfont_large(btn), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(OVERLAY_CHALK), 0);
    lv_label_set_text(icon, glyph ? glyph : "");
    eyebrow(btn, label, OVERLAY_CHALK, OVERLAY_OPA_MUTED);

    lv_obj_set_user_data(btn, (void *) (intptr_t) index);
    lv_obj_add_event_cb(btn, mode_clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(btn, key_cb, LV_EVENT_KEY, view);
    view->mode_btns[index] = btn;
    view->mode_count++;
    return btn;
}

void hid_pt_view_set_modes(hid_pt_view_t *view, bool show, unsigned lit, unsigned enabled, unsigned visible)
{
    if (!view || !view->mode_row) {
        return;
    }
    for (int i = 0; i < view->mode_count; ++i) {
        set_obj_state(view->mode_btns[i], LV_STATE_CHECKED, (lit & (1u << i)) != 0);
        set_obj_state(view->mode_btns[i], LV_STATE_DISABLED, (enabled & (1u << i)) == 0);
        show_obj(view->mode_btns[i], (visible & (1u << i)) != 0);
    }
    show_obj(view->mode_heading, show);
    show_obj(view->mode_row, show);
}

void hid_pt_view_set_mode_caption(hid_pt_view_t *view, const char *text)
{
    if (!view || !view->mode_caption) {
        return;
    }
    if (text && strcmp(lv_label_get_text(view->mode_caption), text) != 0) {
        lv_label_set_text(view->mode_caption, text);
    }
    show_obj(view->mode_caption, text != NULL);
}

int hid_pt_view_mode_of(const hid_pt_view_t *view, lv_obj_t *obj)
{
    if (!view || !obj) {
        return -1;
    }
    for (int i = 0; i < view->mode_count; ++i) {
        if (view->mode_btns[i] == obj) {
            return i;
        }
    }
    return -1;
}

lv_obj_t *hid_pt_view_step_mode(const hid_pt_view_t *view, lv_obj_t *from, int dir)
{
    const int at = hid_pt_view_mode_of(view, from);
    if (at < 0 || dir == 0) {
        return NULL;
    }
    for (int i = at + dir; i >= 0 && i < view->mode_count; i += dir) {
        if (hid_pt_view_obj_is_focusable(view, view->mode_btns[i])) {
            return view->mode_btns[i];
        }
    }
    return NULL;
}

/* ---- the LIGHTBAR row ------------------------------------------------------ */

/**
 * A swatch is a disc in its own colour, with the row's two marks on it: lit,
 * the colour chosen, is a chalk ring around the disc with a gap -- it must
 * read on a black disc and on a white one alike -- and the cursor is a thick
 * chalk edge on the disc itself, so both can be on one swatch and tell apart.
 * The row's slab lights up behind whichever swatch has the cursor, as for
 * every other control (bind_slab_focus()).
 *
 * Not LV_OBJ_FLAG_CHECKABLE, for the mode buttons' reason: a colour is chosen
 * by OK alone, never by walking past it.
 */
lv_obj_t *hid_pt_view_add_swatch(hid_pt_view_t *view, uint32_t rgb, const char *text, const char *glyph)
{
    if (!view || !view->lightbar_body || view->swatch_count >= HID_PT_MAX_SWATCHES) {
        return NULL;
    }
    const int index = view->swatch_count;
    lv_obj_t *line = view->lightbar_lines[index / HID_PT_SWATCHES_PER_LINE];
    lv_obj_t *swatch = lv_btn_create(line);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, SWATCH_D, SWATCH_D);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(swatch, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(swatch, LV_DPX(1), 0);
    lv_obj_set_style_border_color(swatch, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_border_opa(swatch, OVERLAY_OPA_FAINT, 0);
    lv_obj_set_style_outline_width(swatch, SWATCH_RING, LV_STATE_CHECKED);
    lv_obj_set_style_outline_pad(swatch, SWATCH_RING_PAD, LV_STATE_CHECKED);
    lv_obj_set_style_outline_color(swatch, lv_color_hex(OVERLAY_CHALK), LV_STATE_CHECKED);
    lv_obj_set_style_outline_opa(swatch, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(swatch, LV_DPX(3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_opa(swatch, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (text || glyph) {
        lv_obj_t *mark = lv_label_create(swatch);
        lv_obj_set_style_text_font(mark, glyph ? lv_theme_moonlight_get_iconfont_small(swatch)
                                               : lv_theme_get_font_small(swatch), 0);
        lv_obj_set_style_text_color(mark, lv_color_hex(OVERLAY_CHALK), 0);
        lv_obj_set_style_text_opa(mark, OVERLAY_OPA_MUTED, 0);
        lv_label_set_text(mark, glyph ? glyph : text);
        lv_obj_center(mark);
    }
    lv_obj_set_user_data(swatch, (void *) (intptr_t) index);
    lv_obj_add_event_cb(swatch, swatch_clicked_cb, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(swatch, key_cb, LV_EVENT_KEY, view);
    bind_slab_focus(swatch, view->lightbar_row);
    /* In front of the game switch's label, which the first line created
     * first. */
    lv_obj_move_to_index(swatch, index % HID_PT_SWATCHES_PER_LINE);
    view->swatches[index] = swatch;
    view->swatch_count++;
    return swatch;
}

void hid_pt_view_set_lightbar(hid_pt_view_t *view, bool show, const char *heading, int lit, bool show_game,
                              bool game_on)
{
    if (!view || !view->lightbar_row) {
        return;
    }
    if (view->lightbar_heading && heading && strcmp(lv_label_get_text(view->lightbar_heading), heading) != 0) {
        lv_label_set_text(view->lightbar_heading, heading);
    }
    for (int i = 0; i < view->swatch_count; ++i) {
        set_obj_state(view->swatches[i], LV_STATE_CHECKED, i == lit);
    }
    if (view->lightbar_game_cb && lv_obj_has_state(view->lightbar_game_cb, LV_STATE_CHECKED) != game_on) {
        set_obj_state(view->lightbar_game_cb, LV_STATE_CHECKED, game_on);
    }
    show_obj(view->lightbar_game_label, show_game);
    show_obj(view->lightbar_game_cb, show_game);
    show_obj(view->lightbar_heading, show);
    show_obj(view->lightbar_row, show);
}

int hid_pt_view_swatch_of(const hid_pt_view_t *view, lv_obj_t *obj)
{
    if (!view || !obj) {
        return -1;
    }
    for (int i = 0; i < view->swatch_count; ++i) {
        if (view->swatches[i] == obj) {
            return i;
        }
    }
    return -1;
}

lv_obj_t *hid_pt_view_step_lightbar(const hid_pt_view_t *view, lv_obj_t *from, int dir)
{
    if (!view || !from || dir == 0) {
        return NULL;
    }
    /* The line @p from is on, left to right: its swatches, and on the first
     * line the switch at its end. */
    const int swatch = hid_pt_view_swatch_of(view, from);
    const int line = swatch >= 0 ? swatch / HID_PT_SWATCHES_PER_LINE : 0;
    if (swatch < 0 && from != view->lightbar_game_cb) {
        return NULL;
    }
    lv_obj_t *strip[HID_PT_SWATCHES_PER_LINE + 1];
    int n = 0;
    for (int i = line * HID_PT_SWATCHES_PER_LINE;
         i < view->swatch_count && i < (line + 1) * HID_PT_SWATCHES_PER_LINE; ++i) {
        strip[n++] = view->swatches[i];
    }
    if (line == 0) {
        strip[n++] = view->lightbar_game_cb;
    }
    int at = -1;
    for (int i = 0; i < n; ++i) {
        if (strip[i] == from) {
            at = i;
        }
    }
    if (at < 0) {
        return NULL;
    }
    for (int i = at + dir; i >= 0 && i < n; i += dir) {
        if (hid_pt_view_obj_is_focusable(view, strip[i])) {
            return strip[i];
        }
    }
    return NULL;
}

lv_obj_t *hid_pt_view_step_lightbar_line(const hid_pt_view_t *view, lv_obj_t *from, int dir)
{
    int swatch = hid_pt_view_swatch_of(view, from);
    if (swatch < 0 && view && from && from == view->lightbar_game_cb) {
        /* The switch ends the first line, past its last swatch: DOWN is the
         * second line's last, never a jump over the whole line. */
        swatch = HID_PT_SWATCHES_PER_LINE - 1;
    }
    if (swatch < 0 || dir == 0) {
        return NULL;
    }
    const int line = swatch / HID_PT_SWATCHES_PER_LINE + (dir > 0 ? 1 : -1);
    const int first = line * HID_PT_SWATCHES_PER_LINE;
    if (line < 0 || first >= view->swatch_count) {
        return NULL;
    }
    /* The same column, or the line's last swatch when it is shorter. */
    int to = first + swatch % HID_PT_SWATCHES_PER_LINE;
    if (to >= view->swatch_count) {
        to = view->swatch_count - 1;
    }
    for (int i = to; i >= first; --i) {
        if (hid_pt_view_obj_is_focusable(view, view->swatches[i])) {
            return view->swatches[i];
        }
    }
    return NULL;
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

void hid_pt_view_update_trigger_label(hid_pt_view_t *view)
{
    if (!view || !view->trigger_value || !view->trigger_slider) {
        return;
    }
    int level = (int) lv_slider_get_value(view->trigger_slider);
    if (level == 0) {
        lv_label_set_text(view->trigger_value, locstr("Off"));
    } else {
        lv_label_set_text_fmt(view->trigger_value, "%d", level);
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
    if (!view) {
        return;
    }
    if (zone == HID_PT_ZONE_PICKER || zone == HID_PT_ZONE_PICKER_BUTTONS) {
        /* The picker has a line of its own under its buttons: the sheet's
         * footer is gone, and the veil would hide it anyway. */
        const char *text = zone == HID_PT_ZONE_PICKER
                               ? locstr("UP/DOWN  choose    LEFT/RIGHT  adjust    BACK  cancel")
                               : locstr("LEFT/RIGHT  choose    OK  select    BACK  cancel");
        if (view->picker.hint && strcmp(lv_label_get_text(view->picker.hint), text) != 0) {
            lv_label_set_text(view->picker.hint, text);
        }
        return;
    }
    if (!view->hint_label) {
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
        case HID_PT_ZONE_DROPDOWN:
            text = locstr("UP/DOWN  choose        OK  confirm        BACK  cancel");
            break;
        case HID_PT_ZONE_OPTIONS:
            text = locstr("UP/DOWN  setting        LEFT/RIGHT  adjust        BACK  devices");
            break;
        case HID_PT_ZONE_MODE:
            text = locstr("LEFT/RIGHT  mode        OK  select        BACK  devices");
            break;
        case HID_PT_ZONE_HEADER:
            text = locstr("LEFT/RIGHT  choose        OK  run        BACK  close");
            break;
        case HID_PT_ZONE_LIST:
        default:
            /* One line whatever the row's state: OK flips a controller between
             * HID and SDL in both directions, so it names both. */
            text = locstr("UP/DOWN  controller        OK  HID/SDL        RIGHT  settings        BACK  close");
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

/** The sheet's switch: chalk track, teal when on. */
static void style_switch(lv_obj_t *sw)
{
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
    style_switch(sw);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    bind_slab_focus(sw, row);
    bind_control(view, sw, id);
    if (switch_out) {
        *switch_out = sw;
    }
    return row;
}

/**
 * A slider row: label, then the track (@p slider_w) and the number
 * (@p value_w), both on the gutter -- or, in a pair_row(), on the right of its
 * half.
 */
static lv_obj_t *slider_row(hid_pt_view_t *view, lv_obj_t *parent, const char *label, int32_t min,
                            int32_t max, hid_pt_ctl_t id, lv_coord_t slider_w, lv_coord_t value_w,
                            lv_obj_t **slider_out, lv_obj_t **value_out, lv_obj_t **label_out)
{
    lv_obj_t *body;
    lv_obj_t *row = option_row(parent, label, &body, label_out);

    lv_obj_t *slider = lv_slider_create(body);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_size(slider, slider_w, LV_DPX(6));
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
    lv_obj_set_width(value, value_w);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    if (value_out) {
        *value_out = value;
    }
    return row;
}

/**
 * A settings row whose control is a dropdown in the shared gutter.
 *
 * No box of its own: the row already is the box. The theme gives a dropdown a
 * filled plate, a border and a blue focus outline, which next to four bare
 * slider rows made this one row look like a different design — and the plate
 * clipped its own text, because the theme's vertical padding is written for a
 * content-sized dropdown, not one that has to fit a fixed row.
 *
 * So: transparent, borderless, its own padding, and the value right-aligned on
 * the same axis every slider's number sits on.
 */
static lv_obj_t *dropdown_row(hid_pt_view_t *view, lv_obj_t *parent, const char *label,
                              const char *options, hid_pt_ctl_t id, lv_obj_t **dropdown_out)
{
    lv_obj_t *body;
    lv_obj_t *row = option_row(parent, label, &body, NULL);
    lv_obj_t *dd = lv_dropdown_create(body);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_size(dd, GUTTER_W, LV_DPX(26));
    lv_obj_set_style_bg_opa(dd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_outline_width(dd, 0, 0);
    lv_obj_set_style_outline_width(dd, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_width(dd, 0, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(OVERLAY_CHALK), 0);
    /* lv_dropdown draws its text at pad_top and its symbol against the right
     * edge, and ignores text_align entirely — so the padding IS the layout: the
     * value starts where every slider's track starts, the chevron ends where
     * every number ends. */
    lv_obj_set_style_pad_hor(dd, 0, 0);
    lv_obj_set_style_pad_ver(dd, LV_DPX(5), 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(OVERLAY_CHALK), LV_PART_INDICATOR);
    lv_obj_set_style_text_opa(dd, OVERLAY_OPA_MUTED, LV_PART_INDICATOR);
    bind_slab_focus(dd, row);
    /* Not bind_control(): a dropdown wants its KEY handler WITHOUT
     * LV_EVENT_PREPROCESS, so LVGL's own list handling runs first, plus a second
     * preprocess handler that turns the arrow keys into panel navigation while
     * the list is closed. */
    lv_obj_set_user_data(dd, (void *) (intptr_t) id);
    lv_obj_add_event_cb(dd, value_changed_cb, LV_EVENT_VALUE_CHANGED, view);
    lv_obj_add_event_cb(dd, key_cb, LV_EVENT_KEY, view);
    lv_obj_add_event_cb(dd, dropdown_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, view);
    /* After LVGL's own open/close handling (no PREPROCESS): see the comment on
     * dropdown_state_sync_cb for why these three events cover every toggle. */
    lv_obj_add_event_cb(dd, dropdown_state_sync_cb, LV_EVENT_RELEASED, view);
    lv_obj_add_event_cb(dd, dropdown_state_sync_cb, LV_EVENT_DEFOCUSED, view);
    lv_obj_add_event_cb(dd, dropdown_state_sync_cb, LV_EVENT_VALUE_CHANGED, view);
    if (dropdown_out) {
        *dropdown_out = dd;
    }
    return row;
}

/**
 * A row of two settings side by side, each a slab of its own (created into it
 * by slider_row()) that takes half. One row's height for two sliders -- what
 * lets the tallest column fit its pane (see the arithmetic at the top).
 * Starts shown; hiding it hides both halves.
 */
static lv_obj_t *pair_row(lv_obj_t *parent)
{
    lv_obj_t *pair = lv_obj_create(parent);
    lv_obj_remove_style_all(pair);
    lv_obj_set_size(pair, LV_PCT(100), OPT_ROW_H);
    lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(pair, OPT_GAP, 0);
    lv_obj_clear_flag(pair, LV_OBJ_FLAG_SCROLLABLE);
    /* Drawing only, as on the mode row: the halves' focus bloom must not be
     * clipped to the pair's box. */
    lv_obj_add_flag(pair, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return pair;
}

/** The half of a pair_row() that @p slab is: it shares the row with its twin. */
static void pair_half(lv_obj_t *slab)
{
    lv_obj_set_flex_grow(slab, 1);
}

/**
 * The LIGHTBAR row: one slab, the swatches (hid_pt_view_add_swatch()) in two
 * lines on the left of its body, "Game may change colour" and its switch at
 * the end of the first. One slab for both lines is what the column's height
 * allows (the arithmetic at the top), and the switch is about the swatches, so
 * it sits beside them. Starts hidden.
 */
static void lightbar_row(hid_pt_view_t *view, lv_obj_t *parent)
{
    view->lightbar_heading = eyebrow(parent, locstr("LIGHTBAR"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(view->lightbar_heading, LV_DPX(3), 0);
    lv_obj_set_style_pad_top(view->lightbar_heading, LV_DPX(4), 0);
    lv_obj_add_flag(view->lightbar_heading, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *row = lv_obj_create(parent);
    slab_style(row, LIGHTBAR_ROW_H);
    slab_rail(row);
    view->lightbar_row = row;
    view->lightbar_body = slab_body(row);
    lv_obj_set_flex_flow(view->lightbar_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->lightbar_body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(view->lightbar_body, SWATCH_LINE_GAP, 0);
    /* The swatches' rings reach past the disc; the body must not cut them. */
    lv_obj_add_flag(view->lightbar_body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (int i = 0; i < HID_PT_SWATCH_LINES; ++i) {
        lv_obj_t *line = lv_obj_create(view->lightbar_body);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, LV_PCT(100), SWATCH_D);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(line, SWATCH_GAP, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(line, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        view->lightbar_lines[i] = line;
    }

    view->lightbar_game_label = body_text(view->lightbar_lines[0], locstr("Game may change colour"));
    lv_label_set_long_mode(view->lightbar_game_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(view->lightbar_game_label, 1);
    lv_obj_set_style_text_align(view->lightbar_game_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_left(view->lightbar_game_label, LV_DPX(6), 0);

    lv_obj_t *sw = lv_switch_create(view->lightbar_lines[0]);
    style_switch(sw);
    bind_slab_focus(sw, row);
    bind_control(view, sw, HID_PT_CTL_LIGHTBAR_GAME);
    view->lightbar_game_cb = sw;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
}

/** One quiet, wrapping line under the settings it is about. Starts hidden. */
static lv_obj_t *caption(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(parent), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(OVERLAY_CHALK), 0);
    lv_obj_set_style_text_opa(label, OVERLAY_OPA_MUTED, 0);
    lv_obj_set_style_pad_left(label, LV_DPX(3), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

lv_obj_t *hid_pt_view_create(hid_pt_view_t *view, lv_obj_t *parent, const hid_pt_view_cbs_t *cbs)
{
    if (!view || !cbs) {
        return NULL;
    }
    view->cbs = *cbs;
    view->custom_swatch = -1;
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
    /* Static height on purpose. Content-sizing made the centered sheet grow
     * when the selected device had more option rows (a bridged DS5), so the
     * whole panel visibly hopped a few pixels upward on selection. The fixed
     * box is what one full option column measures; anything taller scrolls
     * inside its pane instead of moving the sheet. */
    lv_obj_set_size(sheet, SHEET_W, SHEET_H);
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
    lv_obj_set_scrollbar_mode(sheet, LV_SCROLLBAR_MODE_OFF);
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
    eyebrow(title_block, locstr("INPUT"), OVERLAY_CHALK, OVERLAY_OPA_FAINT);
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
    lv_obj_set_width(body_row, LV_PCT(100));
    /* Exactly what the static sheet has left under the header, never more. The
     * error bar above is a flex sibling and LVGL's flex does not shrink, so a
     * content-sized body would be pushed past the sheet's fixed bottom edge the
     * moment the bar appears -- and the sheet clips, so the lowest rows would
     * simply be gone. Growing into the remainder makes the bar take its height
     * out of the panes, which is what their scrolling is there for. */
    lv_obj_set_flex_grow(body_row, 1);
    lv_obj_set_flex_flow(body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(body_row, BODY_PAD, 0);
    lv_obj_set_style_pad_gap(body_row, LV_DPX(12), 0);
    lv_obj_clear_flag(body_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left_pane = lv_obj_create(body_row);
    lv_obj_remove_style_all(left_pane);
    lv_obj_set_size(left_pane, DEV_COL_W, LV_PCT(100));
    lv_obj_set_flex_flow(left_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(left_pane, LV_DPX(8), 0);
    lv_obj_clear_flag(left_pane, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *devices_title = eyebrow(left_pane, locstr("CONTROLLERS"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(devices_title, LV_DPX(3), 0);

    view->list = lv_obj_create(left_pane);
    lv_obj_remove_style_all(view->list);
    lv_obj_set_width(view->list, LV_PCT(100));
    /* Everything the body box has left below the CONTROLLERS label -- a fixed cap
     * would ignore both the label and the error bar and send the last rows
     * below the sheet's edge. Past it the list scrolls: 787 px at 1080p take
     * six rows (6 x 108 + 5 x 12), a seventh scrolls in as the cursor reaches
     * it (hid_pt_view_scroll_row_into_view()). No scrollbar, as nowhere on the
     * sheet: the rows running on past the edge already say there is more. */
    lv_obj_set_flex_grow(view->list, 1);
    lv_obj_set_style_pad_right(view->list, LV_DPX(4), 0);
    lv_obj_add_flag(view->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(view->list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *right_pane = lv_obj_create(body_row);
    lv_obj_remove_style_all(right_pane);
    lv_obj_set_height(right_pane, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(right_pane, LV_PCT(100), 0);
    lv_obj_set_flex_grow(right_pane, 1);
    lv_obj_set_flex_flow(right_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(right_pane, OPT_GAP, 0);
    /* Sized to fit at 1080p (the arithmetic is at the top of this file). The
     * scroll is the fallback for a smaller panel, a longer translation or the
     * error bar and the audio advisory up at once, and it follows the cursor
     * (scroll_into_view_cb()) without a scrollbar, as nowhere on the sheet. */
    lv_obj_add_flag(right_pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(right_pane, LV_SCROLLBAR_MODE_OFF);
    view->customize_panel = right_pane;

    /* Device header: who is being edited and how it is doing, stacked left of
     * RESET -- the state line beside the button rather than a row of its own
     * under it is 50 px of the column's height. */
    lv_obj_t *head_row = lv_obj_create(right_pane);
    lv_obj_remove_style_all(head_row);
    lv_obj_set_size(head_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(head_row, LV_DPX(3), 0);
    lv_obj_set_style_pad_bottom(head_row, LV_DPX(2), 0);
    lv_obj_clear_flag(head_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head_text = lv_obj_create(head_row);
    lv_obj_remove_style_all(head_text);
    lv_obj_set_height(head_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(head_text, 1);
    lv_obj_set_flex_flow(head_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(head_text, LV_DPX(2), 0);
    lv_obj_clear_flag(head_text, LV_OBJ_FLAG_SCROLLABLE);

    view->customize_title = body_text(head_text, locstr("Controller settings"));
    lv_obj_set_style_text_font(view->customize_title, lv_theme_get_font_large(head_text), 0);
    /* One line, cut with an ellipsis, like a device row's name: a long name
     * wrapping to a second line would cost the column a line's height. */
    lv_label_set_long_mode(view->customize_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(view->customize_title, LV_PCT(100));
    lv_obj_set_height(view->customize_title,
                      lv_font_get_line_height(lv_obj_get_style_text_font(view->customize_title, LV_PART_MAIN)));

    /* State and battery on one line: recoloured so "HID" carries the same teal
     * as the rail on the device's row. */
    view->customize_state = eyebrow(head_text, "", OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_label_set_recolor(view->customize_state, true);

    view->reset_settings_btn = ghost_button(view, head_row, locstr("RESET"), HID_PT_CTL_RESET);

    /* How the controller reaches the host, first: it decides which of the rows
     * below mean anything at all. The panel fills the row with its buttons. */
    view->mode_heading = eyebrow(right_pane, locstr("MODE"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(view->mode_heading, LV_DPX(3), 0);
    lv_obj_add_flag(view->mode_heading, LV_OBJ_FLAG_HIDDEN);
    view->mode_row = lv_obj_create(right_pane);
    lv_obj_remove_style_all(view->mode_row);
    lv_obj_set_size(view->mode_row, LV_PCT(100), MODE_BTN_H);
    lv_obj_set_flex_flow(view->mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(view->mode_row, OPT_GAP, 0);
    lv_obj_clear_flag(view->mode_row, LV_OBJ_FLAG_SCROLLABLE);
    /* The row is exactly the buttons' height, so without this it would clip
     * the focus bloom to the slivers between them. Drawing only: the column's
     * layout and scroll extent read the row's own box, not the bloom. */
    lv_obj_add_flag(view->mode_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(view->mode_row, LV_OBJ_FLAG_HIDDEN);
    /* One line, cut with an ellipsis: a game's name is as long as the host
     * says, and a second line would cost the column its height. */
    view->mode_caption = caption(right_pane);
    lv_label_set_long_mode(view->mode_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_height(view->mode_caption,
                      lv_font_get_line_height(lv_obj_get_style_text_font(view->mode_caption, LV_PART_MAIN)));

    view->composite_row = switch_row(view, right_pane, locstr("Recognize as native Flydigi on PC"),
                                     HID_PT_CTL_COMPOSITE, &view->composite_cb);

    lightbar_row(view, right_pane);

    view->audio_heading = eyebrow(right_pane, locstr("AUDIO & HAPTICS"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(view->audio_heading, LV_DPX(3), 0);
    lv_obj_set_style_pad_top(view->audio_heading, LV_DPX(4), 0);

    view->audio_row = dropdown_row(view, right_pane, locstr("Audio output"),
                                   locstr("Auto (game decides)\nOff\nController speaker\nHeadphone jack\nSpeaker + jack"),
                                   HID_PT_CTL_AUDIO_MODE, &view->audio_dropdown);

    /* Side by side in pairs, under the section heading that already says
     * what they are about -- so a half says "Speaker", not "Speaker volume":
     * the label has what the half leaves next to its track and number. */
    view->volume_pair = pair_row(right_pane);
    view->speaker_row = slider_row(view, view->volume_pair, locstr("Speaker"), 0, DS_VOLUME_MAX,
                                   HID_PT_CTL_SPEAKER, PAIR_SLIDER_W, PAIR_VALUE_W, &view->speaker_slider,
                                   &view->speaker_value, NULL);
    pair_half(view->speaker_row);
    view->headset_row = slider_row(view, view->volume_pair, locstr("Headphones"), 0, DS_VOLUME_MAX,
                                   HID_PT_CTL_HEADSET, PAIR_SLIDER_W, PAIR_VALUE_W, &view->headset_slider,
                                   &view->headset_value, NULL);
    pair_half(view->headset_row);
    view->haptics_pair = pair_row(right_pane);
    view->haptics_row = slider_row(view, view->haptics_pair, locstr("Haptics"), 0, DS_HAPTICS_MAX,
                                   HID_PT_CTL_HAPTICS, PAIR_SLIDER_W, PAIR_VALUE_W, &view->haptics_slider,
                                   &view->haptics_value, NULL);
    pair_half(view->haptics_row);
    view->trigger_row = slider_row(view, view->haptics_pair, locstr("Soften triggers"), 0, DS_TRIGGER_REDUCE_MAX,
                                   HID_PT_CTL_TRIGGER_REDUCE, PAIR_SLIDER_W, PAIR_VALUE_W, &view->trigger_slider,
                                   &view->trigger_value, NULL);
    pair_half(view->trigger_row);
    view->latency_row = slider_row(view, right_pane, locstr("Latency"), DS_LATENCY_MIN, DS_LATENCY_MAX,
                                   HID_PT_CTL_LATENCY, SLIDER_W, VALUE_W, &view->latency_slider,
                                   &view->latency_value, &view->latency_label);

    /* The advisory sits under the settings it is about, one quiet line rather
     * than a coloured block: it is a consequence to know, not an error. */
    view->audio_warning_label = caption(right_pane);
    lv_obj_set_style_pad_top(view->audio_warning_label, LV_DPX(4), 0);
    lv_label_set_recolor(view->audio_warning_label, true);

    /* No footer key-hint bar: the sheet reads cleaner without it. hint_label
     * stays NULL, which hid_pt_view_set_hints() already tolerates, so the zone
     * bookkeeping call sites can stay as they are. */
    view->hint_label = NULL;

    hid_pt_view_rebuild_focus_order(view);

    view->container = cont;
    lv_obj_add_event_cb(cont, deleted_cb, LV_EVENT_DELETE, view);
    return cont;
}

/* ---- the Custom swatch ---------------------------------------------------- */

/* The fully saturated colour at @p hue degrees, as the screen shows it. */
static lv_color_t hue_colour(unsigned hue)
{
    const uint32_t rgb = lightbar_colour_from_hsv(hue % 360, 100, 255);
    return lv_color_make((uint8_t) (rgb >> 16), (uint8_t) (rgb >> 8), (uint8_t) rgb);
}

/**
 * The Custom swatch: a disc of SWATCH_D whose outer third is a rainbow ring --
 * hue by angle, red at the top, clockwise, as colour wheels go -- drawn once
 * into an ARGB image (LVGL 8 has no conic gradient), with a disc in the middle
 * for the custom colour. The swatch keeps the others' ring and cursor marks;
 * its border is drawn after the image so the cursor's thick edge stays on top.
 */
lv_obj_t *hid_pt_view_add_custom_swatch(hid_pt_view_t *view)
{
    lv_obj_t *swatch = hid_pt_view_add_swatch(view, OVERLAY_SLAB, NULL, NULL);
    if (!swatch) {
        return NULL;
    }
    view->custom_swatch = view->swatch_count - 1;
    lv_obj_set_style_bg_opa(swatch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_post(swatch, true, 0);

    const lv_coord_t d = SWATCH_D;
    view->custom_ring_px = malloc((size_t) d * (size_t) d * sizeof(lv_color_t));
    if (view->custom_ring_px) {
        const float c = (float) d / 2.0f;
        const float outer = c;
        const float inner = c * 0.62f;
        for (lv_coord_t y = 0; y < d; ++y) {
            for (lv_coord_t x = 0; x < d; ++x) {
                const float dx = (float) x + 0.5f - c;
                const float dy = (float) y + 0.5f - c;
                const float r = sqrtf(dx * dx + dy * dy);
                /* One pixel of edge on both circles, for a smooth ring. */
                float a = 1.0f;
                if (r > outer - 1.0f) {
                    a = outer - r;
                } else if (r < inner + 1.0f) {
                    a = r - inner;
                }
                a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
                float deg = atan2f(dx, -dy) * (180.0f / (float) M_PI);
                if (deg < 0.0f) {
                    deg += 360.0f;
                }
                lv_color_t px = hue_colour((unsigned) deg);
                px.ch.alpha = (uint8_t) (a * 255.0f + 0.5f);
                view->custom_ring_px[y * d + x] = px;
            }
        }
        memset(&view->custom_ring, 0, sizeof(view->custom_ring));
        view->custom_ring.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        view->custom_ring.header.w = (uint32_t) d;
        view->custom_ring.header.h = (uint32_t) d;
        view->custom_ring.data_size = (uint32_t) d * (uint32_t) d * sizeof(lv_color_t);
        view->custom_ring.data = (const uint8_t *) view->custom_ring_px;
        lv_obj_t *ring = lv_img_create(swatch);
        lv_img_set_src(ring, &view->custom_ring);
        lv_obj_center(ring);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *disc = lv_obj_create(swatch);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, d * 56 / 100, d * 56 / 100);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(OVERLAY_SLAB), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(disc);
    view->custom_disc = disc;
    return swatch;
}

void hid_pt_view_set_custom_swatch(hid_pt_view_t *view, bool has_colour, uint32_t rgb)
{
    if (!view || !view->custom_disc) {
        return;
    }
    const lv_color_t want = lv_color_hex(has_colour ? rgb : OVERLAY_SLAB);
    if (lv_obj_get_style_bg_color(view->custom_disc, LV_PART_MAIN).full != want.full) {
        lv_obj_set_style_bg_color(view->custom_disc, want, 0);
    }
}

/* ---- the colour picker --------------------------------------------------- */

#define PICKER_W        LV_DPX(460)
#define PICKER_SLIDER_W LV_DPX(200)
#define PICKER_TRACK_H  LV_DPX(10)
#define PICKER_PREVIEW  LV_DPX(56)

enum { PICKER_HUE = 0, PICKER_BRIGHTNESS, PICKER_INTENSITY };

/**
 * The hue track: LVGL 8's gradients have two stops (LV_GRADIENT_MAX_STOPS 2,
 * kept), so the rainbow is a small image generated once at the slider's own
 * size -- a line of hues repeated down its height -- and drawn under the
 * slider before its own parts, clipped to its rounded ends. The slider draws
 * only its knob on top.
 */
static void hue_track_draw_cb(lv_event_t *event)
{
    hid_pt_view_t *view = lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    if (!view || !view->picker.hue_px || !draw_ctx) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(slider, &area);
    /* The image is exactly the slider's box; any other box would read past it. */
    if ((uint32_t) lv_area_get_width(&area) != view->picker.hue_img.header.w ||
        (uint32_t) lv_area_get_height(&area) != view->picker.hue_img.header.h) {
        return;
    }
    lv_draw_mask_radius_param_t mask;
    lv_draw_mask_radius_init(&mask, &area, PICKER_TRACK_H / 2, false);
    const int16_t mask_id = lv_draw_mask_add(&mask, NULL);
    lv_draw_img_dsc_t dsc;
    lv_draw_img_dsc_init(&dsc);
    lv_draw_img(draw_ctx, &dsc, &area, &view->picker.hue_img);
    lv_draw_mask_remove_id(mask_id);
}

static bool picker_build_hue_image(hid_pt_view_t *view, lv_coord_t w, lv_coord_t h)
{
    view->picker.hue_px = malloc((size_t) w * (size_t) h * sizeof(lv_color_t));
    if (!view->picker.hue_px) {
        return false;
    }
    for (lv_coord_t x = 0; x < w; ++x) {
        view->picker.hue_px[x] = hue_colour((unsigned) ((x * (LIGHTBAR_HUE_MAX + 1) + w / 2) / w));
    }
    for (lv_coord_t y = 1; y < h; ++y) {
        memcpy(&view->picker.hue_px[y * w], view->picker.hue_px, (size_t) w * sizeof(lv_color_t));
    }
    memset(&view->picker.hue_img, 0, sizeof(view->picker.hue_img));
    view->picker.hue_img.header.cf = LV_IMG_CF_TRUE_COLOR;
    view->picker.hue_img.header.w = (uint32_t) w;
    view->picker.hue_img.header.h = (uint32_t) h;
    view->picker.hue_img.data_size = (uint32_t) w * (uint32_t) h * sizeof(lv_color_t);
    view->picker.hue_img.data = (const uint8_t *) view->picker.hue_px;
    return true;
}

/* A picker slider: a slider_row() whose track is the colour it adjusts --
 * a gradient, or the rainbow -- with no fill, just the knob. */
static lv_obj_t *picker_slider(hid_pt_view_t *view, lv_obj_t *parent, const char *label, int32_t min, int32_t max,
                               hid_pt_ctl_t id, int which)
{
    lv_obj_t *row = slider_row(view, parent, label, min, max, id, PICKER_SLIDER_W, VALUE_W,
                               &view->picker.sliders[which], &view->picker.values[which], NULL);
    lv_obj_t *slider = view->picker.sliders[which];
    lv_obj_set_height(slider, PICKER_TRACK_H);
    lv_obj_set_style_radius(slider, PICKER_TRACK_H / 2, 0);
    lv_obj_set_style_bg_opa(slider, which == PICKER_HUE ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    if (which == PICKER_HUE && picker_build_hue_image(view, PICKER_SLIDER_W, PICKER_TRACK_H)) {
        lv_obj_add_event_cb(slider, hue_track_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, view);
    }
    return row;
}

void hid_pt_view_open_picker(hid_pt_view_t *view, const char *heading, int hue, int brightness, int intensity)
{
    if (!view || !view->container || view->picker.veil) {
        return;
    }
    view->picker.return_focus = view->group ? lv_group_get_focused(view->group) : NULL;

    /* The veil takes the whole screen over the sheet, which stays visible
     * behind it; it swallows a pointer click rather than pass it through. */
    lv_obj_t *veil = lv_obj_create(view->container);
    lv_obj_remove_style_all(veil);
    lv_obj_set_size(veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(veil, lv_color_hex(OVERLAY_INK), 0);
    lv_obj_set_style_bg_opa(veil, OVERLAY_OPA_VEIL, 0);
    lv_obj_add_flag(veil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(veil, LV_OBJ_FLAG_SCROLLABLE);
    view->picker.veil = veil;

    /* The sheet's look: ink, a hairline, a soft shadow. */
    lv_obj_t *box = lv_obj_create(veil);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, PICKER_W, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(OVERLAY_INK), 0);
    lv_obj_set_style_bg_opa(box, OVERLAY_OPA_SHEET, 0);
    lv_obj_set_style_radius(box, LV_DPX(10), 0);
    lv_obj_set_style_border_width(box, LV_DPX(1), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(OVERLAY_SEAM), 0);
    lv_obj_set_style_shadow_width(box, LV_DPX(30), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_pad_all(box, LV_DPX(16), 0);
    lv_obj_set_style_pad_gap(box, OPT_GAP, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    view->picker.heading = eyebrow(box, heading ? heading : locstr("LIGHTBAR"), OVERLAY_CHALK, OVERLAY_OPA_MUTED);
    lv_obj_set_style_pad_left(view->picker.heading, LV_DPX(3), 0);
    lv_obj_t *title = body_text(box, locstr("Custom colour"));
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(box), 0);
    lv_obj_set_style_pad_left(title, LV_DPX(3), 0);

    /* The colour as the screen can show it, and the value the bar gets. */
    lv_obj_t *preview_row = lv_obj_create(box);
    lv_obj_remove_style_all(preview_row);
    lv_obj_set_size(preview_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(preview_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(preview_row, LV_DPX(3), 0);
    lv_obj_set_style_pad_gap(preview_row, LV_DPX(12), 0);
    lv_obj_clear_flag(preview_row, LV_OBJ_FLAG_SCROLLABLE);
    view->picker.preview = lv_obj_create(preview_row);
    lv_obj_remove_style_all(view->picker.preview);
    lv_obj_set_size(view->picker.preview, PICKER_PREVIEW * 2, PICKER_PREVIEW);
    lv_obj_set_style_radius(view->picker.preview, OVERLAY_RADIUS, 0);
    lv_obj_set_style_bg_opa(view->picker.preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->picker.preview, LV_DPX(1), 0);
    lv_obj_set_style_border_color(view->picker.preview, lv_color_hex(OVERLAY_SEAM), 0);
    view->picker.hex = body_text(preview_row, "");
    lv_obj_set_style_text_font(view->picker.hex, lv_theme_get_font_large(preview_row), 0);

    picker_slider(view, box, locstr("Colour"), 0, LIGHTBAR_HUE_MAX, HID_PT_CTL_PICKER_HUE, PICKER_HUE);
    picker_slider(view, box, locstr("Brightness"), LIGHTBAR_BRIGHTNESS_MIN, LIGHTBAR_BRIGHTNESS_MAX,
                  HID_PT_CTL_PICKER_BRIGHTNESS, PICKER_BRIGHTNESS);
    picker_slider(view, box, locstr("Intensity"), 0, LIGHTBAR_INTENSITY_MAX, HID_PT_CTL_PICKER_INTENSITY,
                  PICKER_INTENSITY);
    lv_slider_set_value(view->picker.sliders[PICKER_HUE], hue, LV_ANIM_OFF);
    lv_slider_set_value(view->picker.sliders[PICKER_BRIGHTNESS], brightness, LV_ANIM_OFF);
    lv_slider_set_value(view->picker.sliders[PICKER_INTENSITY], intensity, LV_ANIM_OFF);

    lv_obj_t *buttons = lv_obj_create(box);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(buttons, LV_DPX(4), 0);
    lv_obj_set_style_pad_gap(buttons, LV_DPX(10), 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(buttons, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    view->picker.ok_btn = ghost_button(view, buttons, locstr("OK"), HID_PT_CTL_PICKER_OK);
    view->picker.cancel_btn = ghost_button(view, buttons, locstr("CANCEL"), HID_PT_CTL_PICKER_CANCEL);
    /* What the keys do here, in the quiet voice of the sheet's captions: the
     * sheet's own footer is gone, and the veil would dim it anyway. */
    view->picker.hint = caption(box);
    lv_obj_clear_flag(view->picker.hint, LV_OBJ_FLAG_HIDDEN);

    hid_pt_view_rebuild_focus_order(view);
    lv_group_focus_obj(view->picker.sliders[PICKER_HUE]);
    hid_pt_view_set_hints(view, HID_PT_ZONE_PICKER, false);
}

void hid_pt_view_close_picker(hid_pt_view_t *view)
{
    if (!view || !view->picker.veil) {
        return;
    }
    lv_obj_t *veil = view->picker.veil;
    lv_obj_t *back = view->picker.return_focus;
    lv_color_t *hue_px = view->picker.hue_px;
    if (hue_px) {
        lv_img_cache_invalidate_src(&view->picker.hue_img);
    }
    /* Deleted while its controls are still in the group: this runs inside
     * one of their own events (BACK on a slider, OK's click), and LVGL resets
     * the key input for a deleted object only when it is the focused one of
     * the input's group -- out of the group first, the input would go on to
     * send the rest of that key to a freed object. The group goes back to the
     * sheet afterwards. */
    lv_obj_del(veil);
    memset(&view->picker, 0, sizeof(view->picker));
    free(hue_px);
    hid_pt_view_rebuild_focus_order(view);
    if (back && lv_obj_is_valid(back) && hid_pt_view_obj_is_focusable(view, back)) {
        lv_group_focus_obj(back);
    }
}

bool hid_pt_view_picker_is_open(const hid_pt_view_t *view)
{
    return view && view->picker.veil != NULL;
}

void hid_pt_view_picker_values(const hid_pt_view_t *view, int *hue, int *brightness, int *intensity)
{
    if (!hid_pt_view_picker_is_open(view)) {
        return;
    }
    *hue = (int) lv_slider_get_value(view->picker.sliders[PICKER_HUE]);
    *brightness = (int) lv_slider_get_value(view->picker.sliders[PICKER_BRIGHTNESS]);
    *intensity = (int) lv_slider_get_value(view->picker.sliders[PICKER_INTENSITY]);
}

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void hid_pt_view_picker_show(hid_pt_view_t *view, uint32_t rgb)
{
    if (!hid_pt_view_picker_is_open(view)) {
        return;
    }
    int hue = 0, brightness = 0, intensity = 0;
    hid_pt_view_picker_values(view, &hue, &brightness, &intensity);
    /* Black is Off's; a colour the picker makes is never black (V >= 1), but
     * intensity 0 at the lowest brightness is a grey the screen shows white. */
    lv_obj_set_style_bg_color(view->picker.preview, lv_color_hex(lightbar_colour_display(rgb)), 0);
    char text[16];
    snprintf(text, sizeof(text), "#%06X", (unsigned) (rgb & 0xFFFFFFu));
    set_text_if_changed(view->picker.hex, text);
    snprintf(text, sizeof(text), "%d°", hue);
    set_text_if_changed(view->picker.values[PICKER_HUE], text);
    snprintf(text, sizeof(text), "%d %%", brightness);
    set_text_if_changed(view->picker.values[PICKER_BRIGHTNESS], text);
    snprintf(text, sizeof(text), "%d %%", intensity);
    set_text_if_changed(view->picker.values[PICKER_INTENSITY], text);
    /* Brightness: dark to this hue and intensity at full; intensity: white to
     * this hue fully saturated -- what moving the knob that way does. */
    const uint32_t full = lightbar_colour_from_hsv((unsigned) hue, (unsigned) intensity, 255);
    const uint32_t pure = lightbar_colour_from_hsv((unsigned) hue, LIGHTBAR_INTENSITY_MAX, 255);
    lv_obj_t *b = view->picker.sliders[PICKER_BRIGHTNESS];
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(full), 0);
    lv_obj_t *s = view->picker.sliders[PICKER_INTENSITY];
    lv_obj_set_style_bg_color(s, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(pure), 0);
}

bool hid_pt_view_picker_nudge(hid_pt_view_t *view, lv_obj_t *obj, int dir)
{
    if (!view || hid_pt_view_kind_of(view, obj) != HID_PT_WK_PICKER_SLIDER) {
        return false;
    }
    const int32_t step = obj == view->picker.sliders[PICKER_HUE] ? 5 : 1;
    const int32_t min = lv_slider_get_min_value(obj);
    const int32_t max = lv_slider_get_max_value(obj);
    int32_t next = lv_slider_get_value(obj) + (int32_t) dir * step;
    next = next < min ? min : next > max ? max : next;
    if (next != lv_slider_get_value(obj)) {
        lv_slider_set_value(obj, next, LV_ANIM_OFF);
        lv_event_send(obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return true;
}

lv_obj_t *hid_pt_view_picker_step(const hid_pt_view_t *view, lv_obj_t *from, int dir)
{
    if (!hid_pt_view_picker_is_open(view) || dir == 0) {
        return NULL;
    }
    lv_obj_t *const chain[4] = {view->picker.sliders[PICKER_HUE], view->picker.sliders[PICKER_BRIGHTNESS],
                                view->picker.sliders[PICKER_INTENSITY], view->picker.ok_btn};
    int at = -1;
    for (int i = 0; i < 4; ++i) {
        if (chain[i] == from) {
            at = i;
        }
    }
    if (from == view->picker.cancel_btn) {
        at = 3;
    }
    const int to = at + (dir > 0 ? 1 : -1);
    return at >= 0 && to >= 0 && to < 4 ? chain[to] : NULL;
}

lv_obj_t *hid_pt_view_picker_step_button(const hid_pt_view_t *view, lv_obj_t *from, int dir)
{
    if (!hid_pt_view_picker_is_open(view)) {
        return NULL;
    }
    if (from == view->picker.ok_btn && dir > 0) {
        return view->picker.cancel_btn;
    }
    if (from == view->picker.cancel_btn && dir < 0) {
        return view->picker.ok_btn;
    }
    return NULL;
}

void hid_pt_view_destroy(hid_pt_view_t *view)
{
    if (!view) {
        return;
    }
    /* The objects that drew them are gone with the panel's tree. */
    if (view->picker.hue_px) {
        lv_img_cache_invalidate_src(&view->picker.hue_img);
        free(view->picker.hue_px);
        view->picker.hue_px = NULL;
    }
    if (view->custom_ring_px) {
        lv_img_cache_invalidate_src(&view->custom_ring);
        free(view->custom_ring_px);
        view->custom_ring_px = NULL;
    }
    memset(&view->picker, 0, sizeof(view->picker));
    if (view->group) {
        lv_group_del(view->group);
        view->group = NULL;
    }
}

#endif
