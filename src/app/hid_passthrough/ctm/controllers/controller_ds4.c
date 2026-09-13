/* DualShock 4 (DS4) controller, BT. Classification + Layout B output
 * patching, ported from CTM-Bridge's GPL-3 ctm-bridge-webos (commit 453e3e4,
 * "DS4 panel: Auto/Headphones/Split modes + volume sliders; Layout B TV-side
 * patch") with our percent volume scaling kept (chain-review db0b4d0f).
 *
 * The host emits Layout B frames: 0x11 effect reports (rumble/LED + volume
 * bytes) and pure-audio reports 0x12/0x14/0x17 whose route byte sits at
 * offset 5 (probed bitmask: 0xFF = stereo headphones, 0xDF = split
 * ch0->speaker / ch1->headphone-L). This hook runs AFTER the host's
 * translation, so forcing here wins without fighting the host. */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <string.h>

/* Status byte inside the BT 0x11 input report: common block starts at 3, the
 * byte sits at common offset 29. Battery level in the low nibble, cable state
 * 0x10, mic 0x20, headphones 0x40. */
#define DS4_BT_STATUS_OFFSET 32

/* matches: claim the DualShock 4 (either PID) over BT. When: classification. */
static bool ds4_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "054c") == 0 &&
           (strcmp(dev->pid, "09cc") == 0 || strcmp(dev->pid, "05c4") == 0) &&
           strcmp(dev->bus, "BT") == 0;
}

/* Layout B route byte (audio frame offset 5) for a forced mode; 0xFF/0xDF per
 * the probed bitmask, 0x00 = firmware "no target" (OFF), and AUTO returns the
 * sentinel 0x01 meaning "leave the host's jack auto-route in charge" (0x01 is
 * not a meaningful route value: the low nibble must contain 0x02 or 0x04 to
 * enable output at all). SPEAKER maps to split — there is no clean
 * speaker-stereo route value; split plays SBC ch0 on the speaker. */
static uint8_t ds4_route_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_HEADSET: return 0xff;  /* stereo headphones */
        case TV_BRIDGE_AUDIO_BOTH: return 0xdf;     /* split: speaker + headphone-L */
        case TV_BRIDGE_AUDIO_SPEAKER: return 0xdf;  /* the panel model collapses this
                                                     * onto BOTH for the DS4; kept for
                                                     * values persisted before that */
        case TV_BRIDGE_AUDIO_OFF: return 0x00;
        case TV_BRIDGE_AUDIO_AUTO:
        default: return 0x01;
    }
}

/* Scale a volume percent onto the DS4 raw byte range (0..0x4F firmware ceiling
 * per the controller wiki): the ceiling is 79, so clamping percent instead of
 * scaling would make every slider value >=79% identical (max) and skew
 * everything below it. Both pads share speaker_volume_percent, but the DS5
 * speaker is no longer linear (ds5_speaker_volume_byte spreads 1..100% over the
 * audible 0x3d..0x64; only the DS5 headset path is still 1:1), so the same
 * slider value sounds different here. The DS4 audible threshold has not been
 * measured yet - this curve stays linear until it has. */
static uint8_t ds4_volume_raw_byte(unsigned int value)
{
    if (value > 100u) value = 100u;
    return (uint8_t)((value * 0x4fu + 50u) / 100u);
}

/* patch_output: Layout B, in place, then re-CRC.
 * - 0x12/0x14/0x17 pure-audio frames: force the route byte [5] when the mode
 *   is Headphones (0xFF), Split/Both/Speaker (0xDF) or Off (0x00); AUTO passes
 *   through — the host's auto-route already wrote it from the jack bit.
 * - 0x11 effect frames: volume bytes BT[21]=headphone-L, BT[22]=headphone-R,
 *   BT[24]=speaker are always the sliders' (TV owns volume; the pad persists
 *   whatever was set last, so an explicit value every frame is the sane
 *   default). BT[3] high bits 0x10/0x20/0x80 are the volume-valid flags —
 *   without them the pad ignores bytes 21/22/24; the low nibble
 *   (rumble/LED/flash valid) stays the game's. Rumble/LED bytes untouched.
 * When: every outbound report, from the pump. Returns 0 (never drops). */
static int ds4_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    if (!data || len < 10) return 0;

    int patched = 0;

    if (data[0] == 0x12 || data[0] == 0x14 || data[0] == 0x17) {
        uint8_t route = ds4_route_for_mode(settings->audio_mode);
        /* 0x00 from the host is not a destination the user's mode overrides --
         * it is the host disarming the audio plane at the end of a stream
         * (Vibepollo ds4_audio.cpp STOP_REPORTS). The DS4 otherwise LOOPS the
         * last effect forever once the 0x17 stream stops, with the app closed
         * and a silence tail already written; only "no target" ends it.
         * Forcing a route back onto those reports re-arms the plane and the
         * loop survives. The host never sends 0x00 as a steady value, so this
         * cannot silence a live stream -- but it does mean a NEW client needs a
         * host from 2026-08-26 or later (an older one used 0x00 for
         * auto-without-jack). */
        if (route != 0x01 && data[5] != 0x00 && data[5] != route) {
            data[5] = route;
            patched = 1;
        }
    } else if (data[0] == 0x11 && len >= 30) {
        uint8_t headset_volume = ds4_volume_raw_byte(settings->headset_volume_percent);
        uint8_t speaker_volume = ds4_volume_raw_byte(settings->speaker_volume_percent);
        uint8_t valid_byte = (uint8_t)(data[3] | 0xb0u);
        if (data[3] != valid_byte) {
            data[3] = valid_byte;
            patched = 1;
        }
        if (data[21] != headset_volume) {
            data[21] = headset_volume;
            patched = 1;
        }
        if (data[22] != headset_volume) {
            data[22] = headset_volume;
            patched = 1;
        }
        if (data[24] != speaker_volume) {
            data[24] = speaker_volume;
            patched = 1;
        }
    }

    if (patched) ctm_bt_sign_output(data, len);
    return 0;
}

/* on_input_report: read-only observer of what the pad sends us.
 *
 * The battery byte is the same one the host reads for the audio jack bit --
 * common offset 29, i.e. data[32] in the BT 0x11 report (data[30] over USB;
 * hid-sony calls it "byte 30, or 32 for BT"). Low nibble is the level, bit 4
 * the cable state, bits 0x20/0x40 mic/headphones. Decoding lives in
 * ctm_controller_update_battery_ds4(), which is deliberately NOT the DS5's
 * _raw() variant: that one reads the high nibble as a charging state and
 * discards anything above 2, so a DS4 with a headset plugged in (0x40) would
 * report no battery at all -- which is exactly what happened before this hook
 * existed, in both the panel and the stats overlay.
 * When: every inbound report, from the pump. */
static void ds4_on_input_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c || !data || len < (size_t) (DS4_BT_STATUS_OFFSET + 1) || data[0] != 0x11) {
        return;
    }
    ctm_controller_update_battery_ds4(c, data[DS4_BT_STATUS_OFFSET]);
}

static void ds4_neutralize_input(ctm_controller_t *c, uint8_t *buf, size_t len)
{
    /* Offsets per Linux hid-sony/hid-playstation dualshock4_input_report
     * (common block at buf+3 for BT 0x11): sticks 0-3, buttons[3] at 4-6 (dpad
     * hat + face, shoulders/PS/touch-click, with the report counter in 6's
     * high bits), triggers 7-8, gyro 12-17, accel 18-23, status 29-30; the
     * touch-frame count sits at 32, each frame 9 bytes (timestamp + two 4-byte
     * contacts, bit7 of a contact byte = finger up). Accel, timestamps,
     * counter and battery stay real so the game sees a live but untouched pad.
     * The stale BT CRC follows the DS5 hook's precedent: the host repacks the
     * payload into the virtual pad's own report instead of forwarding these
     * bytes. */
    (void) c;
    if (!buf || len < 36 || buf[0] != 0x11) {
        return;
    }
    uint8_t *p = buf + 3;
    p[0] = p[1] = p[2] = p[3] = 0x80; /* LX LY RX RY centered */
    p[4] = 0x08;                      /* dpad neutral, face buttons clear */
    p[5] = 0;                         /* L1 R1 L2 R2 share options L3 R3 */
    p[6] &= 0xfc;                     /* PS + touchpad click; counter bits stay */
    p[7] = p[8] = 0;                  /* L2 R2 released */
    memset(&p[12], 0, 6);             /* gyro: no rotation (accel keeps gravity) */
    unsigned frames = p[32] > 4 ? 4 : p[32]; /* the BT report carries at most 4 */
    for (unsigned m = 0; m < frames; m++) {
        size_t contact0 = 33u + m * 9u + 1u;
        if (3u + contact0 + 4u < len) {
            p[contact0] |= 0x80;      /* touch finger 1 up */
            p[contact0 + 4] |= 0x80;  /* touch finger 2 up */
        }
    }
}

/* Pump policy. The DS4 shares the DS5's BT premise — a connected pad streams
 * input continuously, and the jail hidraw node never signals the drop — so it
 * gets the same 2 s liveness watchdog. Identical consecutive 0x11 effect
 * frames are deduped (games re-assert unchanged rumble/LED at report rate, and
 * every skipped write is BT airtime the 62.5/s audio stream needs). The DS5
 * concealment/rumble-slot machinery stays off: it parses 0x36/0x39 internals
 * that do not exist here. */
static const ctm_pump_policy_t ds4_policy = {
    .input_idle_timeout_ms = 2000,
    .hid_eagain_wait_ms = 3,
    .dedup_report_id = 0x11,
};

const ctm_controller_ops_t ctm_controller_ds4_ops = {
    .kind = "ds4",
    .policy = &ds4_policy,
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .raw_acl_output = true,
    .matches = ds4_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .patch_output = ds4_patch_output,
    .set_settings = NULL,   /* live values read via get_settings in patch_output */
    .on_input_report = ds4_on_input_report,
    .neutralize_input = ds4_neutralize_input,
};
