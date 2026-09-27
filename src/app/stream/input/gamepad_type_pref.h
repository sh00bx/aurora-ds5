#pragma once

#include <stdbool.h>

/**
 * Which controller type the host is asked to emulate for a pad that reaches it
 * over SDL (the Moonlight gamepad path), as chosen per controller on the
 * Controllers page.
 *
 * AUTO is what SDL detects, i.e. the behaviour every pad had before the choice
 * existed, and it is the value of a controller nobody ever set. The other two
 * override only the type in the arrival event: the capabilities stay what the
 * pad really has, so a DualShock announced as an Xbox pad keeps its touchpad
 * and motion (the host just ignores what an X360 target cannot use).
 *
 * Plain values on purpose: the pref store (webOS only), the arrival path and the
 * overlay all read it, and only the first of those can see CTM.
 */
typedef enum {
    GAMEPAD_TYPE_PREF_AUTO = 0,
    GAMEPAD_TYPE_PREF_XBOX,
    /* A DualShock 4 on the host: there is no virtual DualSense for SDL mode. */
    GAMEPAD_TYPE_PREF_PLAYSTATION,
} gamepad_type_pref_t;

#define GAMEPAD_TYPE_PREF_COUNT 3

/**
 * The pad the host builds for one announced with @p pref: the choice itself, or
 * for AUTO what it makes of the pad SDL detected -- a DualShock 4 for a
 * PlayStation pad or a Nintendo pad with motion sensors, an Xbox 360 for
 * everything else, the only two virtual pads it can create. The caller works
 * that out (@p detected_playstation, stream_input_gamepad_auto_builds_ds4()).
 * Never AUTO: this is what the Controllers page lights and what the overlay's
 * badge names.
 */
static inline gamepad_type_pref_t gamepad_type_pref_effective(gamepad_type_pref_t pref, bool detected_playstation)
{
    if (pref == GAMEPAD_TYPE_PREF_XBOX || pref == GAMEPAD_TYPE_PREF_PLAYSTATION) {
        return pref;
    }
    return detected_playstation ? GAMEPAD_TYPE_PREF_PLAYSTATION : GAMEPAD_TYPE_PREF_XBOX;
}

/**
 * A game's fixed controller mode: set on the Controllers page per host app
 * (Forza only takes an Xbox pad), it replaces the remembered mode of every
 * controller for as long as that app is streamed. NONE is no lock -- each
 * controller runs in its own mode.
 *
 * X360 and DS4 are the two SDL types; HID mounts every controller the bridge
 * can mount and leaves the others on SDL in their own type.
 */
typedef enum {
    GAMEPAD_MODE_NONE = 0,
    GAMEPAD_MODE_HID,
    GAMEPAD_MODE_X360,
    GAMEPAD_MODE_DS4,
} gamepad_mode_t;

/** The SDL type a locked mode forces, or AUTO for NONE and HID (no type forced). */
static inline gamepad_type_pref_t gamepad_mode_sdl_type(gamepad_mode_t mode)
{
    switch (mode) {
        case GAMEPAD_MODE_X360:
            return GAMEPAD_TYPE_PREF_XBOX;
        case GAMEPAD_MODE_DS4:
            return GAMEPAD_TYPE_PREF_PLAYSTATION;
        default:
            return GAMEPAD_TYPE_PREF_AUTO;
    }
}
