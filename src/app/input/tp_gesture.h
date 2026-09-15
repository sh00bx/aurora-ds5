/**
 * Controller touchpad -> mouse gestures, as the host does them.
 *
 * A port of Vibepollo's `namespace tpmouse` (src/ds5_touchpad_mouse.cpp), the
 * code that moves the Windows cursor when the DS5 touchpad is used on the
 * desktop through Aurora. Same contacts, same settle rules, same velocity
 * estimate, same acceleration curve, same tap and scroll thresholds — so a
 * swipe in Aurora's own menus feels exactly like a swipe on the streamed
 * desktop, which is the whole point of having it here.
 *
 * Specifically it ports the host's `feed_touch_event()` path: normalized
 * 0..1 contacts, no pad clock. That is precisely what SDL hands us in
 * SDL_ControllerTouchpadEvent, for a DualSense and a DualShock 4 alike, so
 * neither pad needs a special case (the host makes the same choice: for
 * normalized input the pad geometry is only an internal unit scale).
 *
 * Deviations from the original are marked DEVIATION-C in the implementation.
 * Everything else is meant to stay diffable against that file line by line.
 *
 * Not thread safe, and deliberately so: every caller is on the LVGL thread.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum tp_touch_ev_t {
    TP_TOUCH_DOWN,
    TP_TOUCH_MOVE,
    TP_TOUCH_UP,
    TP_TOUCH_CANCEL,
} tp_touch_ev_t;

/**
 * Where the synthesized mouse goes. `notches` is signed, one wheel detent each.
 *
 * `physical` tells the two kinds of button apart: the pad's own click, which
 * spans reports and can drag, versus a tap, which the engine reports as a
 * down/up pair inside a single call and which therefore has to be stretched by
 * whoever consumes it.
 */
typedef struct tp_gesture_sink_t {
    void (*move)(void *ud, int dx, int dy);
    void (*scroll)(void *ud, int notches);
    void (*button)(void *ud, bool right, bool down, bool physical);
    void *ud;
} tp_gesture_sink_t;

typedef struct tp_contact_t {
    bool down;
    uint8_t id; /* contact counter / pointer slot tag */
    int x, y;
    int start_x, start_y;
    uint64_t t_down_us;
    bool moved;
} tp_contact_t;

typedef struct tp_gesture_t {
    tp_gesture_sink_t sink;
    tp_contact_t c[2];
    int fingers_seen; /* max concurrent contacts during the current gesture */
    bool click_down;
    bool click_right;
    bool gesture_clicked; /* physical click happened during this contact */
    float acc_x, acc_y;
    int scroll_acc;
    /* Settling: contacts were disturbed by something that is not a movement,
     * so motion and scroll are held until a finger travels deliberately again. */
    bool settling;
    int settle_x[2], settle_y[2];
    /* Pointer velocity tracking (see tp_move_pointer) */
    uint64_t last_move_us;
    bool have_move_time;
    double velocity_ema; /* pad units per microsecond, smoothed */
    /* SDL path: map SDL finger index -> slot */
    uint32_t ptr_id[2];
    bool ptr_used[2];
    /* The pad currently driving the gesture state (0 = none). Everything here
     * is per-gesture, so interleaved reports from a second pad would corrupt
     * it -- the non-owner is ignored instead. */
    uintptr_t owner;
    /* Output scale, so the cursor covers the same stretch of the TV panel as
     * it would on the streamed desktop. 1.0 = the host's numbers verbatim. */
    double ui_scale;
} tp_gesture_t;

void tp_gesture_init(tp_gesture_t *g, const tp_gesture_sink_t *sink);

void tp_gesture_set_scale(tp_gesture_t *g, double ui_scale);

/** One normalized contact update. @p x and @p y are SDL's 0..1. */
void tp_gesture_feed_touch(tp_gesture_t *g, uintptr_t source, tp_touch_ev_t ev, uint32_t finger_id,
                           float x, float y);

/** The pad's physical click, which the host's normalized path never sees. */
void tp_gesture_feed_click(tp_gesture_t *g, uintptr_t source, bool down);

/** @return true while a contact or the click is down, i.e. mid-gesture. */
bool tp_gesture_busy(const tp_gesture_t *g);

/** Drop the gesture and release a held button through the sink. */
void tp_gesture_reset(tp_gesture_t *g);
