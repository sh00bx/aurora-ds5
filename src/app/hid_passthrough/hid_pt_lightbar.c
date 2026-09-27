#if defined(TARGET_WEBOS)

#include "hid_pt_lightbar.h"

#include "hid_pt_gamepad_match.h"
#include "input/ds5_idle_lightbar.h"
#include "input/input_gamepad.h"
#include "stream/input/lightbar_pref.h"

#include <SDL.h>

#include <string.h>

/* Moonlight slots: gs_ids are 0..15, the announce mask is 16 bit. */
#define LB_SLOTS 16

typedef struct {
    /* An SDL pad sits in this slot. */
    bool open;
    /* ...and it is a DualSense, i.e. one the daemon's idle painter covers. */
    bool ds5;
    lightbar_pref_t user;
    /* The game painted a non-black colour and has not handed the bar back. */
    bool game_owns;
    uint32_t game_rgb;
} pad_lightbar_t;

static pad_lightbar_t g_pads[LB_SLOTS];
/* Guards g_pads: the host LED callback writes game_owns on the connection
 * thread. Held for a few stores only, never across an SDL call. */
static SDL_SpinLock g_pads_lock;

static bool slot_ok(int gs_id)
{
    return gs_id >= 0 && gs_id < LB_SLOTS;
}

/* The DualSense ids input_gamepad.c counts for the painter's SDL state. */
static bool gamepad_is_ds5(const app_gamepad_state_t *gamepad)
{
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(gamepad->controller);
    if (!joy || SDL_JoystickGetVendor(joy) != 0x054C) {
        return false;
    }
    const Uint16 product = SDL_JoystickGetProduct(joy);
    return product == 0x0CE6 || product == 0x0DF2;
}

/* What the bar shows for a pad with a colour. Caller holds g_pads_lock. */
static uint32_t shown_rgb(const pad_lightbar_t *p)
{
    return (p->user.game && p->game_owns) ? p->game_rgb : (p->user.rgb & 0xFFFFFFu);
}

static void set_led(SDL_GameController *controller, uint32_t rgb)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_GameControllerSetLED(controller, (Uint8) (rgb >> 16), (Uint8) (rgb >> 8), (Uint8) rgb);
#else
    (void) controller;
    (void) rgb;
#endif
}

/**
 * Hand ds5_txd's idle painter the colour of the DualSenses we hold over SDL.
 *
 * It paints one colour on every idle DualSense link, so it can follow the
 * user only while they all agree: none has a colour -> the old dark red, every
 * one shows the same colour -> that colour, anything else -> nothing at all.
 * A painter colour that is right for one pad would fight SDL on every other,
 * and an Automatic pad next to a coloured one is exactly such a disagreement.
 */
static void update_painter(void)
{
    bool coloured = false;
    bool automatic = false;
    bool differ = false;
    uint32_t rgb = 0;
    SDL_AtomicLock(&g_pads_lock);
    for (int i = 0; i < LB_SLOTS; ++i) {
        const pad_lightbar_t *p = &g_pads[i];
        if (!p->open || !p->ds5) {
            continue;
        }
        if (p->user.automatic) {
            automatic = true;
            continue;
        }
        const uint32_t shown = shown_rgb(p);
        differ = differ || (coloured && shown != rgb);
        rgb = shown;
        coloured = true;
    }
    SDL_AtomicUnlock(&g_pads_lock);
    if (!coloured) {
        ds5_idle_lb_set_sdl_colour(DS5_IDLE_LB_SDL_DEFAULT, 0);
    } else if (differ || automatic) {
        ds5_idle_lb_set_sdl_colour(DS5_IDLE_LB_SDL_NONE, 0);
    } else {
        ds5_idle_lb_set_sdl_colour(DS5_IDLE_LB_SDL_COLOUR, rgb);
    }
}

/* Resolve @p gamepad's choice and paint it. @p fresh_host_pad: the host has a
 * new pad for it, whose game has painted nothing yet. */
static void apply(app_input_t *input, app_gamepad_state_t *gamepad, bool fresh_host_pad)
{
    if (!gamepad || !gamepad->controller || !slot_ok(gamepad->gs_id)) {
        return;
    }
    const lightbar_pref_t user = hid_pt_gamepad_lightbar(input, gamepad);
    const bool ds5 = gamepad_is_ds5(gamepad);
    SDL_AtomicLock(&g_pads_lock);
    pad_lightbar_t *p = &g_pads[gamepad->gs_id];
    if (!p->open || fresh_host_pad) {
        p->game_owns = false;
    }
    p->open = true;
    p->ds5 = ds5;
    p->user = user;
    const uint32_t rgb = shown_rgb(p);
    SDL_AtomicUnlock(&g_pads_lock);
    /* Automatic paints nothing: whatever SDL, the host or the firmware put on
     * the bar stays, as it always did. */
    if (!user.automatic) {
        set_led(gamepad->controller, rgb);
    }
    update_painter();
}

void hid_pt_lightbar_pad_opened(app_input_t *input, app_gamepad_state_t *gamepad)
{
    apply(input, gamepad, true);
}

void hid_pt_lightbar_pad_arrived(app_input_t *input, app_gamepad_state_t *gamepad)
{
    apply(input, gamepad, true);
}

void hid_pt_lightbar_pad_closed(app_gamepad_state_t *gamepad)
{
    if (!gamepad || !slot_ok(gamepad->gs_id)) {
        return;
    }
    SDL_AtomicLock(&g_pads_lock);
    memset(&g_pads[gamepad->gs_id], 0, sizeof(g_pads[0]));
    SDL_AtomicUnlock(&g_pads_lock);
    update_painter();
}

bool hid_pt_lightbar_host_led(app_input_t *input, int gs_id, uint8_t r, uint8_t g, uint8_t b)
{
    if (!slot_ok(gs_id)) {
        return false;
    }
    const uint32_t rgb = ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
    SDL_AtomicLock(&g_pads_lock);
    pad_lightbar_t *p = &g_pads[gs_id];
    if (!p->open) {
        SDL_AtomicUnlock(&g_pads_lock);
        return false;
    }
    /* The ownership rule, tracked whatever the choice: a switch flipped to
     * "the game may" later shows what the game painted last. */
    p->game_owns = rgb != 0;
    if (rgb != 0) {
        p->game_rgb = rgb;
    }
    const lightbar_pref_t user = p->user;
    const uint32_t shown = shown_rgb(p);
    SDL_AtomicUnlock(&g_pads_lock);
    if (user.automatic) {
        return false;
    }
    if (user.game) {
        /* The game's colour while it owns the bar, the user's once it hands
         * it back with black. */
        app_input_gamepad_set_controller_led(input, (unsigned short) gs_id, (uint8_t) (shown >> 16),
                                             (uint8_t) (shown >> 8), (uint8_t) shown);
        update_painter();
    }
    return true;
}

void hid_pt_lightbar_refresh(app_input_t *input, uint16_t skip_mask)
{
    if (!input) {
        return;
    }
    for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
        if (!gp || !gp->controller || !slot_ok(gp->gs_id) || (skip_mask & (1u << (unsigned) gp->gs_id))) {
            continue;
        }
        apply(input, gp, false);
    }
}

#endif
