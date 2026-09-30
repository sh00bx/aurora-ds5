#pragma once

/*
 * The Controllers page's own settings of a controller -- latency, audio route,
 * speaker and headphone volume, DualSense haptics and trigger softening, the
 * Flydigi composite switch -- as the pref store remembers them per controller
 * (`<id>.latency = 60` ... in [hid_pt_devices], the same id as its mode and
 * lightbar colour).
 *
 * Pure C with no platform calls: the ini words, the ranges a value must be in
 * to be taken, and the copy to and from the bridge's settings record. The
 * store (hid_pt_device_prefs.c) owns where the values live and when they are
 * written; tests/app/hid_passthrough exercises this file alone.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm/ctm_settings.h"

typedef enum {
    PAD_SETTING_LATENCY = 0,
    PAD_SETTING_AUDIO,
    PAD_SETTING_SPEAKER,
    PAD_SETTING_HEADSET,
    PAD_SETTING_HAPTICS,
    PAD_SETTING_TRIGGERS,
    PAD_SETTING_COMPOSITE,
    PAD_SETTING_COUNT
} pad_setting_t;

#define PAD_SETTING_BIT(s) (1u << (unsigned) (s))
/* What the audio/latency block of the page writes together. */
#define PAD_SETTINGS_AUDIO                                                                                    \
    (PAD_SETTING_BIT(PAD_SETTING_LATENCY) | PAD_SETTING_BIT(PAD_SETTING_AUDIO) |                              \
     PAD_SETTING_BIT(PAD_SETTING_SPEAKER) | PAD_SETTING_BIT(PAD_SETTING_HEADSET))
/* DualSense only. */
#define PAD_SETTINGS_DS5 (PAD_SETTING_BIT(PAD_SETTING_HAPTICS) | PAD_SETTING_BIT(PAD_SETTING_TRIGGERS))
#define PAD_SETTINGS_ALL ((1u << PAD_SETTING_COUNT) - 1u)

/* The ranges the page's sliders offer (hid_pt_panel_view.h); a stored value
 * outside them is dropped on load and the default stays. */
#define PAD_SETTING_LATENCY_MAX 200
#define PAD_SETTING_VOLUME_MAX 100
#define PAD_SETTING_HAPTICS_MAX 200
#define PAD_SETTING_TRIGGERS_MAX 9

typedef struct {
    uint8_t set;                        /* PAD_SETTING_BIT()s stored */
    uint16_t value[PAD_SETTING_COUNT];  /* only where set; audio = tv_bridge_audio_mode_t,
                                         * composite = 0/1 */
} pad_settings_t;

/* The key suffix of @p s (".latency" ...); NULL outside the enum. */
const char *pad_setting_suffix(pad_setting_t s);

/* Parse an ini value for @p s, range-checked: "auto|off|speaker|headset|both"
 * for the audio route, "on|off" for composite, plain decimal for the rest.
 * False (and *out untouched) for anything else. */
bool pad_setting_parse(pad_setting_t s, const char *text, uint16_t *out);

/* The ini value of @p v for @p s; "" when @p v is out of range. */
void pad_setting_format(pad_setting_t s, uint16_t v, char *buf, size_t len);

/* Copy the fields in @p mask from @p from into @p p (and mark them set). */
void pad_settings_capture(pad_settings_t *p, const tv_bridge_worker_settings_t *from, unsigned mask);

/* Overlay every stored field onto @p to. Returns whether anything was stored. */
bool pad_settings_apply(const pad_settings_t *p, tv_bridge_worker_settings_t *to);
