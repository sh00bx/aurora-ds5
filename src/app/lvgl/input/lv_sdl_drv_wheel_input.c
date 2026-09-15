#include "lvgl/lv_sdl_drv_input.h"

#include <SDL.h>

#include "app.h"
#include "stream/session.h"
#include "stream/session_events.h"
#include "ui/root.h"
#include "ui/ui_input.h"
#include "lv_gridview.h"

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

static bool wheel_handle_gridview(lv_obj_t *target, int notches) {
    if (notches == 0 || target == NULL || !lv_obj_check_type(target, &lv_gridview_class)) {
        return false;
    }
    /* Wheel down (notches > 0) advances one page; wheel up goes back one page. */
    lv_gridview_page(target, notches > 0);
    return true;
}

/**
 * Scroll the nearest vertical scrollable ancestor of @p target.
 * Prevents Magic Remote / mouse wheel from driving LVGL encoder focus navigation
 * on settings panes and host lists.
 */
static bool wheel_handle_scrollable(lv_obj_t *target, int notches) {
    if (notches == 0 || target == NULL) {
        return false;
    }

    lv_obj_t *scrollable = target;
    while (scrollable != NULL) {
        if (lv_obj_has_flag(scrollable, LV_OBJ_FLAG_SCROLLABLE)) {
            const lv_dir_t dir = lv_obj_get_scroll_dir(scrollable);
            if (dir & LV_DIR_VER) {
                const lv_coord_t overflow =
                        lv_obj_get_scroll_top(scrollable) + lv_obj_get_scroll_bottom(scrollable);
                if (overflow > 0) {
                    /* Confirmed inverted on-device with the original mapping. Match
                     * wheel_handle_gridview's convention: wheel down (y > 0) reveals
                     * further/later content, same direction as paging forward. */
                    const lv_coord_t step = LV_MAX(lv_dpx(48), lv_obj_get_height(scrollable) / 6);
                    const lv_coord_t dy = (notches > 0) ? step : -step;
                    /* LV_ANIM_OFF: track wheel input directly instead of queuing a
                     * ~200-400ms animated hop per tick, which felt sluggish/laggy
                     * when scrolling quickly (browser/native scrolling doesn't
                     * animate each discrete wheel tick either). */
                    lv_obj_scroll_by_bounded(scrollable, 0, dy, LV_ANIM_OFF);
                    return true;
                }
            }
        }
        scrollable = lv_obj_get_parent(scrollable);
    }
    return false;
}

/**
 * One wheel notch's worth of scrolling, aimed at @p target: page a grid, else
 * scroll its nearest scrollable ancestor. Positive @p notches is "wheel down",
 * the direction that reveals later content.
 *
 * Split out of the read_cb so the touchpad cursor can scroll what the pointer
 * is ON, while the wheel keeps scrolling what the focus is on. Feeding the
 * touchpad through a synthetic SDL_MOUSEWHEEL instead would be a trap: the
 * read_cb below samples the input gate when it READS, so a gate flip in the
 * millisecond between push and read would inject the scroll into the game.
 *
 * @return true when something scrolled.
 */
bool lv_ui_scroll_at(lv_obj_t *target, int notches) {
    if (wheel_handle_gridview(target, notches)) {
        return true;
    }
    return wheel_handle_scrollable(target, notches);
}

int lv_sdl_init_wheel(lv_indev_drv_t *drv, app_ui_input_t *input) {
    lv_indev_drv_init(drv);
    drv->user_data = input;
    drv->type = LV_INDEV_TYPE_ENCODER;
    drv->read_cb = sdl_input_read;

    return 0;
}

/* The wheel aims at whatever has focus -- unchanged behaviour for the remote. */
static lv_obj_t *wheel_focused_target(app_ui_input_t *input) {
    lv_group_t *group = app_input_get_group(input);
    return group != NULL ? lv_group_get_focused(group) : NULL;
}

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    app_ui_input_t *input = drv->user_data;
    SDL_Event e;
    data->state = LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0;
    if (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL)) {
        app_t *app = input->ui->app;
        if (app->session != NULL && !ui_should_block_input()) {
            session_handle_input_event(app->session, &e);
        } else if (e.wheel.y != 0
                   && lv_ui_scroll_at(wheel_focused_target(input), e.wheel.y > 0 ? 1 : -1)) {
            /* Page the game grid / scroll the list instead of changing focus. */
        } else if (e.wheel.y != 0) {
            /* Encoder scroll only: never set PRESSED — that triggers LVGL "encoder button" clicks. */
            /* Fallback for lists that fit without overflow (wheel_handle_scrollable above only
             * fires when there's actual scroll room). Match wheel_handle_gridview's convention:
             * wheel down (y > 0) should move focus forward/down (positive enc_diff). */
            data->enc_diff = (e.wheel.y > 0) ? 1 : -1;
        }
    }
    data->continue_reading = false;
}
