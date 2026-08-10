#include "session_video.h"

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stream/session.h"

#include "app_settings.h"
#include "sps_parser.h"

#include "ui/streaming/streaming.controller.h"
#include "util/bus.h"
#include "logging.h"
#include "ss4s.h"
#include "stream/connection/session_connection.h"
#include "stream/session_priv.h"
#include "stream/adaptive_bitrate.h"
#include "app.h"

#include <SDL.h>
#include <assert.h>

/* Starting capacity for the decode-unit reassembly buffer. Grows on
 * demand to accommodate larger frames (e.g. 4K IDR frames at high
 * bitrate), capped to keep a malformed stream from exhausting memory. */
#define DECODER_BUFFER_MAX_SIZE (32 * 1024 * 1024)

/** Slices hint for pipeline decode while later slices still arrive (high bitrate / 120 Hz).
 * Scale the count with resolution x fps (ported from upstream 0efdb0ea): more slices let the
 * decoder pipeline earlier slices while later ones still arrive, stabilizing high-load streams. */
#define VDEC_STREAM_SLICES_MIN 4
#define VDEC_STREAM_SLICES_MAX 8

static unsigned vdec_slices_for_stream(int width, int height, int fps) {
    if (fps <= 0) {
        fps = 60;
    }
    const int64_t load = (int64_t) width * (int64_t) height * (int64_t) fps;
    if (load >= (int64_t) 3840 * 2160 * 90) {
        return VDEC_STREAM_SLICES_MAX;
    }
    if (load >= (int64_t) 2560 * 1440 * 90) {
        return 6;
    }
    return VDEC_STREAM_SLICES_MIN;
}

// Escalating recovery for SS4S_VIDEO_FEED_ERROR: a single transient NDL
// error drops the frame and requests an IDR; only N consecutive failures
// within a short window interrupt the session (a wedged decoder fails
// every Feed, so escalation still fires within a few frames).
#define VDEC_FEED_ERROR_LIMIT 3
#define VDEC_FEED_ERROR_WINDOW_MS 2000
// A transient NOT_READY (HDR toggle / resolution change) is expected and must
// not tear down the stream. But a decoder that stays NOT_READY forever would
// silently freeze with no recovery, so a generous watchdog still interrupts if
// NOT_READY persists continuously well beyond any legitimate exclusive op.
#define VDEC_NOT_READY_WEDGE_MS 5000
/*
 * Minimum interval between IDR requests we forward to the host.
 *
 * When the sink asks for a keyframe (buffer full, decode error), the obvious answer is
 * DR_NEED_IDR. But an IDR frame is several times the size of a P-frame, so feeding it
 * refills the buffer that was already full and provokes the next request — a feedback
 * loop that keeps the link saturated. IDR requests also share the control channel with
 * gamepad input, so a flood of them shows up as input lag. Rate-limit them and drop the
 * frames in between; the stream recovers on its own once the buffer drains.
 */
#define IDR_REQUEST_MIN_INTERVAL_MS 1000

/* Everything that belongs to ONE stream, i.e. to one vdec_delegate_setup() /
 * vdec_delegate_cleanup() pair. Grouped so that "nothing here is read by the
 * next stream" can be checked against one reset rather than against fifteen
 * scattered assignments. */
struct vdec_stream_state {
    session_t *session;
    SS4S_Player *player;
    int stream_format;                  /* VIDEO_FORMAT_* the host negotiated */
    int target_fps;
    int frames_since_idr;
    int lastFrameNumber;
    /* Set when SS4S_PlayerVideoFeed returns NOT_READY. Consumed on the next
     * successful Feed: ask Limelight for one IDR so the decoder can resync. */
    bool need_idr_on_resume;
    int feed_error_count;
    unsigned long feed_error_first_ms;
    unsigned long not_ready_first_ms;
    unsigned long last_idr_request_ms;
    bool warned_near_buffer_limit;
    struct VIDEO_STATS temp_stats;
#if defined(TARGET_WEBOS)
    Uint32 soft_rec_high_since;
    Uint32 soft_rec_cooldown_until;
#endif
};

static struct vdec_stream_state vs;

/* The decode-unit reassembly buffer is deliberately NOT part of the per-stream
 * state (P15): it is allocated on the first setup() and kept — including any
 * capacity a grow-on-demand realloc added — so a later stream's connection
 * setup pays neither the multi-megabyte malloc nor the first-touch faults.
 * Lifetime, stated here once: `data` is owned from the first successful
 * setup() until either a setup() failure path frees it or the process exits;
 * vdec_delegate_cleanup() does not free it, which is the entire point. */
struct vdec_reassembly {
    unsigned char *data;
    size_t size;            /* capacity currently allocated at `data` */
    size_t initial_size;    /* capacity a fresh stream asks for */
};

static struct vdec_reassembly reasm;

VIDEO_STATS vdec_summary_stats;
/* Seqlock for vdec_summary_stats: odd while vdec_stat_submit is mid-write.
 * Single writer (session thread); cross-thread readers use vdec_stats_snapshot. */
static unsigned vdec_stats_seq;
VIDEO_INFO vdec_stream_info;

#if defined(TARGET_WEBOS)
#define SOFT_REC_SUSTAIN_MS 3000u
#define SOFT_REC_COOLDOWN_MS 20000u
#define SOFT_REC_HOLD_MS 15000
#define SOFT_REC_DROP_PERCENT 75
#define SOFT_REC_LATENCY_MS 80.0f

static void soft_recovery_reset(void) {
    vs.soft_rec_high_since = 0;
    vs.soft_rec_cooldown_until = 0;
}

static bool soft_recovery_is_4k(void) {
    int w = vs.session != NULL ? vs.session->config.stream.width : 0;
    int h = vs.session != NULL ? vs.session->config.stream.height : 0;
    if (vdec_stream_info.width > 0 && vdec_stream_info.height > 0) {
        w = vdec_stream_info.width;
        h = vdec_stream_info.height;
    }
    return ((int64_t) w * (int64_t) h) >= ((int64_t) 3840 * 2160);
}

static bool soft_recovery_pressure(const struct VIDEO_STATS *dst) {
    /* Upstream's fastest signal is the render-queue depth, but that comes from
     * SS4S_PlayerGetVideoRenderQueueLength, which is Starfish/SMP-only and does not
     * exist in our ss4s at all — we decode via NDL. So pressure is detected from the
     * two signals that do carry data here: frames arriving but not decoding, and the
     * decoder latency ss4s computes itself. */
    float target = (float) vs.target_fps;
    if (target < 1.0f) {
        target = 60.0f;
    }
    if (dst->receivedFps >= target * 0.9f && dst->decodedFps < target * 0.85f && dst->submittedFrames > 0) {
        return true;
    }
    if (vdec_stream_info.has_decoder_latency && dst->avgDecoderLatency > SOFT_REC_LATENCY_MS) {
        return true;
    }
    return false;
}

static void soft_recovery_tick(const struct VIDEO_STATS *dst) {
    if (vs.session == NULL || !vs.session->config.soft_recovery || vs.session->abr == NULL) {
        return;
    }
    if (!soft_recovery_is_4k()) {
        vs.soft_rec_high_since = 0;
        return;
    }
    Uint32 now = SDL_GetTicks();
    if (vs.soft_rec_cooldown_until != 0 && now < vs.soft_rec_cooldown_until) {
        return;
    }
    if (!soft_recovery_pressure(dst)) {
        vs.soft_rec_high_since = 0;
        return;
    }
    if (vs.soft_rec_high_since == 0) {
        vs.soft_rec_high_since = now;
        return;
    }
    if (now - vs.soft_rec_high_since < SOFT_REC_SUSTAIN_MS) {
        return;
    }
    if (adaptive_bitrate_request_drop(vs.session->abr, SOFT_REC_DROP_PERCENT, SOFT_REC_HOLD_MS,
                                      "4K decode backlog")) {
        commons_log_info("Session",
                         "Soft recovery: sustained 4K backlog (receivedFps=%.1f decodedFps=%.1f "
                         "latency=%.1fms) -> request %d%% bitrate",
                         dst->receivedFps, dst->decodedFps, dst->avgDecoderLatency,
                         SOFT_REC_DROP_PERCENT);
        vs.soft_rec_cooldown_until = now + SOFT_REC_COOLDOWN_MS;
    }
    vs.soft_rec_high_since = 0;
}
#endif

static int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags);

static void vdec_delegate_cleanup(void);

static int vdec_delegate_submit(PDECODE_UNIT decodeUnit);

static int vdec_finish_feed(SS4S_VideoFeedResult result, PDECODE_UNIT decodeUnit);

static void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now);

static void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info);

static size_t vdec_buffer_initial_bytes(void) {
    return (size_t) VDEC_REASSEMBLY_BUFFER_MB * 1024U * 1024U;
}

DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks = {
        .setup = vdec_delegate_setup,
        .cleanup = vdec_delegate_cleanup,
        .submitDecodeUnit = vdec_delegate_submit,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void session_video_prepare_stream(void) {
    int caps = CAPABILITY_DIRECT_SUBMIT;
    const bool hevc = app_configuration != NULL && app_configuration->hevc;
    const bool av1 = app_configuration != NULL && app_configuration->av1;
    if (hevc) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
    }
    if (av1) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
    }
    if (hevc || av1) {
        unsigned slices = VDEC_STREAM_SLICES_MIN;
        if (app_configuration != NULL) {
            slices = vdec_slices_for_stream(app_configuration->stream.width,
                                            app_configuration->stream.height,
                                            app_configuration->stream.fps);
        }
        caps |= CAPABILITY_SLICES_PER_FRAME(slices);
        commons_log_info("Session", "Video SDP caps: RFI + %u slices/frame (HEVC=%d AV1=%d)",
                         slices, hevc ? 1 : 0, av1 ? 1 : 0);
    } else {
        commons_log_info("Session", "Video SDP caps: direct submit only (H.264)");
    }
    ss4s_dec_callbacks.capabilities = caps;
}

static const char *video_format_name(int videoFormat) {
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            return "H264";
        case VIDEO_FORMAT_H265:
            return "H265";
        case VIDEO_FORMAT_H265_MAIN10:
            return "H265 10bit";
        case VIDEO_FORMAT_AV1_MAIN8:
            return "AV1 8bit";
        case VIDEO_FORMAT_AV1_MAIN10:
            return "AV1 10bit";
        default:
            if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
                return "AV1";
            }
            return "Unknown";
    }
}

int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
    (void) drFlags;
    vs.session = context;
    vs.player = vs.session->player;
    reasm.initial_size = vdec_buffer_initial_bytes();
    /* P15: allocate only on the first stream — see struct vdec_reassembly for
     * why the buffer outlives one. */
    if (reasm.data == NULL) {
        reasm.size = reasm.initial_size;
        reasm.data = malloc(reasm.size);
        if (reasm.data) {
            /* Pre-fault the pages here (one-time, off the frame deadline) so the
             * first big IDR doesn't pay first-touch faults under MCL_ONFAULT.
             * NOT done on the grow-on-demand realloc path, which runs in
             * vdec_delegate_submit under the frame deadline. */
            memset(reasm.data, 0, reasm.size);
        }
    }
    if (!reasm.data) {
        /* Pre-alloc/fallback malloc failed: bail with a decoder error instead of
         * NULL-dereferencing in the reassembly memcpy on the first frame. */
        commons_log_error("Session", "Failed to allocate video reassembly buffer (%zu bytes)", reasm.size);
        reasm.size = 0;
        return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
    memset(&vs.temp_stats, 0, sizeof(vs.temp_stats));
    memset(&vdec_stream_info, 0, sizeof(vdec_stream_info));
    vs.stream_format = videoFormat;
    vdec_stream_info.format = video_format_name(videoFormat);
    vs.lastFrameNumber = 0;
    vs.need_idr_on_resume = false;
    vs.feed_error_count = 0;
    vs.feed_error_first_ms = 0;
    vs.last_idr_request_ms = 0;  /* a new stream must never have its first keyframe
                                  * request swallowed by the previous stream's
                                  * throttle window */
    vs.not_ready_first_ms = 0;   /* the NOT_READY wedge watchdog must not carry a
                                  * stale epoch from a stream that ended mid-
                                  * NOT_READY: the next stream's first transient
                                  * NOT_READY (normal at HDR start) would read as
                                  * a >5s wedge -> spurious decoder interrupt */
    vs.frames_since_idr = 0;
    vs.target_fps = redrawRate > 0 ? redrawRate : 60;
    vs.warned_near_buffer_limit = false;
#if defined(TARGET_WEBOS)
    soft_recovery_reset();
#endif

    if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        vdec_stream_info.width = width;
        vdec_stream_info.height = height;
    }

    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    /* Prefer host fractional refresh (e.g. 11988/100) for Starfish maxFrameRate / smooth
     * pacing so the decoder grid matches NTSC clientRefreshRateX100 when set. */
    if (vs.session != NULL && vs.session->config.stream.clientRefreshRateX100 > 0 &&
        (vs.session->config.stream.clientRefreshRateX100 % 100) != 0) {
        info.frameRateNumerator = vs.session->config.stream.clientRefreshRateX100;
        info.frameRateDenominator = 100;
    } else {
        info.frameRateNumerator = vs.target_fps;
        info.frameRateDenominator = 1;
    }
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            info.codec = SS4S_VIDEO_H264;
            break;
        case VIDEO_FORMAT_H265:
        case VIDEO_FORMAT_H265_MAIN10:
            info.codec = SS4S_VIDEO_H265;
            break;
        case VIDEO_FORMAT_AV1_MAIN8:
        case VIDEO_FORMAT_AV1_MAIN10:
            info.codec = SS4S_VIDEO_AV1;
            break;
        default: {
            commons_log_error("Session", "Unsupported codec %s", vdec_stream_info.format);
            free(reasm.data);
            reasm.data = NULL;
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        }
    }

    app_t *app = vs.session->app;
    if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_EXCLUSIVE) {
        app_bus_post_sync(app, (bus_actionfunc) app_ui_close, &app->ui);
    }

    switch (SS4S_PlayerVideoOpen(vs.player, &info)) {
        case SS4S_VIDEO_OPEN_OK: {
            return 0;
        }
        case SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC:
            free(reasm.data);
            reasm.data = NULL;
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        default:
            free(reasm.data);
            reasm.data = NULL;
            return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
}

void vdec_delegate_cleanup(void) {
    assert(vs.player != NULL);
    /* P15: reasm.data is deliberately not freed here — see struct vdec_reassembly. */
#if defined(TARGET_WEBOS)
    soft_recovery_reset();
#endif
    SS4S_PlayerVideoClose(vs.player);
    vs.session = NULL;
}

static int vdec_finish_feed(SS4S_VideoFeedResult result, PDECODE_UNIT decodeUnit) {
    if (result == SS4S_VIDEO_FEED_OK) {
        vs.feed_error_count = 0;
        vs.not_ready_first_ms = 0;
        if (decodeUnit->frameType == FRAME_TYPE_IDR) {
            vs.frames_since_idr = 0;
        } else {
            vs.frames_since_idr++;
        }
        const int idr_ms = app_configuration ? app_configuration->idr_refresh_interval_ms : 0;
        const bool hevc_stream = vs.stream_format == VIDEO_FORMAT_H265 ||
                                 vs.stream_format == VIDEO_FORMAT_H265_MAIN10;
        if (hevc_stream && idr_ms >= 500 && vs.target_fps > 0) {
            const int frames_threshold = (vs.target_fps * idr_ms + 500) / 1000;
            if (frames_threshold > 0 && vs.frames_since_idr >= frames_threshold) {
                LiRequestIdrFrame();
                vs.frames_since_idr = 0;
            }
        }
        if (vdec_stream_info.width == 0 || vdec_stream_info.height == 0) {
            stream_info_parse_size(decodeUnit, &vdec_stream_info);
        }
        /* Both sides are microseconds since the same epoch (LiGetMicroseconds and
         * DECODE_UNIT timestamps share it), and the accumulator stays in microseconds:
         * a submit typically takes far less than a millisecond, so dividing per frame
         * would floor most of them to zero and the overlay would read 0.00 ms. */
        vs.temp_stats.totalSubmitTimeUs += (uint32_t) (LiGetMicroseconds() - decodeUnit->enqueueTimeUs);
        vs.temp_stats.submittedFrames++;
        if (vs.need_idr_on_resume) {
            vs.need_idr_on_resume = false;
            return DR_NEED_IDR;
        }
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_REQUEST_KEYFRAME) {
        unsigned long now = SDL_GetTicks();
        if (vs.last_idr_request_ms == 0 || now - vs.last_idr_request_ms >= IDR_REQUEST_MIN_INTERVAL_MS) {
            vs.last_idr_request_ms = now;
            return DR_NEED_IDR;
        }
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_NOT_READY) {
        /* A transient NOT_READY (HDR toggle / resolution change) is expected. Only a
         * decoder that stays NOT_READY well past any legitimate exclusive operation
         * gets torn down — otherwise the stream would silently freeze forever. */
        vs.need_idr_on_resume = true;
        unsigned long now = SDL_GetTicks();
        if (vs.not_ready_first_ms == 0) {
            vs.not_ready_first_ms = now;
        } else if (now - vs.not_ready_first_ms > VDEC_NOT_READY_WEDGE_MS) {
            commons_log_error("Session", "Video decoder wedged in NOT_READY for %lu ms; interrupting",
                              now - vs.not_ready_first_ms);
            vs.not_ready_first_ms = 0;
            session_interrupt(vs.session, false, STREAMING_INTERRUPT_DECODER);
        }
        return DR_OK;
    } else {
        /* Escalating recovery: upstream interrupts on the first feed error, but a single
         * transient NDL error is survivable — drop the frame and ask for an IDR. Only
         * VDEC_FEED_ERROR_LIMIT failures inside the window tear the session down (a truly
         * wedged decoder fails every Feed, so that still fires within a few frames). */
        unsigned long now = SDL_GetTicks();
        if (vs.feed_error_count == 0 || now - vs.feed_error_first_ms > VDEC_FEED_ERROR_WINDOW_MS) {
            vs.feed_error_count = 0;
            vs.feed_error_first_ms = now;
        }
        vs.feed_error_count++;
        if (vs.feed_error_count >= VDEC_FEED_ERROR_LIMIT) {
            commons_log_error("Session", "Video feed error %d (%d consecutive)", result, vs.feed_error_count);
            session_interrupt(vs.session, false, STREAMING_INTERRUPT_DECODER);
            return DR_OK;
        }
        commons_log_warn("Session", "Video feed error %d; dropping frame, requesting IDR (%d/%d)",
                         result, vs.feed_error_count, VDEC_FEED_ERROR_LIMIT);
        return DR_NEED_IDR;
    }
}

int vdec_delegate_submit(PDECODE_UNIT decodeUnit) {
    if ((size_t) decodeUnit->fullLength > reasm.size) {
        if ((size_t) decodeUnit->fullLength > DECODER_BUFFER_MAX_SIZE) {
            commons_log_error("Session", "Decode unit %d bytes exceeds %zu byte cap, dropping",
                              decodeUnit->fullLength, (size_t) DECODER_BUFFER_MAX_SIZE);
            return DR_NEED_IDR;
        }
        size_t new_size = reasm.size > 0 ? reasm.size : reasm.initial_size;
        if (new_size == 0) {
            new_size = vdec_buffer_initial_bytes();
        }
        while (new_size < (size_t) decodeUnit->fullLength) {
            new_size *= 2;
        }
        unsigned char *new_buffer = realloc(reasm.data, new_size);
        if (new_buffer == NULL) {
            commons_log_error("Session", "Failed to grow decode buffer to %zu bytes", new_size);
            return DR_NEED_IDR;
        }
        commons_log_info("Session", "Grew decode buffer %zu -> %zu bytes (frame needed %d)",
                         reasm.size, new_size, decodeUnit->fullLength);
        reasm.data = new_buffer;
        reasm.size = new_size;
    }
    unsigned long ticksms = SDL_GetTicks();
    if (vs.lastFrameNumber <= 0) {
        vs.temp_stats.measurementStartTimestamp = ticksms;
        vs.lastFrameNumber = decodeUnit->frameNumber;
    } else {
        vs.temp_stats.networkDroppedFrames += decodeUnit->frameNumber - (vs.lastFrameNumber + 1);
        vs.temp_stats.totalFrames += decodeUnit->frameNumber - (vs.lastFrameNumber + 1);
        vs.lastFrameNumber = decodeUnit->frameNumber;
    }
    unsigned stats_window_ms = streaming_stats_shown() ? 1000u : 2000u;
    if (ticksms - vs.temp_stats.measurementStartTimestamp > stats_window_ms) {
        vdec_stat_submit(&vs.temp_stats, ticksms);
        memset(&vs.temp_stats, 0, sizeof(vs.temp_stats));
        vs.temp_stats.measurementStartTimestamp = ticksms;
    }

    vs.temp_stats.receivedFrames++;
    vs.temp_stats.totalFrames++;
    vs.temp_stats.receivedBytes += (uint64_t) decodeUnit->fullLength;

    vs.temp_stats.totalCaptureLatency += decodeUnit->frameHostProcessingLatency;
    vs.temp_stats.totalReassemblyTimeUs += (uint32_t) (decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs);
    vdec_stream_info.has_host_latency |= decodeUnit->frameHostProcessingLatency > 0;
    if (!vs.warned_near_buffer_limit && reasm.initial_size > 0 &&
        (size_t) decodeUnit->fullLength > (reasm.initial_size * 9 / 10)) {
        vs.warned_near_buffer_limit = true;
        commons_log_warn("Session", "Video frame size %d is near initial decoder buffer (%zu)",
                         decodeUnit->fullLength, reasm.initial_size);
    }
    size_t length = 0;
    PLENTRY entry = decodeUnit->bufferList;
    if (entry != NULL && entry->next == NULL) {
        memcpy(reasm.data, entry->data, entry->length);
        length = (size_t) entry->length;
    } else {
        for (; entry != NULL; entry = entry->next) {
            memcpy(reasm.data + length, entry->data, entry->length);
            length += entry->length;
        }
    }
    SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
    }
    /* Host capture PTS (RTP 90kHz → µs); -1 = no host PTS, ss4s falls back to
     * arrival wall-clock. Only takes effect when smooth pacing env is ON. */
    const int64_t pts_us = decodeUnit->presentationTimeUs > 0 ? (int64_t) decodeUnit->presentationTimeUs : -1;
    SS4S_VideoFeedResult result = SS4S_PlayerVideoFeedWithPTS(vs.player, reasm.data, length, flags, pts_us);
    return vdec_finish_feed(result, decodeUnit);
}

static inline void vdec_stats_write_begin(void) {
    /* Atomic increment so the odd (write-in-progress) value is always published
     * to readers; a plain ++ may be register-coalesced with write_end's ++ under
     * -O2/LTO, hiding the odd state the reader's seqlock relies on. */
    __atomic_add_fetch(&vdec_stats_seq, 1, __ATOMIC_RELAXED); /* odd: write in progress */
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void vdec_stats_write_end(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&vdec_stats_seq, 1, __ATOMIC_RELAXED); /* even: consistent */
}

int vdec_negotiated_format(void) {
    return vs.stream_format;
}

void vdec_stats_snapshot(struct VIDEO_STATS *out) {
    unsigned s1, s2;
    do {
        s1 = __atomic_load_n(&vdec_stats_seq, __ATOMIC_ACQUIRE);
        memcpy(out, &vdec_summary_stats, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n(&vdec_stats_seq, __ATOMIC_RELAXED);
    } while ((s1 & 1u) != 0 || s1 != s2);
}

void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now) {
    struct VIDEO_STATS *dst = &vdec_summary_stats;
    vdec_stats_write_begin();
    memcpy(dst, src, sizeof(struct VIDEO_STATS));
    unsigned long delta = now - dst->measurementStartTimestamp;
    if (delta <= 0) {
        vdec_stats_write_end();
        return;
    }
    dst->totalFps = (float) dst->totalFrames / ((float) delta / 1000);
    dst->receivedFps = (float) dst->receivedFrames / ((float) delta / 1000);
    dst->decodedFps = (float) dst->submittedFrames / ((float) delta / 1000);
    dst->currentBitrateKbps = (uint32_t) ((dst->receivedBytes * 8) / (delta / 1000.0f));
    const bool show_stats = streaming_stats_shown();
    if (show_stats) {
        LiGetEstimatedRttInfo(&dst->rtt, &dst->rttVariance);
    }
    /* Sample the decoder latency unconditionally: soft recovery needs it even while
     * the stats overlay is closed, so the old !show_stats early-return is gone.
     * SS4S_PlayerGetVideoLatency only reads ss4s' own stats counter, so it is cheap
     * and — unlike the render-queue depth, which is Starfish/SMP-only — available
     * on our NDL decoder. */
    int latencyUs = 0;
    if (vs.player != NULL && SS4S_PlayerGetVideoLatency(vs.player, 0, &latencyUs)) {
        dst->avgDecoderLatency = (float) latencyUs / 1000.0f;
        vdec_stream_info.has_decoder_latency = true;
    } else {
        dst->avgDecoderLatency = 0;
    }
    /* Upstream samples the render-queue depth here via SS4S_PlayerGetVideoRenderQueueLength.
     * That entry point is Starfish/SMP-only and does not exist in our ss4s fork at all — we
     * decode through NDL, where the queue depth is never exposed. Sampling it would mean
     * carrying a submodule bump for a value that is structurally always absent, so the flag
     * simply stays false and soft recovery relies on decoder latency + the fps gap instead. */
    vdec_stream_info.has_render_queue = false;
    vdec_stats_write_end();

#if defined(TARGET_WEBOS)
    soft_recovery_tick(dst);
#endif


    if (show_stats) {
        app_bus_post(vs.session->app, (bus_actionfunc) streaming_refresh_stats, NULL);
    }
}

void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info) {
    if (decodeUnit->frameType != FRAME_TYPE_IDR) { return; }
    if (vs.stream_format & VIDEO_FORMAT_MASK_AV1) {
        return;
    }
    for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
        if (entry->bufferType != BUFFER_TYPE_SPS) { continue; }
        sps_dimension_t dimension;
        if (vs.stream_format & VIDEO_FORMAT_MASK_H264) {
            sps_parse_dimension_h264((const unsigned char *) &entry->data[4], &dimension);
        } else if (vs.stream_format & VIDEO_FORMAT_MASK_H265) {
            sps_parse_dimension_hevc((const unsigned char *) &entry->data[4], &dimension);
        } else {
            info->width = info->height = -1;
            return;
        }
        info->width = dimension.width;
        info->height = dimension.height;
        return;
    }
}
