/**
 * The controller touchpad as a mouse for Aurora's own UI.
 *
 * Glue between tp_gesture (the host's pointer feel, ported verbatim) and LVGL:
 * owns the cursor object, moves the shared pointer position, queues clicks for
 * the pointer indev, focuses what the cursor hovers, and scrolls what it sits
 * on.
 *
 * Who gets the touchpad is not decided here and needs no switch: the caller in
 * lv_drv_sdl_key.c offers every touch event to the session first, and the
 * session refuses exactly when Aurora's UI owns input (overlay, HID sheet, soft
 * keyboard) or when no stream is running. What it refuses lands here.
 *
 * LVGL thread only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/tp_gesture.h"
#include "lvgl.h"

typedef struct app_ui_input_t app_ui_input_t;

/** Create the cursor and bind it to the pointer indev. After indev register. */
void lv_tp_cursor_attach(app_ui_input_t *input);

/** Drop the cursor. Before the pointer indev is deleted. */
void lv_tp_cursor_detach(app_ui_input_t *input);

/** One normalized touchpad contact update; @p source identifies the pad. */
void lv_tp_cursor_touch(app_ui_input_t *input, uintptr_t source, tp_touch_ev_t ev, uint32_t finger,
                        float x, float y);

/** The pad's physical click. */
void lv_tp_cursor_click(app_ui_input_t *input, uintptr_t source, bool down);

/**
 * Lift everything and release a held press.
 *
 * Must be called whenever the ground moves under an in-flight gesture -- the
 * overlay closing with a finger still down, the stream taking input back, the
 * UI being torn down -- or the press stays down on an object that is about to
 * go away, and the next UI that opens inherits a phantom click.
 */
void lv_tp_cursor_cancel(void);

/** Re-clamp the cursor and recompute the output scale after a display resize. */
void lv_tp_cursor_notify_resize(app_ui_input_t *input);

/** Called by the pointer indev when a real mouse/remote event moves the cursor. */
void lv_tp_cursor_note_mouse(app_ui_input_t *input);
