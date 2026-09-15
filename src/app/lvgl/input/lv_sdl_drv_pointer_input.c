#include "lvgl/lv_sdl_drv_input.h"

#include "app.h"

#include <SDL.h>
#include "ui/root.h"
#include "logging.h"

#include "lvgl.h"
#include "lv_tp_cursor.h"
#include "stream/session.h"
#include "stream/session_events.h"
#include "ui/ui_input.h"

/* How long a tap is reported as pressed. It has to outlive at least one read
 * (LVGL only raises CLICKED for a press it has actually processed), and at a
 * 1 ms read period against a 17 ms refresh it also has to outlive a few
 * frames -- otherwise the button never paints its pressed state and the tap
 * reads as a dropped input. Still far below LV_INDEV_DEF_LONG_PRESS_TIME. */
#define TP_TAP_PRESS_HOLD_MS 60

static void indev_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

int lv_sdl_init_pointer(lv_indev_drv_t *drv, app_ui_input_t *input) {
    lv_indev_drv_init(drv);
    drv->user_data = input;
    drv->type = LV_INDEV_TYPE_POINTER;
    drv->read_cb = indev_pointer_read;
    return 0;
}

/**
 * Serve the touchpad cursor: position moves as the gesture engine feeds it,
 * and a tap has to be stretched into a press long enough for LVGL to process
 * and for the screen to show.
 *
 * The position is deliberately NOT moved between press and release -- more
 * than LV_INDEV_DEF_SCROLL_LIMIT of travel makes LVGL adopt a scroll target
 * and swallow the CLICKED. The gesture engine holds motion around click
 * edges, so this costs nothing.
 */
static void indev_pointer_synthetic(app_ui_pointer_state_t *st) {
    if (!st->synth_down && (st->held || st->press_pending > 0)) {
        if (!st->held) {
            st->press_pending--;
        }
        st->synth_down = true;
        st->synth_down_tick = lv_tick_get();
        st->state = LV_INDEV_STATE_PRESSED;
    } else if (st->synth_down && !st->held
               && lv_tick_elaps(st->synth_down_tick) >= TP_TAP_PRESS_HOLD_MS) {
        st->synth_down = false;
        st->state = LV_INDEV_STATE_RELEASED;
    } else if (st->synth_down) {
        st->state = LV_INDEV_STATE_PRESSED;
    }
}

static void indev_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    app_ui_input_t *input = drv->user_data;
    app_t *app = input->ui->app;
    app_ui_pointer_state_t *st = &input->pointer_state;
    SDL_Event e;
    data->continue_reading = SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEBUTTONUP) > 0;
    if (!data->continue_reading) {
        indev_pointer_synthetic(st);
        data->point = st->point;
        data->state = st->state;
        return;
    }
    /* A real pointer device is being used, so the platform draws its own
     * cursor and ours gets out of the way. Both write the same position, so
     * either source can pick the other one up where it left off. */
    lv_tp_cursor_note_mouse(input);
    if (e.type == SDL_MOUSEMOTION) {
        if (app->session != NULL) {
            session_handle_input_event(app->session, &e);
        }
        st->state = data->state = e.motion.state;
        st->point = data->point = (lv_point_t) {.x = e.motion.x, .y = e.motion.y};
    } else {
        if (app->session != NULL) {
            session_handle_input_event(app->session, &e);
        }
        st->state = data->state = e.button.state;
        st->point = data->point = (lv_point_t) {.x = e.button.x, .y = e.button.y};
        ui_set_input_mode(input, UI_INPUT_MODE_MOUSE);
    }
}