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
#define OPUS_BYTES     200
#define OPUS_FRAME     480          /* samples per channel, 10 ms @ 48 kHz */
#define HAPTIC_BYTES    64

/* 0x39 offsets */
#define OFF39_HAPTIC_A  12
#define OFF39_HAPTIC_B  76
#define OFF39_OPUS_A   142
#define OFF39_OPUS_B   342

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

static OpusEncoder *opus_setup(int complexity, const char **which) {
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
    p_opus_ctl(e, OPUS_SET_BITRATE_REQUEST, 160000);
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
    uint8_t  r32[R32_LEN];
    uint8_t  seq;          /* 4-bit sequence nibble, shared across report ids */
    uint8_t  pktctr;       /* audio packet counter: +2 per batched report      */
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
        o->g50 = r[24] | (r[25] << 8); o->g80 = r[26] | (r[27] << 8);
        o->flush = r[28] | (r[29] << 8); o->nocp_age = r[30] | (r[31] << 8);
        o->drop_age = r[32] | (r[33] << 8); o->drop_ovf = r[34] | (r[35] << 8);
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
    int    mute = 0, servo = 1;
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
    OpusEncoder *enc = opus_setup(complexity, &opuslib);
    if (!enc) return 1;

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
    int      adj_us = 0;                    /* one-sided rate servo, see below   */
    int      warned_drop = 0, warned_invalid = 0, warned_late = 0;
    uint32_t last_inj = s.inj;
    uint64_t last_sent_mark = 0;
    struct st s0 = s;
    printf("# ds5_synth_audio b=%d freq=%.0f amp=%.0fdBFS fifo=%d complexity=%d servo=%d "
           "mute=%d pad=%s\n",
           b_ms, freq, amp_dbfs, fifo_depth, complexity, servo, mute, mac);
    fflush(stdout);

    while (!g_stop) {
        if (seconds > 0 && (int64_t) (now_us() - t0) >= (int64_t) seconds * 1000000) break;

        if (build_0x39(&B, enc, &tone) < 0) break;
        memcpy(dg + ACL_TAG_LEN, B.r39, R39_LEN);
        if (sendto(fd, dg, ACL_TAG_LEN + R39_LEN, MSG_DONTWAIT,
                   (struct sockaddr *) &sa, sizeof sa) < 0) send_err++;
        else sent++;

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
                    if (backlog > 0) { adj_us += 8 * backlog; if (adj_us > 280) adj_us = 280; }
                    else             { adj_us -= 6; if (adj_us < 0) adj_us = 0; }
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
                if (d_sent > 40) {
                    uint32_t d_inj = cur.inj - last_inj;
                    if (d_inj * 100 < d_sent * 80) {
                        printf("[synth] STOPPING: the daemon injected %u of the %llu reports "
                               "handed over over the last %.1f s — the link is not carrying "
                               "this load\n", d_inj, (unsigned long long) d_sent,
                               (double) d_sent * REPORT_US / 1e6);
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
            printf("[synth] t=%.0fs sent=%llu rate=%.1f/s err=%llu late=%llu(max %llums) "
                   "adj=%dus | st: q=%d fifo=%d inj=%u drop=%u dage=%d dovf=%d g50=%d g80=%d\n",
                   secs, (unsigned long long) sent, (double) sent / (secs > 0 ? secs : 1),
                   (unsigned long long) send_err, (unsigned long long) late,
                   (unsigned long long) (late_us_max / 1000), adj_us,
                   s.q, s.fifo, s.inj - s0.inj, s.drop - s0.drop,
                   s.drop_age, s.drop_ovf, s.g50, s.g80);
            fflush(stdout);
        }

        next += (uint64_t) REPORT_US + (uint64_t) adj_us;
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
           "inj=%u drop=%u drop_age=%d drop_ovf=%d\n",
           secs, (unsigned long long) sent, (double) sent / (secs > 0 ? secs : 1),
           (unsigned long long) send_err, (unsigned long long) late,
           (unsigned long long) (late_us_max / 1000),
           fin.inj - s0.inj, fin.drop - s0.drop, fin.drop_age, fin.drop_ovf);
    /* Hand the daemon back its boot default so the next real session starts from
     * the state the supervisor set, not from ours. */
    uint8_t ctrl[4] = { ACL_TAG_M0, ACL_TAG_CTRL, ACL_CTRL_FIFO_DEPTH, 0xFF };
    sendto(fd, ctrl, sizeof ctrl, 0, (struct sockaddr *) &sa, sizeof sa);
    close(fd);
    return 0;
}
