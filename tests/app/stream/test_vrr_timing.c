/*
 * Pseudo-VRR scheduler (src/app/stream/video/vrr_timing.c): unit tests plus a
 * cadence simulation that prints the spacing error before/after.
 *
 * Model of one frame's life, all on a synthetic clock:
 *   game present  : variable 55-72 fps (slow random walk), +-1 ms per-frame game
 *                   jitter, capped at the 72 fps stream rate, quantised to the
 *                   host's 1000 Hz virtual display and to the 90 kHz RTP clock
 *   network       : fixed latency + one-sided jitter (exponential, sd ~2.2 ms) +
 *                   0.5 % spikes of +8 ms + a 120 ms Wi-Fi stall every 15 s whose
 *                   frames arrive bunched; the TV clock runs 30 ppm fast
 *   decoder thread: one frame at a time (feed costs 0.3 ms), hold until the
 *                   target, a newer frame waiting ends the hold early, sleep
 *                   overshoot 0-80 us
 *   panel         : shows a frame on the first 144 Hz vsync after feed + a fixed
 *                   decode time (present-on-arrival)
 * "before" is feeding on arrival (today's wall-clock mode), "after" is the
 * scheduler. Spacing error = |delta presented - delta host PTS| per consecutive
 * frame pair, reported raw (feed times) and vsync-quantised.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "stream/video/vrr_timing.h"

static vrr_timing_t ctl;

void setUp(void) {
}

void tearDown(void) {
}

/* ---- deterministic RNG ---- */

static uint64_t rng_state;

static double rng_uniform(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double) (rng_state >> 11) / (double) (1ULL << 53);
}

static double rng_normal(void) {
    double u1 = rng_uniform(), u2 = rng_uniform();
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static uint32_t rtp_of_us(int64_t us, uint32_t base) {
    return base + (uint32_t) ((us * 90000LL) / 1000000LL);
}

/* ---- unit tests ---- */

static void testPeriodRecoversAfterLowRateStretch(void) {
    /* 30 s at 22 fps, then 72 fps: the period must follow within about a second
     * and smoothing must engage again (regression: it stayed near 45 ms). */
    vrr_timing_init(&ctl, VRR_TIMING_PROFILE_BALANCED, true, 7200);
    vrr_timing_decision_t d;
    int64_t host = 0;
    int frame = 0;
    for (int i = 0; i < 22 * 30; i++, frame++) {
        host += 1000000 / 22;
        vrr_timing_schedule(&ctl, rtp_of_us(host, 5000), frame + 1, frame == 0, host + 20000, &d);
    }
    TEST_ASSERT_DOUBLE_WITHIN(3000.0, 1000000.0 / 22, ctl.period_us);
    for (int i = 0; i < 72; i++, frame++) {
        host += 1000000 / 72;
        vrr_timing_schedule(&ctl, rtp_of_us(host, 5000), frame + 1, false, host + 20000, &d);
    }
    TEST_ASSERT_DOUBLE_WITHIN(500.0, 1000000.0 / 72, ctl.period_us);
    TEST_ASSERT_TRUE(ctl.smoothing_engaged);
    /* And back down below a third of the rate: 72 fps -> 20 fps. */
    for (int i = 0; i < 40; i++, frame++) {
        host += 1000000 / 20;
        vrr_timing_schedule(&ctl, rtp_of_us(host, 5000), frame + 1, false, host + 20000, &d);
    }
    TEST_ASSERT_DOUBLE_WITHIN(3000.0, 1000000.0 / 20, ctl.period_us);
}

static void testSteadyStreamIsExact(void) {
    /* No jitter at all: every frame is held to host + offset + delay and the
     * presented spacing equals the host spacing to the microsecond. */
    vrr_timing_init(&ctl, VRR_TIMING_PROFILE_LOW_LATENCY, true, 7200);
    vrr_timing_decision_t d;
    int64_t prev_target = 0, prev_host = 0;
    int max_err = 0;
    for (int i = 0; i < 2000; i++) {
        int64_t host = (int64_t) i * 1000000 / 72;
        vrr_timing_schedule(&ctl, rtp_of_us(host, 1000), i + 1, i == 0, host + 20000, &d);
        TEST_ASSERT_FALSE(i > 0 && d.epoch_reset);
        if (i > 100) {
            int err = (int) llabs((d.target_us - prev_target) - (d.host_us - prev_host));
            if (err > max_err) {
                max_err = err;
            }
        }
        prev_target = d.target_us;
        prev_host = d.host_us;
    }
    /* 90 kHz rounding of a 72 fps stamp plus the slow delay release: tens of us. */
    TEST_ASSERT_LESS_OR_EQUAL_INT(20, max_err);
    /* With zero lateness the delay releases from its 6 ms start: held for 6 s, then
     * 125 us/s, so 27.8 s of stream ends near 6 - 2.7 = 3.3 ms. */
    TEST_ASSERT_INT64_WITHIN(300, 3300, d.delay_us);
}

static void testRtpWrapIsNotAnEpoch(void) {
    vrr_timing_init(&ctl, VRR_TIMING_PROFILE_BALANCED, true, 7200);
    vrr_timing_decision_t d;
    const uint32_t base = 0xFFFFFFFFu - 90000u; /* wraps after one second */
    for (int i = 0; i < 300; i++) {
        int64_t host = (int64_t) i * 1000000 / 72;
        vrr_timing_schedule(&ctl, rtp_of_us(host, base), i + 1, false, host + 15000, &d);
        if (i > 0) {
            TEST_ASSERT_FALSE(d.epoch_reset);
            TEST_ASSERT_FALSE(d.cadence_break);
        }
    }
    TEST_ASSERT_INT64_WITHIN(20, (int64_t) 299 * 1000000 / 72, d.host_us);
    TEST_ASSERT_EQUAL_UINT32(0, ctl.epoch_resets);
}

static void testGapBreaksCadenceButKeepsOffset(void) {
    vrr_timing_init(&ctl, VRR_TIMING_PROFILE_LOW_LATENCY, true, 7200);
    vrr_timing_decision_t d;
    int64_t host = 0;
    for (int i = 0; i < 200; i++) {
        host = (int64_t) i * 1000000 / 72;
        vrr_timing_schedule(&ctl, rtp_of_us(host, 5), i + 1, false, host + 30000, &d);
    }
    const int64_t offset_before = ctl.applied_offset_us;
    /* A static scene: the host sends nothing for 400 ms, then resumes. */
    host += 400000;
    vrr_timing_schedule(&ctl, rtp_of_us(host, 5), 201, false, host + 30000, &d);
    TEST_ASSERT_TRUE(d.cadence_break);
    TEST_ASSERT_FALSE(d.epoch_reset);
    TEST_ASSERT_EQUAL_INT64(offset_before, ctl.applied_offset_us);
    /* Lost frames: frame number jumps by 3. */
    host += 1000000 / 72 * 3;
    vrr_timing_schedule(&ctl, rtp_of_us(host, 5), 204, false, host + 30000, &d);
    TEST_ASSERT_TRUE(d.cadence_break);
    /* Explicit break (decoder reload). */
    host += 1000000 / 72;
    vrr_timing_break_cadence(&ctl);
    vrr_timing_schedule(&ctl, rtp_of_us(host, 5), 205, false, host + 30000, &d);
    TEST_ASSERT_TRUE(d.cadence_break);
    host += 1000000 / 72;
    vrr_timing_schedule(&ctl, rtp_of_us(host, 5), 206, false, host + 30000, &d);
    TEST_ASSERT_FALSE(d.cadence_break);
}

static void testBackwardsTimestampIsAnEpoch(void) {
    vrr_timing_init(&ctl, VRR_TIMING_PROFILE_LOW_LATENCY, true, 7200);
    vrr_timing_decision_t d;
    for (int i = 0; i < 50; i++) {
        int64_t host = (int64_t) i * 1000000 / 72;
        vrr_timing_schedule(&ctl, rtp_of_us(host, 900000), i + 1, false, host + 30000, &d);
    }
    /* Host restarted its clock. */
    vrr_timing_schedule(&ctl, rtp_of_us(0, 7), 51, true, 50 * 1000000 / 72 + 30000, &d);
    TEST_ASSERT_TRUE(d.epoch_reset);
    TEST_ASSERT_EQUAL_UINT32(1, ctl.epoch_resets);
    /* Sender and receiver disagree about one gap by more than a second. */
    vrr_timing_schedule(&ctl, rtp_of_us(5000000, 7), 52, false, 50 * 1000000 / 72 + 30000 + 14000, &d);
    TEST_ASSERT_TRUE(d.epoch_reset);
}

static void testNeverBeforeMappedSlotAndBoundedHold(void) {
    rng_state = 0x1234567ULL;
    const vrr_timing_profile_t profiles[] = {VRR_TIMING_PROFILE_LOW_LATENCY, VRR_TIMING_PROFILE_BALANCED,
                                             VRR_TIMING_PROFILE_SMOOTH};
    for (unsigned p = 0; p < 3; p++) {
        vrr_timing_init(&ctl, profiles[p], true, 7200);
        vrr_timing_decision_t d;
        double t = 0;
        for (int i = 0; i < 20000; i++) {
            t += 1e6 / (55.0 + 17.0 * rng_uniform());
            int64_t host = (int64_t) t;
            int64_t ready = host + 20000 + (int64_t) (-2200.0 * log(1.0 - rng_uniform() * 0.999));
            vrr_timing_schedule(&ctl, rtp_of_us(host, 77), i + 1, false, ready, &d);
            /* Mapped slot = host + applied offset; the schedule never goes before it. */
            TEST_ASSERT_TRUE(d.target_us >= d.host_us + ctl.applied_offset_us);
            TEST_ASSERT_TRUE(d.delay_us <= vrr_timing_delay_cap_us(&ctl) + 1);
            TEST_ASSERT_TRUE(d.target_us - ready <= vrr_timing_max_hold_us(&ctl));
            TEST_ASSERT_TRUE(d.retiming_us <= 6000);
        }
    }
}

static void testMetricsQuantiles(void) {
    static vrr_metrics_t m;
    vrr_metrics_reset(&m);
    vrr_metrics_summary_t s;
    TEST_ASSERT_FALSE(vrr_metrics_flush(&m, 1, 10000000, &s));
    for (int i = 0; i < 100; i++) {
        /* host spacing 10 ms, present spacing 10 ms + i us, arrival exact */
        int64_t host = (int64_t) i * 10000;
        int64_t present = host + (int64_t) i * (i + 1) / 2;
        vrr_metrics_record(&m, host, host, present, true, i % 4 == 0, false);
    }
    TEST_ASSERT_TRUE(vrr_metrics_flush(&m, 20000000, 10000000, &s));
    TEST_ASSERT_EQUAL_UINT(99, s.pairs);
    TEST_ASSERT_EQUAL_UINT(100, s.frames);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.050f, s.spacing_p50_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.098f, s.spacing_p99_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s.arrival_p99_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, s.immediate_pct);
}

/* ---- simulation ---- */

typedef struct sim_result_t {
    double before_p50, before_p90, before_p99;
    double after_p50, after_p90, after_p99;
    double before_vs_p99, after_vs_p99;       /* vsync-quantised */
    double before_vs_bad, after_vs_bad;       /* share of pairs off by >= one vsync */
    double game_before_p99, game_after_p99, game_before_p90, game_after_p90; /* vs the game's own clock */
    double hold_p50, hold_max, immediate_pct, preempt_pct;
    double final_delay_ms;
    double added_latency_mean_ms;
} sim_result_t;

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

static double quant(double *v, size_t n, double q) {
    size_t rank = (size_t) ceil(q * (double) n);
    if (rank < 1) {
        rank = 1;
    }
    if (rank > n) {
        rank = n;
    }
    return v[rank - 1];
}

#define SIM_FRAMES 72 * 120

static double sim_game[SIM_FRAMES], sim_gerr_before[SIM_FRAMES], sim_gerr_after[SIM_FRAMES];
static double sim_host[SIM_FRAMES], sim_ready[SIM_FRAMES], sim_feed[SIM_FRAMES];
static double sim_err_before[SIM_FRAMES], sim_err_after[SIM_FRAMES];
static double sim_vs_before[SIM_FRAMES], sim_vs_after[SIM_FRAMES], sim_hold[SIM_FRAMES];

static double vsync_after(double t_us) {
    const double period = 1e6 / 144.0;
    return ceil(t_us / period) * period;
}

static void simulate(vrr_timing_profile_t profile, bool reduce_judder, double jitter_mean_us, bool stalls,
                     sim_result_t *r) {
    rng_state = 0x9E3779B97F4A7C15ULL;
    vrr_timing_init(&ctl, profile, reduce_judder, 7200);
    const double cap_period = 1e6 / 72.0;
    const double drift = 1.0 + 30e-6;
    double fps = 64.0, game_t = 0, last_present = -1e9;
    int n = 0;
    /* Host side: game presents, capped at the stream rate, stamped at 1 ms. */
    while (n < SIM_FRAMES) {
        if (n % 36 == 0) {
            fps += (rng_uniform() - 0.5) * 6.0;
            fps = fps < 55.0 ? 55.0 : fps > 72.0 ? 72.0 : fps;
        }
        double jitter = rng_normal() * 500.0;
        jitter = jitter > 1000 ? 1000 : jitter < -1000 ? -1000 : jitter;
        game_t += 1e6 / fps;
        double present = game_t + jitter;
        if (present < last_present + cap_period) {
            present = last_present + cap_period;
        }
        last_present = present;
        sim_game[n] = game_t;
        sim_host[n] = floor(present / 1000.0) * 1000.0; /* 1000 Hz VDD */
        n++;
    }
    /* Network. */
    const double base = 25000.0;
    double stall_until = -1, last_ready = -1e9;
    for (int i = 0; i < n; i++) {
        double j = -jitter_mean_us * log(1.0 - rng_uniform() * 0.9999);
        if (rng_uniform() < 0.005) {
            j += 8000.0;
        }
        double ready = sim_host[i] + base + j;
        if (stalls && fmod(sim_host[i], 15e6) < 1e6 / 72.0 && sim_host[i] > 1e6) {
            stall_until = ready + 120000.0;
        }
        if (ready < stall_until) {
            ready = stall_until;
        }
        /* In-order delivery: a frame cannot complete before its predecessor. */
        if (ready < last_ready + 300.0) {
            ready = last_ready + 300.0;
        }
        last_ready = ready;
        sim_ready[i] = ready * drift;
    }
    /* Decoder thread. */
    double busy_until = 0;
    unsigned immediate = 0, preempted = 0;
    double added = 0;
    const int queue = vrr_timing_queue_frames(&ctl);
    for (int i = 0; i < n; i++) {
        vrr_timing_decision_t d;
        double start = sim_ready[i] > busy_until ? sim_ready[i] : busy_until;
        vrr_timing_schedule(&ctl, rtp_of_us((int64_t) sim_host[i], 0xFFF00000u), i + 1, i % 600 == 0,
                            (int64_t) sim_ready[i], &d);
        double feed = start;
        if ((double) d.target_us > start) {
            double target = (double) d.target_us;
            /* A newer frame waiting ends the hold. */
            if (i + queue < n && sim_ready[i + queue] < target) {
                target = sim_ready[i + queue] > start ? sim_ready[i + queue] : start;
                preempted++;
            }
            feed = target + rng_uniform() * 80.0;
        } else {
            immediate++;
        }
        sim_feed[i] = feed;
        sim_hold[i] = (feed - sim_ready[i]) / 1000.0;
        added += feed - sim_ready[i];
        busy_until = feed + 300.0;
    }
    const double decode = 5000.0;
    size_t pairs = 0;
    unsigned before_bad = 0, after_bad = 0;
    for (int i = 1; i < n; i++) {
        double dh = sim_host[i] - sim_host[i - 1];
        if (dh > 100000) {
            continue;
        }
        sim_err_before[pairs] = fabs((sim_ready[i] - sim_ready[i - 1]) - dh) / 1000.0;
        sim_err_after[pairs] = fabs((sim_feed[i] - sim_feed[i - 1]) - dh) / 1000.0;
        double dg = sim_game[i] - sim_game[i - 1];
        sim_gerr_before[pairs] = fabs((sim_ready[i] - sim_ready[i - 1]) - dg) / 1000.0;
        sim_gerr_after[pairs] = fabs((sim_feed[i] - sim_feed[i - 1]) - dg) / 1000.0;
        double vb = vsync_after(sim_ready[i] + decode) - vsync_after(sim_ready[i - 1] + decode);
        double va = vsync_after(sim_feed[i] + decode) - vsync_after(sim_feed[i - 1] + decode);
        sim_vs_before[pairs] = fabs(vb - dh) / 1000.0;
        sim_vs_after[pairs] = fabs(va - dh) / 1000.0;
        if (sim_vs_before[pairs] >= 1e3 / 144.0) {
            before_bad++;
        }
        if (sim_vs_after[pairs] >= 1e3 / 144.0) {
            after_bad++;
        }
        pairs++;
    }
    qsort(sim_err_before, pairs, sizeof(double), cmp_double);
    qsort(sim_err_after, pairs, sizeof(double), cmp_double);
    qsort(sim_gerr_before, pairs, sizeof(double), cmp_double);
    qsort(sim_gerr_after, pairs, sizeof(double), cmp_double);
    r->game_before_p90 = quant(sim_gerr_before, pairs, 0.9);
    r->game_before_p99 = quant(sim_gerr_before, pairs, 0.99);
    r->game_after_p90 = quant(sim_gerr_after, pairs, 0.9);
    r->game_after_p99 = quant(sim_gerr_after, pairs, 0.99);
    r->final_delay_ms = ctl.delay_us / 1000.0;
    qsort(sim_vs_before, pairs, sizeof(double), cmp_double);
    qsort(sim_vs_after, pairs, sizeof(double), cmp_double);
    qsort(sim_hold, (size_t) n, sizeof(double), cmp_double);
    r->before_p50 = quant(sim_err_before, pairs, 0.5);
    r->before_p90 = quant(sim_err_before, pairs, 0.9);
    r->before_p99 = quant(sim_err_before, pairs, 0.99);
    r->after_p50 = quant(sim_err_after, pairs, 0.5);
    r->after_p90 = quant(sim_err_after, pairs, 0.9);
    r->after_p99 = quant(sim_err_after, pairs, 0.99);
    r->before_vs_p99 = quant(sim_vs_before, pairs, 0.99);
    r->after_vs_p99 = quant(sim_vs_after, pairs, 0.99);
    r->before_vs_bad = 100.0 * before_bad / (double) pairs;
    r->after_vs_bad = 100.0 * after_bad / (double) pairs;
    r->hold_p50 = quant(sim_hold, (size_t) n, 0.5);
    r->hold_max = sim_hold[n - 1];
    r->immediate_pct = 100.0 * immediate / (double) n;
    r->preempt_pct = 100.0 * preempted / (double) n;
    r->added_latency_mean_ms = added / (double) n / 1000.0;
}

static void print_result(const char *name, const sim_result_t *r) {
    printf("SIM %-26s spacing err ms p50/p90/p99  arrival %.2f/%.2f/%.2f -> paced %.2f/%.2f/%.2f | "
           "144Hz p99 %.2f -> %.2f, pairs off >=1 vsync %.1f%% -> %.1f%% | hold p50 %.2f max %.2f ms, "
           "immediate %.1f%%, preempted %.1f%%, +%.2f ms mean, delay end %.2f ms | vs game clock p90/p99 "
           "%.2f/%.2f -> %.2f/%.2f\n",
           name, r->before_p50, r->before_p90, r->before_p99, r->after_p50, r->after_p90, r->after_p99,
           r->before_vs_p99, r->after_vs_p99, r->before_vs_bad, r->after_vs_bad, r->hold_p50, r->hold_max,
           r->immediate_pct, r->preempt_pct, r->added_latency_mean_ms, r->final_delay_ms, r->game_before_p90,
           r->game_before_p99, r->game_after_p90, r->game_after_p99);
}

static void testSimulationLowLatency(void) {
    sim_result_t r;
    simulate(VRR_TIMING_PROFILE_LOW_LATENCY, true, 2200.0, true, &r);
    print_result("low-latency jitter2.2 stalls", &r);
    TEST_ASSERT_TRUE(r.after_p90 < r.before_p90 * 0.5);
    TEST_ASSERT_TRUE(r.after_vs_bad < r.before_vs_bad);
}

static void testSimulationBalanced(void) {
    sim_result_t r;
    simulate(VRR_TIMING_PROFILE_BALANCED, true, 2200.0, true, &r);
    print_result("balanced jitter2.2 stalls", &r);
    TEST_ASSERT_TRUE(r.after_p99 < r.before_p99);
}

static void testSimulationSmooth(void) {
    sim_result_t r;
    simulate(VRR_TIMING_PROFILE_SMOOTH, true, 2200.0, true, &r);
    print_result("smooth jitter2.2 stalls", &r);
    TEST_ASSERT_TRUE(r.after_p99 < r.before_p99);
}

static void testSimulationNoJudderReduction(void) {
    sim_result_t r;
    simulate(VRR_TIMING_PROFILE_LOW_LATENCY, false, 2200.0, true, &r);
    print_result("low-latency no-smoothing", &r);
    TEST_ASSERT_TRUE(r.after_p90 < r.before_p90);
}

static void testSimulationCleanLink(void) {
    sim_result_t r;
    simulate(VRR_TIMING_PROFILE_LOW_LATENCY, true, 300.0, false, &r);
    print_result("low-latency clean link", &r);
    /* On a clean link the scheduler must not add judder of its own. */
    TEST_ASSERT_TRUE(r.after_p99 <= r.before_p99 + 0.5);
    /* The 6 ms start delay releases slowly by design (Nonary's hold + release);
     * what matters is where it ends up. */
    TEST_ASSERT_TRUE(r.final_delay_ms < 5.0);
    TEST_ASSERT_TRUE(r.added_latency_mean_ms < 5.0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(testSteadyStreamIsExact);
    RUN_TEST(testPeriodRecoversAfterLowRateStretch);
    RUN_TEST(testRtpWrapIsNotAnEpoch);
    RUN_TEST(testGapBreaksCadenceButKeepsOffset);
    RUN_TEST(testBackwardsTimestampIsAnEpoch);
    RUN_TEST(testNeverBeforeMappedSlotAndBoundedHold);
    RUN_TEST(testMetricsQuantiles);
    RUN_TEST(testSimulationLowLatency);
    RUN_TEST(testSimulationBalanced);
    RUN_TEST(testSimulationSmooth);
    RUN_TEST(testSimulationNoJudderReduction);
    RUN_TEST(testSimulationCleanLink);
    return UNITY_END();
}
