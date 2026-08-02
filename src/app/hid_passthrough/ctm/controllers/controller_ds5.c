/* DualSense (DS5) controller, BT. Classification + the 0x36/0x32 BT output
 * patching (audio route / volume / latency / haptics) ported verbatim from
 * tv_bridge_worker.c's apply_ds5_settings, now the patch_output hook. */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <math.h>
#include <string.h>

/* matches: claim the DualSense over BT. When: factory classification. */
static bool ds5_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "054c") == 0 &&
           strcmp(dev->pid, "0ce6") == 0 &&
           strcmp(dev->bus, "BT") == 0;
}

/* Sub-block geometry, shared by both patch branches.
 *
 * A DS5 audio/haptic sub-block header is [id][payload length]. Bit 6 of the id
 * marks a DOUBLED payload: the batched 0x39 report carries TWO 64-byte coil
 * blocks / TWO 200-byte Opus frames under one header while the length byte keeps
 * naming the SINGLE block size (0x12|0xC0 = 0xD2, len 64 => 128 bytes on the
 * wire). 0x36's own blocks never set bit 6, so every rule below is a byte-for-byte
 * no-op on 0x36 — walking a 0x39 with the plain length would step into the middle
 * of the second block and let arbitrary audio bytes masquerade as a 0x90/0x91
 * header.
 *
 * Inferred from two independent implementations (awalol/DS5Dongle src/audio.cpp,
 * Kodzinho/DualSense-Bluetooth-Audio), not from a vendor spec. */
static size_t ds5_block_wire_len(uint8_t block_id, size_t payload_len)
{
    return (block_id & 0x40u) ? payload_len * 2u : payload_len;
}

/* Sub-block id with the doubling bit masked off, so the matchers below can be
 * written once against the 0x9x ids and cover the 0xDx (batched) forms too. */
static uint8_t ds5_block_base(uint8_t block_id)
{
    return (uint8_t)(block_id & ~0x40u);
}

/* Map an audio mode to the DS5 BT 0x36 sub-block header byte. */
static uint8_t ds5_audio_block_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_SPEAKER: return 0x93;
        case TV_BRIDGE_AUDIO_HEADSET: return 0x96;
        case TV_BRIDGE_AUDIO_BOTH: return 0x95;
        case TV_BRIDGE_AUDIO_OFF:
        default: return 0x00;
    }
}

/* Perceptual haptics-gain curve (1.0 == unity), clamped 0..5. */
static double ds5_haptics_gain(unsigned int gain_centi)
{
    double gain = (double)gain_centi / 100.0;
    if (gain <= 0.0) return 0.0;
    if (gain >= 5.0) return 5.0;
    if (gain <= 1.0) return gain;
    return 1.0 + pow((gain - 1.0) / 4.0, 1.35) * 4.0;
}

/* Clamp a volume percent to the DS5 raw byte range (0..0x64). */
static uint8_t ds5_volume_raw_byte(unsigned int value)
{
    return (uint8_t)(value > 0x64u ? 0x64u : value);
}

/* patch_output: rewrite a DS5 0x36/0x32 BT output report in place per the live
 * settings — audio route (0x9x), volume + audio-ctrl bits (0x90), latency
 * (0x91), haptics gain (0x92) — then re-CRC. AUTO touches only the latency
 * block. When: every outbound report, from the pump. Returns 0 (never drops). */
static int ds5_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    /* 0x39 is the batched audio/haptic report (two Opus frames + two coil blocks,
     * 547 B) — same sub-block grammar as 0x36, so it patches through the same
     * walker; only the geometry helpers above differ. */
    if (!data || len < 12 ||
        (data[0] != 0x36 && data[0] != 0x32 && data[0] != 0x39)) return 0;

    int patched = 0;
    size_t pos = 2;
    size_t limit = len - 4;

    if (settings->audio_mode == TV_BRIDGE_AUDIO_AUTO) {
        /* Floor 1, not 0: 0x00 in the 0x91 latency block is unverified against
         * the DS5 firmware (possible sentinel/ignore value) — review S6. Every
         * user-visible setting >=1ms stays byte-identical; only the unverified
         * 0x00 is never emitted. (The old floor of 20 had been lowered to 0 for
         * low-latency testing.) */
        uint8_t auto_latency = (uint8_t)(settings->latency_ms < 1 ? 1 : settings->latency_ms);
        uint8_t auto_headset = ds5_volume_raw_byte(settings->headset_volume_percent);
        uint8_t auto_speaker = ds5_volume_raw_byte(settings->speaker_volume_percent);
        while (pos + 2 <= limit) {
            uint8_t block_id = data[pos];
            size_t payload_len = data[pos + 1];
            size_t block_len = ds5_block_wire_len(block_id, payload_len) + 2;
            uint8_t block_base = ds5_block_base(block_id);
            if (block_id == 0 && payload_len == 0) break;
            if (block_len > limit - pos) break;
            if (block_base == 0x91 && payload_len >= 6) {
                /* Payload is [marker][latency ...][audio packet counter]: patch
                 * everything between the two. 0x36 carries five latency bytes
                 * (payload_len 7 -> i 3..7, the historical hardcoded range),
                 * 0x39 four (payload_len 6 -> i 3..6). Running the old fixed
                 * range on 0x39 would overwrite the packet counter — which the
                 * pad uses to sequence audio — with a latency value. */
                for (size_t i = 3; i <= payload_len; ++i) {
                    if (data[pos + i] != auto_latency) {
                        data[pos + i] = auto_latency;
                        patched = 1;
                    }
                }
            } else if (block_base == 0x90 && payload_len >= 8) {
                /* AUTO leaves route/audio-control to the game, but the volume
                 * sliders must still work: games practically never set the
                 * volume-valid flags, so the firmware default (full volume)
                 * always won and the UI sliders were inert (2026-07-05).
                 * Patch ONLY the volume bytes + their valid bits (flags0
                 * 0x10=headset, 0x20=speaker); no 0x80 audio-control, no
                 * payload[9] route byte -- those stay the game's. */
                if ((data[pos + 2] & 0x30u) != 0x30u) {
                    data[pos + 2] = (uint8_t)(data[pos + 2] | 0x30u);
                    patched = 1;
                }
                if (data[pos + 6] != auto_headset) {
                    data[pos + 6] = auto_headset;
                    patched = 1;
                }
                if (data[pos + 7] != auto_speaker) {
                    data[pos + 7] = auto_speaker;
                    patched = 1;
                }
            }
            pos += block_len;
        }
        if (patched) ctm_bt_sign_output(data, len);
        return 0;
    }

    uint8_t audio_block = ds5_audio_block_for_mode(settings->audio_mode);
    /* Same 1ms floor as the AUTO branch: 0x00 in the 0x91 latency block is
     * unverified against the DS5 firmware (possible sentinel) — review S6. */
    uint8_t latency = (uint8_t)(settings->latency_ms < 1 ? 1 : settings->latency_ms);
    uint8_t headset_volume = ds5_volume_raw_byte(settings->headset_volume_percent);
    uint8_t speaker_volume = ds5_volume_raw_byte(settings->speaker_volume_percent);
    uint8_t target_headset_volume = 0;
    uint8_t target_speaker_volume = 0;
    uint8_t target_audio_flags = 0;

    switch (settings->audio_mode) {
        case TV_BRIDGE_AUDIO_HEADSET:
            target_headset_volume = headset_volume;
            break;
        case TV_BRIDGE_AUDIO_SPEAKER:
            target_speaker_volume = speaker_volume;
            target_audio_flags = 0x30;
            break;
        case TV_BRIDGE_AUDIO_BOTH:
            target_headset_volume = headset_volume;
            target_speaker_volume = speaker_volume;
            target_audio_flags = 0x30;
            break;
        case TV_BRIDGE_AUDIO_OFF:
        default:
            break;
    }

    while (pos + 2 <= limit) {
        uint8_t block_id = data[pos];
        size_t payload_len = data[pos + 1];
        size_t block_len = ds5_block_wire_len(block_id, payload_len) + 2;
        uint8_t block_base = ds5_block_base(block_id);
        if (block_id == 0 && payload_len == 0) break;
        if (block_len > limit - pos) break;

        if (block_base == 0x90 && payload_len >= 8) {
            /* Only patch confirmed audio fields; preserve effect/rumble bytes. */
            if ((data[pos + 2] & 0xb0u) != 0xb0u) {
                data[pos + 2] = (uint8_t)(data[pos + 2] | 0xb0u);
                patched = 1;
            }
            if ((data[pos + 3] & 0x80u) != 0x80u) {
                data[pos + 3] = (uint8_t)(data[pos + 3] | 0x80u);
                patched = 1;
            }
            if (data[pos + 6] != target_headset_volume) {
                data[pos + 6] = target_headset_volume;
                patched = 1;
            }
            if (data[pos + 7] != target_speaker_volume) {
                data[pos + 7] = target_speaker_volume;
                patched = 1;
            }
            if (data[pos + 9] != target_audio_flags) {
                data[pos + 9] = target_audio_flags;
                patched = 1;
            }
        } else if ((block_base == 0x93 || block_base == 0x94 || block_base == 0x95 || block_base == 0x96) &&
                   audio_block != 0) {
            /* The route IS the block id; keep the doubling bit when rewriting it,
             * or a batched 0x39 audio block would shrink to half its length for
             * every later walker (ours and the pad's). */
            uint8_t routed = (uint8_t)(audio_block | (block_id & 0x40u));
            if (data[pos] != routed) {
                data[pos] = routed;
                patched = 1;
            }
        } else if (block_base == 0x91 && payload_len >= 6) {
            /* See the AUTO branch: patch payload[1 .. len-2], never the trailing
             * audio packet counter. */
            for (size_t i = 3; i <= payload_len; ++i) {
                if (data[pos + i] != latency) {
                    data[pos + i] = latency;
                    patched = 1;
                }
            }
        } else if (block_base == 0x92 && payload_len >= 2 && settings->haptics_gain_centi != 100) {
            double gain = ds5_haptics_gain(settings->haptics_gain_centi);
            for (size_t i = 2; i < block_len; ++i) {
                int sample = (int)(int8_t)data[pos + i];
                int scaled = (int)lrint((double)sample * gain);
                if (scaled < -128) scaled = -128;
                if (scaled > 127) scaled = 127;
                uint8_t value = (uint8_t)(int8_t)scaled;
                if (data[pos + i] != value) {
                    data[pos + i] = value;
                    patched = 1;
                }
            }
        }
        pos += block_len;
    }

    if (patched) ctm_bt_sign_output(data, len);
    return 0;
}

static int ds5_on_plug_init(ctm_controller_t *c, ctm_transport_t *t)
{
    (void)t;
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    if (!s.block_bt_audio_sink) {
        return 0;
    }
    /* Request DS5 BT HID-only mode: report 0x05, byte[1] bit4 = disable A2DP. */
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    feature[1] = 0x18;
    return ctm_controller_write_feature(c, feature, sizeof(feature));
}

static void ds5_on_input_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    /* DS5 BT input 0x31: payload starts at byte 2; status[0] (battery) is at
     * offset 52 within the payload (Linux hid-playstation dualsense_input_report). */
    if (!c || !data || len < 55 || data[0] != 0x31) {
        return;
    }
    ctm_controller_update_battery_raw(c, data[54]);
}

static void ds5_neutralize_input(ctm_controller_t *c, uint8_t *buf, size_t len)
{
    /* Offsets per Linux hid-playstation dualsense_input_report (payload at
     * buf+2 for BT 0x31): sticks 0-3, triggers 4-5, seq 6, buttons 7-10,
     * gyro 15-20, accel 21-26, touch points at 32/36 (bit7 of the contact
     * byte = finger up). Accel, timestamps, seq and battery stay real so the
     * game sees a live but untouched pad. The stale BT CRC is fine: the host
     * bridge repacks the payload into the USB 0x01 report and never validates
     * it (ds5_reports.h bt_input_to_usb). */
    (void) c;
    if (!buf || len < 55 || buf[0] != 0x31) {
        return;
    }
    uint8_t *p = buf + 2;
    p[0] = p[1] = p[2] = p[3] = 0x80; /* LX LY RX RY centered */
    p[4] = p[5] = 0;                  /* L2 R2 released */
    p[7] = 0x08;                      /* dpad neutral, face buttons clear */
    p[8] = 0;                         /* L1 R1 L2 R2 create options L3 R3 */
    p[9] = 0;                         /* PS, touchpad click, mute */
    p[10] = 0;                        /* vendor */
    memset(&p[15], 0, 6);             /* gyro: no rotation (accel keeps gravity) */
    p[32] |= 0x80;                    /* touch finger 1 up */
    p[36] |= 0x80;                    /* touch finger 2 up */
}

const ctm_controller_ops_t ctm_controller_ds5_ops = {
    .kind = "ds5",
    .needs_host_config = true,
    .grab_evdev = true,
    .request_bt_mode = true,
    .raw_acl_output = true,
    .matches = ds5_matches,
    .select_node = NULL,
    .on_plug_init = ds5_on_plug_init,
    .patch_output = ds5_patch_output,
    .set_settings = NULL,   /* live values read via get_settings in patch_output */
    .on_input_report = ds5_on_input_report,
    .neutralize_input = ds5_neutralize_input,
};
