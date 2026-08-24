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
        case TV_BRIDGE_AUDIO_SPEAKER: return 0xdf;
        case TV_BRIDGE_AUDIO_OFF: return 0x00;
        case TV_BRIDGE_AUDIO_AUTO:
        default: return 0x01;
    }
}

/* Scale a volume percent onto the DS4 raw byte range (0..0x4F firmware ceiling
 * per the controller wiki). Unlike the DS5 (raw 0..0x64 = percent 1:1) the DS4
 * ceiling is 79, so clamping percent instead of scaling would make every
 * slider value >=79% identical (max) and skew everything below it. */
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
        if (route != 0x01 && data[5] != route) {
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
};
