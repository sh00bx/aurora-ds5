#include "pref_fps.h"

#include "app.h"
#include "app_settings.h"
#include "pref_obj.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

#include "util/i18n.h"

#include <stdlib.h>
#include <string.h>

/*
 * FPS preference: a preset dropdown plus a "Custom FPS" editor.
 *
 * The editor is built for a d-pad first. A refresh rate is a number, and the
 * obvious way to enter a number is to type it -- but on this platform typing is
 * the one thing a controller cannot do: the text field summons webOS's own
 * on-screen keyboard, which is a system surface our gamepad never reaches (that
 * is why the theme no longer raises it for a pad at all, see lv_start_text_input
 * in lvgl/theme/lv_theme_moonlight.c). So the rate is edited with LEFT/RIGHT on
 * a stepper, the fractional part is a checkbox rather than three decimals to
 * type, and the text field is there for whoever has a keyboard or a pointer.
 *
 * The second thing the editor owns is the NTSC rate. It used to live two panes
 * away as an "experimental" checkbox, and typing 119.88 here did nothing at all:
 * settings_reconcile_refresh_rate() reads a rate that equals the NTSC value of a
 * preset as "the preset, at NTSC", and with that checkbox off it cleared the
 * value again. Both ends of that now meet here -- the checkbox writes
 * use_ntsc_refresh when the rate it produces is a preset's NTSC rate -- and the
 * dropdown says what was actually stored instead of rounding it away.
 */

#define FPS_MIN 30
/** Repeats of a held key before the stepper starts moving in fives. */
#define FPS_STEP_ACCEL_AFTER 8
#define FPS_STEP_ACCEL 5
/** A gap longer than this between two arrow keys is a new press, not a hold. */
#define FPS_STEP_HOLD_GAP_MS 300

typedef struct pref_dropdown_fps_ctx {
    lv_obj_t *dropdown;
    pref_dropdown_int_entry_t *entries;
    int num_entries;
    int *value_ref;
    int *refresh_rate_x100_ref;
    char rate_text[48];
    uint16_t selected_index;
    /* Set while we send LV_EVENT_VALUE_CHANGED at the dropdown ourselves, to
     * tell that echo apart from the user picking an entry. */
    bool suppress_open;
    /* Set by the VALUE_CHANGED that opened the editor, so the RELEASED of the
     * same press does not open a second one. */
    bool change_ev_received;

    /* Editor dialog. Only valid while `dialog` is non-NULL. */
    lv_obj_t *dialog;
    lv_obj_t *readout;
    lv_obj_t *detail;
    lv_obj_t *rate_value;
    lv_obj_t *ntsc_check;
    lv_obj_t *input;
    int min_fps, max_fps;
    /** The rate being edited, in centi-FPS -- the unit the host is told. */
    int edit_x100;
    bool saved;
    /** Set while the editor writes its own widgets, to break feedback loops. */
    bool syncing;
    uint16_t hold_repeats;
    uint32_t hold_last_tick;
} pref_dropdown_fps_ctx_t;

static bool is_valid_fps(int fps) {
    return fps > 0;
}

static void dropdown_fps_select_cb(lv_event_t *e);

static void dropdown_fps_released_cb(lv_event_t *e);

static void dropdown_delete_cb(lv_event_t *e);

static void fps_editor_open(pref_dropdown_fps_ctx_t *ctx);

static void fps_editor_sync(pref_dropdown_fps_ctx_t *ctx);

static void fps_editor_sync_ex(pref_dropdown_fps_ctx_t *ctx, bool rewrite_input);

static void fps_dropdown_refresh_label(pref_dropdown_fps_ctx_t *ctx);

/* --- The rate itself -------------------------------------------------------
 *
 * One number carries the whole state: the rate in centi-FPS, which is exactly
 * what Limelight's clientRefreshRateX100 wants. Everything else -- the whole
 * rate on the stepper, the NTSC tick, the text in the field -- is derived from
 * it, so the three can never disagree.
 */

static int fps_from_x100(int x100) {
    return (x100 + 50) / 100;
}

static int fps_x100_exact(int fps) {
    return fps * 100;
}

/** 1000/1001 of a whole rate, rounded to centi-FPS: 120 -> 11988, 144 -> 14386. */
static int fps_x100_ntsc(int fps) {
    return (int) (((long long) fps * 100000LL + 500LL) / 1001LL);
}

static bool fps_x100_is_ntsc(int x100) {
    int fps = fps_from_x100(x100);
    return fps > 0 && x100 == fps_x100_ntsc(fps);
}

/**
 * @return true if this rate is something other than the plain integer the
 *         dropdown entry claims, and therefore needs to be spelled out.
 */
static bool fps_rate_needs_label(int fps, int x100) {
    return fps > 0 && x100 > 0 && x100 != fps_x100_exact(fps);
}

static void fps_format_rate(char *buf, size_t len, int x100, const char *suffix) {
    if (x100 % 100 == 0) {
        snprintf(buf, len, "%d%s", x100 / 100, suffix);
    } else {
        snprintf(buf, len, "%.2f%s", x100 / 100.0, suffix);
    }
}

lv_obj_t *pref_dropdown_fps(lv_obj_t *parent, const int *options, int max, int *value, int *refresh_rate_x100) {
    if (max <= 0) {
        max = 60; // Default max FPS if not specified
    }
    bool has_max_fps = true;
    int num_options, num_entries;
    for (num_options = 0; options[num_options]; num_options++) {
        if (options[num_options] == max) {
            has_max_fps = false;
        }
        if (options[num_options] > max) {
            break;
        }
    }
    num_entries = num_options;
    if (has_max_fps) {
        // The max option is included if it's not already in the list
        num_entries++;
    }
    // Include an extra entry for the custom FPS option
    num_entries++;

    pref_dropdown_int_entry_t *entries = lv_mem_alloc(sizeof(pref_dropdown_int_entry_t) * num_entries);
    char buf[32];
    for (int i = 0; i < num_options; i++) {
        snprintf(buf, sizeof(buf), locstr("%d FPS"), options[i]);
        entries[i].name = strndup(buf, sizeof(buf));
        entries[i].value = options[i];
        entries[i].fallback = false;
    }
    if (has_max_fps) {
        snprintf(buf, sizeof(buf), locstr("%d FPS"), max);
        entries[num_options].name = strndup(buf, sizeof(buf));
        entries[num_options].value = max;
        entries[num_options].fallback = false;
    }
    // Custom FPS option
    entries[num_entries - 1].name = strdup(locstr("Custom FPS..."));
    entries[num_entries - 1].value = 0;
    entries[num_entries - 1].fallback = true;

    lv_obj_t *fps_dropdown = pref_dropdown_int(parent, entries, num_entries, value, is_valid_fps);

    pref_dropdown_fps_ctx_t *ctx = lv_mem_alloc(sizeof(pref_dropdown_fps_ctx_t));
    lv_memset_00(ctx, sizeof(pref_dropdown_fps_ctx_t));
    ctx->entries = entries;
    ctx->num_entries = num_entries;
    ctx->value_ref = value;
    ctx->refresh_rate_x100_ref = refresh_rate_x100;
    ctx->dropdown = fps_dropdown;
    ctx->selected_index = lv_dropdown_get_selected(fps_dropdown);
    ctx->min_fps = FPS_MIN;
    ctx->max_fps = max > FPS_MIN ? max : FPS_MIN;

    lv_obj_add_event_cb(fps_dropdown, dropdown_fps_select_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(fps_dropdown, dropdown_fps_released_cb, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(fps_dropdown, dropdown_delete_cb, LV_EVENT_DELETE, ctx);

    fps_dropdown_refresh_label(ctx);
    return fps_dropdown;
}

/**
 * Say on the collapsed dropdown what is actually stored.
 *
 * A rate the entries cannot express -- 119.88 rather than 120 -- gets spelled
 * out, tagged with where it came from. Anything the selected entry already says
 * correctly clears the override, so the list keeps speaking for itself.
 */
static void fps_dropdown_refresh_label(pref_dropdown_fps_ctx_t *ctx) {
    int fps = *ctx->value_ref;
    int x100 = ctx->refresh_rate_x100_ref != NULL ? *ctx->refresh_rate_x100_ref : 0;
    /* The custom entry is a command ("Custom FPS..."), not a value, so whenever
     * it is the selected one the rate has to be spelled out even if it happens
     * to be a whole number the entries simply do not carry (100, say). */
    bool custom_entry = lv_dropdown_get_selected(ctx->dropdown) == ctx->num_entries - 1;
    if (!custom_entry && !fps_rate_needs_label(fps, x100)) {
        lv_dropdown_set_text(ctx->dropdown, NULL);
        return;
    }
    int shown = x100 > 0 ? x100 : fps_x100_exact(fps);
    fps_format_rate(ctx->rate_text, sizeof(ctx->rate_text), shown,
                    fps_x100_is_ntsc(shown) ? locstr(" FPS (NTSC)") : locstr(" FPS (Custom)"));
    lv_dropdown_set_text(ctx->dropdown, ctx->rate_text);
}

/** The dropdown entry that stands for `fps`, or the custom one if none does. */
static uint16_t fps_entry_index_for(const pref_dropdown_fps_ctx_t *ctx, int fps) {
    for (int i = 0; i < ctx->num_entries - 1; i++) {
        if (ctx->entries[i].value == fps) {
            return (uint16_t) i;
        }
    }
    return (uint16_t) (ctx->num_entries - 1);
}

static void dropdown_fps_select_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = (pref_dropdown_fps_ctx_t *) lv_event_get_user_data(e);
    if (ctx->suppress_open) {
        /* Our own echo, sent so the pane can react to the new rate. */
        return;
    }
    uint16_t index = lv_dropdown_get_selected(lv_event_get_current_target(e));
    if (index != ctx->num_entries - 1) {
        ctx->selected_index = index;
#if defined(TARGET_WEBOS)
        settings_apply_ntsc_preset_refresh(app_configuration, *ctx->value_ref);
#else
        if (ctx->refresh_rate_x100_ref != NULL) {
            *ctx->refresh_rate_x100_ref = 0;
        }
#endif
        fps_dropdown_refresh_label(ctx);
        return;
    }
    // Prevent the event from propagating further
    lv_event_stop_processing(e);
    ctx->change_ev_received = true;
    fps_editor_open(ctx);
}

/**
 * Re-open the editor when "Custom FPS…" is picked while it is already selected.
 *
 * LVGL only fires VALUE_CHANGED when the selection actually moves (see
 * btn_release_handler in lv_dropdown.c), so without this the one entry that is a
 * command rather than a value becomes a dead end: set a custom rate once and
 * there is no way back into the editor to change it.
 *
 * By the time this runs the widget's own RELEASED handler has already opened or
 * closed the list, so `is_open` distinguishes the press that opened it from the
 * press that chose from it.
 */
static void dropdown_fps_released_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *dropdown = lv_event_get_current_target(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (lv_indev_get_scroll_obj(indev) != NULL) {
        return;
    }
    if (lv_dropdown_is_open(dropdown)) {
        return; /* This press opened the list. */
    }
    if (ctx->change_ev_received) {
        /* The selection moved, so VALUE_CHANGED already opened the editor. */
        ctx->change_ev_received = false;
        return;
    }
    if (ctx->dialog != NULL) {
        return;
    }
    if (lv_dropdown_get_selected(dropdown) != ctx->num_entries - 1) {
        return;
    }
    fps_editor_open(ctx);
}

static void dropdown_delete_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = (pref_dropdown_fps_ctx_t *) lv_event_get_user_data(e);
    for (int i = 0; i < ctx->num_entries; i++) {
        free((void *) ctx->entries[i].name);
    }
    lv_mem_free(ctx->entries);
    lv_mem_free(ctx);
}

/* --- Editor ---------------------------------------------------------------- */

static void fps_editor_set_x100(pref_dropdown_fps_ctx_t *ctx, int x100) {
    int lo = fps_x100_ntsc(ctx->min_fps), hi = fps_x100_exact(ctx->max_fps);
    if (x100 < lo) {
        x100 = lo;
    } else if (x100 > hi) {
        x100 = hi;
    }
    ctx->edit_x100 = x100;
}

/**
 * Rebuild every widget from `edit_x100`. The one place the editor draws itself.
 *
 * `rewrite_input` is false while the user is typing into the field: rewriting it
 * under the cursor would fight them ("11" would become "11.00" mid-word), and
 * the field already shows what it means.
 */
static void fps_editor_sync_ex(pref_dropdown_fps_ctx_t *ctx, bool rewrite_input) {
    if (ctx->dialog == NULL) {
        return;
    }
    ctx->syncing = true;

    int x100 = ctx->edit_x100;
    int whole = fps_from_x100(x100);
    bool ntsc = fps_x100_is_ntsc(x100);

    char buf[48];
    fps_format_rate(buf, sizeof(buf), x100, locstr(" FPS"));
    lv_label_set_text(ctx->readout, buf);

    /* Out of range is shown, not silently corrected: the value is only clamped
     * when it is saved, so a half-typed "1" on the way to "120" does not get
     * rewritten under the cursor. */
    int lo = fps_x100_ntsc(ctx->min_fps), hi = fps_x100_exact(ctx->max_fps);
    bool out_of_range = x100 < lo || x100 > hi;
    lv_obj_set_style_text_color(ctx->readout,
                                out_of_range ? lv_palette_main(LV_PALETTE_AMBER)
                                             : ml_color_hex(ML_COLOR_PRIMARY), 0);
    if (out_of_range) {
        lv_label_set_text_fmt(ctx->detail, locstr("Outside %d-%d FPS; will be saved as %.2f"),
                              ctx->min_fps, ctx->max_fps, (x100 < lo ? lo : hi) / 100.0);
    } else if (ntsc) {
        /* Say what the host is actually told: that number ends up in the stream
         * configuration and in every log line about pacing. */
        lv_label_set_text_fmt(ctx->detail, locstr("Sent to the host as %d (rate x100) · NTSC 1000/1001"), x100);
    } else {
        lv_label_set_text_fmt(ctx->detail, locstr("Sent to the host as %d (rate x100)"), x100);
    }

    lv_label_set_text_fmt(ctx->rate_value, locstr("%d FPS"), whole);

    if (ntsc) {
        lv_obj_add_state(ctx->ntsc_check, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ctx->ntsc_check, LV_STATE_CHECKED);
    }

    if (rewrite_input) {
        fps_format_rate(buf, sizeof(buf), x100, "");
        if (strcmp(lv_textarea_get_text(ctx->input), buf) != 0) {
            lv_textarea_set_text(ctx->input, buf);
        }
    }

    ctx->syncing = false;
}

static void fps_editor_sync(pref_dropdown_fps_ctx_t *ctx) {
    fps_editor_sync_ex(ctx, true);
}

static void fps_stepper_key_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) {
        ctx->hold_repeats = 0;
        return;
    }
    /* Holding the key walks the whole range in a couple of seconds instead of a
     * hundred presses, while a single tap still moves exactly one FPS.
     *
     * "Still held" is read off the clock rather than counted, because LVGL sends
     * no event when an arrow key comes back up: repeats arrive every
     * LV_INDEV_DEF_LONG_PRESS_REP_TIME (100 ms), so a gap well past that is a
     * new press, and without this a session of taps would silently graduate to
     * jumping five at a time. */
    if (ctx->hold_last_tick != 0 && lv_tick_elaps(ctx->hold_last_tick) > FPS_STEP_HOLD_GAP_MS) {
        ctx->hold_repeats = 0;
    }
    ctx->hold_last_tick = lv_tick_get();
    int step = ctx->hold_repeats >= FPS_STEP_ACCEL_AFTER ? FPS_STEP_ACCEL : 1;
    if (ctx->hold_repeats < 0xFFFF) {
        ctx->hold_repeats++;
    }
    int whole = fps_from_x100(ctx->edit_x100);
    bool ntsc = fps_x100_is_ntsc(ctx->edit_x100);
    whole = key == LV_KEY_RIGHT ? whole + step : whole - step;
    if (whole < ctx->min_fps) {
        whole = ctx->min_fps;
    } else if (whole > ctx->max_fps) {
        whole = ctx->max_fps;
    }
    fps_editor_set_x100(ctx, ntsc ? fps_x100_ntsc(whole) : fps_x100_exact(whole));
    fps_editor_sync(ctx);
}

/**
 * UP/DOWN between the editor's rows, attached to each focus stop directly.
 *
 * The message box's own handler (msgbox_key in lvgl/theme/lv_theme_moonlight.c)
 * would do this, but only for rows whose LV_EVENT_KEY can bubble all the way up
 * to the box -- and LVGL only bubbles an event one level at a time, so EVERY
 * container in between has to carry LV_OBJ_FLAG_EVENT_BUBBLE. That chain broke
 * here twice over (the message box content, and the wrapper around the text
 * field), and it breaks again the moment a row gains a wrapper, silently and
 * only for the row that gained it. So the box opts out of the generic handling
 * with LV_OBJ_FLAG_USER_4 and the rows answer for themselves.
 */
static void fps_editor_nav_key_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_group_t *group = lv_obj_get_group(obj);
    if (group == NULL) {
        return;
    }
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP: {
            lv_group_focus_prev(group);
            break;
        }
        case LV_KEY_DOWN: {
            lv_group_focus_next(group);
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * The text field raises webOS's on-screen keyboard on ENTER, not on focus.
 *
 * The theme starts text input the moment a field is focused, which is right for
 * a form you tab into and wrong for a row you merely walk past: with UP/DOWN
 * stepping through the editor, every pass over this row would throw a
 * full-screen system keyboard over the dialog. Since the rate can be set
 * without ever typing, the field asks first.
 */
static void fps_input_focused_cb(lv_event_t *e) {
    lv_theme_t *theme = lv_theme_get_from_obj(lv_event_get_current_target(e));
    app_t *app = theme != NULL ? theme->user_data : NULL;
    if (app == NULL) {
        return;
    }
    /* Cancels the start the theme just scheduled -- both go through the same
     * async slot, so the pair nets out to "never started". */
    app_stop_text_input(&app->ui.input);
}

static void fps_input_activate_cb(lv_event_t *e) {
    lv_obj_t *input = lv_event_get_current_target(e);
    lv_theme_t *theme = lv_theme_get_from_obj(input);
    app_t *app = theme != NULL ? theme->user_data : NULL;
    if (app == NULL) {
        return;
    }
    const lv_area_t *coords = &input->coords;
    lv_coord_t w = lv_area_get_width(coords), h = lv_area_get_height(coords);
    if (w <= 0 || h <= 0) {
        return;
    }
    app_start_text_input(&app->ui.input, coords->x1, coords->y1, w, h);
}

/** Back, from any row: leave without saving. */
static void fps_editor_nav_cancel_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->dialog != NULL) {
        lv_msgbox_close_async(ctx->dialog);
    }
}

/** Make `obj` a row of the editor: focusable, and answering UP/DOWN and Back. */
static void fps_editor_add_nav(lv_obj_t *obj, pref_dropdown_fps_ctx_t *ctx) {
    lv_obj_add_event_cb(obj, fps_editor_nav_key_cb, LV_EVENT_KEY, ctx);
    lv_obj_add_event_cb(obj, fps_editor_nav_cancel_cb, LV_EVENT_CANCEL, ctx);
}

static void fps_stepper_defocus_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    ctx->hold_repeats = 0;
    ctx->hold_last_tick = 0;
}

static void fps_ntsc_click_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *check = lv_event_get_current_target(e);
    bool ntsc = !lv_obj_has_state(check, LV_STATE_CHECKED);
    int whole = fps_from_x100(ctx->edit_x100);
    fps_editor_set_x100(ctx, ntsc ? fps_x100_ntsc(whole) : fps_x100_exact(whole));
    fps_editor_sync(ctx);
}

static void fps_input_changed_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->syncing) {
        return;
    }
    const char *text = lv_textarea_get_text(ctx->input);
    if (text[0] == '\0') {
        return; /* Mid-edit; nothing to interpret yet. */
    }
    double value = strtod(text, NULL);
    if (value <= 0) {
        return;
    }
    /* Clamp on the way out (Save), not here: clamping mid-typing fights the
     * user, because "1" is on the way to "120" and is out of range by itself. */
    ctx->edit_x100 = (int) (value * 100.0 + 0.5);
    fps_editor_sync_ex(ctx, false);
}

static void fps_editor_save(pref_dropdown_fps_ctx_t *ctx) {
    /* What the pane has to be told about is a CHANGE; Save on an unchanged value
     * must stay silent, or the pane's VALUE_CHANGED handler arms
     * needs_stream_reconnect and the user is offered a reconnect for nothing. */
    const int prev_fps = *ctx->value_ref;
    const int prev_rate_x100 = ctx->refresh_rate_x100_ref != NULL ? *ctx->refresh_rate_x100_ref : 0;
    fps_editor_set_x100(ctx, ctx->edit_x100);
    int x100 = ctx->edit_x100;
    int whole = fps_from_x100(x100);

    *ctx->value_ref = whole;
    if (ctx->refresh_rate_x100_ref != NULL) {
        /* An exact integer rate has nothing fractional to send; storing 0 keeps
         * the dropdown showing the plain preset instead of "120.00 (Custom)". */
        *ctx->refresh_rate_x100_ref = x100 == fps_x100_exact(whole) ? 0 : x100;
    }
    /* settings_reconcile_refresh_rate() treats a rate that equals a preset's
     * NTSC value as "that preset, at NTSC", and clears it unless
     * use_ntsc_refresh says so. Follow the rate the user just chose rather than
     * letting a checkbox in another pane silently undo it. Rates that are not a
     * preset's NTSC value (143.86, say) are carried as-is and must not touch the
     * global flag. */
    if (settings_ntsc_refresh_rate_x100_for_fps(whole) > 0) {
        app_configuration->use_ntsc_refresh = (x100 == fps_x100_ntsc(whole));
    }
    settings_sync_refresh_rate(app_configuration);

    uint16_t index = fps_entry_index_for(ctx, *ctx->value_ref);
    lv_dropdown_set_selected(ctx->dropdown, index);
    ctx->selected_index = index;

    /* Let the pane recompute what depends on the rate (bitrate hint, warnings)
     * without the echo being read as the user opening the editor again. Only on
     * a real difference: LVGL itself sends no VALUE_CHANGED for an unchanged
     * selection, so this echo would be the only source of a phantom reconnect
     * prompt. */
    const bool rate_changed = *ctx->value_ref != prev_fps ||
                              (ctx->refresh_rate_x100_ref != NULL && *ctx->refresh_rate_x100_ref != prev_rate_x100);
    if (rate_changed) {
        ctx->suppress_open = true;
        lv_event_send(ctx->dropdown, LV_EVENT_VALUE_CHANGED, NULL);
        ctx->suppress_open = false;
    }

    fps_dropdown_refresh_label(ctx);
}

static void fps_editor_msgbox_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    pref_dropdown_fps_ctx_t *ctx = lv_obj_get_user_data(mbox);
    /* The content bubbles its own VALUE_CHANGED up here (that is how the key
     * handling reaches the box); only the button row means "the user answered". */
    if (lv_event_get_target(e) != lv_msgbox_get_btns(mbox)) {
        return;
    }
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        ctx->saved = true;
        fps_editor_save(ctx);
    }
    lv_msgbox_close_async(mbox);
}

static void fps_editor_delete_cb(lv_event_t *e) {
    pref_dropdown_fps_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx->saved) {
        /* Cancel, Back and the close button all land here, so the dropdown is
         * put back in one place rather than in three. */
        lv_dropdown_set_selected(ctx->dropdown, ctx->selected_index);
        fps_dropdown_refresh_label(ctx);
    }
    ctx->dialog = NULL;
    ctx->readout = NULL;
    ctx->detail = NULL;
    ctx->rate_value = NULL;
    ctx->ntsc_check = NULL;
    ctx->input = NULL;
    ctx->change_ev_received = false;
}

/** A dialog row that is a focus stop: the overlay's slab, laid out as a line. */
static lv_obj_t *fps_editor_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(row, LV_DPX(8), 0);
    lv_obj_set_style_radius(row, LV_DPX(4), 0);
    lv_obj_set_style_bg_color(row, ml_color_hex(ML_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(row, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_FOCUS_KEY);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(8), 0);
    /* KEY has to reach the message box: that is where UP/DOWN moves the focus
     * between the rows (msgbox_key in lvgl/theme/lv_theme_moonlight.c). */
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    return row;
}

static lv_obj_t *fps_editor_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color,
                                  lv_opa_t opa) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_color(label, ml_color_hex(color), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    return label;
}

static void fps_editor_open(pref_dropdown_fps_ctx_t *ctx) {
    if (ctx->dialog != NULL) {
        return;
    }
    int x100 = ctx->refresh_rate_x100_ref != NULL ? *ctx->refresh_rate_x100_ref : 0;
    if (x100 <= 0) {
        x100 = fps_x100_exact(*ctx->value_ref > 0 ? *ctx->value_ref : ctx->min_fps);
    }
    ctx->hold_repeats = 0;
    ctx->saved = false;

    const static char *btn_texts[] = {translatable("Cancel"), translatable("Save"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Custom FPS"), NULL, btn_texts, false);
    ctx->dialog = msgbox;
    lv_obj_set_user_data(msgbox, ctx);
    /* The theme's generic message-box key/cancel handling stands down for this
     * one; the rows below carry their own (see fps_editor_add_nav). */
    lv_obj_add_flag(msgbox, LV_OBJ_FLAG_USER_4);
    lv_obj_set_style_max_width(msgbox, LV_PCT(72), 0);
    lv_obj_set_width(msgbox, LV_PCT(56));

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, LV_DPX(8), 0);

    const lv_font_t *font_small = lv_theme_get_font_small(content);
    const lv_font_t *font_large = lv_theme_get_font_large(content);

    /* The rate, big enough to read from the couch — this is the answer to the
     * only question the dialog asks. */
    ctx->readout = fps_editor_label(content, "", font_large, ML_COLOR_PRIMARY, LV_OPA_COVER);
    lv_obj_set_width(ctx->readout, LV_PCT(100));
    lv_obj_set_style_text_align(ctx->readout, LV_TEXT_ALIGN_CENTER, 0);

    ctx->detail = fps_editor_label(content, "", font_small, ML_COLOR_TEXT, OVERLAY_OPA_MUTED);
    lv_obj_set_width(ctx->detail, LV_PCT(100));
    lv_obj_set_style_text_align(ctx->detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ctx->detail, LV_LABEL_LONG_WRAP);

    /* Stepper. LEFT/RIGHT is the app's idiom for "change this row" everywhere
     * else in the settings (sliders, dropdowns), so it is the idiom here. */
    lv_obj_t *stepper = fps_editor_row(content);
    lv_obj_t *stepper_title = fps_editor_label(stepper, locstr("Rate"), NULL, ML_COLOR_TEXT, LV_OPA_COVER);
    lv_obj_set_flex_grow(stepper_title, 1);
    fps_editor_label(stepper, "-", NULL, ML_COLOR_TEXT, OVERLAY_OPA_MUTED);
    ctx->rate_value = fps_editor_label(stepper, "", NULL, ML_COLOR_TEXT, LV_OPA_COVER);
    lv_obj_set_style_min_width(ctx->rate_value, LV_DPX(90), 0);
    lv_obj_set_style_text_align(ctx->rate_value, LV_TEXT_ALIGN_CENTER, 0);
    fps_editor_label(stepper, "+", NULL, ML_COLOR_TEXT, OVERLAY_OPA_MUTED);
    lv_obj_add_event_cb(stepper, fps_stepper_key_cb, LV_EVENT_KEY, ctx);
    lv_obj_add_event_cb(stepper, fps_stepper_defocus_cb, LV_EVENT_DEFOCUSED, ctx);
    fps_editor_add_nav(stepper, ctx);

    /* The fractional part as one tick instead of three decimals to type. This is
     * what a "custom" rate is for in practice: 59.94, 119.88, 143.86. */
    lv_obj_t *ntsc = lv_checkbox_create(content);
    lv_checkbox_set_text(ntsc, locstr("NTSC rate (x 1000/1001)"));
    lv_obj_set_size(ntsc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(ntsc, LV_DPX(10), 0);
    lv_obj_set_style_radius(ntsc, LV_DPX(4), 0);
    pref_checkbox_prepare_for_dpad(ntsc);
    lv_obj_add_flag(ntsc, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(ntsc, fps_ntsc_click_cb, LV_EVENT_CLICKED, ctx);
    fps_editor_add_nav(ntsc, ctx);
    ctx->ntsc_check = ntsc;

    /* Typed entry, for a keyboard or a pointer. Deliberately last: it is the
     * fallback, not the main road. */
    lv_obj_t *input_row = lv_obj_create(content);
    lv_obj_remove_style_all(input_row);
    lv_obj_set_size(input_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, LV_DPX(10), 0);
    lv_obj_set_style_pad_hor(input_row, LV_DPX(10), 0);

    lv_obj_t *input_label = fps_editor_label(input_row, locstr("Type a value"), font_small, ML_COLOR_TEXT,
                                             OVERLAY_OPA_MUTED);
    lv_obj_set_flex_grow(input_label, 1);

    lv_obj_t *input = lv_textarea_create(input_row);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_accepted_chars(input, "0123456789.");
    lv_textarea_set_max_length(input, 7);
    lv_obj_set_width(input, LV_DPX(120));
    lv_obj_add_flag(input, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(input, fps_input_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(input, fps_input_focused_cb, LV_EVENT_FOCUSED, ctx);
    lv_obj_add_event_cb(input, fps_input_activate_cb, LV_EVENT_CLICKED, ctx);
    fps_editor_add_nav(input, ctx);
    ctx->input = input;

    lv_obj_t *hint = fps_editor_label(content, "", font_small, ML_COLOR_TEXT, OVERLAY_OPA_FAINT);
    lv_label_set_text_fmt(hint, locstr("Left / Right changes the rate · Up / Down moves · %d-%d FPS"),
                          ctx->min_fps, ctx->max_fps);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* The button row is created first (inside lv_msgbox_create), so the theme's
     * child-to-group hook puts it at the head of the focus order while it sits
     * at the foot of the dialog. Re-adding it moves it to the end, so walking
     * DOWN goes where the eye goes. */
    lv_obj_t *btns = lv_msgbox_get_btns(msgbox);
    if (btns != NULL) {
        lv_group_t *group = lv_obj_get_group(btns);
        if (group != NULL) {
            lv_group_remove_obj(btns);
            lv_group_add_obj(group, btns);
        }
        fps_editor_add_nav(btns, ctx);
    }

    lv_obj_add_event_cb(msgbox, fps_editor_msgbox_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(msgbox, fps_editor_delete_cb, LV_EVENT_DELETE, ctx);

    fps_editor_set_x100(ctx, x100);
    fps_editor_sync(ctx);

    /* Open on the control, not on the buttons: the dialog exists to change a
     * number, and landing on "Cancel" makes the user hunt for the way in. */
    lv_group_focus_obj(stepper);
    lv_obj_center(msgbox);
}
