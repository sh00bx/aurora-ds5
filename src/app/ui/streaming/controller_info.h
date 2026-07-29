#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct app_t app_t;

/** How much the pad is willing to say about its charge. */
typedef enum {
    CONTROLLER_POWER_UNKNOWN = 0,
    CONTROLLER_POWER_EXACT,  /**< percent read straight off the pad */
    CONTROLLER_POWER_COARSE, /**< SDL bucket; percent is the bucket's upper bound */
    CONTROLLER_POWER_WIRED,
} controller_power_t;

/** Rows the panel can show; extra pads are counted but not listed. */
#define CONTROLLER_INFO_MAX 4

typedef struct {
    char name[48];       /**< as the pad reports itself, e.g. "DualSense Wireless Controller" */
    bool bridged;        /**< handed to the host as a real USB device instead of through SDL */
    controller_power_t power;
    int percent;         /**< 0..100, meaningful for EXACT and COARSE */
    bool charging;
    char power_text[16]; /**< ready to render: "95%", "< 20%", "Wired", "-" */
} controller_info_t;

/* Snapshot every controller the session can see, bridged ones first.
 *
 * DualSense pads report an exact percentage because their hidraw node is read
 * directly — that works whether the pad is bridged to the host via usbip or driven
 * locally by SDL. Every other pad falls back to SDL's coarse power level.
 *
 * The hidraw reads are cached for a few seconds, so calling this once per stats
 * refresh is cheap. Returns the number of entries written (at most max). */
int controller_info_collect(app_t *app, controller_info_t *out, int max);
