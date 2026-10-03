/*
 * Pseudo-VRR presentation scheduler — see vrr_timing.h.
 *
 * Ported from Nonary's moonlight-qt vrr17 (GPLv3), vrrtimingcontroller.cpp:
 *   - cadenceSmoothingAdjustUs(): 85 % predicted slot / 15 % raw slot
 *     (kPlayoutSmoothingGainPerMille = 150), 2.5 % period EMA
 *     (kPlayoutSmoothingPeriodAlphaPerMille = 25), 2 % phase feedback into the period
 *     (kPlayoutSmoothingPeriodFeedbackPerMillion = 20000), positive retiming capped
 *     at 6 ms (kPlayoutSmoothingMaxLagUs), never earlier than the mapped source slot,
 *     snap/reset beyond three periods (kPlayoutMetronomeSnapPerMille = 3000), bounded
 *     interval test (<= 2.5 periods, >= half a period), four-interval cadence
 *     qualification (playoutSmoothingWindowedCadence), re-seed from the rate fit when
 *     the EMA is off by a quarter, 1 ms/frame slew back after a reset
 *     (kPlayoutSmoothingResetSlewUs).
 *   - observePlayoutOffset(): window minimum of (local - sender) over 3 s of sender
 *     time, 64 warm-up samples that adopt a lower minimum at once, then a slew of
 *     2400 us/s with a 100 us step cap per observation.
 *   - Profile allowances 1 / 2 / 4 source frames with 16 / 16 / 24 ms ceilings (low
 *     latency lowered to 10 ms here),
 *     on-time targets 99 / 99.5 / 99.99 %, start delay 6 ms, minimum 1 ms, margin
 *     0.5 ms, growth <= 250 us per 250 ms and <= 125 us per frame, holds 6 / 8 / 10 s and releases
 *     125 / 250 / 50 us/s (docs/vrr17-calibration.md, "Selected policy").
 *
 * Deliberately NOT ported: the swapchain/present-feedback machinery (NDL gives no
 * present callback), GPU-readiness prediction, the smoothing reserve (p98 of
 * smoother-caused shortfall — our decoder cannot be "not ready" at its slot, the
 * frame is already complete when we schedule it), the interval-quality growth gate
 * (replaced by a plain lateness percentile over the last 1024 frames), metronome
 * mode, rate-epoch memory and the replay/trace plumbing.
 */
#include "vrr_timing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define US_PER_S 1000000LL
#define RTP_HZ 90000LL

#define OFFSET_WINDOW_US (3 * US_PER_S)
#define OFFSET_WARMUP_SAMPLES 64u
#define OFFSET_SLEW_US_PER_S 2400LL
#define OFFSET_MAX_STEP_US 100LL

#define SMOOTH_GAIN 0.15          /* weight of the raw slot */
#define PERIOD_ALPHA 0.025
#define PERIOD_FEEDBACK 0.02
#define SMOOTH_MAX_LAG_US 6000.0
#define SMOOTH_SNAP_PERIODS 3.0
#define SMOOTH_RESET_SLEW_US 1000.0

#define DELAY_START_US 6000.0
#define DELAY_MIN_US 1000.0
#define DELAY_MARGIN_US 500.0
#define DELAY_ATTACK_US 125.0          /* per frame */
#define DELAY_GROWTH_US_PER_S 1000.0   /* 250 us per 250 ms */
#define DELAY_GROWTH_BURST_US 250.0
/* A quantile over fewer frames is just the maximum, i.e. whichever spike came
 * first; vrr17 likewise qualifies initial learning before it may grow. */
#define DELAY_MIN_SAMPLES 256u
#define DELAY_EVAL_EVERY 8u

/* A frame is a new cadence after this much silence on either clock. */
#define CADENCE_GAP_US (100 * 1000LL)
/* Sender and receiver disagree by this much about one gap: a new sender epoch. */
#define EPOCH_MISMATCH_US (1 * US_PER_S)
/* Frames after a stall arrive bunched; keep them out of the jitter statistic. */
#define BURST_EXCLUDE_FRAMES 3

/* Pacing floor. Below ~40 fps the hold made spacing worse than feeding on arrival
 * (TV 2026-10-01, low latency: menu/desktop at ~28 fps p90/p99 6.1/13.6 ms against
 * 4.7/12.3 on arrival; 2026-09-30 balanced 20-40 fps p99 16.7 against 7.5), while
 * from 55 fps up it still won. Frames go out on arrival below 38 fps and pacing
 * resumes above 42; the gap keeps a rate hovering at 40 from flapping. The
 * period, offset and delay keep learning meanwhile, so pacing resumes warm. */
#define SUSPEND_PERIOD_US (US_PER_S / 38.0)
#define RESUME_PERIOD_US (US_PER_S / 42.0)

typedef struct profile_params_t {
    double cap_frames;
    double cap_ceiling_us;
    double on_time_quantile;
    int64_t hold_us;
    double release_us_per_s;
    int queue_frames;
} profile_params_t;

static const profile_params_t PROFILE_PARAMS[] = {
        /* OFF: behaves like balanced if ever scheduled. */
        {2.0, 16000.0, 0.995, 8 * US_PER_S, 250.0, 2},
        /* Low latency: Nonary's 16 ms ceiling lowered to 10 ms — one 72 fps frame is
         * 13.9 ms, and every millisecond of reserve is a millisecond of latency. */
        {1.0, 10000.0, 0.99, 6 * US_PER_S, 125.0, 1},
        /* Balanced */
        {2.0, 16000.0, 0.995, 8 * US_PER_S, 250.0, 2},
        /* Smooth */
        {4.0, 24000.0, 0.9999, 10 * US_PER_S, 50.0, 4},
};

static const profile_params_t *params_of(const vrr_timing_t *t) {
    unsigned idx = (unsigned) t->profile;
    if (idx >= sizeof(PROFILE_PARAMS) / sizeof(PROFILE_PARAMS[0])) {
        idx = VRR_TIMING_PROFILE_BALANCED;
    }
    return &PROFILE_PARAMS[idx];
}

const char *vrr_timing_profile_name(vrr_timing_profile_t profile) {
    switch (profile) {
        case VRR_TIMING_PROFILE_LOW_LATENCY:
            return "low-latency";
        case VRR_TIMING_PROFILE_BALANCED:
            return "balanced";
        case VRR_TIMING_PROFILE_SMOOTH:
            return "smooth";
        default:
            return "off";
    }
}

static int64_t ticks_to_us(int64_t ticks) {
    /* Round to nearest: 90 kHz does not divide a microsecond evenly. */
    return ticks >= 0 ? (ticks * US_PER_S + RTP_HZ / 2) / RTP_HZ : -((-ticks * US_PER_S + RTP_HZ / 2) / RTP_HZ);
}

int64_t vrr_timing_delay_cap_us(const vrr_timing_t *t) {
    const profile_params_t *p = params_of(t);
    double period = t->period_us > 0 ? t->period_us : t->nominal_period_us;
    double cap = p->cap_frames * period;
    if (cap > p->cap_ceiling_us) {
        cap = p->cap_ceiling_us;
    }
    if (cap < DELAY_MIN_US) {
        cap = DELAY_MIN_US;
    }
    return (int64_t) cap;
}

int vrr_timing_queue_frames(const vrr_timing_t *t) {
    return params_of(t)->queue_frames;
}

int64_t vrr_timing_max_hold_us(const vrr_timing_t *t) {
    return vrr_timing_delay_cap_us(t) + (int64_t) SMOOTH_MAX_LAG_US;
}

static void reset_offset(vrr_timing_t *t) {
    t->offq_head = 0;
    t->offq_len = 0;
    t->offset_valid = false;
    t->applied_offset_us = 0;
    t->offset_samples = 0;
    t->offset_last_obs_us = 0;
    t->offset_slew_remainder = 0;
}

static void reset_cadence(vrr_timing_t *t) {
    t->cadence_len = 0;
    t->cadence_idx = 0;
    t->have_basis = false;
    t->smoothing_engaged = false;
}

void vrr_timing_init(vrr_timing_t *t, vrr_timing_profile_t profile, bool reduce_judder, int fps_x100) {
    memset(t, 0, sizeof(*t));
    t->profile = profile;
    t->reduce_judder = reduce_judder;
    if (fps_x100 < 1000) {
        fps_x100 = 6000;
    }
    t->nominal_period_us = 100.0 * (double) US_PER_S / (double) fps_x100;
    t->period_us = t->nominal_period_us;
    t->delay_us = DELAY_START_US;
    double cap = (double) vrr_timing_delay_cap_us(t);
    if (t->delay_us > cap) {
        t->delay_us = cap;
    }
    t->delay_target_us = t->delay_us;
}

void vrr_timing_reset_epoch(vrr_timing_t *t) {
    t->have_frame = false;
}

void vrr_timing_break_cadence(vrr_timing_t *t) {
    t->force_break = true;
}

/* ---- offset: window minimum over 3 s of sender time, slewed ---- */

static void offset_observe(vrr_timing_t *t, int64_t obs_us, int64_t offset_us) {
    /* Monotonic deque: entries increase in offset from head to tail, so the head is
     * the window minimum. */
    while (t->offq_len > 0) {
        unsigned back = (t->offq_head + t->offq_len - 1) % VRR_TIMING_OFFSET_CAP;
        if (t->offq[back].offset_us >= offset_us) {
            t->offq_len--;
        } else {
            break;
        }
    }
    if (t->offq_len == VRR_TIMING_OFFSET_CAP) {
        t->offq_head = (t->offq_head + 1) % VRR_TIMING_OFFSET_CAP;
        t->offq_len--;
    }
    unsigned slot = (t->offq_head + t->offq_len) % VRR_TIMING_OFFSET_CAP;
    t->offq[slot].at_us = obs_us;
    t->offq[slot].offset_us = offset_us;
    t->offq_len++;
    while (t->offq_len > 1 && t->offq[t->offq_head].at_us < obs_us - OFFSET_WINDOW_US) {
        t->offq_head = (t->offq_head + 1) % VRR_TIMING_OFFSET_CAP;
        t->offq_len--;
    }
    const int64_t window_min = t->offq[t->offq_head].offset_us;

    t->offset_samples++;
    if (!t->offset_valid) {
        t->applied_offset_us = offset_us;
        t->offset_valid = true;
        t->offset_last_obs_us = obs_us;
        t->offset_slew_remainder = 0;
        return;
    }
    int64_t elapsed = obs_us - t->offset_last_obs_us;
    if (elapsed < 0) {
        elapsed = 0;
    } else if (elapsed > US_PER_S) {
        elapsed = US_PER_S;
    }
    t->offset_last_obs_us = obs_us;
    if (t->offset_samples <= OFFSET_WARMUP_SAMPLES) {
        /* The first frame of an epoch is an arbitrary arrival: adopt an earlier one
         * at once so startup converges in a few frames. */
        if (window_min < t->applied_offset_us) {
            t->applied_offset_us = window_min;
        }
        t->offset_slew_remainder = 0;
        return;
    }
    const int64_t credit = elapsed * OFFSET_SLEW_US_PER_S + t->offset_slew_remainder;
    int64_t allowed = credit / US_PER_S;
    if (allowed >= OFFSET_MAX_STEP_US) {
        allowed = OFFSET_MAX_STEP_US;
        t->offset_slew_remainder = 0;
    } else {
        t->offset_slew_remainder = credit % US_PER_S;
    }
    if (window_min > t->applied_offset_us) {
        int64_t d = window_min - t->applied_offset_us;
        t->applied_offset_us += d < allowed ? d : allowed;
    } else if (window_min < t->applied_offset_us) {
        int64_t d = t->applied_offset_us - window_min;
        t->applied_offset_us -= d < allowed ? d : allowed;
    }
    if (t->applied_offset_us == window_min) {
        t->offset_slew_remainder = 0;
    }
}

/* ---- period: EMA with a median fit as the authority ---- */

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
    return (x > y) - (x < y);
}

static bool interval_bounded(const vrr_timing_t *t, int64_t interval_us) {
    /* One RTP quantum of slack so 4166/4167-style rounding never reads as a burst. */
    return (double) interval_us <= t->period_us * 2.5 && (double) (interval_us * 2 + 12) >= t->period_us;
}

/* Every continuous interval feeds the median fit, but only bounded ones step the EMA.
 * Gating the fit too locked the period out: after a long 22 fps stretch the period
 * sat near 45 ms, so 72 fps intervals (13.9 ms) failed interval_bounded() and were
 * never observed again (seen on the TV 2026-09-30: period 41-45 ms at a steady 72). */
static void period_observe(vrr_timing_t *t, int64_t interval_us, bool bounded) {
    t->fit[t->fit_idx] = interval_us;
    t->fit_idx = (t->fit_idx + 1) % VRR_TIMING_FIT_WINDOW;
    if (t->fit_len < VRR_TIMING_FIT_WINDOW) {
        t->fit_len++;
    }
    int64_t sorted[VRR_TIMING_FIT_WINDOW];
    memcpy(sorted, t->fit, sizeof(int64_t) * t->fit_len);
    qsort(sorted, t->fit_len, sizeof(int64_t), cmp_i64);
    const double fitted = (double) sorted[t->fit_len / 2];
    if (t->fit_len >= 5 && (t->period_us > fitted * 1.25 || t->period_us * 1.25 < fitted)) {
        /* A rate change the fit has absorbed, or a bad seed: follow the fit. */
        t->period_us = fitted;
        return;
    }
    if (!bounded) {
        return;
    }
    t->period_us += ((double) interval_us - t->period_us) * PERIOD_ALPHA;
    if (t->period_us < 1000.0) {
        t->period_us = 1000.0;
    }
}

/* ---- playout delay: cover the profile's on-time quantile of lateness ---- */

static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *) a, y = *(const int32_t *) b;
    return (x > y) - (x < y);
}

static void delay_update(vrr_timing_t *t, int64_t now_us) {
    const profile_params_t *p = params_of(t);
    const double cap = (double) vrr_timing_delay_cap_us(t);
    if (t->late_len >= DELAY_MIN_SAMPLES && ++t->late_since_eval >= DELAY_EVAL_EVERY) {
        t->late_since_eval = 0;
        int32_t sorted[VRR_TIMING_LATE_CAP];
        memcpy(sorted, t->late, sizeof(int32_t) * t->late_len);
        qsort(sorted, t->late_len, sizeof(int32_t), cmp_i32);
        unsigned rank = (unsigned) ceil(p->on_time_quantile * (double) t->late_len);
        if (rank < 1) {
            rank = 1;
        } else if (rank > t->late_len) {
            rank = t->late_len;
        }
        double target = (double) sorted[rank - 1] + DELAY_MARGIN_US;
        t->delay_target_us = target;
    }
    double target = t->delay_target_us;
    if (target < DELAY_MIN_US) {
        target = DELAY_MIN_US;
    } else if (target > cap) {
        target = cap;
    }
    int64_t elapsed = t->last_delay_update_us != 0 ? now_us - t->last_delay_update_us : 0;
    if (elapsed < 0) {
        elapsed = 0;
    } else if (elapsed > 100000) {
        elapsed = 100000;
    }
    t->last_delay_update_us = now_us;
    t->growth_credit_us += DELAY_GROWTH_US_PER_S * (double) elapsed / (double) US_PER_S;
    if (t->growth_credit_us > DELAY_GROWTH_BURST_US) {
        t->growth_credit_us = DELAY_GROWTH_BURST_US;
    }
    if (target > t->delay_us) {
        /* Growth is rate-limited like vrr17's (request <= 250 us per 250 ms, apply
         * <= 125 us per frame): a handful of late frames in one window must not
         * ratchet the delay up faster than the slow release can bring it back. */
        double step = target - t->delay_us;
        if (step > DELAY_ATTACK_US) {
            step = DELAY_ATTACK_US;
        }
        if (step > t->growth_credit_us) {
            step = t->growth_credit_us;
        }
        t->delay_us += step;
        t->growth_credit_us -= step;
        t->last_pressure_us = now_us;
    } else if (target < t->delay_us && now_us - t->last_pressure_us > p->hold_us) {
        double step = p->release_us_per_s * (double) elapsed / (double) US_PER_S;
        double room = t->delay_us - target;
        t->delay_us -= step < room ? step : room;
    }
    /* A shorter period (rate went up) can lower the cap under the current delay. */
    if (t->delay_us > cap) {
        t->delay_us = cap;
    }
}

/* ---- cadence smoothing ("reduce judder") ---- */

static double slew_back(vrr_timing_t *t) {
    /* A reset used to drop the accumulated retiming in one frame — up to a 6 ms
     * step on screen. Ease it back to the raw slot instead. */
    double r = t->last_retiming_us;
    if (r > SMOOTH_RESET_SLEW_US) {
        r -= SMOOTH_RESET_SLEW_US;
    } else if (r < -SMOOTH_RESET_SLEW_US) {
        r += SMOOTH_RESET_SLEW_US;
    } else {
        r = 0;
    }
    return r;
}

static double clamp_retiming(const vrr_timing_t *t, double r) {
    if (r > SMOOTH_MAX_LAG_US) {
        r = SMOOTH_MAX_LAG_US;
    }
    /* Never before the mapped source slot: the delay is all the earliness we have. */
    if (r < -t->delay_us) {
        r = -t->delay_us;
    }
    return r;
}

static double smoothing_adjust(vrr_timing_t *t, bool continuous, int64_t interval_us, double raw_slot_us,
                               bool *engaged) {
    *engaged = false;
    bool stable = false;
    if (continuous && interval_bounded(t, interval_us)) {
        t->cadence[t->cadence_idx] = interval_us;
        t->cadence_idx = (t->cadence_idx + 1) % VRR_TIMING_CADENCE_WINDOW;
        if (t->cadence_len < VRR_TIMING_CADENCE_WINDOW) {
            t->cadence_len++;
        }
        int64_t total = 0;
        for (unsigned i = 0; i < t->cadence_len; i++) {
            total += t->cadence[i];
        }
        const double expect = t->period_us * (double) t->cadence_len;
        stable = t->cadence_len == VRR_TIMING_CADENCE_WINDOW && fabs((double) total - expect) <= expect * 0.25;
    } else {
        t->cadence_len = 0;
        t->cadence_idx = 0;
    }
    if (!t->reduce_judder) {
        return 0;
    }
    if (!stable || !t->have_basis) {
        if (t->smoothing_engaged) {
            t->cadence_resets++;
        }
        t->smoothing_engaged = false;
        return clamp_retiming(t, slew_back(t));
    }
    const double predicted = t->last_basis_us + t->period_us;
    const double error = predicted - raw_slot_us;
    if (fabs(error) > t->period_us * SMOOTH_SNAP_PERIODS) {
        if (t->smoothing_engaged) {
            t->cadence_resets++;
        }
        t->smoothing_engaged = false;
        return clamp_retiming(t, slew_back(t));
    }
    /* A drifting game rate leaves the interval average behind, and the phase blend
     * turns that period error into a standing offset. Integrate the phase error into
     * the period too, so the smoothed slot follows a rate ramp instead of lagging it. */
    t->period_us -= error * PERIOD_FEEDBACK;
    if (t->period_us < 1000.0) {
        t->period_us = 1000.0;
    }
    t->smoothing_engaged = true;
    *engaged = true;
    return clamp_retiming(t, error * (1.0 - SMOOTH_GAIN));
}

void vrr_timing_schedule(vrr_timing_t *t, uint32_t rtp_ts, int frame_number, bool keyframe, int64_t ready_us,
                         vrr_timing_decision_t *out) {
    memset(out, 0, sizeof(*out));
    bool epoch = !t->have_frame;
    bool continuous = t->have_frame;
    int64_t interval_us = 0;
    int32_t dticks = 0;
    if (t->have_frame) {
        dticks = (int32_t) (rtp_ts - t->last_rtp);
        const int64_t dhost = ticks_to_us(dticks);
        const int64_t dready = ready_us - t->last_ready_us;
        const int fdelta = frame_number - t->last_frame_number;
        if (dticks <= 0 || llabs(dhost - dready) > EPOCH_MISMATCH_US) {
            /* The sender clock went backwards or jumped relative to ours: whatever
             * mapping we had describes a different timeline. */
            epoch = true;
        } else if (t->force_break || fdelta != 1 || dhost > CADENCE_GAP_US || dready > CADENCE_GAP_US) {
            continuous = false;
        }
        interval_us = dhost;
    }
    if (epoch) {
        if (t->have_frame) {
            t->epoch_resets++;
        }
        reset_offset(t);
        reset_cadence(t);
        t->host_ticks = 0;
        t->last_retiming_us = 0;
        continuous = false;
        interval_us = 0;
        t->burst_left = BURST_EXCLUDE_FRAMES;
    } else {
        t->host_ticks += dticks;
        if (!continuous || !interval_bounded(t, interval_us)) {
            t->burst_left = BURST_EXCLUDE_FRAMES;
        }
    }
    const int64_t host_us = ticks_to_us(t->host_ticks);
    t->have_frame = true;
    t->force_break = false;
    t->last_rtp = rtp_ts;
    t->last_host_us = host_us;
    t->last_ready_us = ready_us;
    t->last_frame_number = frame_number;

    /* Sender clock → local clock. */
    offset_observe(t, host_us, ready_us - host_us);
    const int64_t mapped = host_us + t->applied_offset_us;
    const int64_t lateness = ready_us - mapped;

    /* Jitter statistic. Keyframes finish reassembly late because they are big, and
     * the frames bunched behind a stall are late because of the stall; neither is
     * the ordinary jitter the delay is sized for. */
    bool excluded = keyframe || t->burst_left > 0;
    if (t->burst_left > 0 && !epoch) {
        t->burst_left--;
    }
    if (!excluded) {
        int64_t l = lateness;
        if (l > INT32_MAX) {
            l = INT32_MAX;
        } else if (l < INT32_MIN) {
            l = INT32_MIN;
        }
        t->late[t->late_idx] = (int32_t) l;
        t->late_idx = (t->late_idx + 1) % VRR_TIMING_LATE_CAP;
        if (t->late_len < VRR_TIMING_LATE_CAP) {
            t->late_len++;
        }
    }

    if (continuous) {
        period_observe(t, interval_us, interval_bounded(t, interval_us));
    }
    delay_update(t, ready_us);

    if (!t->suspended && t->period_us > SUSPEND_PERIOD_US) {
        t->suspended = true;
        t->suspends++;
    } else if (t->suspended && t->period_us < RESUME_PERIOD_US) {
        t->suspended = false;
    }

    const double raw_slot = (double) mapped + t->delay_us;
    if (t->suspended) {
        /* No hold and no smoothing; the cadence starts over when pacing resumes. */
        reset_cadence(t);
        t->last_retiming_us = 0;
        out->target_us = ready_us;
        out->host_us = host_us;
        out->raw_slot_us = (int64_t) llround(raw_slot);
        out->delay_us = (int64_t) llround(t->delay_us);
        out->lateness_us = lateness;
        out->epoch_reset = epoch;
        out->cadence_break = !continuous;
        out->suspended = true;
        return;
    }
    bool engaged = false;
    double retiming = smoothing_adjust(t, continuous, interval_us, raw_slot, &engaged);
    t->last_retiming_us = retiming;
    const double target = raw_slot + retiming;
    t->last_basis_us = target;
    t->have_basis = true;

    out->target_us = (int64_t) llround(target);
    out->host_us = host_us;
    out->raw_slot_us = (int64_t) llround(raw_slot);
    out->delay_us = (int64_t) llround(t->delay_us);
    out->retiming_us = (int64_t) llround(retiming);
    out->lateness_us = lateness;
    out->epoch_reset = epoch;
    out->cadence_break = !continuous;
    out->smoothed = engaged;
}

/* ---- metrics ---- */

static uint16_t clamp_u16(int64_t v) {
    if (v < 0) {
        v = -v;
    }
    return v > 65535 ? 65535 : (uint16_t) v;
}

void vrr_metrics_reset(vrr_metrics_t *m) {
    memset(m, 0, sizeof(*m));
}

void vrr_metrics_record(vrr_metrics_t *m, int64_t host_us, int64_t ready_us, int64_t present_us, bool pair_valid,
                        bool immediate, bool preempted, bool suspended) {
    if (pair_valid && m->have_prev && m->pairs < VRR_METRICS_CAP) {
        const int64_t dhost = host_us - m->prev_host_us;
        m->spacing_us[m->pairs] = clamp_u16((present_us - m->prev_present_us) - dhost);
        m->arrival_us[m->pairs] = clamp_u16((ready_us - m->prev_ready_us) - dhost);
        m->pairs++;
    }
    if (m->frames < VRR_METRICS_CAP) {
        int64_t held = present_us - ready_us;
        m->held_us[m->frames] = held > 0 ? clamp_u16(held) : 0;
    }
    m->frames++;
    if (immediate) {
        m->immediate++;
    }
    if (preempted) {
        m->preempted++;
    }
    if (suspended) {
        m->suspended++;
    }
    m->have_prev = true;
    m->prev_host_us = host_us;
    m->prev_ready_us = ready_us;
    m->prev_present_us = present_us;
}

static int cmp_u16(const void *a, const void *b) {
    return (int) *(const uint16_t *) a - (int) *(const uint16_t *) b;
}

static float quantile_ms(uint16_t *values, unsigned n, double q) {
    if (n == 0) {
        return 0;
    }
    unsigned rank = (unsigned) ceil(q * (double) n);
    if (rank < 1) {
        rank = 1;
    } else if (rank > n) {
        rank = n;
    }
    return (float) values[rank - 1] / 1000.0f;
}

bool vrr_metrics_flush(vrr_metrics_t *m, int64_t now_us, int64_t window_us, vrr_metrics_summary_t *out) {
    if (m->window_start_us == 0) {
        m->window_start_us = now_us;
        return false;
    }
    if (now_us - m->window_start_us < window_us) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->frames = m->frames;
    out->pairs = m->pairs;
    qsort(m->spacing_us, m->pairs, sizeof(uint16_t), cmp_u16);
    qsort(m->arrival_us, m->pairs, sizeof(uint16_t), cmp_u16);
    unsigned held_n = m->frames < VRR_METRICS_CAP ? m->frames : VRR_METRICS_CAP;
    qsort(m->held_us, held_n, sizeof(uint16_t), cmp_u16);
    out->spacing_p50_ms = quantile_ms(m->spacing_us, m->pairs, 0.50);
    out->spacing_p90_ms = quantile_ms(m->spacing_us, m->pairs, 0.90);
    out->spacing_p99_ms = quantile_ms(m->spacing_us, m->pairs, 0.99);
    out->arrival_p50_ms = quantile_ms(m->arrival_us, m->pairs, 0.50);
    out->arrival_p90_ms = quantile_ms(m->arrival_us, m->pairs, 0.90);
    out->arrival_p99_ms = quantile_ms(m->arrival_us, m->pairs, 0.99);
    out->held_p50_ms = quantile_ms(m->held_us, held_n, 0.50);
    out->held_max_ms = held_n > 0 ? (float) m->held_us[held_n - 1] / 1000.0f : 0;
    if (m->frames > 0) {
        out->immediate_pct = 100.0f * (float) m->immediate / (float) m->frames;
        out->preempted_pct = 100.0f * (float) m->preempted / (float) m->frames;
        out->suspended_pct = 100.0f * (float) m->suspended / (float) m->frames;
    }
    /* New window; the pair chain continues across it. */
    const bool have_prev = m->have_prev;
    const int64_t ph = m->prev_host_us, pr = m->prev_ready_us, pp = m->prev_present_us;
    memset(m, 0, sizeof(*m));
    m->window_start_us = now_us;
    m->have_prev = have_prev;
    m->prev_host_us = ph;
    m->prev_ready_us = pr;
    m->prev_present_us = pp;
    return true;
}
