/*
 * Pseudo-VRR presentation scheduler for present-on-arrival decoders.
 *
 * webOS NDL shows each HEVC frame as soon as it is decoded; the PTS we hand it is
 * not used for scheduling (live bias sweep 2026-07-01: render queue flat at one
 * frame for every fed offset). The panel itself runs at twice the measured video
 * rate (144 Hz for a 72 fps HEVC stream), so the only lever on WHEN a frame
 * appears is WHEN we feed it. This module decides that moment from the host's
 * RTP timestamps, so the presented spacing follows the game's own cadence instead
 * of the network's arrival jitter.
 *
 * The cadence smoother, the offset tracker and their constants are a C port of the
 * timing controller in Nonary's moonlight-qt VRR branch
 * (https://github.com/Nonary/moonlight-qt, branch vrr17 @ 1ad5848b,
 * app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp;
 * upstream PR moonlight-stream/moonlight-qt#1956). That file is GPLv3 like this
 * project. What was ported and what was left out is listed in vrr_timing.c.
 *
 * Pure C, no platform calls: every time is a microsecond value on the caller's
 * monotonic clock, so the unit tests drive it with a synthetic clock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum vrr_timing_profile_t {
    VRR_TIMING_OFF = 0,
    /** Reserve up to one source frame (10 ms ceiling), p99 lateness covered. */
    VRR_TIMING_PROFILE_LOW_LATENCY = 1,
    /** Up to two source frames (16 ms ceiling), p99.5 covered. */
    VRR_TIMING_PROFILE_BALANCED = 2,
    /** Up to four source frames (24 ms ceiling), p99.99 covered. */
    VRR_TIMING_PROFILE_SMOOTH = 3,
} vrr_timing_profile_t;

/* Ring capacities. Offset window is 3 s of host time: 720 entries at 240 fps. */
#define VRR_TIMING_OFFSET_CAP 1024u
#define VRR_TIMING_LATE_CAP 1024u
#define VRR_TIMING_CADENCE_WINDOW 4u
#define VRR_TIMING_FIT_WINDOW 15u

typedef struct vrr_timing_offset_sample_t {
    int64_t at_us;
    int64_t offset_us;
} vrr_timing_offset_sample_t;

typedef struct vrr_timing_t {
    /* Configuration. */
    vrr_timing_profile_t profile;
    bool reduce_judder;
    double nominal_period_us;

    /* Host clock, unwrapped from the 32-bit 90 kHz RTP stamp. */
    bool have_frame;
    uint32_t last_rtp;
    int64_t host_ticks;
    int64_t last_host_us;
    int64_t last_ready_us;
    int last_frame_number;
    /* Set by vrr_timing_break_cadence(): the next frame does not continue the last. */
    bool force_break;

    /* Sender-to-local offset: monotonic-min deque over 3 s of host time, and the
     * applied offset that slews toward that minimum. */
    vrr_timing_offset_sample_t offq[VRR_TIMING_OFFSET_CAP];
    unsigned offq_head, offq_len;
    bool offset_valid;
    int64_t applied_offset_us;
    uint32_t offset_samples;
    int64_t offset_last_obs_us;
    int64_t offset_slew_remainder;

    /* Source period: EMA, re-seeded from a median fit when they disagree by 25 %. */
    double period_us;
    int64_t fit[VRR_TIMING_FIT_WINDOW];
    unsigned fit_idx, fit_len;

    /* Cadence smoothing ("reduce judder"). */
    int64_t cadence[VRR_TIMING_CADENCE_WINDOW];
    unsigned cadence_idx, cadence_len;
    bool have_basis;
    double last_basis_us;
    double last_retiming_us;
    bool smoothing_engaged;
    /* Below ~40 fps the hold costs more than it smooths: frames go out on arrival. */
    bool suspended;

    /* Playout delay (the reserve that absorbs arrival jitter). */
    double delay_us;
    int32_t late[VRR_TIMING_LATE_CAP];
    unsigned late_idx, late_len;
    unsigned late_since_eval;
    double delay_target_us;
    double growth_credit_us;
    int burst_left;
    int64_t last_pressure_us;
    int64_t last_delay_update_us;

    /* Counters for the log. */
    uint32_t epoch_resets;
    uint32_t cadence_resets;
    uint32_t suspends;
} vrr_timing_t;

typedef struct vrr_timing_decision_t {
    /** Local time at which to hand the frame to the decoder. */
    int64_t target_us;
    /** Unwrapped host timestamp of this frame (µs since the current epoch). */
    int64_t host_us;
    /** host_us mapped to the local clock plus the playout delay, before smoothing. */
    int64_t raw_slot_us;
    int64_t delay_us;
    int64_t retiming_us;
    /** Local arrival minus the mapped slot without delay: the jitter this frame carried. */
    int64_t lateness_us;
    /** A new host epoch started on this frame (offset and cadence forgotten). */
    bool epoch_reset;
    /** This frame does not continue the previous one (gap, stall, frame loss). Its
     * spacing to the previous frame is not a cadence sample. */
    bool cadence_break;
    /** Cadence smoothing moved this frame. */
    bool smoothed;
    /** Source rate below the pacing floor: target is the arrival time, no hold. */
    bool suspended;
} vrr_timing_decision_t;

/** fps_x100: the negotiated stream rate (e.g. 7200), used to seed the period. */
void vrr_timing_init(vrr_timing_t *t, vrr_timing_profile_t profile, bool reduce_judder, int fps_x100);

/** Forget the sender clock mapping and the cadence. The learnt delay is kept, as
 * Nonary keeps earned protection across rate changes. */
void vrr_timing_reset_epoch(vrr_timing_t *t);

/** The next frame starts a new cadence (decoder reload, feed error, NOT_READY). */
void vrr_timing_break_cadence(vrr_timing_t *t);

/**
 * Schedule one frame.
 * rtp_ts        the frame's 90 kHz RTP timestamp (DECODE_UNIT.rtpTimestamp)
 * frame_number  DECODE_UNIT.frameNumber, to detect lost frames
 * keyframe      IDR frames are large and complete late by construction; they are
 *               scheduled like any other frame but kept out of the jitter statistic
 * ready_us      local time the frame was fully reassembled (DECODE_UNIT.enqueueTimeUs)
 */
void vrr_timing_schedule(vrr_timing_t *t, uint32_t rtp_ts, int frame_number, bool keyframe, int64_t ready_us,
                         vrr_timing_decision_t *out);

/** Frames waiting behind the held one that end the hold early (1/2/4 per profile). */
int vrr_timing_queue_frames(const vrr_timing_t *t);

/** Upper bound for any single hold: the delay cap plus the 6 ms smoothing budget. */
int64_t vrr_timing_max_hold_us(const vrr_timing_t *t);

/** Current delay ceiling for this profile at the tracked source period. */
int64_t vrr_timing_delay_cap_us(const vrr_timing_t *t);

const char *vrr_timing_profile_name(vrr_timing_profile_t profile);

/* ---------------------------------------------------------------------------------
 * Spacing metric: how far the presented spacing of consecutive frames departs from
 * the host's spacing. |Δ feed − Δ host PTS| per pair, next to the same number for
 * arrival times (what feeding on arrival would have produced), so one log line
 * holds both the baseline and the result.
 * ------------------------------------------------------------------------------- */

#define VRR_METRICS_CAP 4096u

typedef struct vrr_metrics_t {
    uint16_t spacing_us[VRR_METRICS_CAP];
    uint16_t arrival_us[VRR_METRICS_CAP];
    uint16_t held_us[VRR_METRICS_CAP];
    unsigned pairs;
    unsigned frames;
    unsigned immediate;
    unsigned preempted;
    unsigned suspended;
    int64_t window_start_us;
    bool have_prev;
    int64_t prev_host_us, prev_ready_us, prev_present_us;
} vrr_metrics_t;

typedef struct vrr_metrics_summary_t {
    unsigned frames;
    unsigned pairs;
    float spacing_p50_ms, spacing_p90_ms, spacing_p99_ms;
    float arrival_p50_ms, arrival_p90_ms, arrival_p99_ms;
    float held_p50_ms, held_max_ms;
    /** Share of frames fed without waiting: already late at the decision. */
    float immediate_pct;
    /** Share of frames whose hold was cut short by a newer frame waiting. */
    float preempted_pct;
    /** Share of frames fed on arrival because the source rate was below the floor. */
    float suspended_pct;
} vrr_metrics_summary_t;

void vrr_metrics_reset(vrr_metrics_t *m);

/** pair_valid=false (epoch/cadence break) keeps this frame out of the spacing pairs. */
void vrr_metrics_record(vrr_metrics_t *m, int64_t host_us, int64_t ready_us, int64_t present_us, bool pair_valid,
                        bool immediate, bool preempted, bool suspended);

/** When window_us has passed since the window began: fill *out, start a new window
 * and return true. The first call only opens the window. */
bool vrr_metrics_flush(vrr_metrics_t *m, int64_t now_us, int64_t window_us, vrr_metrics_summary_t *out);
