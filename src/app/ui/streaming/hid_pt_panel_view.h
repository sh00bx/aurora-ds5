#pragma once

/**
 * The HID passthrough panel's widgets: what they are, how they are built, and
 * every operation that only needs LVGL to answer.
 *
 * Nothing here knows what a device is. The list is filled row by row from
 * strings and flags the caller supplies, and every event the widgets raise is
 * handed straight back through hid_pt_view_cbs_t -- so this file cannot decide
 * anything about a controller, and hid_passthrough_panel.c stays the only place
 * where a widget event meets the device model.
 */

#include <lvgl.h>

#include <stdbool.h>

#define HID_PT_MAX_ROWS 64

#define DS_LATENCY_MIN 0
#define DS_LATENCY_MAX 200
#define DS_VOLUME_MAX 100
#define DS_HAPTICS_MAX 200

/** The controls the option column carries, in focus order. */
typedef enum {
    HID_PT_CTL_COMPOSITE = 0,
    HID_PT_CTL_AUTO_PLUGIN,
    HID_PT_CTL_LATENCY,
    HID_PT_CTL_AUDIO_MODE,
    HID_PT_CTL_SPEAKER,
    HID_PT_CTL_HEADSET,
    HID_PT_CTL_HAPTICS,
    HID_PT_CTL_RESET,
    HID_PT_CTL_REFRESH,
    HID_PT_CTL_CLOSE,
} hid_pt_ctl_t;

/**
 * What the panel's key handling needs to know about an object.
 *
 * The order matters: everything from HID_PT_WK_SWITCH on is an option control,
 * i.e. lives in the right-hand pane, which is what
 * hid_pt_view_kind_is_option() tests.
 */
typedef enum {
    HID_PT_WK_NONE = 0,
    /** A device row. It is the whole control: OK on it plugs the device. */
    HID_PT_WK_ROW,
    /** A button in the sheet header: Rescan or Close. */
    HID_PT_WK_HEADER_BTN,
    HID_PT_WK_SWITCH,
    HID_PT_WK_SLIDER,
    HID_PT_WK_DROPDOWN,
    HID_PT_WK_OPTION_BTN,
} hid_pt_widget_kind_t;

static inline bool hid_pt_view_kind_is_option(hid_pt_widget_kind_t kind)
{
    return kind >= HID_PT_WK_SWITCH;
}

/**
 * Which part of the sheet the cursor is in.
 *
 * The three are laid out as the eye sees them — the header above, the device
 * list left, the settings right — and the arrow keys move within a zone rather
 * than along one flat focus chain. It is also what the footer names its keys
 * for: OK plugs a device in the list and does nothing to a slider.
 */
typedef enum {
    HID_PT_ZONE_LIST = 0,
    HID_PT_ZONE_OPTIONS,
    HID_PT_ZONE_HEADER,
} hid_pt_zone_t;

typedef struct {
    void *userdata;
    /** A control the user just changed (LV_EVENT_VALUE_CHANGED). */
    void (*value_changed)(void *userdata, hid_pt_ctl_t id);
    /** A button the user just pressed (LV_EVENT_CLICKED). */
    void (*clicked)(void *userdata, hid_pt_ctl_t id);
    /** A device row was activated — by OK, or by a click. */
    void (*row_clicked)(void *userdata, int row);
    void (*row_focused)(void *userdata, int row);
    /** LV_EVENT_KEY on any control, and on the sheet itself. */
    void (*key)(void *userdata, lv_event_t *event);
    /** LV_EVENT_KEY | LV_EVENT_PREPROCESS on the audio dropdown only. */
    void (*dropdown_key)(void *userdata, lv_event_t *event);
    /** The panel's root object is being deleted. */
    void (*deleted)(void *userdata);
} hid_pt_view_cbs_t;

typedef struct {
    lv_obj_t *container;
    lv_obj_t *sheet;
    lv_obj_t *status_label;
    lv_obj_t *error_label;
    lv_obj_t *error_row;
    lv_obj_t *list;
    lv_obj_t *composite_row;
    lv_obj_t *composite_cb;
    lv_obj_t *auto_plugin_row;
    lv_obj_t *auto_plugin_cb;
    lv_obj_t *customize_panel;
    lv_obj_t *customize_title;
    lv_obj_t *customize_state;
    lv_obj_t *audio_heading;
    lv_obj_t *latency_row;
    lv_obj_t *latency_label;
    lv_obj_t *latency_value;
    lv_obj_t *latency_slider;
    lv_obj_t *audio_row;
    lv_obj_t *audio_dropdown;
    lv_obj_t *speaker_row;
    lv_obj_t *speaker_value;
    lv_obj_t *speaker_slider;
    lv_obj_t *headset_row;
    lv_obj_t *headset_value;
    lv_obj_t *headset_slider;
    lv_obj_t *haptics_row;
    lv_obj_t *haptics_label;
    lv_obj_t *haptics_value;
    lv_obj_t *haptics_slider;
    lv_obj_t *audio_warning_label;
    lv_obj_t *reset_settings_btn;
    lv_obj_t *refresh_btn;
    lv_obj_t *close_btn;
    lv_obj_t *hint_label;
    lv_group_t *group;
    lv_obj_t *row_buttons[HID_PT_MAX_ROWS];
    lv_obj_t *row_state_labels[HID_PT_MAX_ROWS];
    lv_obj_t *row_rails[HID_PT_MAX_ROWS];
    /** The dropdown whose list OK opened, so ESC can close the same one. */
    lv_obj_t *active_dropdown;
    /* Set while hid_pt_view_rebuild_focus_order() is emptying and refilling the
     * focus group. LVGL auto-focuses the first object added to an empty group,
     * which fires the plug-focus callback on row 0; the panel reads this flag
     * there and skips the selection bookkeeping. */
    bool rebuilding;
    hid_pt_view_cbs_t cbs;
} hid_pt_view_t;

/** Build the whole panel. Returns its root object, or NULL. */
lv_obj_t *hid_pt_view_create(hid_pt_view_t *view, lv_obj_t *parent, const hid_pt_view_cbs_t *cbs);

/** Release what the view owns outside the LVGL object tree (the focus group). */
void hid_pt_view_destroy(hid_pt_view_t *view);

/* ---- the device list ---------------------------------------------------- */

/** Empty the list and forget every row. */
void hid_pt_view_list_clear(hid_pt_view_t *view);

/** Put the "nothing here" placeholder in the emptied list. */
void hid_pt_view_list_show_empty(hid_pt_view_t *view);

/** Lay the emptied list out for rows. Call once before the first add_row(). */
void hid_pt_view_list_prepare(hid_pt_view_t *view);

void hid_pt_view_add_row(hid_pt_view_t *view, int row, const char *label, bool plugged, bool selected);

bool hid_pt_view_has_row(const hid_pt_view_t *view, int row);

void hid_pt_view_set_row_selected(hid_pt_view_t *view, int row, bool selected);

/** Repaint @p row's rail and state line for its new bridge state. */
void hid_pt_view_set_row_state(hid_pt_view_t *view, int row, bool plugged);

/** Which of the panel's controls @p obj is, or HID_PT_WK_NONE. */
hid_pt_widget_kind_t hid_pt_view_kind_of(const hid_pt_view_t *view, lv_obj_t *obj);

/** Which zone @p obj lives in. HID_PT_ZONE_LIST for anything unrecognised. */
hid_pt_zone_t hid_pt_view_zone_of(const hid_pt_view_t *view, lv_obj_t *obj);

/** Name the arrow keys for what they do in @p zone, along the sheet's foot. */
void hid_pt_view_set_hints(hid_pt_view_t *view, hid_pt_zone_t zone, bool plugged);

/* ---- focus -------------------------------------------------------------- */

/** Rebuild the focus group: device rows first, then the option column. */
void hid_pt_view_rebuild_focus_order(hid_pt_view_t *view);

bool hid_pt_view_is_rebuilding(const hid_pt_view_t *view);

/** The row @p obj is, or -1. */
int hid_pt_view_row_of(const hid_pt_view_t *view, lv_obj_t *obj);

/** Focus @p row, if it exists. */
void hid_pt_view_focus_row(hid_pt_view_t *view, int row);

/**
 * The option control the cursor should land on when it enters the settings
 * column, or NULL when the column is empty for this device.
 */
lv_obj_t *hid_pt_view_first_option(const hid_pt_view_t *view);

/** The option control @p from should move to, or NULL at the end of the column. */
lv_obj_t *hid_pt_view_step_option(const hid_pt_view_t *view, lv_obj_t *from, int step);

/** Scroll @p row's list entry into view without touching focus. */
void hid_pt_view_scroll_row_into_view(hid_pt_view_t *view, int row);

/**
 * True while @p obj sits under a hidden ancestor (or is hidden itself).
 *
 * Deliberately not lv_obj_is_visible(): that also answers "false" for a control
 * that is merely scrolled out of the settings column, which is a place the user
 * can legitimately be.
 */
bool hid_pt_view_obj_is_hidden(const hid_pt_view_t *view, lv_obj_t *obj);

/* ---- the audio dropdown's list ------------------------------------------ */

bool hid_pt_view_dropdown_is_open(const hid_pt_view_t *view, lv_obj_t *target);
void hid_pt_view_open_dropdown(hid_pt_view_t *view, lv_obj_t *dropdown);
void hid_pt_view_close_dropdown(hid_pt_view_t *view, lv_obj_t *dropdown);

/* ---- the option column's labels ----------------------------------------- */

/** @p default_ms is named in the row's label; the panel supplies it. */
void hid_pt_view_update_latency_label(hid_pt_view_t *view, int default_ms);
void hid_pt_view_update_speaker_label(hid_pt_view_t *view);
void hid_pt_view_update_headset_label(hid_pt_view_t *view);
void hid_pt_view_update_haptics_label(hid_pt_view_t *view);

/**
 * Move a focused slider by one step of its own range.
 *
 * The sliders take LEFT and RIGHT directly, with no edit mode in between: on a
 * 0..200 range LVGL's own ±1 would need two hundred presses, and the OK-to-edit
 * dance it belongs to made every value change a three-key affair. Returns false
 * when @p obj is not a slider, so the caller can treat the key as navigation.
 */
bool hid_pt_view_nudge_slider(hid_pt_view_t *view, lv_obj_t *obj, int dir);
