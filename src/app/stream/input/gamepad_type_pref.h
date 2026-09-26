#pragma once

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
