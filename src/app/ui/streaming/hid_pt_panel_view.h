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
 * The order matters: everything from HID_PT_WK_CHECKBOX on is an option
 * control, i.e. lives in the right-hand pane, which is what
 * hid_pt_view_kind_is_option() tests.
 */
typedef enum {
    HID_PT_WK_NONE = 0,
    /** A plug button in the device list. */
    HID_PT_WK_PLUG,
    /** A button in the sheet header: Refresh or Close. */
    HID_PT_WK_HEADER_BTN,
    HID_PT_WK_CHECKBOX,
    HID_PT_WK_SLIDER,
    HID_PT_WK_DROPDOWN,
    HID_PT_WK_OPTION_BTN,
    /** Some other part of an option row, e.g. the haptics label. */
    HID_PT_WK_OPTION_OTHER,
} hid_pt_widget_kind_t;

static inline bool hid_pt_view_kind_is_option(hid_pt_widget_kind_t kind)
{
    return kind >= HID_PT_WK_CHECKBOX;
}

typedef struct {
    void *userdata;
    /** A control the user just changed (LV_EVENT_VALUE_CHANGED). */
    void (*value_changed)(void *userdata, hid_pt_ctl_t id);
    /** A button the user just pressed (LV_EVENT_CLICKED). */
    void (*clicked)(void *userdata, hid_pt_ctl_t id);
    void (*row_clicked)(void *userdata, int row);
    void (*plug_clicked)(void *userdata, int row);
    void (*plug_focused)(void *userdata, int row);
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
    lv_obj_t *list;
    lv_obj_t *composite_row;
    lv_obj_t *composite_cb;
    lv_obj_t *auto_plugin_row;
    lv_obj_t *auto_plugin_cb;
    lv_obj_t *customize_panel;
    lv_obj_t *customize_title;
    lv_obj_t *latency_label;
    lv_obj_t *latency_slider;
    lv_obj_t *audio_dropdown;
    lv_obj_t *speaker_label;
    lv_obj_t *speaker_slider;
    lv_obj_t *headset_label;
    lv_obj_t *headset_slider;
    lv_obj_t *haptics_row;
    lv_obj_t *haptics_label;
    lv_obj_t *haptics_slider;
    lv_obj_t *battery_label;
    lv_obj_t *audio_warning_label;
    lv_obj_t *reset_settings_btn;
    lv_obj_t *refresh_btn;
    lv_obj_t *close_btn;
    lv_group_t *group;
    lv_obj_t *row_buttons[HID_PT_MAX_ROWS];
    lv_obj_t *plug_buttons[HID_PT_MAX_ROWS];
    lv_obj_t *plug_labels[HID_PT_MAX_ROWS];
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

void hid_pt_view_set_plug_label(hid_pt_view_t *view, int row, bool plugged);

/** Which of the panel's controls @p obj is, or HID_PT_WK_NONE. */
hid_pt_widget_kind_t hid_pt_view_kind_of(const hid_pt_view_t *view, lv_obj_t *obj);

/* ---- focus -------------------------------------------------------------- */

/** Rebuild the focus group: device rows first, then the option column. */
void hid_pt_view_rebuild_focus_order(hid_pt_view_t *view);

bool hid_pt_view_is_rebuilding(const hid_pt_view_t *view);

/** The row whose plug button is @p obj, or -1. */
int hid_pt_view_plug_row_of(const hid_pt_view_t *view, lv_obj_t *obj);

/** Focus the plug button of @p row, if that row exists. */
void hid_pt_view_focus_plug(hid_pt_view_t *view, int row);

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

/** @p default_ms is shown next to the current value; the panel supplies it. */
void hid_pt_view_update_latency_label(hid_pt_view_t *view, int default_ms);
void hid_pt_view_update_speaker_label(hid_pt_view_t *view);
void hid_pt_view_update_headset_label(hid_pt_view_t *view);
void hid_pt_view_update_haptics_label(hid_pt_view_t *view);
