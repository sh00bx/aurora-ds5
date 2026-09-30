#include "hid_pt_pad_settings.h"

#include <stdio.h>
#include <string.h>

static const char *const k_suffix[PAD_SETTING_COUNT] = {
    [PAD_SETTING_LATENCY] = ".latency",
    [PAD_SETTING_AUDIO] = ".audio",
    [PAD_SETTING_SPEAKER] = ".speaker",
    [PAD_SETTING_HEADSET] = ".headset",
    [PAD_SETTING_HAPTICS] = ".haptics",
    [PAD_SETTING_TRIGGERS] = ".triggers",
    [PAD_SETTING_COMPOSITE] = ".composite",
};

/* Indexed by tv_bridge_audio_mode_t. */
static const char *const k_audio[] = {"auto", "off", "speaker", "headset", "both"};
#define AUDIO_WORDS (sizeof(k_audio) / sizeof(k_audio[0]))

/* Largest value a numeric setting takes; 0 for the two that are words. */
static unsigned numeric_max(pad_setting_t s)
{
    switch (s) {
        case PAD_SETTING_LATENCY:
            return PAD_SETTING_LATENCY_MAX;
        case PAD_SETTING_SPEAKER:
        case PAD_SETTING_HEADSET:
            return PAD_SETTING_VOLUME_MAX;
        case PAD_SETTING_HAPTICS:
            return PAD_SETTING_HAPTICS_MAX;
        case PAD_SETTING_TRIGGERS:
            return PAD_SETTING_TRIGGERS_MAX;
        default:
            return 0;
    }
}

const char *pad_setting_suffix(pad_setting_t s)
{
    return (unsigned) s < PAD_SETTING_COUNT ? k_suffix[s] : NULL;
}

bool pad_setting_parse(pad_setting_t s, const char *text, uint16_t *out)
{
    if (!text || !out || (unsigned) s >= PAD_SETTING_COUNT) {
        return false;
    }
    if (s == PAD_SETTING_AUDIO) {
        for (unsigned i = 0; i < AUDIO_WORDS; ++i) {
            if (strcmp(text, k_audio[i]) == 0) {
                *out = (uint16_t) i;
                return true;
            }
        }
        return false;
    }
    if (s == PAD_SETTING_COMPOSITE) {
        /* Not "true": an older build reads `<id>.composite = true` as an
         * auto-plug opt-in of a device called "<id>.composite". */
        if (strcmp(text, "on") == 0 || strcmp(text, "off") == 0) {
            *out = text[1] == 'n';
            return true;
        }
        return false;
    }
    /* Plain decimal only: no sign, no blanks, no hex -- a hand-edited or
     * truncated file must not turn into some other volume. */
    size_t n = strlen(text);
    if (n == 0 || n > 3) {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        v = v * 10u + (unsigned) (text[i] - '0');
    }
    if (v > numeric_max(s)) {
        return false;
    }
    *out = (uint16_t) v;
    return true;
}

void pad_setting_format(pad_setting_t s, uint16_t v, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    if ((unsigned) s >= PAD_SETTING_COUNT) {
        return;
    }
    if (s == PAD_SETTING_AUDIO) {
        if (v < AUDIO_WORDS) {
            snprintf(buf, len, "%s", k_audio[v]);
        }
    } else if (s == PAD_SETTING_COMPOSITE) {
        snprintf(buf, len, "%s", v ? "on" : "off");
    } else if (v <= numeric_max(s)) {
        snprintf(buf, len, "%u", (unsigned) v);
    }
}

/* The record's value of @p s, clamped into what the ini can hold. */
static uint16_t read_field(const tv_bridge_worker_settings_t *from, pad_setting_t s)
{
    unsigned v;
    switch (s) {
        case PAD_SETTING_LATENCY:
            v = from->latency_ms;
            break;
        case PAD_SETTING_AUDIO:
            return (unsigned) from->audio_mode < AUDIO_WORDS ? (uint16_t) from->audio_mode
                                                              : (uint16_t) TV_BRIDGE_AUDIO_AUTO;
        case PAD_SETTING_SPEAKER:
            v = from->speaker_volume_percent;
            break;
        case PAD_SETTING_HEADSET:
            v = from->headset_volume_percent;
            break;
        case PAD_SETTING_HAPTICS:
            v = from->haptics_gain_centi;
            break;
        case PAD_SETTING_TRIGGERS:
            v = from->ds5_trigger_reduce;
            break;
        case PAD_SETTING_COMPOSITE:
            return from->composite_passthrough ? 1 : 0;
        default:
            return 0;
    }
    const unsigned max = numeric_max(s);
    return (uint16_t) (v > max ? max : v);
}

void pad_settings_capture(pad_settings_t *p, const tv_bridge_worker_settings_t *from, unsigned mask)
{
    if (!p || !from) {
        return;
    }
    for (int s = 0; s < PAD_SETTING_COUNT; ++s) {
        if (mask & PAD_SETTING_BIT(s)) {
            p->value[s] = read_field(from, (pad_setting_t) s);
            p->set |= (uint8_t) PAD_SETTING_BIT(s);
        }
    }
}

bool pad_settings_apply(const pad_settings_t *p, tv_bridge_worker_settings_t *to)
{
    if (!p || !to || !p->set) {
        return false;
    }
    for (int s = 0; s < PAD_SETTING_COUNT; ++s) {
        if (!(p->set & PAD_SETTING_BIT(s))) {
            continue;
        }
        const uint16_t v = p->value[s];
        switch ((pad_setting_t) s) {
            case PAD_SETTING_LATENCY:
                to->latency_ms = v;
                break;
            case PAD_SETTING_AUDIO:
                to->audio_mode = (tv_bridge_audio_mode_t) v;
                break;
            case PAD_SETTING_SPEAKER:
                to->speaker_volume_percent = v;
                break;
            case PAD_SETTING_HEADSET:
                to->headset_volume_percent = v;
                break;
            case PAD_SETTING_HAPTICS:
                to->haptics_gain_centi = v;
                break;
            case PAD_SETTING_TRIGGERS:
                to->ds5_trigger_reduce = v;
                break;
            case PAD_SETTING_COMPOSITE:
                to->composite_passthrough = v != 0;
                break;
            default:
                break;
        }
    }
    return true;
}
