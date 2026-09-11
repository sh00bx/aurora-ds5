/* ds5_synth_audio.c — a host-free, game-free continuous pad-audio source.
 *
 * WHY THIS EXISTS
 * ---------------
 * Every buffer-floor and lever measurement in the DS5 latency programme is only
 * valid under CONTINUOUS pad audio: with sporadic audio the gap ledger measures
 * silence, which is the mistake that cost this project three wrong conclusions
 * (see "a presence filter is not an activity filter"). Continuous audio has so
 * far meant a human holding R1 in Ratchet & Clank with the Win11 host streaming
 * — i.e. every A/B block costs a person half an hour of gameplay, and blocks
 * cannot be interleaved over hours or repeated overnight.
 *
 * This tool manufactures the same workload locally on the TV: it Opus-encodes a
 * test tone and hands ds5_txd production-identical batched 0x39 audio reports at
 * the production cadence, so the transport sees the same offered load with no
 * host, no stream and no human. It is an INSTRUMENT, not a lever: it changes
 * nothing about how the daemon transmits.
 *
 * WHAT IT IS NOT
 * --------------
 * It cannot reproduce the video stream's wifi load or the decoder's CPU load —
 * both share the MT7921 combo chip and the CPU with the bluetooth link. Any
 * number it produces is comparable to gameplay numbers ONLY after the
 * equivalence check in the preregistration passes (reproduce the known gapge
 * distribution at B=40/60 within the two-session spread).
 *
 * Build (webOS SDK compiler, NOT the musl toolchain — we dlopen the app's glibc
 * libopus at runtime):
 *   $SDK/bin/arm-webos-linux-gnueabi-gcc -O2 -Wall -Wextra ds5_synth_audio.c \
 *       -o ds5_synth_audio -ldl -lm -lpthread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

/* ---- report geometry (host reference: Vibepollo ds5_haptics.cpp) ----------- */
#define R39_LEN        547
#define R32_LEN        142
#define R36_LEN        398
#define R35_LEN        334          /* one size-ladder step below 0x36, see g_r35 */
#define OPUS_BYTES     200
#define OPUS_FRAME     480          /* samples per channel, 10 ms @ 48 kHz */
#define HAPTIC_BYTES    64

/* 0x39 offsets */
#define OFF39_HAPTIC_A  12
#define OFF39_HAPTIC_B  76
#define OFF39_OPUS_A   142
#define OFF39_OPUS_B   342

/* 0x36 offsets (single-frame form; canonical: ds5_haptics.cpp / ds5_av_play.c).
 * Unlike the 0x39 it has room for the 63-byte SetState INLINE and carries the
 * LONG 0x91 timing form: [0xFE][latency ms x5] with the audio counter at
 * buf[10] (the 0x39 packs a 6-byte short form and counts at buf[9]). */
#define OFF36_SETSTATE  13
#define OFF36_HAPTIC    78
#define OFF36_OPUS     144

/* The pad drains audio on its own ~45 kHz device clock, so one 480-sample frame
 * is 10.667 ms of playback, and a two-frame 0x39 is 21.333 ms — NOT 20 ms.
 *
 * But 21334 is the pad's bare DRAIN period, and production never emits at it:
 * the host's pacer always adds an adjustment, and with no feedback it falls back
 * to +35 us per frame. Emitting at the drain period exactly leaves zero margin,
 * which is the documented ratchet condition — a transient stall is then never
 * repaid and the credit window and pad buffer only ever fill. So the default is
 * production's no-feedback cadence, 2 x (10667 + 35). */
#define REPORT_US    21404
/* Runtime-adjustable copy. The cadence is normally fixed — an instrument that
 * changes shape between arms measures itself — but the 2026-08-16 session showed
 * the real app feeding 41.8/s against this program's 46.9/s and holding one
 * packet in flight where this holds four. Reproducing THAT is the experiment, so
 * the period becomes a declared lever, read from a file and logged when it moves. */
static uint32_t g_period_us = REPORT_US;
static const char *g_period_file = "/tmp/ds5_period_us";
/* Burst pattern. The 18-minute Ratchet recording shows the real client is not a
 * slower metronome at all: its instantaneous rate is 48.4/s median, 53.6 p90,
 * 56.3 max — ABOVE the pad's 46.87/s drain — interleaved with quiet stretches
 * (p10 = 0.2/s), averaging 41.8/s. Uniformly slowing the cadence, which is what
 * the `feed` lever did, reproduces the average and nothing else. This does the
 * pattern: burst above drain, then stop. "on_ms,off_ms" in /tmp/ds5_burst. */
static uint32_t g_burst_on_ms = 0, g_burst_off_ms = 0;
/* Co-traffic. A real session does not carry audio alone: the client also sends
 * rumble and trigger state on the SAME link, and the daemon has a type-aware
 * credit split precisely because the two compete. The recording bears it out —
 * instantaneous injection peaks at 56.3/s where audio alone needs 46.9. Those
 * extra packets consume the same credits the audio stream is waiting on, which
 * is the last untested way gameplay differs from this rig. Reports per second in
 * /tmp/ds5_cotraffic; 0 disables, and a rate the current tick rate cannot carry
 * is refused rather than approximated. */
static uint32_t g_cotraffic_hz = 0;
/* Report-format lever (/tmp/ds5_r36 exists = unbatched). The floor arithmetic
 * is: underrun <=> gap > B + one report period in flight. Batched 0x39 puts
 * 21.33 ms in that term, single-frame 0x36 halves it to 10.67 — so even an
 * UNCHANGED gap distribution is worth ~10 ms of pad buffer. The 2026-08-15
 * "batching is load-bearing" verdict predates the out>=2 filter, the audio
 * gate, interleaving, the kernel clock and the H-A finding (long packets
 * retransmit; 398 B rides shorter baseband packets than 547 B) — this lever
 * re-runs that question on the modern instrument. The daemon needs no change:
 * it reads exactly one byte of every report, the id. */
static int g_r36 = 0;
/* Short-packet lever (/tmp/ds5_r35 exists; wins over /tmp/ds5_r36). Port plan
 * 2026-09-11 W2-02, a HYPOTHESIS: a shorter baseband packet is the same lever as
 * the L18 packet-type clamp (halved 60-65 ms gaps, replicated three times), but
 * without HCI. Bitrate alone changes nothing on air — 0x36 is fixed at 398 B by
 * the descriptor and a smaller Opus frame is just zero-padded — so the frame has
 * to shrink AND move one rung down the 0x31..0x39 size ladder. At 96 kbit/s CBR
 * a 10 ms frame is 120 B, which fits 0x35 (334 B) with the same skeleton as the
 * 0x36 (long 0x91 form, SetState inline, one coil block). Whether the pad
 * accepts a 0x13 block inside a 0x35 at all is unknown, exactly as it was for
 * 0x39 once: listen first (--amp audible, no --mute), measure second. Same
 * single-frame cadence as the 0x36, so the A/B against /tmp/ds5_r36 changes only
 * packet length and codec bitrate. */
static int g_r35 = 0;
static int g_r35_opus_bytes = 120;  /* --bitrate / 800 */
static const char *fmt_name(void) { return g_r35 ? "0x35" : (g_r36 ? "0x36" : "0x39"); }
/* Ticks per second the audio loop actually runs at right now. Both inputs are
 * declared levers that move under a running rig — the 0x36 format puts one frame
 * per report and so halves the period, the feed lever changes the period itself
 * — which is why anything that has to agree with the tick rate reads it here
 * instead of assuming the default. */
/* One-sided rate servo, moved out of the loop so tick_rate_hz can read it. */
static int g_adj_us = 0;

static double tick_rate_hz(void) {
    /* g_adj_us belongs in here: the serviced period is g_period_us + g_adj_us, and at
     * the servo's clamp that is enough to make the top co-traffic rung deliverable
     * in one arm and not in the other -- exactly the arm asymmetry this function
     * exists to rule out. */
    long p = (long) g_period_us + (long) g_adj_us;
    if (p < 1) {
        p = 1;
    }
    return 1e6 * ((g_r36 || g_r35) ? 2.0 : 1.0) / (double) p;
}

static void cotraffic_poll(void) {
    FILE *f = fopen("/tmp/ds5_cotraffic", "r");
    uint32_t v = 0;
    if (f) { if (fscanf(f, "%u", &v) != 1) v = 0; fclose(f); }
    /* Out-of-range used to fold silently into 0, i.e. an arm that declared a
     * co-traffic load and carried none. Name the refused value once per value:
     * this poll runs five times a second. */
    if (v > 60) {
        static uint32_t refused;
        if (v != refused) {
            refused = v;
            printf("[synth] REFUSING co-traffic %u reports/s: this rig caps what it will put "
                   "on the link at 60/s — co-traffic stays off\n", v);
            fflush(stdout);
        }
        v = 0;
    }
    if (v != g_cotraffic_hz) {
        printf("[synth] co-traffic %u -> %u reports/s (0x32 alongside the audio)\n",
               g_cotraffic_hz, v);
        fflush(stdout);
        g_cotraffic_hz = v;
    }
}
static void r36_poll(void) {
    int v = (access("/tmp/ds5_r36", F_OK) == 0);
    if (v != g_r36) {
        g_r36 = v;
        printf("[synth] report format -> %s%s\n", fmt_name(),
               g_r35 ? " (/tmp/ds5_r35 still wins)" : (v ? " single-frame (10.67 ms cadence)" : " batched (21.33 ms cadence)"));
        fflush(stdout);
    }
}

static void r35_poll(void) {
    int v = (access("/tmp/ds5_r35", F_OK) == 0);
    if (v != g_r35) {
        printf("[synth] report format -> %s\n",
               v ? "0x35 single-frame short packet" : (g_r36 ? "0x36 single-frame (10.67 ms cadence)"
                                                       : "0x39 batched (21.33 ms cadence)"));
        fflush(stdout);
        g_r35 = v;
    }
}

static void burst_poll(void) {
    FILE *f = fopen("/tmp/ds5_burst", "r");
    uint32_t on = 0, off = 0;
    if (f) { if (fscanf(f, "%u,%u", &on, &off) != 2) { on = off = 0; } fclose(f); }
    if (on > 20000 || off > 20000) on = off = 0;
    if (on != g_burst_on_ms || off != g_burst_off_ms) {
        printf("[synth] burst %u/%u -> %u/%u ms\n", g_burst_on_ms, g_burst_off_ms, on, off);
        fflush(stdout);
        g_burst_on_ms = on; g_burst_off_ms = off;
    }
}
/* In the off phase we send NOTHING — no encode, no datagram — exactly as the app
 * does when the game falls silent. The daemon then stops binning gaps after
 * AUDIO_IDLE_MS, which is not a flaw here: gameplay's tail is measured under the
 * same suppression, so the comparison stays like for like. */
static int burst_silent(uint64_t t_us) {
    if (!g_burst_on_ms || !g_burst_off_ms) return 0;
    uint64_t cycle = (uint64_t)(g_burst_on_ms + g_burst_off_ms) * 1000ull;
    return (t_us % cycle) >= (uint64_t) g_burst_on_ms * 1000ull;
}
static void period_poll(void) {
    FILE *f = fopen(g_period_file, "r");
    uint32_t v = REPORT_US;
    if (f) { if (fscanf(f, "%u", &v) != 1) v = REPORT_US; fclose(f); }
    if (v < 15000 || v > 40000) v = REPORT_US;      /* refuse nonsense outright */
    if (v != g_period_us) {
        printf("[synth] period %u -> %u us (%.1f/s)\n", g_period_us, v, 1e6 / v);
        fflush(stdout);
        g_period_us = v;
    }
}
#define SETSTATE_MS   1000          /* periodic standalone SetState re-assert */

/* DS5 BT output CRC: CRC32 over the 0xA2 HID-output seed byte + report[0..len-4). */
#define PS_OUTPUT_CRC_SEED 0xA2

/* Audio-only SetState payload (63 B). Asserts ONLY the audio Allow bits so it can
 * never fight a game's rumble/trigger/LED writes on the same pad. */
static const uint8_t state_audio_data[63] = {
    0xB0, 0x82,             /* ValidFlags: audio only                         */
    0x00, 0x00,             /* RumbleEmulation R/L (not allowed)              */
    0x5f,                   /* VolumeHeadphones ) what PRODUCTION puts on air:  */
    0x5f,                   /* VolumeSpeaker    ) the host writes 0x7f/0xff, but
                             * the TV app rewrites both bytes from its own volume
                             * setting (default 95 % -> 0x5f) on every outbound
                             * report, and the app is the last writer before the
                             * daemon. Reproducing the host's 0xff would recreate
                             * a stream that never actually existed on this link.
                             * (Note the 0x80..0xff speaker range: 0x5f sits below
                             * its floor, which is exactly what production does.) */
    0x00,                   /* VolumeMic                                      */
    0x00,                   /* AudioControl (auto mic, speaker output path)   */
    0x00,                   /* MuteLightMode                                  */
    0x00,                   /* MuteControl: everything unmuted                */
    0,0,0,0,0,0,0,0,0,0,0,  /* RightTriggerFFB                                */
    0,0,0,0,0,0,0,0,0,0,0,  /* LeftTriggerFFB                                 */
    0,0,0,0,                /* HostTimestamp                                  */
    0x00,                   /* MotorPowerLevel                                */
    0x02,                   /* AudioControl2: SpeakerCompPreGain = 2          */
    0,0,0,0,0,0,            /* light / haptic-LPF / animation (not allowed)   */
    0,0,0                   /* LedRed/Green/Blue (not allowed)                */
};

/* ---- CRC32 (reflected, poly 0xEDB88320) ----------------------------------- */
static uint32_t crc_tab[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
}
static uint32_t crc_upd(uint32_t crc, const uint8_t *d, size_t n) {
    while (n--) crc = crc_tab[(crc ^ *d++) & 0xff] ^ (crc >> 8);
    return crc;
}
static void sign_report(uint8_t *out, int len) {
    const uint8_t seed = PS_OUTPUT_CRC_SEED;
    uint32_t crc = crc_upd(0xFFFFFFFFu, &seed, 1);
    crc = ~crc_upd(crc, out, (size_t) len - 4);
    out[len - 4] = (uint8_t) (crc & 0xff);
    out[len - 3] = (uint8_t) ((crc >> 8) & 0xff);
    out[len - 2] = (uint8_t) ((crc >> 16) & 0xff);
    out[len - 1] = (uint8_t) ((crc >> 24) & 0xff);
}

/* ---- Opus, dlopen'd from the app's own bundle ------------------------------ *
 * The webOS SDK sysroot ships no libopus and the musl cross-toolchain cannot
 * link a glibc .so, so the encoder is resolved at runtime out of the Aurora
 * bundle (verified present and encoder-complete on the device). Encoding live
 * with ONE persistent encoder — rather than looping a pre-encoded frame table —
 * is deliberate: Opus is stateful, and a replayed frame is the documented cause
 * of the "choppy standalone test" artefact. Cost measured on this TV: 411 us per
 * 10 ms frame at complexity 10 = 4.1 % of one core. */
typedef struct OpusEncoder OpusEncoder;
static OpusEncoder *(*p_opus_create)(int32_t, int, int, int *);
static int (*p_opus_ctl)(OpusEncoder *, int, ...);
static int32_t (*p_opus_encode_f)(OpusEncoder *, const float *, int, unsigned char *, int32_t);

#define OPUS_APPLICATION_RESTRICTED_LOWDELAY 2051
#define OPUS_SET_BITRATE_REQUEST             4002
#define OPUS_SET_VBR_REQUEST                 4006
#define OPUS_SET_COMPLEXITY_REQUEST          4010
#define OPUS_SET_EXPERT_FRAME_DURATION_REQ   4040
#define OPUS_FRAMESIZE_10_MS                 5003

static const char *OPUS_CANDIDATES[] = {
    "/media/developer/apps/usr/palm/applications/com.aurora.ds5/lib/libopus.so.0",
    "/media/developer/apps/usr/palm/applications/com.aurora.gamestream/lib/libopus.so.0",
    "libopus.so.0",
    NULL
};

static OpusEncoder *opus_setup(int complexity, int bitrate, const char **which) {
    void *h = NULL;
    for (int i = 0; OPUS_CANDIDATES[i]; i++) {
        h = dlopen(OPUS_CANDIDATES[i], RTLD_NOW);
        if (h) { *which = OPUS_CANDIDATES[i]; break; }
    }
    if (!h) { fprintf(stderr, "[synth] no libopus: %s\n", dlerror()); return NULL; }
    p_opus_create    = (OpusEncoder *(*)(int32_t, int, int, int *)) dlsym(h, "opus_encoder_create");
    p_opus_ctl       = (int (*)(OpusEncoder *, int, ...))           dlsym(h, "opus_encoder_ctl");
    p_opus_encode_f  = (int32_t (*)(OpusEncoder *, const float *, int, unsigned char *, int32_t))
                       dlsym(h, "opus_encode_float");
    if (!p_opus_create || !p_opus_ctl || !p_opus_encode_f) {
        fprintf(stderr, "[synth] libopus lacks the encoder API\n"); return NULL;
    }
    int err = 0;
    OpusEncoder *e = p_opus_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (!e || err) { fprintf(stderr, "[synth] opus_encoder_create failed %d\n", err); return NULL; }
    /* Production settings, byte for byte: 10 ms frames, 160 kbit CBR -> the 200 B
     * the 0x13/0xD3 sub-block declares. VBR would break the fixed geometry. */
    p_opus_ctl(e, OPUS_SET_EXPERT_FRAME_DURATION_REQ, OPUS_FRAMESIZE_10_MS);
    p_opus_ctl(e, OPUS_SET_BITRATE_REQUEST, bitrate);
    p_opus_ctl(e, OPUS_SET_VBR_REQUEST, 0);
    p_opus_ctl(e, OPUS_SET_COMPLEXITY_REQUEST, complexity);
    return e;
}

/* ---- tone source ----------------------------------------------------------- */
struct tone {
    double phase, step, amp;
};
static void tone_init(struct tone *t, double hz, double dbfs) {
    t->phase = 0.0;
    t->step  = 2.0 * M_PI * hz / 48000.0;
    t->amp   = pow(10.0, dbfs / 20.0);
}
static void tone_fill(struct tone *t, float *pcm /* OPUS_FRAME*2 */) {
    for (int i = 0; i < OPUS_FRAME; i++) {
        float s = (float) (t->amp * sin(t->phase));
        t->phase += t->step;
        if (t->phase > 2.0 * M_PI) t->phase -= 2.0 * M_PI;
        pcm[i * 2] = s;
        pcm[i * 2 + 1] = s;
    }
}

/* ---- report builders ------------------------------------------------------- */
struct builder {
    uint8_t  r39[R39_LEN];
    uint8_t  r36[R36_LEN];
    uint8_t  r35[R35_LEN];
    uint8_t  r32[R32_LEN];
    uint8_t  seq;          /* 4-bit sequence nibble, shared across report ids */
    uint8_t  pktctr;       /* audio counter, +1 per FRAME (so +2 per 0x39,
                              +1 per 0x36 — continuous across a format switch) */
    int      b_ms;         /* pad audio buffer depth we advertise              */
};

static void builder_init(struct builder *B, int b_ms) {
    memset(B, 0, sizeof *B);
    B->b_ms = b_ms;

    uint8_t *b = B->r39;
    b[0]  = 0x39;
    b[2]  = 0x11 | 0x80;          /* 0x91 timing sub-packet, SHORT form         */
    b[3]  = 6;
    b[4]  = 0x7E;
    /* b[5..8] = buffer depth in ms. In production the TV app patches these on
     * every outbound report from its latency slider; with the app absent this
     * program is the only writer, which is why B is an explicit argument and is
     * printed into the stats line — the old "the latency setting is nowhere in
     * any log" instrumentation hole cannot reappear here. */
    b[5] = b[6] = b[7] = b[8] = (uint8_t) b_ms;
    b[10] = 0x12 | 0xC0;          /* two coil blocks follow (bit 6 = doubled)   */
    b[11] = HAPTIC_BYTES;
    b[140] = 0x13 | 0xC0;         /* two Opus frames follow                     */
    b[141] = OPUS_BYTES;
    /* coil payloads stay zero: silent voice coil, so the only thing on air is
     * the speaker stream we are measuring. */

    uint8_t *s = B->r32;
    s[0] = 0x32;
    s[2] = 0x10 | 0x80;
    s[3] = 63;
    memcpy(s + 4, state_audio_data, 63);

    /* Single-frame 0x36 skeleton (long 0x91 form, SetState inline — the
     * production unbatched path needs no periodic 0x32). */
    uint8_t *u = B->r36;
    u[0]  = 0x36;
    u[2]  = 0x11 | 0x80;
    u[3]  = 7;
    u[4]  = 0xFE;
    u[5] = u[6] = u[7] = u[8] = u[9] = (uint8_t) b_ms;   /* five latency bytes */
    u[11] = 0x10 | 0x80;
    u[12] = 63;
    memcpy(u + OFF36_SETSTATE, state_audio_data, 63);
    u[76] = 0x12 | 0x80;
    u[77] = HAPTIC_BYTES;         /* zeroed coil: silent, same as the 0x39 arm */
    u[142] = 0x13 | 0x80;
    u[143] = OPUS_BYTES;

    /* 0x35: the 0x36 skeleton with a shorter Opus block, one rung down. */
    memcpy(B->r35, B->r36, R35_LEN);
    B->r35[0]   = 0x35;
    B->r35[143] = (uint8_t) g_r35_opus_bytes;
}

/* Fill one 0x39 with two freshly encoded frames. Both frames go through the SAME
 * encoder in order — two calls, never one call reused, because the codec carries
 * state between frames. */
static int build_0x39(struct builder *B, OpusEncoder *enc, struct tone *t) {
    float pcm[OPUS_FRAME * 2];
    for (int half = 0; half < 2; half++) {
        tone_fill(t, pcm);
        uint8_t *dst = B->r39 + (half ? OFF39_OPUS_B : OFF39_OPUS_A);
        int32_t n = p_opus_encode_f(enc, pcm, OPUS_FRAME, dst, OPUS_BYTES);
        if (n != OPUS_BYTES) {
            fprintf(stderr, "[synth] opus frame %d B (want %d)\n", (int) n, OPUS_BYTES);
            return -1;
        }
    }
    B->r39[1] = (uint8_t) ((B->seq++ & 0x0F) << 4);
    B->r39[9] = B->pktctr;
    B->pktctr = (uint8_t) (B->pktctr + 2);
    sign_report(B->r39, R39_LEN);
    return 0;
}

/* One 0x36 = one fresh frame through the SAME stateful encoder. */
static int build_0x36(struct builder *B, OpusEncoder *enc, struct tone *t) {
    float pcm[OPUS_FRAME * 2];
    tone_fill(t, pcm);
    int32_t n = p_opus_encode_f(enc, pcm, OPUS_FRAME, B->r36 + OFF36_OPUS, OPUS_BYTES);
    if (n != OPUS_BYTES) {
        fprintf(stderr, "[synth] opus frame %d B (want %d)\n", (int) n, OPUS_BYTES);
        return -1;
    }
    B->r36[1] = (uint8_t) ((B->seq++ & 0x0F) << 4);
    B->r36[10] = B->pktctr;
    B->pktctr = (uint8_t) (B->pktctr + 1);
    sign_report(B->r36, R36_LEN);
    return 0;
}

/* One 0x35 = one fresh frame through the SAME encoder, re-targeted to the 0x35
 * bitrate by the loop (CBR: the next frame already comes out at that size). */
static int build_0x35(struct builder *B, OpusEncoder *enc, struct tone *t) {
    float pcm[OPUS_FRAME * 2];
    tone_fill(t, pcm);
    int32_t n = p_opus_encode_f(enc, pcm, OPUS_FRAME, B->r35 + OFF36_OPUS, g_r35_opus_bytes);
    if (n != g_r35_opus_bytes) {
        fprintf(stderr, "[synth] opus frame %d B (want %d)\n", (int) n, g_r35_opus_bytes);
        return -1;
    }
    B->r35[1] = (uint8_t) ((B->seq++ & 0x0F) << 4);
    B->r35[10] = B->pktctr;
    B->pktctr = (uint8_t) (B->pktctr + 1);
    sign_report(B->r35, R35_LEN);
    return 0;
}

static void build_0x32(struct builder *B) {
    B->r32[1] = (uint8_t) ((B->seq++ & 0x0F) << 4);
    sign_report(B->r32, R32_LEN);
}

/* ---- pacing ---------------------------------------------------------------- */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

/* ---- device discovery ------------------------------------------------------ */
/* Find the hidraw node of a DualSense, optionally one specific address. Returns
 * the node index, -1 = none, -2 = several (ambiguous: refuse rather than guess,
 * because writing a SetState to the wrong pad is a real side effect). */
static int find_dualsense(const char *want_mac, char *mac_out, size_t mac_sz) {
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) return -1;
    struct dirent *e;
    int found = -1, n = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hidraw", 6) != 0) continue;
        char p[320], line[512];
        snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int is_ds = 0;
        char uniq[512] = "";
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "HID_NAME=DualSense")) is_ds = 1;
            if (strncmp(line, "HID_UNIQ=", 9) == 0) {
                snprintf(uniq, sizeof uniq, "%s", line + 9);
                uniq[strcspn(uniq, "\r\n")] = 0;
            }
        }
        fclose(f);
        if (!is_ds) continue;
        if (want_mac && *want_mac && strcasecmp(uniq, want_mac) != 0) continue;
        n++;
        if (found < 0) {
            found = atoi(e->d_name + 6);
            snprintf(mac_out, mac_sz, "%s", uniq);
        }
    }
    closedir(d);
    if (n > 1) return -2;
    return found;
}

/* HIDIOCGFEATURE(len) — hidraw's feature-report read. Declared here rather than
 * pulled from linux/hidraw.h so the tool builds against a bare SDK sysroot. */
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x07, len)
#endif

/* Read feature report 0x05, exactly as the app does before it starts forwarding.
 * This is not cosmetic: the read is what moves a DualSense out of its minimal BT
 * report mode, and the pad's uplink rate is part of the air budget the downlink
 * competes for. A rig that skips it loads the reverse channel differently from
 * every session the reference numbers came from. Failure is not fatal — the pad
 * may already be in full mode — but it is reported, because an unexplained
 * uplink rate is the kind of difference that later gets blamed on the radio. */
static int ds5_feature_probe(int fd) {
    uint8_t buf[64] = { 0x05 };
    return ioctl(fd, HIDIOCGFEATURE(sizeof buf), buf) < 0 ? -1 : 0;
}

static int mac_to_bytes(const char *mac, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    /* on-wire BD_ADDR order is least significant byte first */
    for (int i = 0; i < 6; i++) out[i] = (uint8_t) v[5 - i];
    return 0;
}

/* ---- daemon IPC ------------------------------------------------------------ *
 * Tagged datagram: [0xA5][kind][bdaddr 6, LSB first][report]. The daemon gates
 * every peer on SO_PEERCRED and admits the app's jail uid AND root, which is the
 * whole reason this program can exist. */
#define ACL_TAG_M0          0xA5
#define ACL_TAG_INJECT      0x5A
#define ACL_TAG_ASSERT      0x5B
#define ACL_TAG_CTRL        0x5C
#define ACL_CTRL_FIFO_DEPTH 0x01
#define ACL_TAG_LEN         8

/* Lightbar 0x31 (78 B) — the bootstrap seed. A bind needs an OUTGOING DS5 HID
 * output observed on air whose bytes match a live identity assertion; with the
 * app gone this is the cheapest such report, and painting the bar is its only
 * side effect. Verified on the device: it binds a pad that was already connected
 * before the daemon started, where no kernel connect event exists to learn from. */
#define DS5_BT_OUT_LEN     78
#define DS5_OUT_COMMON_LEN 47

static void build_lightbar(uint8_t out[DS5_BT_OUT_LEN], uint32_t rgb, uint8_t seq) {
    uint8_t common[DS5_OUT_COMMON_LEN];
    memset(common, 0, sizeof common);
    common[1]  = 0x04;   /* valid_flag1: LIGHTBAR_CONTROL_ENABLE       */
    common[38] = 0x02;   /* valid_flag2: LIGHTBAR_SETUP_CONTROL_ENABLE */
    common[41] = 0x02;   /* lightbar_setup: LIGHT_OUT                  */
    common[44] = (uint8_t) (rgb >> 16);
    common[45] = (uint8_t) (rgb >> 8);
    common[46] = (uint8_t) rgb;
    memset(out, 0, DS5_BT_OUT_LEN);
    out[0] = 0x31; out[1] = (uint8_t) (seq << 4); out[2] = 0x10;
    memcpy(out + 3, common, DS5_OUT_COMMON_LEN);
    sign_report(out, DS5_BT_OUT_LEN);
}

/* The per-address telemetry record the daemon republishes ~5/s. It is the only
 * feedback this program has: the pad itself reports nothing about how full its
 * audio buffer is, so delivery-vs-consumption imbalance is visible ONLY here. */
struct st {
    int      valid, q, fifo, maxq, fifo_cap;
    uint32_t inj, drop, seq;
    int      g50, g80, flush, nocp_age, drop_age, drop_ovf;
    int      v2;                 /* the gap-ledger half was actually published */
};
static int st_read(const char *path, struct st *o) {
    uint8_t r[36];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, r, sizeof r);
    close(fd);
    if (n < 24 || r[0] != 'D' || r[1] != 'S' || r[2] != '5' || r[3] != 'Q') return -1;
    o->valid = r[5]; o->q = r[6]; o->fifo = r[7];
    o->maxq = r[8] | (r[9] << 8); o->fifo_cap = r[10] | (r[11] << 8);
    o->inj  = (uint32_t) r[12] | ((uint32_t) r[13] << 8) | ((uint32_t) r[14] << 16) | ((uint32_t) r[15] << 24);
    o->drop = (uint32_t) r[16] | ((uint32_t) r[17] << 8) | ((uint32_t) r[18] << 16) | ((uint32_t) r[19] << 24);
    o->seq  = (uint32_t) r[20] | ((uint32_t) r[21] << 8) | ((uint32_t) r[22] << 16) | ((uint32_t) r[23] << 24);
    if (n >= 36 && r[4] >= 2) {
        o->v2 = 1;
        o->g50 = r[24] | (r[25] << 8); o->g80 = r[26] | (r[27] << 8);
        o->flush = r[28] | (r[29] << 8); o->nocp_age = r[30] | (r[31] << 8);
        o->drop_age = r[32] | (r[33] << 8); o->drop_ovf = r[34] | (r[35] << 8);
    } else {
        /* A v1 record is 24 bytes and byte-identical up to there, so it parses
         * fine — it just has no gap-ledger half. Zero that half here instead of
         * leaving whatever the caller's stack held: the age-drop guard compares
         * this sample's drop_age against the previous one, and two pieces of
         * garbage that happen to differ fire "age-drops began" and condemn a
         * perfectly good block, while DONE prints random drop_age/drop_ovf that
         * the analysis then reads as data. Version skew is the real case here —
         * a rig staged by ds5_stage.sh against the older ds5_txd still inside
         * the installed IPK. */
        o->v2 = 0;
        o->g50 = o->g80 = o->flush = o->nocp_age = 0;
        o->drop_age = o->drop_ovf = 0;
        /* Zeroing alone traded garbage for silence: a run against a v1 daemon
         * then prints drop_age=0 and g50=0 and reads as a clean block, when in
         * truth that half of the record was never published. Say it once, in the
         * stream the harness keeps — the zeros mean ABSENT, not clean. */
        static int said_v1;
        if (!said_v1) {
            said_v1 = 1;
            printf("[synth] NOTICE: no gap ledger in this record (%d bytes, version %u) — "
                   "either the daemon still publishes v1 or the read was short: g50/g80/"
                   "flush/nocp_age/drop_age/drop_ovf read 0 because they are ABSENT, not "
                   "clean, and the age-drop guard has nothing to watch\n",
                   (int) n, (unsigned) r[4]);
            fflush(stdout);
        }
    }
    return 0;
}

/* ---- guards ---------------------------------------------------------------- *
 * Refusals, not warnings. A synthetic load running underneath a real session
 * would corrupt both the session and the measurement, and two rigs feeding one
 * pad would make every number meaningless. */
static int app_is_running(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int hit = 0;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300], buf[1024];
        snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        for (ssize_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
        if (strstr(buf, "applications/com.aurora.ds5/moonlight") ||
            strstr(buf, "applications/com.aurora.gamestream/moonlight")) hit = 1;
    }
    closedir(d);
    return hit;
}

static volatile sig_atomic_t g_stop = 0;

/* Seed until the daemon reports a bound link, then STOP — the hidraw fd is local
 * to this function and closed on every exit path, because a second writer left
 * open would put each frame on the L2CAP interrupt channel twice.
 *
 * The node is opened non-blocking: a blocking write into a congested pad would
 * park this process with that fd open, which is the one state the design must
 * not reach. This is also re-enterable, since a link that idles out mid-run
 * needs exactly the same handshake again. */
static int seed_until_bound(int fd, struct sockaddr_un *sa, const char *st_path,
                            const char *hidraw_path, const uint8_t addr[6], struct st *out) {
    int hf = open(hidraw_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (hf < 0) {
        fprintf(stderr, "[synth] open(%s): %s\n", hidraw_path, strerror(errno));
        return -1;
    }
    if (ds5_feature_probe(hf) < 0)
        fprintf(stderr, "[synth] feature 0x05 read failed (%s) — pad may report at a "
                        "different rate than production\n", strerror(errno));
    uint8_t dg[ACL_TAG_LEN + DS5_BT_OUT_LEN];
    dg[0] = ACL_TAG_M0; dg[1] = ACL_TAG_ASSERT; memcpy(dg + 2, addr, 6);
    int bound = 0;
    for (int i = 0; i < 40 && !bound && !g_stop; i++) {
        uint8_t rep[DS5_BT_OUT_LEN];
        build_lightbar(rep, 0x000018, (uint8_t) (i & 0x0F));
        memcpy(dg + ACL_TAG_LEN, rep, DS5_BT_OUT_LEN);
        /* assert first, then put the same bytes on air: the daemon binds on the
         * on-air frame and matches it byte-for-byte against the live assertion */
        sendto(fd, dg, sizeof dg, MSG_DONTWAIT, (struct sockaddr *) sa, sizeof *sa);
        if (write(hf, rep, DS5_BT_OUT_LEN) != DS5_BT_OUT_LEN && errno != EAGAIN)
            fprintf(stderr, "[synth] hidraw seed: %s\n", strerror(errno));
        struct timespec ts = { 0, 60 * 1000 * 1000L };
        nanosleep(&ts, NULL);
        bound = (st_read(st_path, out) == 0 && out->valid);
    }
    close(hf);
    if (bound) fprintf(stderr, "[synth] link bound via identity assert\n");
    return bound ? 0 : -1;
}
static void on_signal(int s) { (void) s; g_stop = 1; }

static void usage(void) {
    fprintf(stderr,
        "ds5_synth_audio — continuous synthetic pad audio, no host and no game.\n"
        "  --b <ms>        pad audio buffer depth to advertise (default 60)\n"
        "  --seconds <n>   run length, 0 = until signalled (default 0)\n"
        "  --freq <hz>     test tone (default 400)\n"
        "  --amp <dbfs>    tone level, negative (default -30)\n"
        "  --fifo <n>      daemon audio FIFO depth via control datagram (default 10,\n"
        "                  which is what the app sets against a rate-servo host;\n"
        "                  the boot default of 3 is NOT what production runs)\n"
        "  --complexity <n> Opus complexity 0..10 (default 10, as the host uses)\n"
        "  --bitrate <bps> Opus CBR for the 0x35 arm (default 96000 = 120 B/frame,\n"
        "                  32000..148800); the 0x36/0x39 arms stay at 160000 (one\n"
        "                  encoder, re-targeted when the format lever flips).\n"
        "                  Formats are file levers so arms can interleave: /tmp/ds5_r35\n"
        "                  (0x35, wins), /tmp/ds5_r36 (0x36), neither = 0x39\n"
        "  --mac <addr>    pad to drive (default: the only DualSense present)\n"
        "  --sock <path>   daemon socket (default: the com.aurora.ds5 jail)\n"
        "  --tmpl <path>   template path, for the .st telemetry sibling\n"
        "  --stats <sec>   stats line interval (default 10)\n"
        "  --mute          keep the transport identical but silence the pad: the 0x32\n"
        "                  SetState carries minimum volume, so all 547 bytes and the\n"
        "                  radio conditions stay exactly as they are. Never silence a\n"
        "                  run by moving the pad away — that is a different experiment.\n"
        "  --no-servo      feed strictly open-loop (see the servo note in the source)\n"
        "  --force         run even if an Aurora session is live (do not)\n");
}

int main(int argc, char **argv) {
    int    b_ms = 60, seconds = 0, fifo_depth = 10, complexity = 10, stats_iv = 10, force = 0;
    int    mute = 0, servo = 1, bitrate35 = 96000;
    double freq = 400.0, amp_dbfs = -30.0;
    const char *mac_arg = NULL;
    const char *sock = "/var/palm/jail/com.aurora.ds5/tmp/ds5_acl.sock";
    const char *tmpl = "/var/palm/jail/com.aurora.ds5/tmp/ds5_acl_tmpl";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int last = (i + 1 >= argc);
        if      (!strcmp(a, "--b") && !last)          b_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--seconds") && !last)    seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--freq") && !last)       freq = atof(argv[++i]);
        else if (!strcmp(a, "--amp") && !last)        amp_dbfs = atof(argv[++i]);
        else if (!strcmp(a, "--fifo") && !last)       fifo_depth = atoi(argv[++i]);
        else if (!strcmp(a, "--complexity") && !last) complexity = atoi(argv[++i]);
        else if (!strcmp(a, "--bitrate") && !last)    bitrate35 = atoi(argv[++i]);
        else if (!strcmp(a, "--stats") && !last)      stats_iv = atoi(argv[++i]);
        else if (!strcmp(a, "--mac") && !last)        mac_arg = argv[++i];
        else if (!strcmp(a, "--sock") && !last)       sock = argv[++i];
        else if (!strcmp(a, "--tmpl") && !last)       tmpl = argv[++i];
        else if (!strcmp(a, "--mute"))                mute = 1;
        else if (!strcmp(a, "--no-servo"))            servo = 0;
        else if (!strcmp(a, "--force"))               force = 1;
        else { usage(); return 2; }
    }
    /* The pad's own firmware floor is ~20 ms and 0 kills the endpoint outright;
     * the field is a byte, so anything above 127 is not a depth the host would
     * ever send (Thrum declares the useful range as 16..127). */
    if (b_ms < 16 || b_ms > 127) { fprintf(stderr, "[synth] --b %d outside 16..127\n", b_ms); return 2; }
    if (amp_dbfs > 0)            { fprintf(stderr, "[synth] --amp must be <= 0 dBFS\n"); return 2; }
    /* CBR bytes per 10 ms frame; the 0x35 has 334 - 4 (CRC) - 144 = 186 B of room. */
    if (bitrate35 < 32000 || bitrate35 > 148800 || bitrate35 % 800) {
        fprintf(stderr, "[synth] --bitrate %d: need 32000..148800 in steps of 800\n", bitrate35); return 2;
    }
    g_r35_opus_bytes = bitrate35 / 800;

    if (!force && app_is_running()) {
        fprintf(stderr, "[synth] REFUSING: an Aurora session is running. A synthetic load "
                        "under a real session corrupts both.\n");
        return 1;
    }

    crc_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    char mac[64] = "";
    int node = find_dualsense(mac_arg, mac, sizeof mac);
    if (node == -2) { fprintf(stderr, "[synth] REFUSING: several DualSense pads, name one with --mac\n"); return 1; }
    if (node < 0)   { fprintf(stderr, "[synth] no DualSense found\n"); return 1; }
    if (mac_arg) snprintf(mac, sizeof mac, "%s", mac_arg);
    uint8_t addr[6];
    if (mac_to_bytes(mac, addr) < 0) { fprintf(stderr, "[synth] cannot parse pad address '%s'\n", mac); return 1; }
    char hidraw_path[64], st_path[600];
    snprintf(hidraw_path, sizeof hidraw_path, "/dev/hidraw%d", node);
    /* the daemon names the record by the address with the colons stripped */
    char hex[16];
    snprintf(hex, sizeof hex, "%02x%02x%02x%02x%02x%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    snprintf(st_path, sizeof st_path, "%s.%s.st", tmpl, hex);

    const char *opuslib = NULL;
    OpusEncoder *enc = opus_setup(complexity, 160000, &opuslib);
    if (!enc) return 1;
    int enc_bitrate = 160000;   /* re-targeted per tick for the 0x35 arm */

    /* Addressed per datagram, never connect()ed. The daemon unlinks and re-binds
     * its socket on every start, so a connected fd would keep pointing at a
     * deleted inode after a daemon restart — sends would fail forever while the
     * rig happily reported its own send rate. Resolving the path each time makes
     * a restart self-healing, which is also how the app does it. */
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[synth] socket"); return 1; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (access(sock, F_OK) != 0) {
        fprintf(stderr, "[synth] %s does not exist — is ds5_txd running?\n", sock);
        return 1;
    }
    fprintf(stderr, "[synth] pad %s on %s, daemon %s, opus %s\n", mac, hidraw_path, sock, opuslib);

    /* Production parity: the app raises the daemon's elastic audio FIFO to 10
     * once the host advertises its rate servo. The supervisor's boot default is
     * 3, so a rig that skips this measures a shallower transport than the one
     * every reference number came from. */
    {
        uint8_t ctrl[4] = { ACL_TAG_M0, ACL_TAG_CTRL, ACL_CTRL_FIFO_DEPTH, (uint8_t) fifo_depth };
        if (sendto(fd, ctrl, sizeof ctrl, 0, (struct sockaddr *) &sa, sizeof sa) < 0)
            fprintf(stderr, "[synth] fifo ctrl: %s\n", strerror(errno));
    }

    struct builder B;
    builder_init(&B, b_ms);
    if (mute) {
        /* Volume lives in the SetState, not in the audio: the speaker byte is a
         * 0x80..0xFF field and the headphone byte a real 7-bit one, so this is
         * their common floor. The Opus payload, the report length and every
         * timing byte are untouched — which is the point. */
        B.r32[4 + 4] = 0x00;   /* VolumeHeadphones */
        B.r32[4 + 5] = 0x80;   /* VolumeSpeaker    */
        /* The 0x36 carries its own copy of the very same SetState inline, and
         * that copy is never rebuilt afterwards (build_0x36 renews only Opus,
         * seq, counter and CRC). Patching just the standalone 0x32 would leave
         * the r36 arm audible: with /tmp/ds5_r36 armed the pad hears ~94
         * unmuted inline SetStates a second against one muted 0x32 a second, so
         * the volume would flap instead of sitting at the floor — and the two
         * arms would then differ in SetState CONTENT, not only in report
         * format, which is the one thing a format lever must not do. */
        B.r36[OFF36_SETSTATE + 4] = 0x00;
        B.r36[OFF36_SETSTATE + 5] = 0x80;
        B.r35[OFF36_SETSTATE + 4] = 0x00;   /* same inline copy in the 0x35 */
        B.r35[OFF36_SETSTATE + 5] = 0x80;
    }
    struct tone tone;
    tone_init(&tone, freq, amp_dbfs);

    /* ---- bootstrap: get a link bound, then hand over to raw ACL ------------ */
    struct st s;
    memset(&s, 0, sizeof s);
    if (st_read(st_path, &s) == 0 && s.valid) {
        fprintf(stderr, "[synth] link already bound\n");
    } else if (seed_until_bound(fd, &sa, st_path, hidraw_path, addr, &s) != 0) {
        fprintf(stderr, "[synth] no link bound after seeding — pad asleep?\n");
        return 1;
    }

    /* Prime the audio path before the first frame: the batched report has no room
     * for a SetState block, so a stream that starts without one has its first
     * second discarded by the pad. */
    uint8_t dg[ACL_TAG_LEN + R39_LEN];
    dg[0] = ACL_TAG_M0; dg[1] = ACL_TAG_INJECT; memcpy(dg + 2, addr, 6);
    build_0x32(&B);
    memcpy(dg + ACL_TAG_LEN, B.r32, R32_LEN);
    sendto(fd, dg, ACL_TAG_LEN + R32_LEN, 0, (struct sockaddr *) &sa, sizeof sa);

    /* ---- steady state ----------------------------------------------------- */
    uint64_t t0 = now_us(), next = t0, last_ss = t0, last_st = t0, last_stats = t0, last_app_check = t0;
    uint64_t sent = 0, send_err = 0, late = 0, late_us_max = 0;
    /* the one-sided rate servo lives at file scope (g_adj_us): tick_rate_hz has
     * to see it, or it reports a tick rate the loop is not actually serving */
    int      warned_drop = 0, warned_invalid = 0, warned_late = 0;
    uint32_t last_inj = s.inj;
    uint64_t last_sent_mark = 0;
    uint64_t co_next = 0;                   /* co-traffic deadline, see below    */
    struct st s0 = s;
    printf("# ds5_synth_audio b=%d freq=%.0f amp=%.0fdBFS fifo=%d complexity=%d servo=%d "
           "mute=%d pad=%s\n",
           b_ms, freq, amp_dbfs, fifo_depth, complexity, servo, mute, mac);
    fflush(stdout);

    while (!g_stop) {
        if (seconds > 0 && (int64_t) (now_us() - t0) >= (int64_t) seconds * 1000000) break;

        int r35_now = g_r35;
        int r36_now = g_r36 || r35_now;   /* one read per tick: format and period must agree */
        /* ONE stateful encoder across the arms, re-targeted on a switch: two
         * encoders would hand the pad's single decoder a stream from an encoder
         * with stale CELT state (energy prediction, prefilter) at every arm
         * change -- a click in exactly the comparison being listened to. */
        {
            int want = r35_now ? bitrate35 : 160000;
            if (want != enc_bitrate) {
                p_opus_ctl(enc, OPUS_SET_BITRATE_REQUEST, want);
                enc_bitrate = want;
            }
        }
        int sent_now = 0;      /* did an audio report actually go out this tick? */
        if (!burst_silent(now_us() - t0)) {
            if (r35_now) {
                if (build_0x35(&B, enc, &tone) < 0) {
                    printf("[synth] STOPPING: could not build a 0x35 report (opus encode failed) at %.1fs\n",
                           (double) (now_us() - t0) / 1e6);
                    fflush(stdout);
                    break;
                }
                memcpy(dg + ACL_TAG_LEN, B.r35, R35_LEN);
                if (sendto(fd, dg, ACL_TAG_LEN + R35_LEN, MSG_DONTWAIT,
                           (struct sockaddr *) &sa, sizeof sa) < 0) send_err++;
                else { sent++; sent_now = 1; }
            } else if (r36_now) {
                if (build_0x36(&B, enc, &tone) < 0) {
                    /* Say it: leaving the loop in silence prints only DONE, and
                     * DONE without a reason reads downstream as "the run simply
                     * reached its budget" -- a broken instrument filed as a
                     * healthy short run. */
                    printf("[synth] STOPPING: could not build a 0x36 report (opus encode failed) at %.1fs\n",
                           (double) (now_us() - t0) / 1e6);
                    fflush(stdout);
                    break;
                }
                memcpy(dg + ACL_TAG_LEN, B.r36, R36_LEN);
                if (sendto(fd, dg, ACL_TAG_LEN + R36_LEN, MSG_DONTWAIT,
                           (struct sockaddr *) &sa, sizeof sa) < 0) send_err++;
                else { sent++; sent_now = 1; }
            } else {
                if (build_0x39(&B, enc, &tone) < 0) {
                    /* Say it: leaving the loop in silence prints only DONE, and
                     * DONE without a reason reads downstream as "the run simply
                     * reached its budget" -- a broken instrument filed as a
                     * healthy short run. */
                    printf("[synth] STOPPING: could not build a 0x39 report (opus encode failed) at %.1fs\n",
                           (double) (now_us() - t0) / 1e6);
                    fflush(stdout);
                    break;
                }
                memcpy(dg + ACL_TAG_LEN, B.r39, R39_LEN);
                if (sendto(fd, dg, ACL_TAG_LEN + R39_LEN, MSG_DONTWAIT,
                           (struct sockaddr *) &sa, sizeof sa) < 0) send_err++;
                else { sent++; sent_now = 1; }
            }
        }

        /* Co-traffic rides the audio ticks but is paced on the clock, not on a
         * report counter. The declared rate is what the arm is supposed to
         * carry, so it has to come out the same in both report formats and at
         * every period — two arms carrying different co-traffic invalidate every
         * ratio measured between them, which is the one thing this lever must
         * not do.
         *
         * "One extra 0x32 every Nth report" cannot deliver that. N is an
         * integer, so plain flooring already makes most of the accepted rates
         * come out different in the 0x36 arm than in the 0x39 one, and from
         * ~47/s upwards the divisor bottoms out at 1: the 0x39 arm then sends
         * one per tick while the 0x36 arm, ticking twice as often, sends exactly
         * twice as much — the doubling this lever was fixed for, surviving in
         * the range the burst numbers point at. A deadline has neither problem:
         * it is exact at any tick rate and does not care which lever moved
         * underneath it.
         *
         * `sent_now` still gates the whole thing — during a burst-off phase the
         * client is silent on this link, co-traffic included, and `sent` freezing
         * on a failing sendto must not make this fire on every tick. The
         * deadline is re-based rather than repaid when it falls more than one
         * interval behind, so a silent phase is not followed by a catch-up
         * burst. */
        if (g_cotraffic_hz && sent_now) {
            uint64_t co_now = now_us();
            uint64_t co_iv  = 1000000ull / g_cotraffic_hz;
            if (!co_next || co_now > co_next + co_iv) co_next = co_now;
            if (co_now >= co_next) {
                co_next += co_iv;
                uint8_t cdg[ACL_TAG_LEN + R32_LEN];
                memcpy(cdg, dg, ACL_TAG_LEN);       /* same tag, same bdaddr */
                build_0x32(&B);                     /* fresh seq + CRC each time */
                memcpy(cdg + ACL_TAG_LEN, B.r32, R32_LEN);
                if (sendto(fd, cdg, sizeof cdg, MSG_DONTWAIT,
                           (struct sockaddr *) &sa, sizeof sa) < 0) send_err++;
            }
        }

        uint64_t now = now_us();
        if (now - last_ss >= SETSTATE_MS * 1000ull) {
            last_ss = now;
            uint8_t sdg[ACL_TAG_LEN + R32_LEN];
            memcpy(sdg, dg, ACL_TAG_LEN);
            build_0x32(&B);
            memcpy(sdg + ACL_TAG_LEN, B.r32, R32_LEN);
            if (sendto(fd, sdg, sizeof sdg, MSG_DONTWAIT,
                       (struct sockaddr *) &sa, sizeof sa) < 0) send_err++;
        }

        /* Rate servo, one-sided exactly like the host's: the pad drains on its own
         * clock and has no refill path, so overfeeding parks latency permanently
         * while underfeeding merely erodes depth. Slow down when the daemon is
         * holding a backlog, decay back to nominal when it is clean. */
        if (now - last_st >= 200000ull) {
            last_st = now;
            period_poll();
            burst_poll();
            cotraffic_poll();
            r36_poll();
            r35_poll();
            /* One 0x32 rides one audio tick, so the tick rate is the hard ceiling
             * on the co-traffic this rig can put on the link: 46.7/s batched at
             * the default period, half that if the feed lever doubles the period.
             * Above it the pacer would quietly deliver the tick rate instead of
             * the declared rate, which is exactly the mislabelled load the
             * deadline pacing exists to prevent — so refuse the run rather than
             * measure one. Checked here, after the polls, because both the period
             * and the format can move mid-run. */
            if (g_cotraffic_hz && (double) g_cotraffic_hz > tick_rate_hz()) {
                printf("[synth] STOPPING: %u co-traffic reports/s asked for, but the audio loop "
                       "ticks %.1f times a second (%s at %u us) and one 0x32 rides one tick — "
                       "this arm cannot carry the load it declares\n",
                       g_cotraffic_hz, tick_rate_hz(), fmt_name(), g_period_us);
                fflush(stdout);
                g_stop = 1;
            }
            struct st cur;
            /* An unreadable or invalidated record is the link going away, and it
             * has to be handled OUTSIDE the success branch: the old code nested
             * the "record went invalid" warning inside `if (cur.valid)` inside
             * `if (... && cur.valid)`, so the only stop for a dropped link was
             * unreachable and an invalid record simply skipped the whole block —
             * silently, with the rig still sending. */
            int st_ok = (st_read(st_path, &cur) == 0);
            if ((!st_ok || !cur.valid) && !warned_invalid) {
                warned_invalid = 1;
                printf("[synth] STOPPING: the daemon's readiness record is %s — the link "
                       "dropped; nothing measured from here is a gap\n",
                       st_ok ? "invalid" : "unreadable");
                fflush(stdout);
                g_stop = 1;
            }
            if (st_ok && cur.valid) {
                /* One-sided, like the host's: the pad drains on its own clock and
                 * has no refill path, so overfeeding parks latency permanently
                 * while underfeeding merely erodes depth. Production ran with this
                 * servo active — often pinned at its clamp — so keeping it is the
                 * faithful choice, not the neutral one. It is bounded at 1.3 % of
                 * the period and `adj` is printed every stats line, because a servo
                 * that backs off during a stall would blunt the very stall being
                 * measured. --no-servo feeds strictly open-loop for comparison. */
                if (servo) {
                    int backlog = cur.fifo + (cur.q > 3 ? cur.q - 3 : 0);
                    if (backlog > 0) { g_adj_us += 8 * backlog; if (g_adj_us > 280) g_adj_us = 280; }
                    else             { g_adj_us -= 6; if (g_adj_us < 0) g_adj_us = 0; }
                }
                /* Age-drops mean the transport did not carry what was offered, and
                 * every gap measured from here on describes starvation rather than
                 * the link. Say it once, loudly, in the stream the harness reads. */
                /* The link can go away underneath us — the daemon invalidates a
                 * template after ~1.5 s without inbound traffic — and every health
                 * number this program reads comes from a record that then simply
                 * stops changing. Without this check a block that delivered
                 * NOTHING looks calm: rate is our own loop, drops stay zero, and
                 * the gap ledger just goes quiet, which pools as "few gaps". So
                 * the witness is the daemon's own injection counter: it must keep
                 * up with what we send, or the run is over. */
                /* The window has to ACCUMULATE. Re-baselining on every 200 ms poll
                 * held d_sent at ~9 (200 ms / 21.4 ms), so the `> 40` precondition
                 * — sized for ~900 ms of sends — could never be met and this guard
                 * never fired once. It sat inert through the run whose link died
                 * at 20:21: the rig kept sending into nothing, printed a healthy
                 * rate, and the DONE line still said drop=0 while the daemon threw
                 * away 12206 frames. So: only re-baseline when the test actually
                 * ran, and treat a FROZEN record (inj not moving) as the same
                 * failure — the record stays `valid` when a template dies, it just
                 * stops changing, which is exactly what a dead link looks like
                 * from here. */
                uint64_t d_sent = sent - last_sent_mark;
                /* The window is ~9 s of sends, not ~1. The daemon republishes its
                 * record about 5 times a second, so `inj` is up to 200 ms stale
                 * against a `sent` read right now. Over a window the lag cancels —
                 * unless the send RATE changed inside it, which is exactly what a
                 * bursty producer does: 44 sent in 0.8 s against an inj snapshot
                 * 10 reports behind reads as 77 % delivery and trips a guard whose
                 * threshold is 80 %. That killed the first burst run 108 s in. At
                 * 400+ reports a 10-report lag is 2.5 %, far inside the margin, and
                 * a genuinely starved link (a third of the load) still trips it.
                 *
                 * Not during bootstrap either. The first seconds are the bind, the
                 * template publish and the FIFO going 3 -> 10, and frames queued
                 * before the link is ready age out by design — judging delivery
                 * there condemns every run at its own startup (it did, on the
                 * first run after this guard was repaired). Keep re-baselining
                 * so the first real window starts clean. */
                if (sent <= 200) {
                    last_inj = cur.inj;
                    last_sent_mark = sent;
                } else if (d_sent > 400) {
                    uint32_t d_inj = cur.inj - last_inj;
                    if (d_inj * 100 < d_sent * 80) {
                        printf("[synth] STOPPING: the daemon injected %u of the %llu reports "
                               "handed over over the last %.1f s — the link is not carrying "
                               "this load\n", d_inj, (unsigned long long) d_sent,
                               (double) d_sent * g_period_us / 1e6);
                        fflush(stdout);
                        g_stop = 1;
                    }
                    last_inj = cur.inj;
                    last_sent_mark = sent;
                }
                if (cur.drop_age != s.drop_age && sent > 200 && !warned_drop) {
                    warned_drop = 1;
                    printf("[synth] WARNING age-drops began at t=%.0fs — delivery is short of "
                           "the offered load; blocks from here are not valid\n",
                           (double) (now - t0) / 1e6);
                    fflush(stdout);
                }
                s = cur;
            }
        }

        /* An unattended run can outlive the operator's patience: if somebody
         * launches Aurora while this is feeding the pad, the session and the
         * measurement would corrupt each other. Yield to the human immediately —
         * the refusal at startup is not enough when runs last hours. */
        if (!force && now - last_app_check >= 2000000ull) {
            last_app_check = now;
            if (app_is_running()) {
                fprintf(stderr, "[synth] an Aurora session appeared — stopping, the pad is theirs\n");
                break;
            }
        }

        if (stats_iv > 0 && now - last_stats >= (uint64_t) stats_iv * 1000000ull) {
            last_stats = now;
            double secs = (double) (now - t0) / 1e6;
            printf("[synth] t=%.0fs%s sent=%llu rate=%.1f/s err=%llu late=%llu(max %llums) "
                   "adj=%dus | st: q=%d fifo=%d inj=%u drop=%u dage=%d dovf=%d g50=%d g80=%d\n",
                   secs, r35_now ? " r35" : (r36_now ? " r36" : ""),
                   (unsigned long long) sent, (double) sent / (secs > 0 ? secs : 1),
                   (unsigned long long) send_err, (unsigned long long) late,
                   (unsigned long long) (late_us_max / 1000), g_adj_us,
                   s.q, s.fifo, s.inj - s0.inj, s.drop - s0.drop,
                   s.drop_age, s.drop_ovf, s.g50, s.g80);
            fflush(stdout);
        }

        /* One frame per 0x36 -> half the period AND half the per-report servo
         * adjustment: g_adj_us is sized against the two-frame cadence, and paying
         * it in full twice as often would double the servo's authority. */
        next += ((uint64_t) g_period_us + (uint64_t) g_adj_us) / (r36_now ? 2 : 1);
        now = now_us();
        if (next <= now) {
            /* Falling behind is a measurement fault, not a hiccup to smooth over:
             * a late report is a gap this program manufactured itself. Count it,
             * and resynchronise rather than sprinting to catch up. */
            late++;
            if (now - next > late_us_max) late_us_max = now - next;
            /* 150 ms is not an arbitrary threshold: it is AUDIO_IDLE_MS in the
             * daemon. A hiccup longer than that makes the daemon treat the pause
             * as "the game stopped producing audio" and stop binning gaps
             * entirely — so our own stall would erase itself from the record and
             * read as a quiet stretch. Say it in the stream the harness keeps. */
            if (now - next > 150000ull && !warned_late) {
                warned_late = 1;
                printf("[synth] WARNING a %llu ms scheduling gap exceeded the daemon's "
                       "audio-idle window — gaps around it were not binned\n",
                       (unsigned long long) ((now - next) / 1000));
                fflush(stdout);
            }
            next = now;
        } else {
            struct timespec ts = { (time_t) (next / 1000000ull), (long) ((next % 1000000ull) * 1000ull) };
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) { }
        }
    }

    double secs = (double) (now_us() - t0) / 1e6;
    struct st fin;
    if (st_read(st_path, &fin) != 0) fin = s;
    printf("[synth] DONE %.1fs sent=%llu rate=%.2f/s send_err=%llu late=%llu late_max=%llums "
           "inj=%u drop=%u drop_age=%d drop_ovf=%d%s\n",
           secs, (unsigned long long) sent, (double) sent / (secs > 0 ? secs : 1),
           (unsigned long long) send_err, (unsigned long long) late,
           (unsigned long long) (late_us_max / 1000),
           fin.inj - s0.inj, fin.drop - s0.drop, fin.drop_age, fin.drop_ovf,
           /* The one line the report reads. A v1 daemon's zeros must not pass
            * through it looking like a measured zero. */
           fin.v2 ? "" : " [v1 record: no gap ledger]");
    /* Hand the daemon back its boot default so the next real session starts from
     * the state the supervisor set, not from ours. */
    uint8_t ctrl[4] = { ACL_TAG_M0, ACL_TAG_CTRL, ACL_CTRL_FIFO_DEPTH, 0xFF };
    sendto(fd, ctrl, sizeof ctrl, 0, (struct sockaddr *) &sa, sizeof sa);
    close(fd);
    return 0;
}
