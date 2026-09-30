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

/* Input poll interval (ms) stamped into byte 1 of every outbound 0x11/0x14/0x17
 * — see ds4_patch_output. */
#define DS4_POLL_INTERVAL_MS 0x04u

/* BT 0x11 effects report, the bytes the lightbar lives in (the full layout is
 * spelled out above DS4_BT_OUTPUT_LEN): [3] valid flags, [8..10] RGB, [11..12]
 * flash on/off time. */
#define DS4_FLAG_LIGHTBAR 0x02u
#define DS4_FLAG_FLASH    0x04u
#define DS4_OUT_RGB       8

/* The colour this pad's bar is to show for a report the game has not painted:
 * the user's, or -- while the game owns the bar and may -- the game's. False on
 * Automatic, where nothing of ours is written at all. */
static bool ds4_lightbar_colour(ctm_controller_t *c, const tv_bridge_worker_settings_t *s, uint32_t *rgb,
                                bool *game)
{
    if (!s->lightbar_user) return false;
    *game = s->lightbar_game && ctm_controller_game_lightbar(c, rgb);
    if (!*game) *rgb = s->lightbar_rgb & 0xFFFFFFu;
    return true;
}

/* Stamp @p rgb as the bar's colour into a 0x11 at @p data, and with
 * @p no_flash an explicit "no blink" (the flash flag with on/off 0/0), so a
 * game's blink cannot take the bar either. Explicit, not the flag dropped: the
 * pad only takes a blink state from a report that carries the flag
 * (hid-playstation sends LED_BLINK only with it), so dropping it would leave a
 * blink the game started earlier running -- in the user's colour, and past the
 * game's own cancel. Returns 1 if a byte changed. */
static int ds4_stamp_lightbar(uint8_t *data, uint32_t rgb, bool no_flash)
{
    uint8_t want[5] = {(uint8_t) (rgb >> 16), (uint8_t) (rgb >> 8), (uint8_t) rgb, 0, 0};
    uint8_t flags = (uint8_t) (data[3] | DS4_FLAG_LIGHTBAR);
    size_t n = 3;
    if (no_flash) {
        flags = (uint8_t) (flags | DS4_FLAG_FLASH);
        n = 5;
    }
    if (data[3] == flags && memcmp(&data[DS4_OUT_RGB], want, n) == 0) return 0;
    data[3] = flags;
    memcpy(&data[DS4_OUT_RGB], want, n);
    return 1;
}

/* The lightbar on a host 0x11 (len >= 30, checked by the caller).
 *
 * First the ownership rule, learnt from what the host sends -- never from a
 * report of ours (ctm_controller_own_output()): the LED flag with a non-black
 * colour is the game painting, and owning, the bar; with black it hands the
 * bar back. A report without the flag changes nothing: a game's rumble-only
 * write must not end its colour. The host paints no colour of its own (its
 * synthetic lightbar is off from 1.7.30), so a coloured flag IS the game.
 *
 * Then the user's choice (Automatic: nothing). While the game owns the bar
 * and may, the report goes as the game sent it. Otherwise the LED flag and
 * the user's colour go into every 0x11 -- a rumble-only one too, so the next
 * report can never leave the pad on another colour -- and a colour the game
 * may not change also carries an explicit "no blink". */
static int ds4_patch_lightbar(ctm_controller_t *c, const tv_bridge_worker_settings_t *s, uint8_t *data)
{
    if (!ctm_controller_own_output(c) && (data[3] & DS4_FLAG_LIGHTBAR)) {
        ctm_controller_note_game_lightbar(c, ((uint32_t) data[DS4_OUT_RGB] << 16) |
                                             ((uint32_t) data[DS4_OUT_RGB + 1] << 8) | data[DS4_OUT_RGB + 2]);
    }
    uint32_t rgb = 0;
    bool game = false;
    const bool ours = ds4_lightbar_colour(c, s, &rgb, &game) && !game;
    const int changed = ours ? ds4_stamp_lightbar(data, rgb, !s->lightbar_game) : 0;
    if (data[3] & DS4_FLAG_LIGHTBAR) {
        ctm_controller_note_lightbar_out(c, ((uint32_t) data[DS4_OUT_RGB] << 16) |
                                            ((uint32_t) data[DS4_OUT_RGB + 1] << 8) | data[DS4_OUT_RGB + 2],
                                         ours);
    }
    return changed;
}

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
 * - 0x11 lightbar: the colour chosen on the Controllers page, unless the game
 *   owns the bar and may (ds4_patch_lightbar). Automatic: untouched.
 * - 0x11/0x14/0x17: byte 1 carries the pad's input poll interval in its low
 *   six bits (ms, hid-playstation DS4_OUTPUT_HWCTL_BT_POLL_MASK) under the
 *   HID/CRC bits 0x80/0x40. A host before 2026-09-26 sends 0xC0/0x40, i.e.
 *   interval 0: up to ~1000 input reports/s on the radio the effects and
 *   the 62.5/s speaker stream share, and DS4Windows notes that a bare 0x40 on
 *   0x17 resets the rate to zero. Forced to 4 ms, what SDL and USB use; a
 *   current host already sends 0xC4/0x44 and this changes nothing.
 * When: every outbound report, from the pump. Returns 0 (never drops). */
static int ds4_patch_output(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    const tv_bridge_worker_settings_t *settings = &s;

    size_t len = len_io ? *len_io : 0;
    if (!data || len < 10) return 0;

    int patched = 0;

    if (data[0] == 0x11 || data[0] == 0x14 || data[0] == 0x17) {
        uint8_t hwctl = (uint8_t) ((data[1] & 0xc0u) | DS4_POLL_INTERVAL_MS);
        if (data[1] != hwctl) {
            data[1] = hwctl;
            patched = 1;
        }
    }

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
        if (ds4_patch_lightbar(c, settings, data)) {
            patched = 1;
        }
    }

    if (patched) ctm_bt_sign_output(data, len);
    return 0;
}

/* BT 0x11 effects report as the host frames it (Vibepollo ds4_reports.h):
 * [0x11][hwctl = HID|CRC|poll ms][0x00][31-byte common][pad][crc32]. Inside:
 * [3] valid flags (0x01 motors, 0x02 lightbar, 0x04 flash, 0xb0 volumes),
 * [6] weak motor, [7] strong motor, [8..10] RGB, [11..12] flash on/off,
 * [21]/[22] headphone L/R volume, [24] speaker volume. */
#define DS4_BT_OUTPUT_LEN 78
#define DS4_OUT_STATE     6   /* motors, RGB, flash: [6..12] */
#define DS4_OUT_STATE_LEN 7
/* BT 0x17 pure-audio report: [0x17][hwctl][0xa0][frame ctr LE16][route]
 * [436 B SBC][pad][crc32]; the counter advances by 4 (SBC frames) per report. */
#define DS4_0X17_LEN      462
#define DS4_0X17_ROUTE    5   /* 0x00 = no target: the audio plane is disarmed */

/* The slider volumes and their valid flags, exactly as ds4_patch_output
 * stamps them into a host report, for a report this file builds itself. */
static void ds4_stamp_volumes(uint8_t *buf, const tv_bridge_worker_settings_t *s)
{
    uint8_t headset_volume = ds4_volume_raw_byte(s->headset_volume_percent);
    buf[3] |= 0xb0;
    buf[21] = headset_volume;
    buf[22] = headset_volume;
    buf[24] = ds4_volume_raw_byte(s->speaker_volume_percent);
}

/* build_settings_report: a 0x11 that carries the volume sliders. patch_output
 * can only stamp them into 0x11 reports the host sends, and the host sends none
 * while a game owns the lightbar and is quiet, or with the synthetic lightbar
 * off -- a slider moved then did nothing until the game happened to write
 * rumble or LED. BT[3] = 0xb0 claims the volume fields (0x10/0x20 headphone
 * L/R, 0x80 speaker).
 *
 * The rest RESTATES the current effect state instead of zeroing it: a push with
 * zeroed motor and lightbar bytes is only harmless on a pad that honours the
 * low flag nibble, and third-party-style firmware applies every field (Linux
 * hid-playstation always sends rumble and lightbar together for that reason)
 * -- there, each link-up and slider move blanked the bar and stopped the
 * motors. So once a 0x11 has been delivered THIS session, its motor, lightbar
 * and flash bytes and its low flag nibble are copied, and the push says again
 * what the pad was last told. Never from an earlier session: ctm_hid_io clears
 * that cache per session, so a reconnect cannot bring stale rumble back.
 * Without one, the old form: low nibble 0, zeroed bytes behind it.
 * Volume bytes, poll bits and CRC exactly as ds4_patch_output writes them, so
 * the patch finds nothing to change.
 * With a lightbar colour chosen on the Controllers page, the push carries it
 * too -- or the game's, while the game owns the bar and may: the link-up push
 * is what paints the bar when no game colour is known, and a change on the
 * page reaches the pad through this push at once instead of waiting for the
 * host's next 0x11. On Automatic it carries the colour the game painted last
 * this session, if it painted at all -- so going back to Automatic takes the
 * user's colour off at once -- and otherwise leaves the bar to the host.
 * When: session thread, at link-up and after a settings change; the pump sends
 * nothing when the result equals the last report it delivered. */
static size_t ds4_build_settings_report(ctm_controller_t *c, uint8_t *buf, size_t cap)
{
    if (!buf || cap < DS4_BT_OUTPUT_LEN) return 0;
    tv_bridge_worker_settings_t s;
    ctm_controller_get_settings(c, &s);
    uint8_t last[DS4_BT_OUTPUT_LEN];
    size_t last_len = ctm_controller_last_output(c, 0x11, last, sizeof(last), NULL);
    memset(buf, 0, DS4_BT_OUTPUT_LEN);
    buf[0] = 0x11;
    buf[1] = 0xc0 | DS4_POLL_INTERVAL_MS;
    if (last_len == DS4_BT_OUTPUT_LEN) {
        buf[3] = (uint8_t) (last[3] & 0x0fu);
        memcpy(&buf[DS4_OUT_STATE], &last[DS4_OUT_STATE], DS4_OUT_STATE_LEN);
    }
    uint32_t rgb = 0;
    bool game = false;
    bool before_known = false;
    if (ds4_lightbar_colour(c, &s, &rgb, &game)) {
        ds4_stamp_lightbar(buf, rgb, !s.lightbar_game);
    } else if (ctm_controller_game_lightbar_last(c, &rgb)) {
        /* Automatic: the bar as the game last painted it, not the colour of
         * ours the restated state may still carry. */
        ds4_stamp_lightbar(buf, rgb, false);
    } else if (ctm_controller_lightbar_ours(c, &before_known, &rgb)) {
        /* Automatic, the game never painted, and ours (a colour taken back,
         * a cancelled preview) is what the bar was told last: the colour it
         * was told before ours, if this session told it one. Otherwise the
         * restated state must at least not say ours again. */
        if (before_known) {
            ds4_stamp_lightbar(buf, rgb, false);
        } else {
            buf[3] = (uint8_t) (buf[3] & ~(DS4_FLAG_LIGHTBAR | DS4_FLAG_FLASH));
        }
    }
    ds4_stamp_volumes(buf, &s);
    ctm_bt_sign_output(buf, DS4_BT_OUTPUT_LEN);
    return DS4_BT_OUTPUT_LEN;
}

/* build_quiesce_reports: what the pad must hear when the session ends.
 *
 * Speaker: the DS4 latches and loops its audio buffer when the 0x17 stream
 * stops without a "no target" route (0x00) -- the host's stop burst is the only
 * thing that ends it, and a session that dies mid-stream (host link lost, the
 * stream quit, a switch to the SDL path) never sends one, so the pad loops
 * until it is switched off. So when the last 0x17 delivered this session left
 * the plane ARMED (route != 0x00), send the same stop the host would: 8
 * reports with route 0x00, the frame counter carried on from the last one
 * delivered (+4 per report, as the pad expects), re-signed. Armed is judged by
 * the route, not by the report's age: a lost host link only ends this session
 * once ENet's peer timeout (5 s minimum, 30 s maximum) has run out, so the
 * last 0x17 is always at least that old on exactly the path this exists for,
 * and the pad loops for as long as it stays on however old it is. A stream
 * that ended cleanly finished on the host's own route-0x00 reports and needs
 * nothing -- unless it ended in the last 5 s, the original rule, kept because
 * "delivered" means handed to ds5_txd, which can still age out or overflow a
 * queued report, so a recent stop run is repeated rather than trusted.
 * They are copies of that last report; with no target the pad decodes none of
 * the SBC payload. ds4_patch_output passes route 0x00 through untouched in
 * every audio mode, so the user's mode cannot re-arm the plane here.
 *
 * Motors: an ERM keeps its last value until told otherwise. One 0x11 with
 * both motors 0 follows. It is full state (0x03) when the lightbar colour is
 * known from a 0x11 delivered this session with its LED flag, so a pad that
 * applies every field keeps its colour; otherwise it claims the motors only
 * (0x01). Volumes and poll bits as ds4_patch_output stamps them. */
static int ds4_build_quiesce_reports(ctm_controller_t *c, uint8_t *buf, size_t cap,
                                     size_t *len, int max)
{
    if (!buf || !len || max <= 0) return 0;
    int n = 0;
    size_t off = 0;

    uint8_t last17[DS4_0X17_LEN];
    uint64_t age17 = 0;
    size_t l17 = ctm_controller_last_output(c, 0x17, last17, sizeof(last17), &age17);
    if (l17 == DS4_0X17_LEN && (last17[DS4_0X17_ROUTE] != 0x00 || age17 < 5000000ull)) {
        uint16_t ctr = (uint16_t) (last17[3] | (last17[4] << 8));
        for (int k = 0; k < 8 && n < max - 1 && off + DS4_0X17_LEN <= cap; ++k) {
            uint8_t *r = buf + off;
            memcpy(r, last17, DS4_0X17_LEN);
            ctr = (uint16_t) (ctr + 4u);
            r[1] = 0x40 | DS4_POLL_INTERVAL_MS;
            r[3] = (uint8_t) (ctr & 0xffu);
            r[4] = (uint8_t) (ctr >> 8);
            r[DS4_0X17_ROUTE] = 0x00;
            ctm_bt_sign_output(r, DS4_0X17_LEN);
            len[n++] = DS4_0X17_LEN;
            off += DS4_0X17_LEN;
        }
    }

    if (n < max && off + DS4_BT_OUTPUT_LEN <= cap) {
        tv_bridge_worker_settings_t s;
        ctm_controller_get_settings(c, &s);
        uint8_t last11[DS4_BT_OUTPUT_LEN];
        size_t l11 = ctm_controller_last_output(c, 0x11, last11, sizeof(last11), NULL);
        uint8_t *r = buf + off;
        memset(r, 0, DS4_BT_OUTPUT_LEN);
        r[0] = 0x11;
        r[1] = 0xc0 | DS4_POLL_INTERVAL_MS;
        r[3] = 0x01;                                   /* motors: both 0 below */
        if (l11 == DS4_BT_OUTPUT_LEN && (last11[3] & 0x02u)) {
            r[3] |= 0x02;
            memcpy(&r[DS4_OUT_RGB], &last11[DS4_OUT_RGB], 3);
        }
        ds4_stamp_volumes(r, &s);
        ctm_bt_sign_output(r, DS4_BT_OUTPUT_LEN);
        len[n++] = DS4_BT_OUTPUT_LEN;
    }
    return n;
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

/* The quit chord in the same layout: buttons[1] (common byte 5) holds L1 0x01,
 * R1 0x02, Share 0x10 and Options 0x20. */
static int ds4_quit_chord(const uint8_t *buf, size_t len)
{
    if (!buf || len < 36 || buf[0] != 0x11) {
        return -1;
    }
    const uint8_t b = buf[3 + 5];
    return ((b & 0x20) ? CTM_CHORD_START : 0) | ((b & 0x10) ? CTM_CHORD_BACK : 0) |
           ((b & 0x01) ? CTM_CHORD_LB : 0) | ((b & 0x02) ? CTM_CHORD_RB : 0);
}

/* Pump policy. The DS4 shares the DS5's BT premise — a connected pad streams
 * input continuously, and the jail hidraw node never signals the drop — so it
 * gets the same 2 s liveness watchdog. Identical consecutive 0x11 effect
 * frames are deduped (games re-assert unchanged rumble/LED at report rate, and
 * every skipped write is BT airtime the 62.5/s audio stream needs). The DS5
 * concealment/rumble-slot machinery stays off: it parses 0x36/0x39 internals
 * that do not exist here.
 *
 * The two audio queues are sized in 0x17 reports (16 ms each), not copied
 * from the DS5's 0x39 counts: a 6-deep daemon FIFO is 96 ms, inside ds5_txd's
 * 150 ms age-out, where the DS5's 10 would be 160 ms and shed audio in an
 * ordinary drain; and the paced ring keeps 6 reports (96 ms) after a WiFi
 * bunch instead of 4 (64 ms), because every 0x17 it trims is a counter gap
 * the pad's small cushion has to absorb. */
static const ctm_pump_policy_t ds4_policy = {
    .input_idle_timeout_ms = 2000,
    .hid_eagain_wait_ms = 3,
    .dedup_report_id = 0x11,
    .acl_fifo_depth = 6,
    .paced_keep = 6,
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
    .set_settings = NULL,   /* live values read via get_settings in patch_output;
                             * the proactive send is build_settings_report, since
                             * set_settings runs on the LVGL thread */
    .build_settings_report = ds4_build_settings_report,
    .build_quiesce_reports = ds4_build_quiesce_reports,
    .on_input_report = ds4_on_input_report,
    .neutralize_input = ds4_neutralize_input,
    .quit_chord = ds4_quit_chord,
};
