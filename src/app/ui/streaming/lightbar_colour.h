#pragma once

/**
 * Colour arithmetic for the Controllers page's LIGHTBAR row and its picker.
 *
 * The bar is run dim: the palette's brightest channel is 0x04, the level the
 * user picked for red (040000), which spares a DS4's battery and still reads
 * in a dark room. On screen such a value is black, so everything the page
 * draws goes through lightbar_colour_display() first, and the picker speaks
 * hue / brightness / intensity rather than bytes. Pure integer functions, no
 * LVGL: the panel and the view both use them.
 */

#include <stdint.h>

/**
 * @p rgb as the page shows it: scaled so its brightest channel is 0xFF, hue
 * and saturation kept (040100 -> FF4000). Black stays black.
 */
static inline uint32_t lightbar_colour_display(uint32_t rgb)
{
    const unsigned r = (rgb >> 16) & 0xFFu, g = (rgb >> 8) & 0xFFu, b = rgb & 0xFFu;
    unsigned max = r > g ? r : g;
    max = max > b ? max : b;
    if (max == 0) {
        return 0;
    }
    const unsigned half = max / 2;
    return ((r * 255u + half) / max) << 16 | ((g * 255u + half) / max) << 8 | ((b * 255u + half) / max);
}
