/* DS5 BT audio loss concealment — see ctm_ds5_audio.h for the two mechanisms
 * and the ownership rules. Moved out of controller_common.c unchanged. */

#define _GNU_SOURCE

#include "ctm_ds5_audio.h"

#include "ctm_controller.h"   /* ctm_bt_sign_output */

#include <string.h>
#include <time.h>

/* Monotonic clock in microseconds. Local copy: this module is deliberately
 * free of the pump's internals, and the pump's own now_us() is static there. */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void ds5_audio_init(ds5_audio_t *a, ds5_audio_write_fn write, void *ctx)
{
    if (!a) return;
    a->write = write;
    a->write_ctx = ctx;
}

/* Fill bookkeeping for a REAL audio-carrying frame about to be written; also
 * the LATE-vs-LOST discriminator. Network stalls here are mostly jitter: the
 * "missing" frames arrive late, AFTER the fill already bridged their slots.
 * Writing them too would double-feed the pad's rate-matched speaker buffer
 * (~5 stalls/min measured -> unbounded buffer creep). The host audio packet
 * counter (0x36 byte 10 stride 1, 0x39 byte 9 stride 2) tells the cases
 * apart: a late real for a bridged slot carries a counter inside the
 * synth-covered window [real_ctr+step .. real_ctr+step*repeat] -> DROP it
 * (returns 1) and shrink the debt; a fresh post-loss frame jumped past the
 * window -> forward it and clear the debt. Either way the frame is the newest
 * real content: it re-arms the cache and the fill clock. */
static int plc_fill_arm_real(ds5_audio_t *a, const uint8_t *data, size_t len,
                             uint8_t ctr, uint8_t step)
{
    int stale = 0;
    if (a->plc_have36 && a->plc_repeat > 0) {
        uint8_t d = (uint8_t)(ctr - a->plc_real_ctr);
        if (d != 0 && d <= (uint8_t)(step * a->plc_repeat)) {
            stale = 1;
            a->plc_repeat--;
            a->st_fill_dupdrop++;
        }
    }
    if (!stale) a->plc_repeat = 0;
    if (len <= sizeof(a->plc_last36)) {
        memcpy(a->plc_last36, data, len);
        a->plc_last36_len = (uint16_t)len;
        a->plc_have36 = 1;
        a->plc_real_ctr = ctr;
        uint64_t now = now_us();
        a->plc_last_real_us = now;
        a->plc_fill_next_us = now + ds5_audio_fill_eff_us(a);
    } else {
        /* Frame bigger than the cache: fill silently degrades to off.
         * Counted so the 60s PLC line makes that visible. */
        a->st_fill_skip++;
    }
    return stale;
}

/* Account for a queued audio report dropped WITHOUT being written (post-outage
 * stale trim): advance the fill's counter anchor and shrink the synth debt so
 * plc_fill_arm_real's stale window stays aligned with what actually reached
 * the pad — otherwise a trimmed in-window frame would leave the debt too high
 * and the NEXT (fresh) frame would be mistaken for a bridged slot. */
void ds5_audio_note_dropped(ds5_audio_t *a, const uint8_t *data, size_t len)
{
    if (!a->plc_fill_enabled || !data || len < 12) return;
    uint8_t ctr;
    if (data[0] == 0x39 && len == 547) ctr = data[9];
    else if (data[0] == 0x36) ctr = data[10];
    else return;
    if (a->plc_repeat > 0) a->plc_repeat--;
    a->plc_real_ctr = ctr;
    a->st_stale_drop++;
}

/* Returns 1 if the report must be DROPPED (a late real for a slot the fill
 * already bridged — see plc_fill_arm_real), 0 to write it. */
int ds5_audio_plc(ds5_audio_t *a, uint8_t *data, size_t len)
{
    /* Batched 0x39: the in-place splice conceal below stays 0x36-only (a sent
     * 0x39 always carries two host-encoded Opus blocks, so the audio-less case
     * it patches does not exist), but the timer-driven FILL is armed here —
     * cache the whole report for re-injection on a transport gap, at the
     * doubled slot period (ds5_audio_fill_eff_us) and halved repeat cap. Fixed
     * offsets instead of the sub-block walk: the 0x39 length byte deliberately
     * names ONE block while two follow (bit 6 in the id), so the walk cannot
     * parse it — layout is [0]=0x39 [1]=seq [2]=0x91 ... [140]=0xD3 (Opus). */
    if (data && len >= 1 && data[0] == 0x39) {
        /* Splice cache stays disarmed either way: left armed, it would patch a
         * minute-old Opus block into the first audio-less 0x36 after a switch
         * back to the unbatched form. A return to 0x36 re-arms cleanly. */
        a->plc_have = 0;
        a->plc_audio_len = 0;
        if (!a->plc_enabled || !a->plc_fill_enabled) {
            a->plc_have36 = 0;
            a->plc_fill_next_us = 0;
            a->plc_repeat = 0;
            return 0;
        }
        if (len == 547 && data[140] == 0xD3) {
            return plc_fill_arm_real(a, data, len, data[9], 2);
        }
        /* Unexpected 0x39 shape: don't cache (a malformed synth would
         * repeat), counted so the 60s PLC line makes that visible. */
        a->st_fill_skip++;
        return 0;
    }
    if (!a->plc_enabled || !data || len < 12 || data[0] != 0x36) {
        return 0;
    }

    size_t limit = len - 4;
    size_t pos = 2;
    int audio_present = 0;
    size_t blocks_end = 2;
    while (pos + 2 <= limit) {
        uint8_t id = data[pos];
        size_t plen = data[pos + 1];
        size_t blen = plen + 2;
        if (id == 0 && plen == 0) {
            break;
        }
        if (blen > limit - pos) {
            return 0;
        }
        if (id >= 0x93 && id <= 0x96 && plen >= 100) {
            audio_present = 1;
            if (blen <= sizeof(a->plc_audio)) {
                memcpy(a->plc_audio, &data[pos], blen);
                a->plc_audio_len = (uint16_t)blen;
                a->plc_have = 1;
            }
        }
        pos += blen;
        blocks_end = pos;
    }
    if (audio_present) {
        /* Cache the whole frame for timer-driven loss concealment and rearm the
         * "next frame due" clock (plc_fill_arm_real; runs only for real
         * arrivals — synth frames bypass this path, so the cache never feeds
         * on itself). Counter at byte 10, stride 1 per report. */
        if (a->plc_fill_enabled) {
            return plc_fill_arm_real(a, data, len, data[10], 1);
        }
        a->plc_repeat = 0;
        return 0;
    }
    a->st_audio_omit++;
    if (!a->plc_have || a->plc_repeat >= 12) {
        a->st_audio_capdrop++;
        return 0;
    }

    size_t al = a->plc_audio_len;
    if (blocks_end + al > limit) {
        a->st_audio_capdrop++;
        return 0;
    }
    /* Append concealed audio after the last known block, not before haptics. */
    memcpy(&data[blocks_end], a->plc_audio, al);
    if (blocks_end + al + 2 <= limit) {
        data[blocks_end + al]     = 0x00;
        data[blocks_end + al + 1] = 0x00;
    }
    ctm_bt_sign_output(data, len);
    a->plc_repeat++;
    a->st_audio_conceal++;
    return 0;
}

void ds5_audio_stamp_tx_seq(ds5_audio_t *a, uint8_t *data, size_t len)
{
    if (!a->plc_fill_enabled || len < 12 || (data[0] != 0x36 && data[0] != 0x39)) {
        return;
    }
    /* Client-authoritative BT seq (byte 1 high nibble): real audio frames
     * and fill synths (ds5_audio_inject_synth) draw one monotonic 4-bit
     * counter, so a synth can never overtake a late-arriving real frame — the
     * 07-08 fill regression class. Only while fill is enabled: with the kill
     * switch set the host's own seq passes through untouched. Re-sign
     * unconditionally — the settings patch only re-CRCs when it changed a
     * byte, and the stamp here always does. */
    a->tx_audio_seq = (uint8_t)((a->tx_audio_seq + 1u) & 0x0fu);
    data[1] = (uint8_t)((a->tx_audio_seq << 4) | (data[1] & 0x0fu));
    ctm_bt_sign_output(data, len);
}

/* Re-inject the last real audio frame (0x36 or batched 0x39) to fill a
 * transport gap, keeping the DS5's rate-matched speaker buffer topped up.
 * Stamps the shared client BT seq + a fresh audio packet counter, re-signs,
 * and rides the same output path as real reports. Capped by plc_repeat
 * (~128ms of bridged outage either form) so a sustained outage stops
 * repeating stale audio.
 * When: the pump loop, once per overdue audio slot. Returns 1 if injected. */
int ds5_audio_inject_synth(ds5_audio_t *a)
{
    if (!a->plc_fill_enabled || !a->plc_have36 || a->plc_last36_len < 12) return 0;
    /* Same ~128ms outage budget for both forms: 12 x 10.67ms or 6 x 21.33ms. */
    if (a->plc_repeat >= (a->plc_last36[0] == 0x39 ? 6 : 12)) return 0;
    size_t n = a->plc_last36_len;
    uint8_t rep[sizeof(a->plc_last36)];
    if (n > sizeof(rep)) return 0;
    memcpy(rep, a->plc_last36, n);
    /* Sequence nibble (byte 1 high): draw from the SAME client-authoritative
     * counter ds5_audio_stamp_tx_seq stamps real frames with. The old scheme
     * (continue +1 from the last real frame's HOST seq) assumed lost frames —
     * under jitter the "lost" frames arrive LATE, still carrying the host seq
     * the synth already used, and the pad rejects them as stale (the 07-08
     * fill regression: valid CRC, silent speaker). One shared monotonic
     * counter makes that collision impossible by construction. Preserve the
     * low nibble verbatim (part of the host-signed frame, not ours to zero). */
    a->tx_audio_seq = (uint8_t)((a->tx_audio_seq + 1u) & 0x0fu);
    rep[1] = (uint8_t)((a->tx_audio_seq << 4) | (rep[1] & 0x0fu));
    /* Audio packet counter: advance past the last REAL frame by the per-report
     * stride (0x36 byte 10 stride 1, 0x39 byte 9 stride 2) so the pad
     * sequences the synth as fresh audio instead of a duplicate. A late real
     * for a bridged slot then lands INSIDE this window and is dropped by
     * plc_fill_arm_real, so the pad's buffer never double-feeds. */
    if (rep[0] == 0x39) {
        if (n > 9) rep[9] = (uint8_t)(a->plc_real_ctr + 2u * (uint8_t)(a->plc_repeat + 1));
    } else {
        if (n > 10) rep[10] = (uint8_t)(a->plc_real_ctr + (uint8_t)(a->plc_repeat + 1));
    }
    ctm_bt_sign_output(rep, n);

    int sent = a->write ? a->write(a->write_ctx, rep, n) : 0;
    if (sent) {
        a->plc_repeat++;
        a->st_audio_fill++;
    }
    return sent;
}
