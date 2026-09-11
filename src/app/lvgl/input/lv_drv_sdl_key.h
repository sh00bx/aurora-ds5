#pragma once

#include "lvgl.h"
#include <SDL_events.h>

typedef struct app_ui_input_t app_ui_input_t;

typedef struct lv_drv_sdl_key_t {
    lv_indev_drv_t base;
    uint32_t key, ev_key;
    lv_indev_state_t state;
    char text[SDL_TEXTINPUTEVENT_TEXT_SIZE];
    uint32_t text_len;
    uint8_t text_remain;
    uint32_t text_next;
    bool changed;
    uint32_t last_read_tick;
    /* Text-entry fallback, for the case where a physical keyboard produces key
     * events but no SDL_TEXTINPUT (see text_key_fallback in lv_drv_sdl_key.c).
     * `pending_text_char` is the character of a press we have not typed yet;
     * `sdl_text_input_works` latches once SDL has delivered text on its own,
     * after which the fallback stays out of the way for good. */
    uint32_t pending_text_char;
    bool sdl_text_input_works;
} lv_drv_sdl_key_t;

int lv_sdl_init_key_input(lv_drv_sdl_key_t *drv, app_ui_input_t *input);

void lv_sdl_key_input_release_key(lv_indev_t *indev);
