/**
 * @file lv_tp_cursor.c
 * @brief The controller touchpad as a mouse for Aurora's own UI (see header).
 */
#include "lv_tp_cursor.h"

#include "app.h"
#include "logging.h"
#include "lvgl/lv_sdl_drv_input.h"
#include "ui/root.h"
#include "ui/ui_input.h"

#if TARGET_WEBOS

#include <SDL.h>

#endif

/* The one UI the cursor belongs to. Same shape as streaming.controller.c's
 * current_controller: lv_tp_cursor_cancel() is called from places that have no
 * business knowing about input state (the gate publisher), and there is never
 * more than one display. */
static app_ui_input_t *tp_input = NULL;
static tp_gesture_t tp_gesture;
/* lv_group_focus_obj() runs application FOCUSED handlers, which may create or
 * delete objects -- including, transitively, another hover. */
static bool tp_hover_busy = false;

static void tp_set_source_touchpad(app_ui_input_t *input);

static lv_obj_t *tp_hit(lv_point_t point);

static void tp_hover_focus(app_ui_input_t *input);

/* ---------------------------------------------------------------- sink --- */

static void tp_sink_move(void *ud, int dx, int dy) {
    app_ui_input_t *input = ud;
    app_ui_pointer_state_t *st = &input->pointer_state;
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
        return;
    }
    lv_coord_t max_x = lv_disp_get_hor_res(disp) - 1;
    lv_coord_t max_y = lv_disp_get_ver_res(disp) - 1;
    lv_coord_t x = st->point.x + dx;
    lv_coord_t y = st->point.y + dy;
    st->point.x = LV_CLAMP(0, x, max_x);
    st->point.y = LV_CLAMP(0, y, max_y);
    tp_set_source_touchpad(input);
    /* While a press is in flight LVGL owns the target: moving the focus out
     * from under it would fight indev_click_focus, and more than
     * LV_INDEV_DEF_SCROLL_LIMIT of travel turns the press into a scroll and
     * eats the CLICKED. The gesture engine holds motion around click edges
     * anyway; this is the belt to that braces. */
    if (!st->held && !st->synth_down) {
        tp_hover_focus(input);
    }
}

static void tp_sink_scroll(void *ud, int notches) {
    app_ui_input_t *input = ud;
    /* What the cursor sits on, and failing that what has focus: the active
     * screen is itself clickable, so a hit over empty space lands there rather
     * than nowhere, and the fallback has to key off the result. */
    if (lv_ui_scroll_at(tp_hit(input->pointer_state.point), notches)) {
        return;
    }
    lv_group_t *group = app_input_get_group(input);
    lv_ui_scroll_at(group != NULL ? lv_group_get_focused(group) : NULL, notches);
}

static void tp_sink_button(void *ud, bool right, bool down, bool physical) {
    app_ui_input_t *input = ud;
    app_ui_pointer_state_t *st = &input->pointer_state;
    if (right) {
        /* LVGL has no secondary button, and a menu has nothing for one to do.
         * Deliberately dropped rather than folded into a left click. */
        return;
    }
    if (physical) {
        /* Spans reports on its own, so it can simply be held -- that is what
         * makes click-and-drag work. */
        st->held = down;
    } else if (down && st->press_pending < 0xff) {
        /* A tap arrives as a down/up pair inside one call, far shorter than
         * the pointer indev's read period. Count it and let the indev stretch
         * it into a press LVGL -- and the viewer -- can actually see. */
        st->press_pending++;
    }
    if (down) {
        tp_set_source_touchpad(input);
    }
}

/* ------------------------------------------------------------- cursor ---- */

static lv_obj_t *tp_cursor_create(lv_obj_t *parent) {
    /* A white dot in a dark halo: the one shape that stays legible both on the
     * near-black UI and on a bright game frame behind the overlay. Sized in
     * LV_DPX so a larger canvas gets a proportionally larger cursor. */
    lv_obj_t *cursor = lv_obj_create(parent);
    lv_obj_remove_style_all(cursor);
    /* The pointer must never hit its own cursor. lv_indev_set_cursor() clears
     * CLICKABLE on this object, but lv_obj_create() sets it on every child, and
     * a clickable dot sitting exactly under the point would swallow every press
     * before it reached the widget underneath -- including in our own hit test. */
    lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cursor, LV_DPX(20), LV_DPX(20));
    lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cursor, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_40, 0);
    lv_obj_set_style_border_color(cursor, lv_color_white(), 0);
    lv_obj_set_style_border_width(cursor, LV_DPX(2), 0);
    lv_obj_set_style_border_opa(cursor, LV_OPA_80, 0);
    lv_obj_set_style_shadow_color(cursor, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(cursor, LV_DPX(6), 0);
    lv_obj_set_style_shadow_opa(cursor, LV_OPA_50, 0);
    /* lv_indev_set_cursor puts the object's TOP-LEFT on the point; translate
     * is applied at draw time, so it centres without fighting that. */
    lv_obj_set_style_translate_x(cursor, -LV_DPX(10), 0);
    lv_obj_set_style_translate_y(cursor, -LV_DPX(10), 0);

    lv_obj_t *dot = lv_obj_create(cursor);
    lv_obj_remove_style_all(dot);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, LV_DPX(8), LV_DPX(8));
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    return cursor;
}

/**
 * How far one finger-millimetre should carry the cursor here.
 *
 * The host moves the Windows pointer in desktop pixels, and the TV scales that
 * desktop and this LVGL canvas onto the same panel. Equal travel on the GLASS
 * is what "the same as on the desktop" means, so the host's numbers are scaled
 * by canvas/stream. At the usual 1080p-into-1080p that is 1.0 and the feel is
 * the host's verbatim.
 */
static void tp_apply_scale(void) {
    lv_disp_t *disp = lv_disp_get_default();
    double scale = 1.0;
    if (disp != NULL && app_configuration != NULL && app_configuration->stream.width > 0 &&
        app_configuration->stream.height > 0) {
        double sx = (double) lv_disp_get_hor_res(disp) / (double) app_configuration->stream.width;
        double sy = (double) lv_disp_get_ver_res(disp) / (double) app_configuration->stream.height;
        scale = sx < sy ? sx : sy;
    }
    tp_gesture_set_scale(&tp_gesture, scale);
}

static void tp_set_source_touchpad(app_ui_input_t *input) {
    if (input->pointer_state.src == APP_POINTER_SRC_TOUCHPAD) {
        return;
    }
    input->pointer_state.src = APP_POINTER_SRC_TOUCHPAD;
    if (input->cursor != NULL) {
        lv_obj_clear_flag(input->cursor, LV_OBJ_FLAG_HIDDEN);
    }
#if TARGET_WEBOS
    /* Exactly one pointer on screen: the TV draws its own for the magic remote,
     * and it comes back on the remote's next move. Same call the D-pad uses to
     * dismiss it (lv_drv_sdl_key.c). */
    SDL_webOSCursorVisibility(SDL_FALSE);
#endif
    ui_set_input_mode(input, UI_INPUT_MODE_MOUSE);
}

void lv_tp_cursor_note_mouse(app_ui_input_t *input) {
    if (input->pointer_state.src == APP_POINTER_SRC_MOUSE) {
        return;
    }
    input->pointer_state.src = APP_POINTER_SRC_MOUSE;
    if (input->cursor != NULL) {
        /* The remote's pointer is the platform's, drawn above our surface. */
        lv_obj_add_flag(input->cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------- hover ----- */

static lv_obj_t *tp_hit(lv_point_t point) {
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
        return NULL;
    }
    /* LVGL's own order from indev_proc_press. The point is passed by pointer
     * and MUTATED on the way down (transform handling), so each search gets a
     * fresh copy. */
    lv_point_t p = point;
    lv_obj_t *found = lv_indev_search_obj(lv_disp_get_layer_sys(disp), &p);
    if (found == NULL) {
        p = point;
        found = lv_indev_search_obj(lv_disp_get_layer_top(disp), &p);
    }
    if (found == NULL) {
        p = point;
        found = lv_indev_search_obj(lv_disp_get_scr_act(disp), &p);
    }
    return found;
}

/**
 * Give the widget under the cursor the same focus the D-pad would give it, so
 * the two never drift apart: park the cursor on a button, press down on the
 * stick, and navigation continues from there rather than from wherever the
 * focus was left.
 */
static void tp_hover_focus(app_ui_input_t *input) {
    if (tp_hover_busy || input->text_input_active) {
        return;
    }
    lv_group_t *group = app_input_get_group(input);
    if (group == NULL) {
        return;
    }
    lv_obj_t *hit = tp_hit(input->pointer_state.point);
    lv_obj_t *target = NULL;
    for (lv_obj_t *obj = hit; obj != NULL; obj = lv_obj_get_parent(obj)) {
        if ((lv_group_t *) lv_obj_get_group(obj) == group) {
            target = obj;
            break;
        }
    }
    /* Nothing focusable under the cursor: leave the focus where it is. Clearing
     * it would strand the D-pad with nothing selected. */
    if (target == NULL || lv_group_get_focused(group) == target) {
        return;
    }

    tp_hover_busy = true;
    /* LV_EVENT_FOCUSED scrolls the target into view. Under a hover that is a
     * feedback loop: the scroll slides a different widget under a cursor that
     * never moved, which focuses, which scrolls. */
    bool scroll_on_focus = lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (scroll_on_focus) {
        lv_obj_clear_flag(target, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    }
    lv_group_focus_obj(target);
    /* FOCUSED runs application handlers, which are free to delete the object
     * they were handed. */
    if (lv_obj_is_valid(target)) {
        if (scroll_on_focus) {
            lv_obj_add_flag(target, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        }
        /* LVGL paints the focus ring only for keypad/encoder indevs. We are
         * usually standing in the key indev's read (the touch events are
         * peeped there), but that is an accident of plumbing and the ring is
         * the point of the exercise -- so add it by hand. DEFOCUSED clears it
         * again on its own. */
        lv_obj_add_state(target, LV_STATE_FOCUS_KEY);
    }
    tp_hover_busy = false;
}

/* -------------------------------------------------------------- api ------ */

void lv_tp_cursor_attach(app_ui_input_t *input) {
    static const tp_gesture_sink_t sink_proto = {
            .move = tp_sink_move,
            .scroll = tp_sink_scroll,
            .button = tp_sink_button,
            .ud = NULL,
    };
    tp_gesture_sink_t sink = sink_proto;
    sink.ud = input;
    tp_gesture_init(&tp_gesture, &sink);
    tp_input = input;
    tp_apply_scale();

    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL || input->pointer.indev == NULL) {
        return;
    }
    input->cursor = tp_cursor_create(lv_disp_get_layer_sys(disp));
    lv_indev_set_cursor(input->pointer.indev, input->cursor);
    lv_obj_add_flag(input->cursor, LV_OBJ_FLAG_HIDDEN);
}

void lv_tp_cursor_detach(app_ui_input_t *input) {
    lv_tp_cursor_cancel();
    if (input->pointer.indev != NULL) {
        /* Forget act_obj/last_obj: the objects they point at are about to go. */
        lv_indev_reset(input->pointer.indev, NULL);
    }
    if (input->cursor != NULL) {
        /* No lv_indev_set_cursor(indev, NULL) -- it dereferences its argument.
         * The indev keeps a pointer to the deleted object, which is harmless
         * because app_ui_input_deinit() deletes the indev on the next line and
         * nothing reads it in between (only the read timer would). */
        lv_obj_del(input->cursor);
        input->cursor = NULL;
    }
    tp_input = NULL;
}

static bool tp_enabled(const app_ui_input_t *input) {
    return input != NULL && app_configuration != NULL && app_configuration->touchpad_ui_mouse;
}

void lv_tp_cursor_touch(app_ui_input_t *input, uintptr_t source, tp_touch_ev_t ev, uint32_t finger,
                        float x, float y) {
    if (!tp_enabled(input)) {
        return;
    }
    tp_gesture_feed_touch(&tp_gesture, source, ev, finger, x, y);
}

void lv_tp_cursor_click(app_ui_input_t *input, uintptr_t source, bool down) {
    if (!tp_enabled(input)) {
        return;
    }
    tp_gesture_feed_click(&tp_gesture, source, down);
}

void lv_tp_cursor_cancel(void) {
    if (tp_input == NULL) {
        return;
    }
    app_ui_pointer_state_t *st = &tp_input->pointer_state;
    if (st->src == APP_POINTER_SRC_TOUCHPAD) {
        /* The cursor lives on the display's system layer, which outlives the
         * overlay being hidden -- left visible, the dot would float over the
         * game for the rest of the session. */
        st->src = APP_POINTER_SRC_NONE;
        if (tp_input->cursor != NULL) {
            lv_obj_add_flag(tp_input->cursor, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Called per event while the stream owns the touchpad, so say nothing
     * when there is nothing to say. */
    if (!tp_gesture_busy(&tp_gesture) && !st->press_pending && !st->held && !st->synth_down) {
        return;
    }
    tp_gesture_reset(&tp_gesture);
    st->press_pending = 0;
    st->held = false;
    if (st->synth_down) {
        st->synth_down = false;
        st->state = LV_INDEV_STATE_RELEASED;
    }
}

void lv_tp_cursor_notify_resize(app_ui_input_t *input) {
    lv_disp_t *disp = lv_disp_get_default();
    tp_apply_scale();
    if (disp == NULL) {
        return;
    }
    app_ui_pointer_state_t *st = &input->pointer_state;
    st->point.x = LV_CLAMP(0, st->point.x, lv_disp_get_hor_res(disp) - 1);
    st->point.y = LV_CLAMP(0, st->point.y, lv_disp_get_ver_res(disp) - 1);
    if (input->cursor != NULL) {
        lv_obj_set_pos(input->cursor, st->point.x, st->point.y);
    }
}
