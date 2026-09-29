#pragma once

#include <Limelight.h>
#include <stdbool.h>

#include "vrr_timing.h"

struct app_settings_t;

extern struct VIDEO_STATS vdec_summary_stats;
extern struct VIDEO_INFO vdec_stream_info;
extern struct AUDIO_INFO audio_stream_info;

extern DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks;

/** Tear-free copy of vdec_summary_stats for cross-thread readers (seqlock retry). */
void vdec_stats_snapshot(struct VIDEO_STATS *out);

/** VIDEO_FORMAT_* the host actually negotiated for this stream, 0 before the first setup. */
int vdec_negotiated_format(void);

/** Call before LiStartConnection. Sets decoder capabilities from settings (RFI + slices for HEVC/AV1 when enabled). */
void session_video_prepare_stream(void);

/** Pseudo-VRR profile (1..3) the next stream will run with, 0 when off or not applicable
 * (not webOS, HEVC off, or AV1 on). Also decides whether clientVrrRequested is sent. */
int session_video_pseudo_vrr_profile(const struct app_settings_t *cfg);

/** Last 10 s Pseudo-VRR window (tear-free). False while no window has completed in this
 * stream or the mode is off. delay_us may be NULL. */
bool vdec_vrr_snapshot(vrr_metrics_summary_t *out, int *delay_us);
