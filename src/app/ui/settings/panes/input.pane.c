#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "hid_passthrough/ctm/controllers/ds5_acl_tx.h"   /* push the idle timeout to ds5_txd */

#include "util/i18n.h"

#include <stdlib.h>

typedef struct input_pane_t {
    lv_fragment_t base;

    lv_obj_t *absmouse_toggle;
    lv_obj_t *absmouse_hint;
    lv_obj_t *deadzone_label;
    lv_obj_t *deadzone_slider;
    lv_obj_t *idle_off_label;
    lv_obj_t *idle_off_slider;
    lv_obj_t *swap_abxy_toggle;
} input_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *view);

static void pane_ctor(lv_fragment_t *self, void *args);

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e);

static void hwmouse_state_update(input_pane_t *pane);
#endif

static void update_deadzone_label(input_pane_t *pane);

static void on_deadzone_changed(lv_event_t *e);

static void on_hid_passthrough_changed(lv_event_t *e);

static void hid_passthrough_ui_update(input_pane_t *pane);

static void update_idle_off_label(input_pane_t *pane);

static void on_controller_idle_changed(lv_event_t *e);

const lv_fragment_class_t settings_pane_input_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(input_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    (void) self;
    (void) args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    input_pane_t *pane = (input_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pref_checkbox(view, locstr("View-only mode"), &app_configuration->viewonly, false);
    pref_desc_label(view, locstr("Don't send mouse, keyboard or gamepad input to host computer."), false);

    pref_checkbox(view, locstr("Capture system keys"), &app_configuration->syskey_capture, false);
    pref_desc_label(view, locstr("Send system keys (e.g. Meta/Win) to the host computer."), false);

#if FEATURE_INPUT_EVKBD
    pref_checkbox(view, locstr("Use keyboard hardware"), &app_configuration->keyboard_capture, false);
    pref_desc_label(view, locstr("Take exclusive control of a plugged keyboard while streaming, so every key "
                                 "reaches the host (the TV otherwise keeps keys like F12 for volume)."), false);
#endif

    pref_header(view, locstr("Mouse"));

#if FEATURE_INPUT_EVMOUSE
    lv_obj_t *hwmouse_toggle = pref_checkbox(view, locstr("Use mouse hardware"),
                                             &app_configuration->hardware_mouse, false);
    lv_obj_add_event_cb(hwmouse_toggle, hwmouse_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Use a plugged mouse directly while streaming. Better performance, "
                                 "but absolute mouse mode is unavailable."),
                    false);
#endif

    pane->absmouse_toggle = pref_checkbox(view, locstr("Absolute mouse mode"),
                                          &app_configuration->absmouse, false);
    pane->absmouse_hint = pref_desc_label(view, locstr("Better for remote desktop. "
                                                       "For some games, mouse will not work properly."), false);

    pref_header(view, locstr("Gamepad"));

    lv_obj_t *hid_pt_toggle = pref_checkbox(view, locstr("Enable HID Passthrough (Experimental)"),
                                            &app_configuration->hid_passthrough, false);
    lv_obj_add_event_cb(hid_pt_toggle, on_hid_passthrough_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view,
                    locstr("Bridge selected controllers to the PC as native HID devices; others keep "
                           "standard Moonlight emulation."),
                    false);

    static const pref_dropdown_int_entry_t tpmouse_entries[] = {
            {"Desktop only", 1, true},
            {"Always", 2, false},
            {"Off", 0, false},
    };
    pref_title_label(view, locstr("DS5 touchpad as desktop mouse"));
    pref_dropdown_int(view, tpmouse_entries, 3, &app_configuration->ds5_touchpad_mouse, NULL);
    pref_desc_label(view, locstr("Move the PC mouse with the DualSense touchpad. \"Desktop only\" hands the "
                                 "touchpad back to any running game."), false);

    pane->idle_off_label = pref_title_label(view, locstr("Turn idle controllers off"));
    /* Starts at 0 = never, so the feature can be turned off from here. */
    pane->idle_off_slider = pref_slider(view, &app_configuration->controller_idle_off_min, 0, 30, 1);
    lv_obj_set_width(pane->idle_off_slider, LV_PCT(100));
    lv_obj_add_event_cb(pane->idle_off_slider, on_controller_idle_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Power a controller off after this long without input, instead of letting "
                                 "it drain on the couch. Applies whenever it is paired with the TV. "
                                 "Leftmost position is Never."),
                    false);

    pane->deadzone_label = pref_title_label(view, locstr("Analog stick deadzone"));
    pane->deadzone_slider = pref_slider(view, &app_configuration->stick_deadzone, 0, 20, 1);
    lv_obj_set_width(pane->deadzone_slider, LV_PCT(100));
    lv_obj_add_event_cb(pane->deadzone_slider, on_deadzone_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Note: Some games can enforce a larger deadzone "
                                 "than what Aurora is configured to use."),
                    false);

    pref_checkbox(view, locstr("Virtual mouse"), &app_configuration->virtual_mouse, false);
    pref_desc_label(view, locstr("Start streams with the virtual mouse active: right stick moves, left stick "
                                 "scrolls, LT/RT click, Y opens the on-screen keyboard. Toggle anytime from "
                                 "the stream overlay."),
                    false);

    pane->swap_abxy_toggle = pref_checkbox(view, locstr("Swap ABXY buttons"), &app_configuration->swap_abxy, false);
    pref_desc_label(view, locstr("Swap A/B and X/Y gamepad buttons, for Nintendo-style layouts."),
                    false);

#if FEATURE_INPUT_EVMOUSE
    hwmouse_state_update(pane);
#endif
    hid_passthrough_ui_update(pane);
    update_deadzone_label(pane);
    update_idle_off_label(pane);
    return view;
}

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e) {
    hwmouse_state_update((input_pane_t *) lv_event_get_user_data(e));
}

static void hwmouse_state_update(input_pane_t *pane) {
    if (app_configuration->hardware_mouse) {
        lv_obj_add_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Absolute mouse mode can't be used when "
                                                      "\"Use mouse hardware\" enabled."));
    } else {
        lv_obj_clear_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Better for remote desktop. "
                                                      "For some games, mouse will not work properly."));
    }
}
#endif

/* Push the new value straight to the daemon so it takes effect now rather than
 * at the next app start. The daemon persists what it receives, so nothing has
 * to re-send this later; if it is not running there is nothing to configure and
 * it will read its own stored value when it next comes up. */
static void update_idle_off_label(input_pane_t *pane) {
    if (app_configuration->controller_idle_off_min <= 0) {
        lv_label_set_text_fmt(pane->idle_off_label, "%s - %s", locstr("Turn idle controllers off"),
                              locstr("Never"));
    } else {
        lv_label_set_text_fmt(pane->idle_off_label, "%s - %d min", locstr("Turn idle controllers off"),
                              app_configuration->controller_idle_off_min);
    }
}

static void on_controller_idle_changed(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    update_idle_off_label(pane);
    /* Push straight to the daemon so it takes effect now rather than at the next
     * app start. The daemon persists what it receives; if it is not running
     * there is nothing to configure and it reads its stored value when it next
     * comes up. Wire unit is seconds -- the slider is the only thing in minutes. */
    ds5_acl_send_idle_timeout(app_configuration->controller_idle_off_min * 60);
}

static void update_deadzone_label(input_pane_t *pane) {
    lv_label_set_text_fmt(pane->deadzone_label, "%s - %d", locstr("Analog stick deadzone"),
                          app_configuration->stick_deadzone);
}

static void on_deadzone_changed(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    update_deadzone_label(pane);
}

static void hid_passthrough_ui_update(input_pane_t *pane) {
    (void) pane;
}

static void on_hid_passthrough_changed(lv_event_t *e) {
    hid_passthrough_ui_update((input_pane_t *) lv_event_get_user_data(e));
}
