#include "app.h"
#include "config.h"

#include "pref_obj.h"
#include "app_settings.h"

#include "ui/settings/settings.controller.h"

#include "util/i18n.h"

typedef struct experimental_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;

    lv_obj_t *idr_checkbox;
    lv_obj_t *idr_slider;
    lv_obj_t *idr_label;
    int idr_refresh_slider_value;

    lv_obj_t *abr_mode_dropdown;
} experimental_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void reconnect_cb(lv_event_t *e);

static void on_ntsc_refresh_changed(lv_event_t *e);

static void abr_state_update(experimental_pane_t *pane);

static void on_abr_changed(lv_event_t *e);

static void idr_refresh_state_update(experimental_pane_t *pane);

static void idr_checkbox_activate(lv_event_t *e);

static void idr_refresh_slider_cb(lv_event_t *e);

const lv_fragment_class_t settings_pane_experimental_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(experimental_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    experimental_pane_t *pane = (experimental_pane_t *) self;
    pane->parent = args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    experimental_pane_t *pane = (experimental_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *ntsc_checkbox = pref_checkbox(view, locstr("Use NTSC refresh (59.94 / 119.88 Hz)"),
                                            &app_configuration->use_ntsc_refresh, false);
    lv_obj_add_event_cb(ntsc_checkbox, on_ntsc_refresh_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("60/120 FPS presets pace at fractional NTSC rates instead of integer 60/120. "
                                 "Choose Custom FPS in Streaming to enter any fractional rate directly."),
                    false);

#if TARGET_WEBOS
    lv_obj_t *pacing_checkbox = pref_checkbox(view, locstr("Smooth frame pacing (host PTS)"),
                                              &app_configuration->smooth_frame_pacing, false);
    lv_obj_add_event_cb(pacing_checkbox, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Pace video presentation from the host capture clock instead of packet "
                                 "arrival time. Takes effect on the next stream."),
                    false);

    lv_obj_t *recovery_checkbox = pref_checkbox(view, locstr("Soft recovery (4K)"),
                                                &app_configuration->soft_recovery, false);
    lv_obj_add_event_cb(recovery_checkbox, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("When decode backlog builds on 4K streams, temporarily lower the bitrate "
                                 "so video and input catch up, instead of a hard refresh."),
                    false);
#endif

    lv_obj_t *idr_checkbox = lv_checkbox_create(view);
    lv_checkbox_set_text(idr_checkbox, locstr("Periodic decoder refresh (HEVC)"));
    pane->idr_checkbox = idr_checkbox;
    pane->idr_refresh_slider_value = app_configuration->idr_refresh_interval_ms >= 500
            ? app_configuration->idr_refresh_interval_ms
            : 10000;
    pref_checkbox_prepare_for_dpad(idr_checkbox);
    pref_desc_label(view, locstr("Request a keyframe every N seconds during HEVC streams, against artifact "
                                 "drift in long sessions. Use 10-30 s if you see blockiness or smearing."),
                    false);
    pane->idr_label = pref_title_label(view, "");
    lv_obj_t *idr_slider = pref_slider(view, &pane->idr_refresh_slider_value, 500, 60000, 500);
    lv_obj_set_width(idr_slider, LV_PCT(100));
    pane->idr_slider = idr_slider;
    lv_obj_add_event_cb(idr_checkbox, idr_checkbox_activate, LV_EVENT_CLICKED, pane);
    lv_obj_add_event_cb(idr_slider, idr_refresh_slider_cb, LV_EVENT_VALUE_CHANGED, pane);
    idr_refresh_state_update(pane);

    lv_obj_t *full_range = pref_checkbox(view, locstr("Full range YUV (SDR only)"),
                                         &app_configuration->force_full_color_range, false);
    lv_obj_add_event_cb(full_range, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Request full-range (0-255) SDR color from the host. No effect on HDR, "
                                 "which always uses limited range. Try it if SDR looks washed out."),
                    false);

    lv_obj_t *abr_checkbox = pref_checkbox(view, locstr("Adaptive bitrate"),
                                           &app_configuration->auto_adjust_bitrate, false);
    lv_obj_add_event_cb(abr_checkbox, on_abr_changed, LV_EVENT_VALUE_CHANGED, pane);

    static const pref_dropdown_int_entry_t abr_entries[] = {
            {translatable("Balanced"), 0, true},
            {translatable("Quality"), 1, false},
            {translatable("Low latency"), 2, false},
    };
    pref_title_label(view, locstr("ABR mode"));
    pane->abr_mode_dropdown = pref_dropdown_int(view, abr_entries, 3, &app_configuration->abr_mode, NULL);
    lv_obj_set_width(pane->abr_mode_dropdown, LV_PCT(100));
    lv_obj_add_event_cb(pane->abr_mode_dropdown, reconnect_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Let the host lower the bitrate when the link drops packets, then ramp "
                                 "back up. Takes effect on the next stream."),
                    false);
    abr_state_update(pane);

    return view;
}

static void reconnect_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
}

static void on_ntsc_refresh_changed(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    settings_apply_ntsc_preset_refresh(app_configuration, app_configuration->stream.fps);
}

static void abr_state_update(experimental_pane_t *pane) {
    if (app_configuration->auto_adjust_bitrate) {
        lv_obj_clear_state(pane->abr_mode_dropdown, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(pane->abr_mode_dropdown, LV_STATE_DISABLED);
    }
}

static void on_abr_changed(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    pane->parent->needs_stream_reconnect = true;
    abr_state_update(pane);
}

static void idr_refresh_state_update(experimental_pane_t *pane) {
    const bool refresh_on = app_configuration->idr_refresh_interval_ms >= 500;
    if (refresh_on) {
        lv_obj_add_state(pane->idr_checkbox, LV_STATE_CHECKED);
        lv_obj_clear_state(pane->idr_slider, LV_STATE_DISABLED);
        lv_label_set_text_fmt(pane->idr_label, locstr("Refresh interval - %.1f s"),
                              app_configuration->idr_refresh_interval_ms / 1000.0);
    } else {
        lv_obj_clear_state(pane->idr_checkbox, LV_STATE_CHECKED);
        lv_obj_add_state(pane->idr_slider, LV_STATE_DISABLED);
        lv_label_set_text_fmt(pane->idr_label, locstr("Refresh interval - %s"), locstr("Off"));
    }
}

static void idr_checkbox_activate(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    experimental_pane_t *pane = lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_current_target(e);
    if (lv_obj_has_state(cb, LV_STATE_DISABLED)) {
        return;
    }
    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        app_configuration->idr_refresh_interval_ms = 0;
    } else {
        if (pane->idr_refresh_slider_value < 500) {
            pane->idr_refresh_slider_value = 10000;
        }
        app_configuration->idr_refresh_interval_ms = pane->idr_refresh_slider_value;
    }
    idr_refresh_state_update(pane);
    /* Announce the flip (CHECKABLE is off, so LVGL won't): the sheet re-folds
     * descriptions and re-renders its footer on this. */
    lv_event_send(cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void idr_refresh_slider_cb(lv_event_t *e) {
    experimental_pane_t *pane = lv_event_get_user_data(e);
    if (app_configuration->idr_refresh_interval_ms >= 500) {
        app_configuration->idr_refresh_interval_ms = pane->idr_refresh_slider_value;
    }
    idr_refresh_state_update(pane);
}
