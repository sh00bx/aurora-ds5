#pragma once

#include "lvgl.h"

#include "lvgl/input/lv_drv_sdl_key.h"

typedef struct app_ui_t app_ui_t;
typedef struct app_ui_input_t app_ui_input_t;
typedef enum app_ui_input_mode_t app_ui_input_mode_t;

typedef struct app_ui_input_lv_pair_t {
    lv_indev_drv_t drv;
    lv_indev_t *indev;
} app_ui_input_lv_pair_t;

typedef enum app_pointer_src_t {
    APP_POINTER_SRC_NONE = 0,
    APP_POINTER_SRC_MOUSE,    /**< a real mouse, or the TV's magic remote */
    APP_POINTER_SRC_TOUCHPAD, /**< a controller touchpad, via lv_tp_cursor */
} app_pointer_src_t;

/**
 * What the pointer indev reports, shared because two sources write it: the
 * SDL mouse absolutely, the touchpad in deltas. One position between them is
 * what makes the handover seamless -- neither source ever teleports the
 * cursor away from where the other left it.
 *
 * It lives here, not in read_cb statics, because app_ui_close()/open() throws
 * the display and every indev away: statics would survive with coordinates
 * from the old window size.
 */
typedef struct app_ui_pointer_state_t {
    lv_point_t point;
    lv_indev_state_t state;
    app_pointer_src_t src;
    uint8_t press_pending; /**< taps seen but not yet pressed */
    bool held;             /**< the pad is physically clicked */
    bool synth_down;       /**< we are currently reporting PRESSED */
    uint32_t synth_down_tick;
} app_ui_pointer_state_t;

enum app_ui_input_mode_t {
    UI_INPUT_MODE_POINTER_FLAG = 0x10,
    UI_INPUT_MODE_MOUSE = 0x11,
    UI_INPUT_MODE_REMOTE = 0x11,
    UI_INPUT_MODE_BUTTON_FLAG = 0x20,
    UI_INPUT_MODE_KEY = 0x21,
    UI_INPUT_MODE_GAMEPAD = 0x22,
};

struct app_ui_input_t {
    app_ui_t *ui;
    lv_group_t *app_group;
    lv_ll_t modal_groups;
    struct {
        lv_drv_sdl_key_t drv;
        lv_indev_t *indev;
    } key;
    app_ui_input_lv_pair_t pointer;
    app_ui_input_lv_pair_t wheel;
    app_ui_input_lv_pair_t button;
    app_ui_input_mode_t mode;
    bool text_input_active;
    app_ui_pointer_state_t pointer_state;
    lv_obj_t *cursor; /**< drawn for the touchpad only; see lv_tp_cursor.h */
};

void app_ui_input_init(app_ui_input_t *input, app_ui_t *ui);

void app_ui_input_deinit(app_ui_input_t *input);

void app_input_set_group(app_ui_input_t *input, lv_group_t *group);

void app_input_push_modal_group(app_ui_input_t *input, lv_group_t *group);

void app_input_remove_modal_group(app_ui_input_t *input, lv_group_t *group);

lv_group_t *app_input_get_group(app_ui_input_t *input);

/** @return true while a dialog/popup group is on the stack and owns input. */
bool app_input_has_modal_group(app_ui_input_t *input);

void app_input_set_button_points(app_ui_input_t *input, const lv_point_t *points);

bool ui_set_input_mode(app_ui_input_t *input, app_ui_input_mode_t mode);

app_ui_input_mode_t app_ui_get_input_mode(const app_ui_input_t *input);

void app_start_text_input(app_ui_input_t *input, int x, int y, int w, int h);

void app_stop_text_input(app_ui_input_t *input);

/**
 * @return true if state was updated
 */
bool app_text_input_state_update(app_ui_input_t *input);