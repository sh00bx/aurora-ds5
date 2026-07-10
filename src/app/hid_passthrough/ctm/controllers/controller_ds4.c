/* DualShock 4 (DS4) controller, BT. Classification + the 0x15 BT output
 * patching (audio enable bits / route / volumes) ported verbatim from
 * tv_bridge_worker.c's apply_ds4_settings, now the patch_output hook. */

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

/* Map an audio mode to the DS4 BTAudio.AudioTarget byte (BT[80]). */
static uint8_t ds4_audio_target_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_SPEAKER: return 0x02;
        case TV_BRIDGE_AUDIO_HEADSET: return 0x24;
        case TV_BRIDGE_AUDIO_BOTH: return 0x26;
        case TV_BRIDGE_AUDIO_OFF:
        default: return 0x00;
    }
}

/* Scale a volume percent onto the DS4 raw byte range (0..0x4F firmware ceiling
 * per the controller wiki). Unlike the DS5 (raw 0..0x64 = percent 1:1) the DS4
 * ceiling is 79, so clamping percent instead of scaling made every slider value
 * >=79% identical (max) and skewed everything below it. */
static uint8_t ds4_volume_raw_byte(unsigned int value)
{
    if (value > 100u) value = 100u;
    return (uint8_t)((value * 0x4fu + 50u) / 100u);
}

/* patch_output: rewrite a DS4 0x15 BT output report in place per the live
 * settings — enable bits (BT[3]=0xB0 mask), route (BT[80]), volumes
 * (BT[21,22,24]), audio-unk (BT[25]) — then re-CRC. AUTO/OFF pass through
 * untouched. When: every outbound report, from the pump. Returns 0. */
static int ds4_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    if (!data || len < 84 || data[0] != 0x15) return 0;

    int patched = 0;
    uint8_t target = ds4_audio_target_for_mode(settings->audio_mode);
    uint8_t headset_volume = ds4_volume_raw_byte(settings->headset_volume_percent);
    uint8_t speaker_volume = ds4_volume_raw_byte(settings->speaker_volume_percent);

    /* AUTO + OFF = pass through unmodified (no DS4 latency block we own). */
    if (settings->audio_mode == TV_BRIDGE_AUDIO_AUTO) {
        return 0;
    }
    if (settings->audio_mode == TV_BRIDGE_AUDIO_OFF) {
        return 0;
    }

    /* BT[3] enable bitfield: 0x10/0x20 = L/R headphone, 0x80 = speaker. Assert
     * 0xB0 in any audible mode, clear it when route=off; preserve the low-nibble
     * rumble/LED enables the game owns. */
    const uint8_t kAudioEnableMask = 0xB0;
    int audible = (settings->audio_mode == TV_BRIDGE_AUDIO_SPEAKER ||
                   settings->audio_mode == TV_BRIDGE_AUDIO_HEADSET ||
                   settings->audio_mode == TV_BRIDGE_AUDIO_BOTH);
    uint8_t enable_byte = data[3];
    if (audible) enable_byte = (uint8_t)(enable_byte | kAudioEnableMask);
    else         enable_byte = (uint8_t)(enable_byte & (uint8_t)~kAudioEnableMask);
    if (data[3] != enable_byte) {
        data[3] = enable_byte;
        patched = 1;
    }

    if (data[2] != 0xa8) {
        data[2] = 0xa8;
        patched = 1;
    }
    if (data[80] != target) {
        data[80] = target;
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
    /* BT[25] audio-unk: 0x85 in audible modes (hypothesis fix for audio-during-
     * rumble glitch), cleared on off. */
    uint8_t unk_audio = (uint8_t)(audible ? 0x85 : 0x00);
    if (data[25] != unk_audio) {
        data[25] = unk_audio;
        patched = 1;
    }
    if (patched) ctm_bt_sign_output(data, len);
    return 0;
}

const ctm_controller_ops_t ctm_controller_ds4_ops = {
    .kind = "ds4",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .matches = ds4_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .patch_output = ds4_patch_output,
    .set_settings = NULL,   /* live values read via get_settings in patch_output */
};
