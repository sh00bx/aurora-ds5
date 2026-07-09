#include "session_video.h"

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
#include "app.h"

#include <SDL.h>
#include <assert.h>

// Starting capacity for the decode-unit reassembly buffer. Grows on
// demand to accommodate larger frames (e.g. 4K IDR frames at high
// bitrate routinely exceed 2 MB), capped to keep a malformed stream
// from exhausting memory.
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

static int vdec_stream_target_fps = 60;

static int frames_since_idr = 0;

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static unsigned char *buffer = NULL;
static size_t buffer_size = 0;
static size_t buffer_initial_size = 0;
static int lastFrameNumber;
// Set when SS4S_PlayerVideoFeed returns NOT_READY (decoder is in an
// exclusive op like HDR toggle or resolution change). Consumed on the
// next successful Feed: we ask Limelight for one IDR so the decoder
// can resync from a known-good keyframe instead of decoding the next
// P-frame against a discontinuity.
static bool need_idr_on_resume = false;
// Escalating recovery for SS4S_VIDEO_FEED_ERROR: a single transient NDL
// error drops the frame and requests an IDR; only N consecutive failures
// within a short window interrupt the session (a wedged decoder fails
// every Feed, so escalation still fires within a few frames).
#define VDEC_FEED_ERROR_LIMIT 3
#define VDEC_FEED_ERROR_WINDOW_MS 2000
static int feed_error_count = 0;
static unsigned long feed_error_first_ms = 0;
// A transient NOT_READY (HDR toggle / resolution change) is expected and must
// not tear down the stream. But a decoder that stays NOT_READY forever would
// silently freeze with no recovery, so a generous watchdog still interrupts if
// NOT_READY persists continuously well beyond any legitimate exclusive op.
#define VDEC_NOT_READY_WEDGE_MS 5000
static unsigned long not_ready_first_ms = 0;
static struct VIDEO_STATS vdec_temp_stats;
static int vdec_stream_format = 0;
static bool vdec_warned_near_buffer_limit;
VIDEO_STATS vdec_summary_stats;
/* Seqlock for vdec_summary_stats: odd while vdec_stat_submit is mid-write.
 * Single writer (session thread); cross-thread readers use vdec_stats_snapshot. */
static unsigned vdec_stats_seq;
VIDEO_INFO vdec_stream_info;

static int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags);

static void vdec_delegate_cleanup();

static int vdec_delegate_submit(PDECODE_UNIT decodeUnit);

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
    session = context;
    player = session->player;
    buffer_initial_size = vdec_buffer_initial_bytes();
    /* P15: reuse a persistent reassembly buffer across streams so the malloc
     * stays off this connection-setup hot path. Allocate only on the first
     * stream; a prior grow-on-demand keeps the larger capacity for reuse
     * (the buffer is retained by vdec_delegate_cleanup, freed at process exit). */
    if (buffer == NULL) {
        buffer_size = buffer_initial_size;
        buffer = malloc(buffer_size);
        if (buffer) {
            /* Pre-fault the pages here (one-time, off the frame deadline) so the
             * first big IDR doesn't pay first-touch faults under MCL_ONFAULT.
             * NOT done on the grow-on-demand realloc path, which runs in
             * vdec_delegate_submit under the frame deadline. */
            memset(buffer, 0, buffer_size);
        }
    }
    if (!buffer) {
        /* Pre-alloc/fallback malloc failed: bail with a decoder error instead of
         * NULL-dereferencing in the reassembly memcpy on the first frame. */
        commons_log_error("Session", "Failed to allocate video reassembly buffer (%zu bytes)", buffer_size);
        buffer_size = 0;
        return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
    memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
    memset(&vdec_stream_info, 0, sizeof(vdec_stream_info));
    vdec_stream_format = videoFormat;
    vdec_stream_info.format = video_format_name(videoFormat);
    lastFrameNumber = 0;
    need_idr_on_resume = false;
    feed_error_count = 0;
    feed_error_first_ms = 0;
    frames_since_idr = 0;
    vdec_stream_target_fps = redrawRate > 0 ? redrawRate : 60;
    vdec_warned_near_buffer_limit = false;

    if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        vdec_stream_info.width = width;
        vdec_stream_info.height = height;
    }

    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    info.frameRateNumerator = vdec_stream_target_fps;
    info.frameRateDenominator = 1;
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
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        }
    }

    app_t *app = session->app;
    if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_EXCLUSIVE) {
        app_bus_post_sync(app, (bus_actionfunc) app_ui_close, &app->ui);
    }

    switch (SS4S_PlayerVideoOpen(player, &info)) {
        case SS4S_VIDEO_OPEN_OK: {
            return 0;
        }
        case SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC:
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        default:
            return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
}

void vdec_delegate_cleanup() {
    assert(player != NULL);
    /* P15: keep the reassembly buffer allocated across streams so the malloc
     * stays off the connection-setup hot path; it is reused by the next
     * vdec_delegate_setup and released at process exit. */
    SS4S_PlayerVideoClose(player);
    session = NULL;
}

int vdec_delegate_submit(PDECODE_UNIT decodeUnit) {
    if ((size_t) decodeUnit->fullLength > buffer_size) {
        if ((size_t) decodeUnit->fullLength > DECODER_BUFFER_MAX_SIZE) {
            commons_log_error("Session", "Decode unit %d bytes exceeds %zu byte cap, dropping",
                              decodeUnit->fullLength, (size_t) DECODER_BUFFER_MAX_SIZE);
            return DR_NEED_IDR;
        }
        size_t new_size = buffer_size > 0 ? buffer_size : buffer_initial_size;
        if (new_size == 0) {
            new_size = vdec_buffer_initial_bytes();
        }
        while (new_size < (size_t) decodeUnit->fullLength) {
            new_size *= 2;
        }
        unsigned char *new_buffer = realloc(buffer, new_size);
        if (new_buffer == NULL) {
            commons_log_error("Session", "Failed to grow decode buffer to %zu bytes", new_size);
            return DR_NEED_IDR;
        }
        commons_log_info("Session", "Grew decode buffer %zu -> %zu bytes (frame needed %d)",
                         buffer_size, new_size, decodeUnit->fullLength);
        buffer = new_buffer;
        buffer_size = new_size;
    }
    unsigned long ticksms = SDL_GetTicks();
    if (lastFrameNumber <= 0) {
        vdec_temp_stats.measurementStartTimestamp = ticksms;
        lastFrameNumber = decodeUnit->frameNumber;
    } else {
        // Any frame number greater than m_LastFrameNumber + 1 represents a dropped frame
        vdec_temp_stats.networkDroppedFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        vdec_temp_stats.totalFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        lastFrameNumber = decodeUnit->frameNumber;
    }
    unsigned stats_window_ms = streaming_stats_shown() ? 1000u : 2000u;
    if (ticksms - vdec_temp_stats.measurementStartTimestamp > stats_window_ms) {
        vdec_stat_submit(&vdec_temp_stats, ticksms);

        // Move this window into the last window slot and clear it for next window
        memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
        vdec_temp_stats.measurementStartTimestamp = ticksms;
    }

    vdec_temp_stats.receivedFrames++;
    vdec_temp_stats.totalFrames++;
    vdec_temp_stats.receivedBytes += (uint64_t) decodeUnit->fullLength;

    vdec_temp_stats.totalCaptureLatency += decodeUnit->frameHostProcessingLatency;
    // mcc moved DECODE_UNIT timestamps from ms to us (upstream e356b2c/a3ebaaf, pulled
    // in by the nanors rebase); convert back to the ms unit the uint32_t accumulator and
    // the stats UI (streaming.controller "submitMs") expect.
    vdec_temp_stats.totalReassemblyTime += (uint32_t) ((decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs) / 1000);
    vdec_stream_info.has_host_latency |= decodeUnit->frameHostProcessingLatency > 0;
    if (!vdec_warned_near_buffer_limit && buffer_initial_size > 0 &&
        (size_t) decodeUnit->fullLength > (buffer_initial_size * 9 / 10)) {
        vdec_warned_near_buffer_limit = true;
        commons_log_warn("Session", "Video frame size %d is near initial decoder buffer (%zu)",
                         decodeUnit->fullLength, buffer_initial_size);
    }
    size_t length = 0;
    PLENTRY entry = decodeUnit->bufferList;
    if (entry != NULL && entry->next == NULL) {
        memcpy(buffer, entry->data, entry->length);
        length = (size_t) entry->length;
    } else {
        for (; entry != NULL; entry = entry->next) {
            memcpy(buffer + length, entry->data, entry->length);
            length += entry->length;
        }
    }
    SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
    }
    SS4S_VideoFeedResult result = SS4S_PlayerVideoFeed(player, buffer, length, flags);
    if (result == SS4S_VIDEO_FEED_OK) {
        feed_error_count = 0;
        not_ready_first_ms = 0;
        if (decodeUnit->frameType == FRAME_TYPE_IDR) {
            frames_since_idr = 0;
        } else {
            frames_since_idr++;
        }
        const int idr_interval = app_configuration ? app_configuration->idr_refresh_interval_sec : 0;
        const bool hevc_stream = vdec_stream_format == VIDEO_FORMAT_H265 ||
                                 vdec_stream_format == VIDEO_FORMAT_H265_MAIN10;
        if (hevc_stream && idr_interval >= 2 && vdec_stream_target_fps > 0 &&
            frames_since_idr >= vdec_stream_target_fps * idr_interval) {
            LiRequestIdrFrame();
            frames_since_idr = 0;
        }
        if (vdec_stream_info.width == 0 || vdec_stream_info.height == 0) {
            stream_info_parse_size(decodeUnit, &vdec_stream_info);
        }
        vdec_temp_stats.totalSubmitTime += (uint32_t) ((LiGetMicroseconds() - decodeUnit->enqueueTimeUs) / 1000);
        vdec_temp_stats.submittedFrames++;
        if (need_idr_on_resume) {
            need_idr_on_resume = false;
            return DR_NEED_IDR;
        }
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_REQUEST_KEYFRAME) {
        return DR_NEED_IDR;
    } else if (result == SS4S_VIDEO_FEED_NOT_READY) {
        need_idr_on_resume = true;
        unsigned long now = SDL_GetTicks();
        if (not_ready_first_ms == 0) {
            not_ready_first_ms = now;
        } else if (now - not_ready_first_ms > VDEC_NOT_READY_WEDGE_MS) {
            commons_log_error("Session", "Video decoder wedged in NOT_READY for %lu ms; interrupting",
                              now - not_ready_first_ms);
            not_ready_first_ms = 0;
            session_interrupt(session, false, STREAMING_INTERRUPT_DECODER);
        }
        return DR_OK;
    } else {
        unsigned long now = SDL_GetTicks();
        if (feed_error_count == 0 || now - feed_error_first_ms > VDEC_FEED_ERROR_WINDOW_MS) {
            feed_error_count = 0;
            feed_error_first_ms = now;
        }
        feed_error_count++;
        if (feed_error_count >= VDEC_FEED_ERROR_LIMIT) {
            commons_log_error("Session", "Video feed error %d (%d consecutive)", result, feed_error_count);
            session_interrupt(session, false, STREAMING_INTERRUPT_DECODER);
            return DR_OK;
        }
        commons_log_warn("Session", "Video feed error %d; dropping frame, requesting IDR (%d/%d)",
                         result, feed_error_count, VDEC_FEED_ERROR_LIMIT);
        return DR_NEED_IDR;
    }
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
    if (!show_stats) {
        vdec_stats_write_end();
        return;
    }
    int latencyUs = 0;
    if (SS4S_PlayerGetVideoLatency(player, 0, &latencyUs)) {
        dst->avgDecoderLatency = (float) latencyUs / 1000.0f;
        vdec_stream_info.has_decoder_latency = true;
    } else {
        dst->avgDecoderLatency = 0;
    }
    vdec_stats_write_end();
    app_bus_post(session->app, (bus_actionfunc) streaming_refresh_stats, NULL);
}

void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info) {
    if (decodeUnit->frameType != FRAME_TYPE_IDR) { return; }
    if (vdec_stream_format & VIDEO_FORMAT_MASK_AV1) {
        return;
    }
    for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
        if (entry->bufferType != BUFFER_TYPE_SPS) { continue; }
        sps_dimension_t dimension;
        if (vdec_stream_format & VIDEO_FORMAT_MASK_H264) {
            sps_parse_dimension_h264((const unsigned char *) &entry->data[4], &dimension);
        } else if (vdec_stream_format & VIDEO_FORMAT_MASK_H265) {
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
