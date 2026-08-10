#ifndef CTM_DS5_AUDIO_H
#define CTM_DS5_AUDIO_H

/* DS5 BT audio loss concealment, extracted from the controller pump.
 *
 * Two independent mechanisms share this state:
 *
 *   splice conceal ("PLC")  — a 0x36 that ARRIVED but carries no audio
 *                             sub-block gets the last real block spliced in.
 *   timer fill ("fill")     — a 0x36/0x39 the air dropped ENTIRELY never
 *                             arrives, so an overdue slot is bridged by
 *                             re-injecting the last real frame.
 *
 * Ownership: every function here is called from the controller's session
 * thread (the pump) only, so the state needs no locking of its own. The one
 * thing it cannot do itself is write to the pad — it has no fd, no mutex and
 * no injector; the pump supplies a write callback at init.
 *
 * The pump owns a ds5_audio_t by value and passes &it; nothing else may hold
 * a pointer to it across a session. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write one prepared output report to the pad. Returns 1 if the report reached
 * the device (or its injector), 0 if it was dropped. Supplied by the pump. */
typedef int (*ds5_audio_write_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    /* --- configuration: seeded from the type's pump policy at session start - */
    int plc_enabled;
    /* Timer-driven PLC: conceal 0x36 audio frames LOST in transit (unreliable
     * ENet over WiFi) — the arrived-report PLC above only covers 0x36 that
     * arrived without an audio block, never a frame the air dropped entirely.
     * When a real audio frame is overdue we re-inject the last one so the DS5's
     * rate-matched speaker buffer does not drain. Off = classic behaviour. */
    int plc_fill_enabled;
    uint32_t plc_fill_interval_us;/* target inter-frame period (~DS5 100 Hz) */

    /* --- splice conceal state ------------------------------------------- */
    int plc_have;
    uint16_t plc_audio_len;
    uint8_t plc_audio[260];
    /* Consecutive audio-less 0x36 reports patched with the cached block, capped
     * at 12. Splice-conceal ONLY: it must not touch synth_debt below, or an
     * audio-less 0x36 makes the next genuinely fresh audio frame look like a
     * late duplicate and drain_paced discards it. */
    int plc_repeat;

    /* --- timer fill state ------------------------------------------------ */
    int plc_have36;               /* plc_last36 holds a valid cached report */
    uint16_t plc_last36_len;
    /* Whole-frame cache. 260 was too small for a full 0x36 (config blocks +
     * >=100-byte audio block(s) + CRC easily exceed it), which silently kept
     * plc_have36 at 0 and made the fill a no-op; 1024 covers any BT 0x36. */
    uint8_t plc_last36[1024];     /* last real 0x36-with-audio report, verbatim */
    uint64_t plc_last_real_us;    /* arrival time of the last real audio frame */
    uint64_t plc_fill_next_us;    /* when the next audio frame is due (0 = idle) */
    uint8_t tx_audio_seq;         /* client-authoritative 4-bit BT seq for 0x36/0x39:
                                   * real frames and fill synths draw ONE monotonic
                                   * counter, so a fill can never overtake a late real
                                   * frame (the 2026-07-08 fill regression class). */
    uint8_t plc_real_ctr;         /* host audio packet counter of the newest REAL
                                   * audio frame (0x36 byte 10 / 0x39 byte 9) — the
                                   * anchor for the synth-covered stale window */
    /* Slots the fill has bridged with a synth and the host has not yet caught
     * up on: the WIDTH of the stale window anchored at plc_real_ctr, and the
     * outage budget (12 x 10.67ms of 0x36, or 6 x 21.33ms of 0x39). Written by
     * ds5_audio_inject_synth (+1 per synth), by plc_fill_arm_real (-1 for a
     * late real inside the window, 0 for a fresh one) and by
     * ds5_audio_note_dropped (-1 for an in-window frame trimmed unwritten). */
    int synth_debt;

    /* --- 60 s telemetry window (read + zeroed by the pump's PLC log) ------ */
    unsigned long st_audio_omit;
    unsigned long st_audio_conceal;
    unsigned long st_audio_capdrop;
    unsigned long st_audio_fill;  /* synthesized (transport-loss) conceal frames */
    unsigned long st_fill_skip;   /* real 0x36 too big for plc_last36 (not cached) */
    unsigned long st_fill_dupdrop;/* late reals dropped: their slot was synth-bridged */
    unsigned long st_stale_drop;  /* queued audio dropped by the post-outage trim */

    ds5_audio_write_fn write;
    void *write_ctx;
} ds5_audio_t;

/* Bind the write callback. When: ctm_controller_create, before any session. */
void ds5_audio_init(ds5_audio_t *a, ds5_audio_write_fn write, void *ctx);

/* Drop everything a previous session left behind: both caches, both counters,
 * the fill clock and the telemetry window. Keeps the configuration (enables,
 * interval, write binding) and tx_audio_seq — see the body for why the seq
 * must NOT restart. When: the top of every session, before the handshake. */
void ds5_audio_reset(ds5_audio_t *a);

/* Effective fill slot period: the batched 0x39 carries TWO 10.67ms frames per
 * report, so slots come at twice the configured (0x36-scale) interval. Valid
 * only while plc_have36 is armed (reads the cached report id). */
static inline uint32_t ds5_audio_fill_eff_us(const ds5_audio_t *a)
{
    return (a->plc_have36 && a->plc_last36[0] == 0x39)
        ? a->plc_fill_interval_us * 2u : a->plc_fill_interval_us;
}

/* Run both mechanisms over one outbound report, in place. Returns 1 if the
 * report must be DROPPED (a late real for a slot the fill already bridged),
 * 0 to write it. When: every output write, from the pump. */
int ds5_audio_plc(ds5_audio_t *a, uint8_t *data, size_t len);

/* Account for a queued audio report dropped WITHOUT being written (post-outage
 * stale trim). When: the pump trims its paced queue. */
void ds5_audio_note_dropped(ds5_audio_t *a, const uint8_t *data, size_t len);

/* Stamp the client-authoritative BT seq nibble on a real audio frame and
 * re-sign it. No-op unless the fill is enabled (see the body). When: the pump,
 * after ds5_audio_plc cleared the report for writing. */
void ds5_audio_stamp_tx_seq(ds5_audio_t *a, uint8_t *data, size_t len);

/* Re-inject the last real audio frame to fill a transport gap. Returns 1 if a
 * frame was written through the pump's callback. When: the pump loop, once per
 * overdue audio slot. */
int ds5_audio_inject_synth(ds5_audio_t *a);

#ifdef __cplusplus
}
#endif

#endif /* CTM_DS5_AUDIO_H */
