#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Which controller type the host is asked to emulate for a pad that reaches it
 * over SDL (the Moonlight gamepad path), as chosen per controller on the
 * Controllers page.
 *
 * AUTO is what SDL detects, i.e. the behaviour every pad had before the choice
 * existed, and it is the value of a controller nobody ever set. The others
 * override the type in the arrival event and ask the host for exactly that
 * virtual pad (the wire contract below): the capabilities stay what the pad
 * really has, so a DualShock announced as an Xbox pad keeps its touchpad and
 * motion (the host just ignores what an X360 target cannot use).
 *
 * Plain values on purpose: the pref store (webOS only), the arrival path and the
 * overlay all read it, and only the first of those can see CTM.
 */
typedef enum {
    GAMEPAD_TYPE_PREF_AUTO = 0,
    GAMEPAD_TYPE_PREF_XBOX,
    /* A DualShock 4 on the host. */
    GAMEPAD_TYPE_PREF_PLAYSTATION,
    /* A DualSense on the host (Vibepollo's own virtual-gamepad driver). */
    GAMEPAD_TYPE_PREF_DUALSENSE,
    /* A Switch Pro controller on the host (the same driver). */
    GAMEPAD_TYPE_PREF_SWITCH,
} gamepad_type_pref_t;

#define GAMEPAD_TYPE_PREF_COUNT 5

/**
 * The word a type goes by in the ini (`<id>.sdl_type = ...`) and in the log --
 * one table, so the two cannot drift apart when a type is added. NULL for a
 * value outside the enum.
 */
static inline const char *gamepad_type_pref_word(gamepad_type_pref_t type)
{
    switch (type) {
        case GAMEPAD_TYPE_PREF_XBOX:
            return "xbox";
        case GAMEPAD_TYPE_PREF_PLAYSTATION:
            return "playstation";
        case GAMEPAD_TYPE_PREF_DUALSENSE:
            return "dualsense";
        case GAMEPAD_TYPE_PREF_SWITCH:
            return "switch";
        case GAMEPAD_TYPE_PREF_AUTO:
            return "auto";
        default:
            return NULL;
    }
}

/**
 * The wire contract: which virtual pad the host is asked to build, carried in
 * the controller arrival (LiSendControllerArrivalEvent()) in bits 13-15 of
 * `capabilities`, which moonlight-common-c passes through unmasked and no
 * LI_CCAP_* flag uses. The Vibepollo host reads the same table:
 *
 *   (caps >> 13) & 7   pad
 *   0                  no request: AUTO, the host decides from `type` as it
 *                      always did
 *   1                  Xbox 360
 *   2                  DualShock 4
 *   3                  DualSense
 *   4                  Switch Pro
 *   5-7                reserved, never sent
 *
 * Every explicit type sends its request, X360 and DS4 included, and `type`
 * stays consistent with it -- LI_CTYPE_XBOX for X360, LI_CTYPE_PS for DS4 and
 * DualSense, LI_CTYPE_NINTENDO for Switch -- so a host without the extension
 * still builds the nearest pad it has. Not the TLV metadata of the arrival:
 * this host refuses an arrival packet longer than the fixed struct.
 */
#define GAMEPAD_WIRE_PAD_SHIFT  13
#define GAMEPAD_WIRE_PAD_MASK   0xE000u
#define GAMEPAD_WIRE_PAD_NONE   0u
#define GAMEPAD_WIRE_PAD_X360   1u
#define GAMEPAD_WIRE_PAD_DS4    2u
#define GAMEPAD_WIRE_PAD_DS5    3u
#define GAMEPAD_WIRE_PAD_SWITCH 4u

/** The arrival's capability bits that ask the host for @p pref's pad. */
static inline uint16_t gamepad_type_pref_wire_caps(gamepad_type_pref_t pref)
{
    unsigned pad;
    switch (pref) {
        case GAMEPAD_TYPE_PREF_XBOX:
            pad = GAMEPAD_WIRE_PAD_X360;
            break;
        case GAMEPAD_TYPE_PREF_PLAYSTATION:
            pad = GAMEPAD_WIRE_PAD_DS4;
            break;
        case GAMEPAD_TYPE_PREF_DUALSENSE:
            pad = GAMEPAD_WIRE_PAD_DS5;
            break;
        case GAMEPAD_TYPE_PREF_SWITCH:
            pad = GAMEPAD_WIRE_PAD_SWITCH;
            break;
        case GAMEPAD_TYPE_PREF_AUTO:
        default:
            pad = GAMEPAD_WIRE_PAD_NONE;
            break;
    }
    return (uint16_t) ((pad << GAMEPAD_WIRE_PAD_SHIFT) & GAMEPAD_WIRE_PAD_MASK);
}

/**
 * The pad the host builds for one announced with @p pref: the choice itself, or
 * for AUTO what it makes of the pad SDL detected -- a DualShock 4 for a
 * PlayStation pad or a Nintendo pad with motion sensors, an Xbox 360 for
 * everything else, its rule for an arrival that asks for nothing. The caller
 * works that out (@p detected_playstation,
 * stream_input_gamepad_auto_builds_ds4()). Never AUTO: this is what the
 * Controllers page lights and what the overlay's badge names.
 */
static inline gamepad_type_pref_t gamepad_type_pref_effective(gamepad_type_pref_t pref, bool detected_playstation)
{
    if (pref != GAMEPAD_TYPE_PREF_AUTO && (unsigned) pref < GAMEPAD_TYPE_PREF_COUNT) {
        return pref;
    }
    return detected_playstation ? GAMEPAD_TYPE_PREF_PLAYSTATION : GAMEPAD_TYPE_PREF_XBOX;
}

/**
 * A controller's mode: mounted as HID, or over SDL as one of the host's
 * virtual pads. It is what a game can fix -- set on the Controllers page per
 * host app (Forza only takes an Xbox pad), it replaces the remembered mode of
 * every controller for as long as that app is streamed -- and what a lightbar
 * colour is chosen per. NONE is no lock: each controller runs in its own mode.
 *
 * X360, DS4, DS5 and SWITCH are the SDL types; HID mounts every controller the
 * bridge can mount and leaves the others on SDL in their own type.
 */
typedef enum {
    GAMEPAD_MODE_NONE = 0,
    GAMEPAD_MODE_HID,
    GAMEPAD_MODE_X360,
    GAMEPAD_MODE_DS4,
    GAMEPAD_MODE_DS5,
    GAMEPAD_MODE_SWITCH,
} gamepad_mode_t;

#define GAMEPAD_MODE_COUNT 6

/** The SDL type a locked mode forces, or AUTO for NONE and HID (no type forced). */
static inline gamepad_type_pref_t gamepad_mode_sdl_type(gamepad_mode_t mode)
{
    switch (mode) {
        case GAMEPAD_MODE_X360:
            return GAMEPAD_TYPE_PREF_XBOX;
        case GAMEPAD_MODE_DS4:
            return GAMEPAD_TYPE_PREF_PLAYSTATION;
        case GAMEPAD_MODE_DS5:
            return GAMEPAD_TYPE_PREF_DUALSENSE;
        case GAMEPAD_MODE_SWITCH:
            return GAMEPAD_TYPE_PREF_SWITCH;
        default:
            return GAMEPAD_TYPE_PREF_AUTO;
    }
}

/** The mode an SDL type runs in; NONE for AUTO (resolve it first). */
static inline gamepad_mode_t gamepad_type_pref_mode(gamepad_type_pref_t type)
{
    switch (type) {
        case GAMEPAD_TYPE_PREF_XBOX:
            return GAMEPAD_MODE_X360;
        case GAMEPAD_TYPE_PREF_PLAYSTATION:
            return GAMEPAD_MODE_DS4;
        case GAMEPAD_TYPE_PREF_DUALSENSE:
            return GAMEPAD_MODE_DS5;
        case GAMEPAD_TYPE_PREF_SWITCH:
            return GAMEPAD_MODE_SWITCH;
        case GAMEPAD_TYPE_PREF_AUTO:
        default:
            return GAMEPAD_MODE_NONE;
    }
}

/** One of the SDL modes, i.e. a lock that keeps every pad off the bridge. */
static inline bool gamepad_mode_is_sdl(gamepad_mode_t mode)
{
    return gamepad_mode_sdl_type(mode) != GAMEPAD_TYPE_PREF_AUTO;
}

/**
 * The name a mode goes by everywhere it is shown -- its button on the
 * Controllers page, a row's state line, the overlay's pad badge, the LIGHTBAR
 * heading -- and in the ini (lowercased there). NULL for NONE.
 */
static inline const char *gamepad_mode_label(gamepad_mode_t mode)
{
    switch (mode) {
        case GAMEPAD_MODE_HID:
            return "HID";
        case GAMEPAD_MODE_X360:
            return "X360";
        case GAMEPAD_MODE_DS4:
            return "DS4";
        case GAMEPAD_MODE_DS5:
            return "DS5";
        case GAMEPAD_MODE_SWITCH:
            return "SWITCH";
        case GAMEPAD_MODE_NONE:
        default:
            return NULL;
    }
}
