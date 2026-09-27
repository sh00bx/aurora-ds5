#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * The lightbar colour chosen for a controller on the Controllers page.
 *
 * Automatic is what every pad had before the choice existed, and the value of a
 * controller nobody ever set: aurora paints nothing of its own, and the game,
 * the host, SDL and the daemon's idle painter do exactly what they did.
 *
 * Anything else is a colour -- Off is one too, 000000, a dark bar rather than
 * "leave it alone" -- painted in every state aurora drives the pad in: over SDL,
 * mounted as HID, and idle in the daemon. @c game says what a game's own colour
 * does to it: with it, a game that paints the bar (Control's health bar) wins
 * until it paints black, which hands the bar back; without, the game's colour is
 * ignored and the bar keeps the user's.
 *
 * Plain values on purpose, like gamepad_type_pref_t: the pref store, the SDL
 * side and the bridge's controller layer all carry it, and only the first of
 * those can see CTM.
 */
typedef struct {
    bool automatic;
    /* 0xRRGGBB, 0 = off. Meaningless while automatic. */
    uint32_t rgb;
    /* The game may change it (the default). */
    bool game;
} lightbar_pref_t;

/** The value of a controller nobody chose a colour for. */
static inline lightbar_pref_t lightbar_pref_automatic(void)
{
    const lightbar_pref_t lb = {true, 0, true};
    return lb;
}

static inline bool lightbar_pref_equal(const lightbar_pref_t *a, const lightbar_pref_t *b)
{
    if (a->automatic || b->automatic) {
        return a->automatic == b->automatic;
    }
    return a->rgb == b->rgb && a->game == b->game;
}
