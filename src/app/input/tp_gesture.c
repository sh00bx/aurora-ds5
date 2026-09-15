/**
 * @file tp_gesture.c
 * @brief Controller touchpad -> mouse gestures (see header).
 *
 * Ported from Vibepollo src/ds5_touchpad_mouse.cpp, normalized-touch path.
 * Keep it diffable against that file: same order, same names where C allows,
 * same constants. Every departure is a numbered DEVIATION-C comment.
 *
 * DEVIATION-C 1 -- no mutex and no atomics. The original is fed from bridge
 * session threads; here the only caller is the LVGL thread.
 *
 * DEVIATION-C 2 -- time comes from CLOCK_MONOTONIC in microseconds rather than
 * std::chrono::steady_clock. Not lv_tick_get(): that is millisecond-granular,
 * and the whole velocity estimate below lives in microseconds -- a 1 ms quantum
 * would collapse the arrival-time path onto the 10 ms substitution and flatten
 * the acceleration curve into its middle band, which is exactly the failure the
 * original's DEVIATION 1 describes.
 *
 * DEVIATION-C 3 -- the pad-clock branch is gone. Normalized SDL touch events
 * carry no sensor timestamp, and the original's own feed_touch_event() turns
 * that branch off too (dev_clock_valid = false), so only libinput's smoothener
 * path can ever run. It is inlined instead of carried as dead code.
 *
 * DEVIATION-C 4 -- the physical touchpad click is wired up. The host's
 * normalized path always passes click=false because moonlight's touch protocol
 * does not carry it; SDL gives it to us as SDL_CONTROLLER_BUTTON_TOUCHPAD, so
 * click-drag and click-to-press work here. The gesture logic is the original's,
 * unchanged -- it was always written for a click it just never received.
 *
 * DEVIATION-C 5 -- an output scale (ui_scale). The host moves the Windows
 * cursor in desktop pixels; we move a cursor on a 1920-wide LVGL canvas that
 * the TV scales to the same panel as the stream. Multiplying the gain by
 * canvas/stream is what makes an identical swipe cover an identical stretch of
 * glass. It is applied to `factor`, not to the integer output, so the
 * sub-pixel carry that carries the deceleration floor survives.
 */
#include "tp_gesture.h"

#include <math.h>
#include <string.h>
#include <time.h>

// DS5 touchpad sensor space (hid-playstation). Normalized input is scaled into
// it, so these are a unit system here rather than a description of any one pad
// -- the original does the same for its normalized path.
#define PAD_W 1920
#define PAD_H 1080
// Y units per X unit: the pad is about 52 x 23 mm, so the ratio is just
// (height in units / width in units) corrected to square: 1080/1920 on that
// area gives 27/21.
#define PAD_XY_SCALE (27.0 / 21.0)

// A tap is a short, still contact.
#define TAP_MAX_DURATION_US 280000
#define TAP_MAX_TRAVEL 14 /* pad units, euclidean */

// Two-finger scroll: pad units of vertical travel per wheel notch.
#define SCROLL_UNITS_PER_NOTCH 36

static uint64_t tp_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) (ts.tv_nsec / 1000);
}

static int tp_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * libinput's pointer acceleration, as the host tuned it against measured use.
 *
 * The deltas are pad units; y is rescaled to the x axis (27/21). The accel
 * filter runs at DEFAULT_MOUSE_DPI = 1000, where normalize_for_dpi() is the
 * identity -- but libinput ALSO scales touchpad deltas by (1000/25.4)/res_x
 * before the filter and on its output, which this port omits (as the original
 * does): velocities here are libinput's divided by 1.458, and "mm/s" below is
 * that raw-pad-unit scale, not physical mm. The thresholds are tuned in THIS
 * unit on measured data, so they need no conversion.
 *
 * Normalized touch events carry no device clock, so libinput's delta smoothener
 * applies verbatim: an event interval below 50 ms is REPLACED by 10 ms for the
 * velocity estimate. Velocity is EMA-smoothed as a stand-in for libinput's
 * tracker set; a substituted interval carries no time information and so keeps
 * the per-event blend the curve was tuned with. Fractional remainders are
 * carried so slow movement is not truncated away.
 *
 * The deceleration threshold is this pad's, not libinput's: libinput's 7 mm/s
 * assumes a pad that reports its resolution, and this one does not, so 7 sits
 * below the 5th percentile of real use -- unreachable, which is why careful
 * aiming got no help. It is placed at the 20th percentile instead. The upper
 * bound stays at libinput's 130, which measurement puts at the 75th.
 */
static void tp_move_pointer(tp_gesture_t *g, int dx, int dy) {
    const double TP_MAGIC_SLOWDOWN = 0.2968;
    const double BASELINE = 0.9;
    const double THRESHOLD_MM_S = 130.0;   // nominal, ~p75 of measured use
    const double DECEL_LIMIT_MM_S = 30.0;  // ~p20 of measured use (libinput: 7)
    const double DECEL_FLOOR = 0.3;        // gain at a standstill
    // Ramp from DECEL_FLOOR at rest to BASELINE exactly at the limit, so the
    // deceleration meets the plateau without a step wherever the limit sits.
    const double DECEL_SLOPE = (BASELINE - DECEL_FLOOR) / DECEL_LIMIT_MM_S;
    const double SMOOTH_THRESHOLD_US = 50000.0;
    const double SMOOTH_VALUE_US = 10000.0;
    // Time constant of the velocity EMA; 1-exp(-4015/11000) = 0.30, the
    // per-event alpha the curve was tuned with at the pad's median cadence.
    // Substituted intervals carry no time information, so they keep the tuned
    // per-event blend instead -- this path (always substituted below 50 ms)
    // would otherwise smooth 2.5x less than it was tuned to.
    const double EMA_TAU_US = 11000.0;
    const double EMA_ALPHA_EVENT = 0.3;

    uint64_t now = tp_now_us();
    double dys = (double) dy * PAD_XY_SCALE;

    double dt_us = SMOOTH_VALUE_US;
    bool dt_substituted = true; /* until a real interval replaces the default */
    if (g->have_move_time) {
        dt_us = (double) (now - g->last_move_us) + 1.0;
        dt_substituted = false;
        if (dt_us < SMOOTH_THRESHOLD_US) {
            dt_us = SMOOTH_VALUE_US;
            dt_substituted = true;
        }
    }
    g->last_move_us = now;
    g->have_move_time = true;

    double v = hypot((double) dx, dys) / dt_us; // units/us
    double alpha = dt_substituted ? EMA_ALPHA_EVENT : 1.0 - exp(-dt_us / EMA_TAU_US);
    g->velocity_ema += alpha * (v - g->velocity_ema);

    // units/us -> nominal mm/s at 1000 dpi: *1e6 (per s) * 25.4/1000
    double speed_in = g->velocity_ema * 25400.0;
    double factor;
    if (speed_in < DECEL_LIMIT_MM_S) {
        factor = DECEL_SLOPE * speed_in + DECEL_FLOOR;
        if (factor > BASELINE) {
            factor = BASELINE;
        }
    } else if (speed_in < THRESHOLD_MM_S) {
        factor = BASELINE;
    } else {
        double capped = speed_in < THRESHOLD_MM_S * 4.0 ? speed_in : THRESHOLD_MM_S * 4.0;
        factor = 0.0025 * (capped / THRESHOLD_MM_S) * (capped - THRESHOLD_MM_S) + BASELINE;
    }
    /* The host also has a user speed here (ds5_touchpad_mouse_speed / 100); it
     * is at its default on our host, so the TV carries no second knob for it. */
    factor *= TP_MAGIC_SLOWDOWN;
    factor *= g->ui_scale; /* DEVIATION-C 5 */

    g->acc_x += (float) ((double) dx * factor);
    g->acc_y += (float) (dys * factor);
    int out_x = (int) g->acc_x;
    int out_y = (int) g->acc_y;
    if (out_x != 0 || out_y != 0) {
        g->acc_x -= (float) out_x;
        g->acc_y -= (float) out_y;
        g->sink.move(g->sink.ud, out_x, out_y);
    }
}

static void tp_scroll(tp_gesture_t *g, int dy) {
    g->scroll_acc += dy;
    while (g->scroll_acc >= SCROLL_UNITS_PER_NOTCH || -g->scroll_acc >= SCROLL_UNITS_PER_NOTCH) {
        int dir = g->scroll_acc > 0 ? 1 : -1;
        g->scroll_acc -= dir * SCROLL_UNITS_PER_NOTCH;
        // Finger travel down (dy > 0) scrolls the view down, matching the
        // Windows precision-touchpad default and the magic remote's wheel.
        g->sink.scroll(g->sink.ud, dir);
    }
}

static void tp_release_click(tp_gesture_t *g) {
    if (g->click_down) {
        g->sink.button(g->sink.ud, g->click_right, false, true);
        g->click_down = false;
    }
}

static void tp_begin_settle(tp_gesture_t *g) {
    g->settling = true;
    for (int i = 0; i < 2; i++) {
        g->settle_x[i] = g->c[i].x;
        g->settle_y[i] = g->c[i].y;
    }
    // Nothing accumulated so far may survive into the resumed gesture.
    g->scroll_acc = 0;
    g->acc_x = g->acc_y = 0.f;
    g->velocity_ema = 0.0;
    g->have_move_time = false;
}

// One contact slot transitioned or moved; run the shared gesture logic.
// click = physical touchpad button.
static void tp_process(tp_gesture_t *g, const tp_contact_t prev[2], bool click) {
    int fingers = (g->c[0].down ? 1 : 0) + (g->c[1].down ? 1 : 0);
    int prev_fingers = (prev[0].down ? 1 : 0) + (prev[1].down ? 1 : 0);
    if (fingers > g->fingers_seen) {
        g->fingers_seen = fingers;
    }

    // g->click_down still holds the previous report's state here -- the edge
    // itself is acted on further down.
    bool contacts_changed = fingers != prev_fingers;
    for (int i = 0; i < 2 && !contacts_changed; i++) {
        // A lift and a fresh landing inside one report keeps the count but is
        // still a new finger.
        contacts_changed = g->c[i].down && prev[i].down && g->c[i].id != prev[i].id;
    }
    if (contacts_changed || click != g->click_down) {
        tp_begin_settle(g);
    } else if (g->settling) {
        for (int i = 0; i < 2; i++) {
            if (!g->c[i].down) {
                continue;
            }
            int tx = g->c[i].x - g->settle_x[i];
            int ty = g->c[i].y - g->settle_y[i];
            if (tx * tx + ty * ty > TAP_MAX_TRAVEL * TAP_MAX_TRAVEL) {
                g->settling = false;
                break;
            }
        }
    }

    // Primary contact: slot 0 when down, else slot 1.
    int pi = g->c[0].down ? 0 : 1;
    const tp_contact_t *p = &g->c[pi];
    const tp_contact_t *pp = &prev[pi];

    // Movement, only for an ongoing contact of the same instance, and only
    // once the contacts have settled.
    if (!g->settling && p->down && pp->down && p->id == pp->id) {
        int dx = p->x - pp->x;
        int dy = p->y - pp->y;
        if (dx != 0 || dy != 0) {
            if (fingers >= 2) {
                tp_scroll(g, dy);
            } else if (!g->click_down || !g->click_right) {
                // One finger: pointer motion. Also while the pad is physically
                // clicked with one finger — that is a drag.
                tp_move_pointer(g, dx, dy);
            }
        }
    }

    // Travel bookkeeping for tap detection.
    for (int i = 0; i < 2; i++) {
        if (g->c[i].down && !g->c[i].moved) {
            int tx = g->c[i].x - g->c[i].start_x;
            int ty = g->c[i].y - g->c[i].start_y;
            if (tx * tx + ty * ty > TAP_MAX_TRAVEL * TAP_MAX_TRAVEL) {
                g->c[i].moved = true;
            }
        }
    }

    // Physical click (the pad is one big button): edge-triggered.
    if (click && !g->click_down) {
        g->click_right = g->fingers_seen >= 2;
        g->sink.button(g->sink.ud, g->click_right, true, true);
        g->click_down = true;
        g->gesture_clicked = true;
    } else if (!click && g->click_down) {
        tp_release_click(g);
    }

    // Tap-to-click: fires when the last finger lifts after a short, still,
    // unclicked gesture.
    if (fingers == 0 && (pp->down || prev[1 - pi].down)) {
        const tp_contact_t *last = pp->down ? pp : &prev[1 - pi];
        uint64_t held = tp_now_us() - last->t_down_us;
        bool any_moved = prev[0].moved || prev[1].moved || g->c[0].moved || g->c[1].moved;
        if (!g->gesture_clicked && !any_moved && held <= TAP_MAX_DURATION_US) {
            bool right = g->fingers_seen >= 2;
            g->sink.button(g->sink.ud, right, true, false);
            g->sink.button(g->sink.ud, right, false, false);
        }
        g->fingers_seen = 0;
        g->gesture_clicked = false;
        g->scroll_acc = 0;
        g->acc_x = g->acc_y = 0.f;
        g->ptr_used[0] = g->ptr_used[1] = false;
    }
}

static void tp_set_contact(tp_gesture_t *g, int slot, bool down, uint8_t id, int x, int y) {
    tp_contact_t *c = &g->c[slot];
    bool fresh = down && (!c->down || c->id != id);
    if (fresh) {
        // libinput resets its velocity trackers when a touch begins.
        g->velocity_ema = 0.0;
        g->have_move_time = false;
    }
    c->down = down;
    c->id = id;
    if (fresh) {
        c->start_x = x;
        c->start_y = y;
        c->t_down_us = tp_now_us();
        c->moved = false;
    }
    if (down) {
        c->x = x;
        c->y = y;
    }
}

/* A touching pad takes the gesture over once the current owner is fully idle;
 * everything else is ignored, so an idle second pad cannot corrupt the owner's
 * gesture. */
static bool tp_acquire_source(tp_gesture_t *g, uintptr_t source, bool engaged) {
    if (g->owner == source) {
        return true;
    }
    if (g->owner != 0 && (g->c[0].down || g->c[1].down || g->click_down)) {
        return false;
    }
    if (!engaged) {
        return false;
    }
    tp_gesture_reset(g);
    g->owner = source;
    return true;
}

void tp_gesture_init(tp_gesture_t *g, const tp_gesture_sink_t *sink) {
    memset(g, 0, sizeof(*g));
    g->sink = *sink;
    g->ui_scale = 1.0;
}

void tp_gesture_set_scale(tp_gesture_t *g, double ui_scale) {
    if (ui_scale > 0.0) {
        g->ui_scale = ui_scale;
    }
}

bool tp_gesture_busy(const tp_gesture_t *g) {
    return g->c[0].down || g->c[1].down || g->click_down || g->owner != 0;
}

void tp_gesture_reset(tp_gesture_t *g) {
    tp_release_click(g);
    memset(g->c, 0, sizeof(g->c));
    g->fingers_seen = 0;
    g->gesture_clicked = false;
    g->acc_x = g->acc_y = 0.f;
    g->scroll_acc = 0;
    g->velocity_ema = 0.0;
    g->have_move_time = false;
    g->settling = false;
    g->ptr_used[0] = g->ptr_used[1] = false;
    g->owner = 0;
}

void tp_gesture_feed_click(tp_gesture_t *g, uintptr_t source, bool down) {
    if (!tp_acquire_source(g, source, down)) {
        return;
    }
    tp_contact_t prev[2] = {g->c[0], g->c[1]};
    tp_process(g, prev, down);
}

void tp_gesture_feed_touch(tp_gesture_t *g, uintptr_t source, tp_touch_ev_t ev, uint32_t finger_id,
                           float x, float y) {
    if (!tp_acquire_source(g, source, ev == TP_TOUCH_DOWN)) {
        // Another pad owns the gesture state; this one stays a pad touchpad.
        return;
    }
    tp_contact_t prev[2] = {g->c[0], g->c[1]};

    // Resolve the pointer to a slot; allocate on DOWN.
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (g->ptr_used[i] && g->ptr_id[i] == finger_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (ev != TP_TOUCH_DOWN) {
            // A finger we never tracked: its DOWN went to the host before the
            // overlay opened. Ignore the rest of that gesture rather than
            // inventing a contact for it.
            return;
        }
        for (int i = 0; i < 2; i++) {
            if (!g->ptr_used[i]) {
                slot = i;
                g->ptr_used[i] = true;
                g->ptr_id[i] = finger_id;
                break;
            }
        }
        if (slot < 0) {
            // Third finger; the pad tracks two.
            return;
        }
    }

    int px = tp_clampi((int) (x * (PAD_W - 1)), 0, PAD_W - 1);
    int py = tp_clampi((int) (y * (PAD_H - 1)), 0, PAD_H - 1);
    switch (ev) {
        case TP_TOUCH_DOWN:
            tp_set_contact(g, slot, true, (uint8_t) (finger_id & 0x7f), px, py);
            break;
        case TP_TOUCH_MOVE:
            tp_set_contact(g, slot, g->c[slot].down, g->c[slot].id, px, py);
            break;
        case TP_TOUCH_UP:
        case TP_TOUCH_CANCEL:
            tp_set_contact(g, slot, false, g->c[slot].id, g->c[slot].x, g->c[slot].y);
            g->ptr_used[slot] = false;
            break;
        default:
            return;
    }
    /* The physical click is fed separately (DEVIATION-C 4), so its current
     * state rides along here rather than being hardcoded false. */
    tp_process(g, prev, g->click_down);
}
