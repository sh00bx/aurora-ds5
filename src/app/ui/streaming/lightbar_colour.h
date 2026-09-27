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

/*
 * The picker's three sliders: hue 0-359 (degrees), brightness 1-100,
 * intensity (saturation) 0-100. Brightness is perceptual rather than linear:
 * the bar's value is V = round(255 * (b / 100)^3), never below 1 -- so the
 * palette's level, 0x04, is brightness 25, and full brightness 0xFF is 100,
 * with most of the slider spent where the eye tells levels apart.
 */
#define LIGHTBAR_HUE_MAX        359
#define LIGHTBAR_BRIGHTNESS_MIN 1
#define LIGHTBAR_BRIGHTNESS_MAX 100
#define LIGHTBAR_INTENSITY_MAX  100
/* Where the picker starts from Automatic or Off: red, full intensity, the
 * palette's brightness. */
#define LIGHTBAR_BRIGHTNESS_PALETTE 25

/** The bar's value (0x01-0xFF) for brightness @p b (1-100). */
static inline unsigned lightbar_colour_value(unsigned b)
{
    if (b > LIGHTBAR_BRIGHTNESS_MAX) {
        b = LIGHTBAR_BRIGHTNESS_MAX;
    }
    /* 255 * b^3 / 100^3, rounded; 255 * 100^3 still fits 32 bits. */
    const unsigned v = (255u * b * b * b + 500000u) / 1000000u;
    return v < 1 ? 1 : v;
}

/** The brightness for value @p v, the inverse of lightbar_colour_value(): the
 * one nearest the curve's exact inverse, cbrt(v / 255) * 100 -- so a round trip
 * gives @p v back wherever the curve reaches it (0x04 is brightness 25, not the
 * 24 that rounds to it too), and the nearest value it reaches elsewhere. */
static inline unsigned lightbar_colour_brightness(unsigned v)
{
    if (v > 255) {
        v = 255;
    }
    unsigned best = LIGHTBAR_BRIGHTNESS_MIN;
    unsigned best_err = ~0u;
    for (unsigned b = LIGHTBAR_BRIGHTNESS_MIN; b <= LIGHTBAR_BRIGHTNESS_MAX; ++b) {
        /* |255 b^3 - v 100^3|: both sides below 2^28. */
        const unsigned curve = 255u * b * b * b;
        const unsigned want = v * 1000000u;
        const unsigned err = curve > want ? curve - want : want - curve;
        if (err < best_err) {
            best = b;
            best_err = err;
        }
    }
    return best;
}

/**
 * HSV to 0xRRGGBB with integer rounding: hue @p h in degrees (0-359),
 * saturation @p s (0-100), value @p v (0-255). Within each 60-degree sector
 * the channels are v, p = v(1 - s), and the rising or falling one,
 * v(1 - s(1 - f)) or v(1 - s f) with f the position in the sector.
 */
static inline uint32_t lightbar_colour_from_hsv(unsigned h, unsigned s, unsigned v)
{
    h %= 360;
    if (s > 100) {
        s = 100;
    }
    if (v > 255) {
        v = 255;
    }
    const unsigned sector = h / 60;
    const unsigned f = h % 60;
    const unsigned p = (v * (100 - s) + 50) / 100;
    const unsigned q = (v * (6000 - s * f) + 3000) / 6000;
    const unsigned t = (v * (6000 - s * (60 - f)) + 3000) / 6000;
    unsigned r, g, b;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return (uint32_t) (r << 16 | g << 8 | b);
}

/**
 * 0xRRGGBB to HSV, the inverse of lightbar_colour_from_hsv(): hue in degrees,
 * saturation 0-100, value 0-255, each rounded. A grey has hue 0, black also
 * saturation 0.
 */
static inline void lightbar_colour_to_hsv(uint32_t rgb, unsigned *h, unsigned *s, unsigned *v)
{
    const int r = (int) ((rgb >> 16) & 0xFFu), g = (int) ((rgb >> 8) & 0xFFu), b = (int) (rgb & 0xFFu);
    int max = r > g ? r : g;
    max = max > b ? max : b;
    int min = r < g ? r : g;
    min = min < b ? min : b;
    const int delta = max - min;
    *v = (unsigned) max;
    *s = max == 0 ? 0 : (unsigned) ((100 * delta + max / 2) / max);
    if (delta == 0) {
        *h = 0;
        return;
    }
    /* 60 degrees per sector, rounded to the nearest degree. */
    int num;
    int base;
    if (max == r) {
        num = g - b;
        base = 0;
    } else if (max == g) {
        num = b - r;
        base = 120;
    } else {
        num = r - g;
        base = 240;
    }
    int hue = base + (60 * num + (num >= 0 ? delta / 2 : -delta / 2)) / delta;
    hue = ((hue % 360) + 360) % 360;
    *h = (unsigned) hue;
}
