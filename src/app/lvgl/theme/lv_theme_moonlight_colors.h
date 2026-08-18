#pragma once

#include "lvgl.h"

/**
 * The app's palette IS the streaming overlay's palette.
 *
 * The overlay (command bar, HID sheet, performance panel) was designed first
 * and its two rules carry over to the launcher and the settings unchanged:
 *
 *   - Colour says WHAT a thing is: teal (OVERLAY_LIVE) marks "on/active/
 *     selected", red marks errors and the one destructive action.
 *   - White says WHERE YOU ARE: focus is a chalk edge, never a hue.
 *
 * The names below are the launcher-side vocabulary for the same constants, so
 * the two surfaces cannot drift apart. See overlay_style.h for the reasoning
 * behind each value.
 */
#include "ui/streaming/overlay_style.h"

#define ML_COLOR_BG           OVERLAY_INK
#define ML_COLOR_SURFACE      OVERLAY_SLAB
#define ML_COLOR_SURFACE_ALT  OVERLAY_SLAB_SEL
#define ML_COLOR_SURFACE_HI   OVERLAY_SLAB_HI
#define ML_COLOR_BORDER       OVERLAY_SEAM
#define ML_COLOR_PRIMARY      OVERLAY_LIVE
/** The teal, dimmed for pressed/secondary uses. */
#define ML_COLOR_PRIMARY_DIM  0x0F9E83
#define ML_COLOR_TEXT         OVERLAY_CHALK
#define ML_COLOR_TEXT_MUTED   0x93A6B3
/** Focus is white ("where you are"), everywhere. */
#define ML_COLOR_FOCUS        OVERLAY_CHALK

static inline lv_color_t ml_color_hex(uint32_t c) {
    return lv_color_hex(c);
}
