#if defined(TARGET_WEBOS)

/**
 * The Controllers panel's wiring.
 *
 * Three files, one job each. hid_pt_panel_view.c builds and drives the widgets;
 * its TU has no include path to the model, so it cannot name a device.
 * hid_pt_panel_model.c owns the selection and is the panel's only door to the
 * CTM globals and the session's SDL pads; this TU in turn has no include path
 * to ctm_state.h. That leaves this file as the one place where a widget event
 * meets a device.
 *
 * The two directions are kept apart:
 *   - sync_customize_ui_from_settings(), update_mode_row(),
 *     update_lightbar_row() and update_device_options() push the model into the
 *     widgets. They run on the 2 s refresh as well as on a selection change, so
 *     they must not overwrite a control the user currently owns -- see the
 *     comment on sync_customize_ui_from_settings() for why the audio dropdown
 *     is the control where that is enforced, and why the sliders need no such
 *     guard.
 *   - customize_setting_changed(), the composite and lightbar-game branches of
 *     panel_value_changed(), panel_mode_clicked(), panel_swatch_clicked() and
 *     the colour picker's OK push the widgets into the model. Every caller is
 *     a widget's LV_EVENT_VALUE_CHANGED or a button's click, i.e. a change the
 *     user just made. The picker's sliders only preview (picker_changed()).
 */

#include "hid_passthrough_panel.h"
#include "hid_pt_panel_model.h"
#include "hid_pt_panel_view.h"
#include "lightbar_colour.h"
#include "overlay_style.h"

#include "hid_passthrough/hid_passthrough_manager.h"
#include "lvgl/font/fa_brands_400_symbols.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "stream/session.h"

#include "util/bus.h"
#include "util/i18n.h"
#include "util/user_event.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The mode row, left to right: mounted as HID, then one button per controller
 * type the host can emulate for a pad over SDL -- Xbox 360 and DualShock 4
 * over ViGEm, DualSense and Switch Pro over Vibepollo's own virtual-gamepad
 * driver (an Xbox Series pad is not wanted for now). A type the host learns
 * later is one more line here -- and one more gamepad_type_pref_t,
 * gamepad_mode_t and wire value (gamepad_type_pref.h). The two PlayStation
 * pads share Font Awesome's mark and differ by name; neither Font Awesome Free
 * nor Simple Icons has a Nintendo mark, so SWITCH wears Material's gamepad
 * (sports_esports, the one whose grips look like a Pro controller's). Last,
 * GAME: not a mode but the lock that fixes the mode for the game being
 * streamed (GAMEPAD_MODE_NONE marks it).
 *
 * The labels are gamepad_mode_label()'s: what a row's state line, the
 * overlay's pad badge and the LIGHTBAR heading say.
 */
static const struct {
    const char *glyph;
    const char *label;
    gamepad_mode_t mode;
} MODES[] = {
        {MAT_SYMBOL_USB,            "HID",    GAMEPAD_MODE_HID},
        {FA_SYMBOL_XBOX,            "X360",   GAMEPAD_MODE_X360},
        {FA_SYMBOL_PLAYSTATION,     "DS4",    GAMEPAD_MODE_DS4},
        {FA_SYMBOL_PLAYSTATION,     "DS5",    GAMEPAD_MODE_DS5},
        {MAT_SYMBOL_SPORTS_ESPORTS, "SWITCH", GAMEPAD_MODE_SWITCH},
        {MAT_SYMBOL_LOCK,           "GAME",   GAMEPAD_MODE_NONE},
};

#define MODE_COUNT ((int) (sizeof(MODES) / sizeof(MODES[0])))

/* The lock button, as opposed to a mode. */
static bool mode_is_lock(int i)
{
    return MODES[i].mode == GAMEPAD_MODE_NONE;
}

static bool mode_is_hid(int i)
{
    return MODES[i].mode == GAMEPAD_MODE_HID;
}

_Static_assert(sizeof(MODES) / sizeof(MODES[0]) <= HID_PT_MAX_MODES, "the view holds HID_PT_MAX_MODES buttons");

/**
 * The LIGHTBAR row, two lines of eight: Automatic (the bar as it always was --
 * a dark disc with an "A", since it is no colour), Off (a dark bar, painted
 * like a colour) and thirteen colours. Dim on purpose: no channel above 0x04,
 * the level the user picked for red (040000), the others about as bright --
 * a DS4's bar is the hungriest thing on it, and a lightbar in a dark room
 * needs little. A disc does not show that value, which would be black on
 * screen, but the colour at full brightness (lightbar_colour_display():
 * 040100 is drawn FF4000). Last comes Custom (hid_pt_view_add_custom_swatch()),
 * which opens the colour picker and stands for any colour not listed here.
 */
static const struct {
    bool automatic;
    uint32_t rgb;
    const char *text;
    const char *glyph;
} SWATCHES[] = {
        {true,  0,        "A",  NULL},
        {false, 0x000000, NULL, MAT_SYMBOL_CLOSE},
        {false, 0x040000, NULL, NULL},    /* red */
        {false, 0x040100, NULL, NULL},    /* orange */
        {false, 0x040300, NULL, NULL},    /* yellow */
        {false, 0x020400, NULL, NULL},    /* lime */
        {false, 0x000400, NULL, NULL},    /* green */
        {false, 0x000402, NULL, NULL},    /* teal */
        {false, 0x000304, NULL, NULL},    /* cyan */
        {false, 0x000204, NULL, NULL},    /* sky */
        {false, 0x000004, NULL, NULL},    /* blue */
        {false, 0x020004, NULL, NULL},    /* violet */
        {false, 0x040003, NULL, NULL},    /* magenta */
        {false, 0x040102, NULL, NULL},    /* pink */
        {false, 0x030303, NULL, NULL},    /* white */
};

#define SWATCH_COUNT ((int) (sizeof(SWATCHES) / sizeof(SWATCHES[0])))

_Static_assert(sizeof(SWATCHES) / sizeof(SWATCHES[0]) + 1 <= HID_PT_MAX_SWATCHES,
               "the view holds HID_PT_MAX_SWATCHES swatches, Custom included");

/* The picker's live preview goes out at most this often (10 Hz): every slider
 * step would otherwise be a report to a mounted pad, or an LED write and a
 * painter datagram. */
#define PICKER_PREVIEW_MS 100

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
    /* The colour picker, while it is open: the colour the controller had
     * (what a cancel leaves stored), the colour the picker holds -- the stored
     * one exactly until a slider moves -- and whether the preview still has to
     * go out on the next tick of its timer. */
    struct {
        lightbar_pref_t from;
        uint32_t rgb;
        bool pending;
        lv_timer_t *timer;
    } picker;
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
static void update_mode_row(hid_pt_panel_t *panel);
static void panel_select_device(hid_pt_panel_t *panel, int row);
static void panel_focus_selected_row(hid_pt_panel_t *panel);
static void panel_update_hints(hid_pt_panel_t *panel, lv_obj_t *focused);
static void panel_focus(hid_pt_panel_t *panel, lv_obj_t *obj);
static void refresh_devices(hid_pt_panel_t *panel, bool rescan);
static void picker_key(hid_pt_panel_t *panel, lv_event_t *event, lv_obj_t *target, uint32_t key,
                       hid_pt_widget_kind_t kind);

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
 * The downward bound is the model's *current* row count, not the number of
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
    for (int j = from + 1; j < hid_pt_model_row_count(&panel->model) && j < HID_PT_MAX_ROWS; ++j) {
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
    const hid_pt_widget_kind_t kind = hid_pt_view_kind_of(&panel->view, focused);
    if (kind == HID_PT_WK_MODE_BTN || kind == HID_PT_WK_SWATCH) {
        zone = HID_PT_ZONE_MODE;
    } else if (kind == HID_PT_WK_PICKER_BTN) {
        zone = HID_PT_ZONE_PICKER_BUTTONS;
    }
    hid_pt_view_set_hints(&panel->view, zone, hid_pt_model_selected_is_plugged(&panel->model));
}

static void panel_dropdown_key(void *userdata, lv_event_t *event)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel || !panel->view.group || lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    if (!lv_obj_has_class(target, &lv_dropdown_class)) {
        return;
    }
    if (hid_pt_view_dropdown_is_open(&panel->view, target)) {
        /* The open list owns the keys; LVGL's own KEY handler (which runs right
         * after this preprocess hook) walks it and, on BACK, restores the
         * selection and closes it. Only BACK needs a note: the panel's later
         * ESC handling must know the key was spent on the list, or it would
         * also step the cursor out of the settings column. */
        if (lv_event_get_key(event) == LV_KEY_ESC) {
            panel->view.dropdown_esc_spent = true;
            hid_pt_view_forget_dropdown(&panel->view);
            panel_update_hints(panel, target);
        }
        return;
    }
    const uint32_t key = lv_event_get_key(event);
    switch (key) {
        case LV_KEY_UP:
        case LV_KEY_DOWN: {
            lv_obj_t *next = hid_pt_view_step_option(&panel->view, target, key == LV_KEY_UP ? -1 : 1);
            if (!next && key == LV_KEY_UP) {
                /* Off the top of the column is the header, exactly as from any
                 * other control there -- stopping the event below would
                 * otherwise keep panel_control_key() from getting there. */
                next = panel->view.close_btn;
            }
            panel_focus(panel, next);
            lv_event_stop_processing(event);
            return;
        }
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT: {
            /* LEFT/RIGHT steps the audio dropdown's value where it steps a
             * slider's, so the audio block answers to one pair of keys. OK
             * still opens the full list for anyone who wants to see every
             * entry. */
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

/* The footer follows the list: while it is up the keys pick an option, and the
 * moment it is gone they are the settings column's again. */
static void panel_dropdown_toggled(void *userdata, lv_obj_t *dropdown, bool open)
{
    hid_pt_panel_t *panel = userdata;
    if (!panel) {
        return;
    }
    if (open) {
        hid_pt_view_set_hints(&panel->view, HID_PT_ZONE_DROPDOWN,
                              hid_pt_model_selected_is_plugged(&panel->model));
    } else {
        panel_update_hints(panel, dropdown);
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
 *   UP/DOWN     the next thing in this column, and no further; the mode row
 *               is one stop of it
 *   RIGHT       from a device, into its settings
 *   LEFT        from a setting, back to the device — or, on a slider, DOWN a
 *               step, because a slider owns both horizontal keys outright
 *   LEFT/RIGHT  on the mode row, the next button that can be pressed; on the
 *               LIGHTBAR row the next swatch of its line, and past the first
 *               line's last its switch; LEFT off a line's first is back to the
 *               device
 *   UP/DOWN     on the LIGHTBAR row, the swatch in the other line first
 *   OK          plugs the focused device in or out; opens the dropdown;
 *               selects a mode or a colour; on Custom, opens the picker
 *   BACK        from the settings, back to the devices; from there, closes
 *
 * The colour picker, while it is up, takes every key (picker_key()): UP/DOWN
 * between its sliders and its buttons, LEFT/RIGHT move a slider (the hue by 5)
 * or step between OK and Cancel, OK presses a button, BACK cancels.
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

    if (zone == HID_PT_ZONE_PICKER) {
        picker_key(panel, event, target, key, kind);
        return;
    }
    switch (key) {
        case LV_KEY_ESC:
            if (kind == HID_PT_WK_DROPDOWN && panel->view.dropdown_esc_spent) {
                /* This BACK already cancelled the open list — LVGL's own KEY
                 * handler restored the selection and closed it before this
                 * handler ran. The cursor stays on the row. */
                panel->view.dropdown_esc_spent = false;
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
            if (kind == HID_PT_WK_DROPDOWN) {
                /* Hands off: LVGL's own press/release handling toggles the list
                 * on the key's RELEASE — open it here on the KEY (which arrives
                 * at key DOWN) and that same release promptly closes it again,
                 * which is the "stays open only while OK is held" bug. The
                 * view's dropdown_state_sync_cb picks the toggle up afterwards. */
                return;
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
                const bool lightbar = kind == HID_PT_WK_SWATCH || target == panel->view.lightbar_game_cb;
                if ((kind == HID_PT_WK_MODE_BTN || lightbar) && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
                    /* Only moves: a mode or a colour changes on OK alone.
                     * Walking the row must never mount a pad, replace the
                     * host's one or repaint a bar. */
                    lv_obj_t *next = lightbar ? hid_pt_view_step_lightbar(&panel->view, target, dir)
                                              : hid_pt_view_step_mode(&panel->view, target, dir);
                    if (next) {
                        panel_focus(panel, next);
                    } else if (key == LV_KEY_LEFT) {
                        panel_focus_selected_row(panel);
                    }
                } else if (key == LV_KEY_LEFT) {
                    panel_focus_selected_row(panel);
                } else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
                    /* The LIGHTBAR row's two lines first: UP/DOWN goes to the
                     * swatch above or below, and off the row from its first or
                     * last line. */
                    lv_obj_t *next = NULL;
                    if (kind == HID_PT_WK_SWATCH) {
                        next = hid_pt_view_step_lightbar_line(&panel->view, target, dir);
                    }
                    if (!next) {
                        next = hid_pt_view_step_option(&panel->view, target, dir);
                    }
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

/* The mode button lit for a row: HID while it is mounted, else the pad the
 * host builds for it over SDL. -1 for a row with no mode (not a controller). */
static int lit_mode(const hid_pt_row_info_t *info)
{
    for (int i = 0; i < MODE_COUNT && info->is_gamepad; ++i) {
        if (mode_is_lock(i)) {
            continue;
        }
        if (mode_is_hid(i) ? info->plugged
                           : (!info->plugged && gamepad_mode_sdl_type(MODES[i].mode) == info->effective_type)) {
            return i;
        }
    }
    return -1;
}

/**
 * A row's second line, and whether it wears the bridge's teal: the name of its
 * lit mode button -- "HID" for a mounted controller, "X360", "DS4", "DS5" or
 * "SWITCH" for the pad the host builds for it over SDL -- or "IDLE" for a
 * device that is not a controller.
 */
static const char *row_state_text(const hid_pt_row_info_t *info, bool *live)
{
    *live = info->plugged;
    if (info->plugged) {
        return locstr("HID");
    }
    const int mode = lit_mode(info);
    return mode >= 0 ? locstr(MODES[mode].label) : locstr("IDLE");
}

/* Repaint the selected row's state line now, rather than on the next re-render
 * the signature change will bring 2 s later. */
static void refresh_selected_row_state(hid_pt_panel_t *panel)
{
    hid_pt_row_info_t info;
    if (!hid_pt_view_has_row(&panel->view, panel->selected_index) ||
        !hid_pt_model_selected_row_info(&panel->model, &info)) {
        return;
    }
    bool live = false;
    const char *state = row_state_text(&info, &live);
    hid_pt_view_set_row_state(&panel->view, panel->selected_index, state, live);
}

/**
 * The one line under the device's name: how it reaches the host, and how it is
 * doing. "HID" is tinted with the same teal the device's rail wears in the
 * list, so the two say the same thing in the same colour.
 */
static void update_state_line(hid_pt_panel_t *panel)
{
    if (!panel || !panel->view.customize_state) {
        return;
    }
    char battery[64];
    const bool has_battery = hid_pt_model_battery_text(&panel->model, battery, sizeof(battery));
    hid_pt_row_info_t info;
    if (!hid_pt_model_selected_row_info(&panel->model, &info)) {
        memset(&info, 0, sizeof(info));
    }
    bool plugged = false;
    const char *state = row_state_text(&info, &plugged);
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
     * the widget itself is the truth about its own list, and the bookkeeping is
     * one event behind it by design (it syncs after LVGL's toggles). */
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
    if (v->trigger_slider) {
        int level = (int) c.trigger_reduce;
        if (level > DS_TRIGGER_REDUCE_MAX) {
            level = DS_TRIGGER_REDUCE_MAX;
        }
        lv_slider_set_value(v->trigger_slider, level, LV_ANIM_OFF);
        hid_pt_view_update_trigger_label(v);
    }
    show_row(v->haptics_pair, hid_pt_model_selected_is_ds5(&panel->model));
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
    if (v->trigger_slider) {
        /* DualSense-only as well. */
        c.trigger_reduce = (unsigned) lv_slider_get_value(v->trigger_slider);
    }
    if (!hid_pt_model_write_controls(&panel->model, &c)) {
        return;
    }
    update_audio_warning(panel, &c);
}

/**
 * Show the settings the selected device actually has.
 *
 * The whole column used to be hidden at once for anything that is not a
 * PlayStation pad, which meant a bridgeable device with a setting of its own
 * had nowhere to show it. Rows are hidden one group at a time now, and the flex
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
    show_row(v->volume_pair, has_audio);
    show_row(v->latency_row, has_audio);
    show_row(v->reset_settings_btn, has_audio);
    /* The haptics pair is hidden from inside sync_...(), which only runs for a
     * device that has the settings record to read it from. */
    if (!has_audio) {
        show_row(v->haptics_pair, false);
        show_row(v->audio_warning_label, false);
    }
    show_row(v->customize_state, have_device);
    update_state_line(panel);
}

/* Shown only for a device the setting applies to, mirroring that device's
 * stored value while it is. The value is written only when the settings record
 * could be read, so a device without one leaves the switch where it was rather
 * than clearing it. */
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

/**
 * The MODE row: shown for a controller, the button of its current mode lit.
 * HID can be pressed only for a device the bridge can mount; an SDL-only pad,
 * or a device CTM classifies as plain HID, has nothing to mount. GAME is there
 * only while a game with a usable name is streamed, lit while it has a lock,
 * and then the line under the row names it.
 */
static void update_mode_row(hid_pt_panel_t *panel)
{
    if (!panel) {
        return;
    }
    hid_pt_row_info_t info;
    const bool show = hid_pt_model_selected_row_info(&panel->model, &info) && info.is_gamepad;
    const bool bridgeable = show && hid_pt_model_selected_is_bridgeable(&panel->model);
    char game[128];
    const bool has_game = hid_pt_model_app_name(&panel->model, game, sizeof(game));
    const bool locked = has_game && hid_pt_model_app_mode(&panel->model) != GAMEPAD_MODE_NONE;
    const int lit_index = show ? lit_mode(&info) : -1;
    unsigned lit = 0;
    unsigned enabled = 0;
    unsigned visible = 0;
    for (int i = 0; i < MODE_COUNT; ++i) {
        const bool on = mode_is_lock(i) ? has_game : (!mode_is_hid(i) || bridgeable);
        const bool shown = !mode_is_lock(i) || has_game;
        lit |= (mode_is_lock(i) ? locked : i == lit_index) ? 1u << i : 0u;
        enabled |= on ? 1u << i : 0u;
        visible |= shown ? 1u << i : 0u;
    }
    lv_obj_t *focused = panel->view.group ? lv_group_get_focused(panel->view.group) : NULL;
    hid_pt_view_set_modes(&panel->view, show, lit, enabled, visible);
    char caption[160];
    if (show && locked) {
        snprintf(caption, sizeof(caption), locstr("Locked for %s"), game);
    }
    hid_pt_view_set_mode_caption(&panel->view, show && locked ? caption : NULL);
    /* The cursor must never stay on a button this just disabled or hid: LVGL
     * hands a disabled focused object no key at all -- no arrow, no OK, not even
     * BACK -- so the page would be stuck on it. The row's entry (the lit button,
     * else the first enabled) when the row is still up, else the next setting,
     * else the device's own row. */
    if (hid_pt_view_mode_of(&panel->view, focused) >= 0 &&
        !hid_pt_view_obj_is_focusable(&panel->view, focused)) {
        lv_obj_t *to = hid_pt_view_first_option(&panel->view);
        if (to) {
            panel_focus(panel, to);
        } else if (hid_pt_view_has_row(&panel->view, panel->selected_index)) {
            hid_pt_view_focus_row(&panel->view, panel->selected_index);
            panel_update_hints(panel, panel->view.row_buttons[panel->selected_index]);
        }
    }
}

/* The swatch for @p lb: its palette entry, else Custom -- a colour the picker
 * made, one of 1.7.30's brighter palette, or one written into the ini by
 * hand. */
static int lit_swatch(const hid_pt_panel_t *panel, const lightbar_pref_t *lb)
{
    for (int i = 0; i < SWATCH_COUNT; ++i) {
        if (SWATCHES[i].automatic ? lb->automatic : (!lb->automatic && SWATCHES[i].rgb == lb->rgb)) {
            return i;
        }
    }
    return panel->view.custom_swatch;
}

/* "LIGHTBAR · DS4": whose colour the swatches, and the picker, are. */
static void lightbar_heading(gamepad_mode_t mode, char *buf, size_t len)
{
    const char *mode_name = gamepad_mode_label(mode);
    if (mode_name) {
        snprintf(buf, len, "%s · %s", locstr("LIGHTBAR"), locstr(mode_name));
    } else {
        snprintf(buf, len, "%s", locstr("LIGHTBAR"));
    }
}

/**
 * The LIGHTBAR row: shown for a controller whose bar aurora can paint, with
 * the colour of the mode its mode row lights -- named in the heading,
 * "LIGHTBAR · DS4" -- ringed, and "Game may change colour" at the end of the
 * first line for anything but Automatic -- Off included, which is a colour too
 * (a dark bar a game could otherwise light up). Under a game's lock that is
 * the locked mode: the controller's own colour for it, never one per game.
 */
static void update_lightbar_row(hid_pt_panel_t *panel)
{
    if (!panel) {
        return;
    }
    lightbar_pref_t lb;
    gamepad_mode_t mode = GAMEPAD_MODE_NONE;
    const bool show = hid_pt_model_selected_has_lightbar(&panel->model) &&
                      hid_pt_model_selected_lightbar(&panel->model, &lb, &mode);
    if (!show) {
        lb = lightbar_pref_automatic();
    }
    /* Which of the controller's colours the swatches are: the lit mode's. */
    char heading[48];
    lightbar_heading(mode, heading, sizeof(heading));
    lv_obj_t *focused = panel->view.group ? lv_group_get_focused(panel->view.group) : NULL;
    const int lit = lit_swatch(panel, &lb);
    const bool custom = lit >= 0 && lit == panel->view.custom_swatch;
    hid_pt_view_set_custom_swatch(&panel->view, custom, custom ? lightbar_colour_display(lb.rgb) : 0);
    hid_pt_view_set_lightbar(&panel->view, show, heading, lit, show && !lb.automatic, lb.game);
    /* The switch just hidden under the cursor (Automatic picked, or another
     * controller): LVGL would keep handing it the keys. Back to the row's
     * entry, else the next setting, else the device's row. */
    if (focused == panel->view.lightbar_game_cb && !hid_pt_view_obj_is_focusable(&panel->view, focused)) {
        lv_obj_t *to = hid_pt_view_first_option(&panel->view);
        if (show) {
            to = hid_pt_view_step_lightbar(&panel->view, focused, -1);
        }
        if (to) {
            panel_focus(panel, to);
        } else if (hid_pt_view_has_row(&panel->view, panel->selected_index)) {
            hid_pt_view_focus_row(&panel->view, panel->selected_index);
            panel_update_hints(panel, panel->view.row_buttons[panel->selected_index]);
        }
    }
}

/* Store @p lb for the selected controller, and say so if the store refused. */
static void panel_set_lightbar(hid_pt_panel_t *panel, const lightbar_pref_t *lb)
{
    if (!hid_pt_model_set_lightbar(&panel->model, lb)) {
        panel_update_status(panel);
    }
    update_lightbar_row(panel);
}

/* ---- the colour picker -------------------------------------------------- */

/* Send the colour the sliders make to the controller, if it changed since the
 * last tick: the throttle that keeps the preview at PICKER_PREVIEW_MS. */
static void picker_timer_cb(lv_timer_t *timer)
{
    hid_pt_panel_t *panel = timer->user_data;
    if (panel && panel->picker.pending) {
        panel->picker.pending = false;
        hid_pt_model_preview_lightbar(&panel->model, panel->picker.rgb);
    }
}

/**
 * OK on Custom: the picker, over the sheet. It starts on the colour the
 * controller has in the lit mode -- hue and intensity as they are, brightness
 * through the inverse of its curve -- or, from Automatic or Off, on the
 * palette's red: hue 0, intensity 100, brightness 25.
 */
static void picker_open(hid_pt_panel_t *panel)
{
    lightbar_pref_t lb;
    gamepad_mode_t mode = GAMEPAD_MODE_NONE;
    if (hid_pt_view_picker_is_open(&panel->view) || !hid_pt_model_selected_lightbar(&panel->model, &lb, &mode)) {
        return;
    }
    unsigned hue = 0;
    unsigned intensity = LIGHTBAR_INTENSITY_MAX;
    unsigned brightness = LIGHTBAR_BRIGHTNESS_PALETTE;
    const bool from_colour = !lb.automatic && lb.rgb != 0;
    if (from_colour) {
        unsigned value = 0;
        lightbar_colour_to_hsv(lb.rgb, &hue, &intensity, &value);
        brightness = lightbar_colour_brightness(value);
    }
    char heading[48];
    lightbar_heading(mode, heading, sizeof(heading));
    panel->picker.from = lb;
    panel->picker.pending = false;
    /* Not moved yet, the picker shows -- and OK keeps -- the colour exactly as
     * stored: the sliders' grid cannot always make it again. */
    panel->picker.rgb = from_colour ? lb.rgb
                                    : lightbar_colour_from_hsv(hue, intensity, lightbar_colour_value(brightness));
    hid_pt_view_open_picker(&panel->view, heading, (int) hue, (int) brightness, (int) intensity);
    hid_pt_view_picker_show(&panel->view, panel->picker.rgb);
    panel->picker.timer = lv_timer_create(picker_timer_cb, PICKER_PREVIEW_MS, panel);
}

/**
 * Close the picker. @p keep: its colour was just stored, and the preview's
 * end must not put the old one back on a mounted pad's record; without, a
 * cancel -- the stored colour goes back on the bar at once.
 */
static void picker_close(hid_pt_panel_t *panel, bool keep)
{
    if (!hid_pt_view_picker_is_open(&panel->view)) {
        return;
    }
    if (panel->picker.timer) {
        lv_timer_del(panel->picker.timer);
        panel->picker.timer = NULL;
    }
    panel->picker.pending = false;
    hid_pt_model_end_lightbar_preview(&panel->model, keep);
    hid_pt_view_close_picker(&panel->view);
    update_lightbar_row(panel);
    panel_update_hints(panel, lv_group_get_focused(panel->view.group));
}

/* A slider moved: the colour it makes, shown at once and previewed on the
 * controller on the next tick. */
static void picker_changed(hid_pt_panel_t *panel)
{
    int hue = 0, brightness = LIGHTBAR_BRIGHTNESS_PALETTE, intensity = LIGHTBAR_INTENSITY_MAX;
    hid_pt_view_picker_values(&panel->view, &hue, &brightness, &intensity);
    panel->picker.rgb = lightbar_colour_from_hsv((unsigned) hue, (unsigned) intensity,
                                                 lightbar_colour_value((unsigned) brightness));
    panel->picker.pending = true;
    hid_pt_view_picker_show(&panel->view, panel->picker.rgb);
}

/* OK: the colour is stored for the lit mode, as a swatch's would be -- the
 * controller's one game switch kept as it is, from Automatic too. */
static void picker_ok(hid_pt_panel_t *panel)
{
    const lightbar_pref_t from = panel->picker.from;
    lightbar_pref_t want = {false, panel->picker.rgb, from.game};
    const bool change = !lightbar_pref_equal(&want, &from);
    if (change && !hid_pt_model_set_lightbar(&panel->model, &want)) {
        panel_update_status(panel);
    }
    /* Unchanged, whatever the preview left on a mounted pad's record goes. */
    picker_close(panel, change);
}

/* The picker's keys; it owns them all while it is up. */
static void picker_key(hid_pt_panel_t *panel, lv_event_t *event, lv_obj_t *target, uint32_t key,
                       hid_pt_widget_kind_t kind)
{
    const int dir = (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) ? 1 : -1;
    switch (key) {
        case LV_KEY_ESC:
            picker_close(panel, false);
            break;
        case LV_KEY_ENTER:
            if (kind == HID_PT_WK_PICKER_BTN) {
                /* LVGL turns it into the button's click. */
                return;
            }
            break;
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            if (!hid_pt_view_picker_nudge(&panel->view, target, dir)) {
                panel_focus(panel, hid_pt_view_picker_step_button(&panel->view, target, dir));
            }
            break;
        case LV_KEY_UP:
        case LV_KEY_DOWN:
            panel_focus(panel, hid_pt_view_picker_step(&panel->view, target, dir));
            break;
        default:
            return;
    }
    lv_event_stop_processing(event);
}

/* A swatch: that colour, or Automatic. A colour keeps the game switch where
 * it is -- ONE switch per controller: coming from Automatic, where it is
 * hidden, it is still the one the other modes have (a mode without a colour
 * reads it too, hid_pt_gamepad_lightbar()), never turned back on. Custom opens
 * the picker instead, whether it is lit or not. */
static void panel_swatch_clicked(void *userdata, int swatch)
{
    hid_pt_panel_t *panel = userdata;
    lightbar_pref_t lb;
    if (!panel || !hid_pt_model_selected_has_lightbar(&panel->model)) {
        return;
    }
    if (swatch >= 0 && swatch == panel->view.custom_swatch) {
        picker_open(panel);
        return;
    }
    if (swatch < 0 || swatch >= SWATCH_COUNT || !hid_pt_model_selected_lightbar(&panel->model, &lb, NULL)) {
        return;
    }
    lightbar_pref_t want = lightbar_pref_automatic();
    if (!SWATCHES[swatch].automatic) {
        want.automatic = false;
        want.rgb = SWATCHES[swatch].rgb;
        want.game = lb.game;
    }
    if (!lightbar_pref_equal(&want, &lb)) {
        panel_set_lightbar(panel, &want);
    }
}

static void update_device_options(hid_pt_panel_t *panel)
{
    update_mode_row(panel);
    update_composite_row(panel);
    update_lightbar_row(panel);
    update_customize_panel(panel);
}

/* HID <-> SDL for the device in @p row: the one path both the row's OK and the
 * mode buttons take, so the two cannot disagree about what a toggle does.
 * Nothing here but the existing plug/unplug. */
static void panel_toggle_plug(hid_pt_panel_t *panel, int row)
{
    if (!panel || !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }

    /* Act on the key the row is LABELLED with, not on its index: the model is
     * rebuilt in readdir order on a different cadence than this panel
     * re-renders on, so an index could bridge the pad in the next row instead.
     * The model also owns the failure cases -- the device having left since the
     * row was drawn, and the plug-in refusing -- and leaves the reason in the
     * plug error the status line shows. */
    if (hid_pt_model_toggle_plug(&panel->model, panel->row_keys[row], panel->session, NULL) !=
        HID_PT_PLUG_DONE) {
        panel_update_status(panel);
        return;
    }
    panel->selected_index = row;

    refresh_selected_row_state(panel);
    update_row_styles(panel);
    update_device_options(panel);
    panel_update_status(panel);
}

/* The MODES entry for HID, or for the SDL type @p type. -1 when there is none. */
static int mode_index(bool hid, gamepad_type_pref_t type)
{
    for (int i = 0; i < MODE_COUNT; ++i) {
        if (!mode_is_lock(i) && (mode_is_hid(i) ? hid : (!hid && gamepad_mode_sdl_type(MODES[i].mode) == type))) {
            return i;
        }
    }
    return -1;
}

/**
 * GAME: fix the streamed game's mode, or free it again.
 *
 * On, the lock takes the selected controller's mode as it runs now -- mounted
 * is HID, else the pad the host builds for it -- so nothing moves for it; any
 * other controller of the session in a different mode is brought to it at
 * once, as the lock is the mode of every controller in that game. Off, every
 * controller goes back to its own remembered mode, again at once and only
 * where that differs (hid_pt_model_set_app_mode()).
 */
static void panel_toggle_lock(hid_pt_panel_t *panel, const hid_pt_row_info_t *info)
{
    gamepad_mode_t mode = GAMEPAD_MODE_NONE;
    if (hid_pt_model_app_mode(&panel->model) == GAMEPAD_MODE_NONE) {
        mode = info->plugged ? GAMEPAD_MODE_HID : gamepad_type_pref_mode(info->effective_type);
    }
    /* The error line either way: a controller the lock could not mount says
     * why even though the lock itself took. */
    hid_pt_model_set_app_mode(&panel->model, mode);
    panel_update_status(panel);
    /* Any controller may have moved, not just this one. */
    refresh_devices(panel, false);
}

/**
 * Choose MODES[@p mode] for the selected controller: mounted as HID, or over
 * SDL as that type. The choice is REMEMBERED -- the controller comes back in it
 * after a reconnect, at the next stream and after an app restart
 * (hid_pt_model_persist_mode()) -- and acted on:
 * - HID: the plug-in path, then the flag. A refused plug-in keeps the choice,
 *   so the reconcile mounts the pad once whatever refused it has cleared; the
 *   reason is in the error line either way.
 * - An SDL type: the type is stored FIRST, re-announcing the pad if it is on
 *   SDL now; mounted, it then leaves HID through the plain unplug, whose slot
 *   restore announces it at once with the stored type. A refused write stops
 *   there and says so, leaving the controller where it is.
 * - The lit button: stored and nothing else, no plug and no re-announce -- it
 *   makes what is lit the mode the controller keeps (the host pad is compared,
 *   not the pref, so "Automatic" becoming an explicit DS4 moves nothing).
 * While the streamed game is locked, a button other than the lit one changes
 * the lock instead -- the game's mode, for every controller in it -- and no
 * controller's own mode is touched. GAME itself switches the lock (panel_toggle_lock()).
 */
static void panel_choose_mode(hid_pt_panel_t *panel, int mode)
{
    hid_pt_row_info_t info;
    if (!panel || mode < 0 || mode >= MODE_COUNT || !hid_pt_view_has_row(&panel->view, panel->selected_index) ||
        !hid_pt_model_selected_row_info(&panel->model, &info) || !info.is_gamepad) {
        return;
    }
    if (mode_is_lock(mode)) {
        panel_toggle_lock(panel, &info);
        return;
    }
    const gamepad_mode_t lock = hid_pt_model_app_mode(&panel->model);
    if (lock != GAMEPAD_MODE_NONE) {
        /* While the game is locked a button changes the GAME's mode, for every
         * controller in it; each controller's own mode stays as it was for
         * when the lock goes. The row's lit button changes nothing: under a
         * HID lock a controller that cannot be mounted runs in its own SDL
         * type and lights that, and pressing what is lit must not quietly
         * move the whole game to it. */
        if (MODES[mode].mode != lock && mode != lit_mode(&info) &&
            (!mode_is_hid(mode) || hid_pt_model_selected_is_bridgeable(&panel->model))) {
            hid_pt_model_set_app_mode(&panel->model, MODES[mode].mode);
            panel_update_status(panel);
            refresh_devices(panel, false);
        }
        return;
    }
    const gamepad_type_pref_t type = gamepad_mode_sdl_type(MODES[mode].mode);
    if (mode_is_hid(mode)) {
        if (!hid_pt_model_selected_is_bridgeable(&panel->model)) {
            return;
        }
        if (!info.plugged) {
            panel_toggle_plug(panel, panel->selected_index);
        }
        /* After the plug: a plug-in clears the error line, and a refused
         * write here has to stay on it. */
        if (!hid_pt_model_persist_mode(&panel->model, true, type)) {
            panel_update_status(panel);
        }
    } else if (!hid_pt_model_persist_mode(&panel->model, false, type)) {
        panel_update_status(panel);
    } else if (info.plugged) {
        panel_toggle_plug(panel, panel->selected_index);
    }
    refresh_selected_row_state(panel);
    update_state_line(panel);
    update_mode_row(panel);
}

/* The row is the button now: OK on a device, or a click anywhere on it, is the
 * plug. There is no second control to aim at. On a controller it is the mode
 * row's own choice -- HID when on SDL, and back to the SDL type the host builds
 * for it when mounted -- so the row remembers it exactly as a button press
 * does. */
static void panel_row_clicked(void *userdata, int row) {
    hid_pt_panel_t *panel = userdata;
    if (!panel || !hid_pt_view_has_row(&panel->view, row)) {
        return;
    }
    if (strcmp(hid_pt_model_selected_key(&panel->model), panel->row_keys[row]) != 0) {
        panel_select_device(panel, row);
    }
    hid_pt_row_info_t info;
    if (!hid_pt_model_selected_row_info(&panel->model, &info)) {
        return;
    }
    if (info.is_gamepad && !hid_pt_model_selected_is_bridgeable(&panel->model)) {
        /* No device behind the pad the bridge can mount (an SDL-only row, or
         * one CTM lists as plain HID). OK takes the cursor to what such a row
         * does have: its mode row, on the type the host builds for it now. */
        panel_focus(panel, hid_pt_view_first_option(&panel->view));
        return;
    }
    if (info.is_gamepad) {
        panel_choose_mode(panel, mode_index(!info.plugged, info.effective_type));
    } else {
        panel_toggle_plug(panel, row);
    }
    panel_update_hints(panel, panel->view.row_buttons[row]);
}

/* A mode button was pressed. */
static void panel_mode_clicked(void *userdata, int mode)
{
    panel_choose_mode(userdata, mode);
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
            /* Composite decides whether the bridge can mount a Flydigi at all,
             * so it enables or disables the HID button -- now, not on the next
             * 2 s refresh. */
            update_mode_row(panel);
            return;
        case HID_PT_CTL_LIGHTBAR_GAME: {
            lightbar_pref_t lb;
            if (panel->view.lightbar_game_cb && hid_pt_model_selected_lightbar(&panel->model, &lb, NULL) &&
                !lb.automatic) {
                lb.game = lv_obj_has_state(panel->view.lightbar_game_cb, LV_STATE_CHECKED);
                panel_set_lightbar(panel, &lb);
            }
            return;
        }
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
        case HID_PT_CTL_TRIGGER_REDUCE:
            hid_pt_view_update_trigger_label(&panel->view);
            break;
        case HID_PT_CTL_AUDIO_MODE:
            break;
        case HID_PT_CTL_PICKER_HUE:
        case HID_PT_CTL_PICKER_BRIGHTNESS:
        case HID_PT_CTL_PICKER_INTENSITY:
            picker_changed(panel);
            return;
        default:
            return;
    }
    customize_setting_changed(panel);
}

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
        case HID_PT_CTL_PICKER_OK:
            picker_ok(panel);
            return;
        case HID_PT_CTL_PICKER_CANCEL:
            picker_close(panel, false);
            return;
        default:
            return;
    }
}

static void render_device_list(hid_pt_panel_t *panel) {
    hid_pt_view_list_clear(&panel->view);
    memset(panel->row_keys, 0, sizeof(panel->row_keys));

    const int row_count = hid_pt_model_row_count(&panel->model);
    if (row_count == 0) {
        hid_pt_view_list_show_empty(&panel->view);
        return;
    }

    hid_pt_view_list_prepare(&panel->view);

    const char *sel = hid_pt_model_selected_key(&panel->model);
    for (int i = 0; i < row_count && i < HID_PT_MAX_ROWS; ++i) {
        hid_pt_row_info_t info;
        if (!hid_pt_model_row_info(&panel->model, i, &info)) {
            /* Only out-of-range fails, and the loop bound is the count the model
             * validates against; stop rather than leave a hole in the arrays. */
            break;
        }
        /* What this row stands for, for as long as it exists. */
        snprintf(panel->row_keys[i], sizeof(panel->row_keys[0]), "%s", info.key);
        bool live = false;
        const char *state = row_state_text(&info, &live);
        hid_pt_view_add_row(&panel->view, i, info.label, state, live,
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
    hid_pt_model_status_text(&panel->model, status, sizeof(status));
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

    uint64_t sig = hid_pt_model_signature(&panel->model);
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
        if (keep_focus && hid_pt_view_obj_is_focusable(&panel->view, prev_focus)) {
            /* A mode button just pressed is still here, the same object on the
             * same controller -- the press itself is what usually changed the
             * list's signature. */
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
    /* Not under the picker: a re-render rebuilds the focus group, and the
     * picker is what the user is editing -- the sheet catches up on close. */
    if (panel && panel->view.container && lv_obj_is_valid(panel->view.container) &&
        !hid_pt_view_picker_is_open(&panel->view)) {
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
    /* The page closing -- BACK out of the overlay, the stream ending -- with
     * the picker up is a cancel: the stored colour goes back on the bar. Its
     * widgets go with the page's tree. */
    if (hid_pt_view_picker_is_open(&panel->view)) {
        if (panel->picker.timer) {
            lv_timer_del(panel->picker.timer);
            panel->picker.timer = NULL;
        }
        hid_pt_model_end_lightbar_preview(&panel->model, false);
    }
    hid_pt_view_destroy(&panel->view);
    free(panel);
}

void hid_passthrough_panel_refresh(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (panel && !hid_pt_view_picker_is_open(&panel->view)) {
        refresh_devices(panel, true);
    }
}

void hid_passthrough_panel_focus_initial(lv_obj_t *panel_root) {
    hid_pt_panel_t *panel = lv_obj_get_user_data(panel_root);
    if (panel && !hid_pt_view_picker_is_open(&panel->view)) {
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
    panel->model.session = session;
    panel->on_close = on_close;
    panel->on_close_userdata = userdata;
    panel->selected_index = -1;

    const hid_pt_view_cbs_t cbs = {
            .userdata = panel,
            .value_changed = panel_value_changed,
            .clicked = panel_clicked,
            .row_clicked = panel_row_clicked,
            .mode_clicked = panel_mode_clicked,
            .swatch_clicked = panel_swatch_clicked,
            .row_focused = panel_row_focused,
            .key = panel_control_key,
            .dropdown_key = panel_dropdown_key,
            .dropdown_toggled = panel_dropdown_toggled,
            .deleted = panel_deleted,
    };
    lv_obj_t *cont = hid_pt_view_create(&panel->view, parent, &cbs);
    if (!cont) {
        free(panel);
        return NULL;
    }
    lv_obj_set_user_data(cont, panel);
    for (int i = 0; i < MODE_COUNT; ++i) {
        hid_pt_view_add_mode(&panel->view, MODES[i].glyph, locstr(MODES[i].label), mode_is_hid(i));
    }
    for (int i = 0; i < SWATCH_COUNT; ++i) {
        /* The screen's version of the colour; Automatic, no colour, the slab's
         * own dark. */
        const uint32_t shown = SWATCHES[i].automatic ? OVERLAY_SLAB : lightbar_colour_display(SWATCHES[i].rgb);
        hid_pt_view_add_swatch(&panel->view, shown, SWATCHES[i].text, SWATCHES[i].glyph);
    }
    hid_pt_view_add_custom_swatch(&panel->view);
    hid_pt_view_rebuild_focus_order(&panel->view);
    panel->refresh_timer = lv_timer_create(refresh_timer_cb, 2000, panel);

    hid_passthrough_panel_refresh(cont);
    return cont;
}

#endif
