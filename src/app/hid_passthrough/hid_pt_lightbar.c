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
    /* The host has an SDL pad for it (from its arrival to its removal), so its
     * colour is the one of the mode the host builds. Main thread only. */
    bool on_host;
    lightbar_pref_t user;
    /* The game painted a non-black colour and has not handed the bar back. */
    bool game_owns;
    uint32_t game_rgb;
    /* The host sent this slot's pad an LED event, and the last one's colour
     * (black included): what Automatic puts back. Per host pad. */
    bool host_seen;
    uint32_t host_rgb;
    /* A colour of ours is on the bar -- SDL wrote it, or the bridge paints it
     * while the pad is mounted -- so Automatic has to take it off again. */
    bool ours;
    /* The colour picker's live preview: shown instead of anything else, and
     * never stored. */
    bool preview;
    uint32_t preview_rgb;
} pad_lightbar_t;

static pad_lightbar_t g_pads[LB_SLOTS];
/* Guards g_pads: the host LED callback writes game_owns and host_seen on the
 * connection thread. Held for a few stores only, never across an SDL call. */
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

/* A colour of ours is to be on the bar: the preview, or a chosen colour. */
static bool coloured(const pad_lightbar_t *p)
{
    return p->preview || !p->user.automatic;
}

/* What the bar shows for a coloured pad. Caller holds g_pads_lock. */
static uint32_t shown_rgb(const pad_lightbar_t *p)
{
    if (p->preview) {
        return p->preview_rgb;
    }
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
 * The colour SDL itself gives @p controller's bar: its HIDAPI PS4/PS5 drivers
 * paint the player-index colour (SetLedsForPlayerIndex(), the list
 * hid-sony.c uses) on open and on every SDL_GameControllerSetPlayerIndex() --
 * but only until an application sets an LED, which latches the driver's
 * colour for good (color_set stays true until the pad is reopened), so a
 * player index set again would just repaint ours. The value is therefore
 * computed here from the same tables and written like any colour. False for a
 * pad SDL paints nothing on.
 */
static bool sdl_default_rgb(SDL_GameController *controller, uint32_t *rgb)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    /* Blue, red, green, pink, orange, teal, white. The first four are what a
     * PS4 assigns; the last three differ between the two drivers. */
    static const uint32_t ps4[7] = {0x000040, 0x400000, 0x004000, 0x200020, 0x020100, 0x000101, 0x010101};
    static const uint32_t ps5[7] = {0x000040, 0x400000, 0x004000, 0x200020, 0x201000, 0x001010, 0x101010};
    const uint32_t *table;
    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_PS4:
            table = ps4;
            break;
        case SDL_CONTROLLER_TYPE_PS5:
            table = ps5;
            break;
        default:
            return false;
    }
    int player = SDL_GameControllerGetPlayerIndex(controller);
    player = player >= 0 ? player % 7 : 0;
    *rgb = table[player];
    return true;
#else
    (void) controller;
    (void) rgb;
    return false;
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
 * Each pad's colour is its mode's (apply()), so in the menus that is the mode
 * it comes back in, and in a stream the one SDL paints.
 */
static void update_painter(void)
{
    bool any = false;
    bool automatic = false;
    bool differ = false;
    uint32_t rgb = 0;
    SDL_AtomicLock(&g_pads_lock);
    for (int i = 0; i < LB_SLOTS; ++i) {
        const pad_lightbar_t *p = &g_pads[i];
        if (!p->open || !p->ds5) {
            continue;
        }
        if (!coloured(p)) {
            automatic = true;
            continue;
        }
        const uint32_t shown = shown_rgb(p);
        differ = differ || (any && shown != rgb);
        rgb = shown;
        any = true;
    }
    SDL_AtomicUnlock(&g_pads_lock);
    if (!any) {
        ds5_idle_lb_set_sdl_colour(DS5_IDLE_LB_SDL_DEFAULT, 0);
    } else if (differ || automatic) {
        ds5_idle_lb_set_sdl_colour(DS5_IDLE_LB_SDL_NONE, 0);
    } else {
        ds5_idle_lb_set_sdl_colour(DS5_IDLE_LB_SDL_COLOUR, rgb);
    }
}

/**
 * Resolve @p gamepad's colour and paint it -- unless @p bridged: the bridge
 * paints a mounted pad (controller_ds4.c/controller_ds5.c), and an SDL write
 * there would be a second writer; its record still follows, for the painter
 * and for what Automatic has to undo later. @p fresh_host_pad: the host has a
 * new pad for it, whose game has painted nothing yet.
 *
 * Which of the controller's colours: HID while bridged; the mode the host
 * builds while it has a pad for it (the game's lock included); otherwise --
 * the menus, or a stream that has not announced it -- the mode it comes back
 * in, which is also what the idle painter is to paint.
 *
 * Automatic paints nothing, as it always did -- except where a colour of ours
 * is on the bar: that goes, for the last colour the host sent this pad, else
 * for SDL's own player colour, so switching back to Automatic (or into a mode
 * that has none) shows at once what the pad would show without us.
 */
static void apply(app_input_t *input, app_gamepad_state_t *gamepad, bool fresh_host_pad, bool bridged)
{
    if (!gamepad || !gamepad->controller || !slot_ok(gamepad->gs_id)) {
        return;
    }
    pad_lightbar_t *p = &g_pads[gamepad->gs_id];
    const gamepad_mode_t mode = bridged ? GAMEPAD_MODE_HID
                                        : p->on_host ? hid_pt_gamepad_host_mode(input, gamepad)
                                                     : hid_pt_gamepad_own_mode(input, gamepad);
    const lightbar_pref_t user = hid_pt_gamepad_lightbar(input, gamepad, mode);
    const bool ds5 = gamepad_is_ds5(gamepad);
    SDL_AtomicLock(&g_pads_lock);
    if (!p->open || fresh_host_pad) {
        p->game_owns = false;
        p->host_seen = false;
    }
    p->open = true;
    p->ds5 = ds5;
    p->user = user;
    const bool colour = coloured(p);
    const uint32_t rgb = shown_rgb(p);
    uint32_t restore = 0;
    bool restoring = false;
    if (bridged) {
        p->ours = p->ours || colour;
    } else {
        restoring = !colour && p->ours;
        restore = p->host_rgb;
        if (restoring && !p->host_seen && !sdl_default_rgb(gamepad->controller, &restore)) {
            restoring = false;
        }
        p->ours = colour;
    }
    SDL_AtomicUnlock(&g_pads_lock);
    if (!bridged && colour) {
        set_led(gamepad->controller, rgb);
    } else if (restoring) {
        set_led(gamepad->controller, restore);
    }
    update_painter();
}

void hid_pt_lightbar_pad_opened(app_input_t *input, app_gamepad_state_t *gamepad)
{
    if (gamepad && slot_ok(gamepad->gs_id)) {
        g_pads[gamepad->gs_id].on_host = false;
    }
    apply(input, gamepad, true, false);
}

void hid_pt_lightbar_pad_arrived(app_input_t *input, app_gamepad_state_t *gamepad)
{
    if (gamepad && slot_ok(gamepad->gs_id)) {
        g_pads[gamepad->gs_id].on_host = true;
    }
    apply(input, gamepad, true, false);
}

void hid_pt_lightbar_pad_removed(app_gamepad_state_t *gamepad)
{
    if (gamepad && slot_ok(gamepad->gs_id)) {
        g_pads[gamepad->gs_id].on_host = false;
    }
}

void hid_pt_lightbar_pad_bridged(app_input_t *input, app_gamepad_state_t *gamepad)
{
    if (!gamepad || !slot_ok(gamepad->gs_id)) {
        return;
    }
    g_pads[gamepad->gs_id].on_host = false;
    if (g_pads[gamepad->gs_id].open) {
        apply(input, gamepad, false, true);
    }
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
     * "the game may" later shows what the game painted last, and Automatic
     * puts back what the host sent last. */
    p->host_seen = true;
    p->host_rgb = rgb;
    p->game_owns = rgb != 0;
    if (rgb != 0) {
        p->game_rgb = rgb;
    }
    const lightbar_pref_t user = p->user;
    const bool preview = p->preview;
    const uint32_t shown = shown_rgb(p);
    SDL_AtomicUnlock(&g_pads_lock);
    if (preview) {
        /* The picker's colour stays on the bar while it is open; the game's
         * is recorded for when it closes. */
        return true;
    }
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
        if (!gp || !gp->controller || !slot_ok(gp->gs_id)) {
            continue;
        }
        /* A skipped (bridged) slot still takes the new choice into its record:
         * it is what the idle painter is handed once the stream ends and the
         * daemon holds the pad again. */
        apply(input, gp, false, (skip_mask & (1u << (unsigned) gp->gs_id)) != 0);
    }
}

void hid_pt_lightbar_preview(app_input_t *input, app_gamepad_state_t *gamepad, uint32_t rgb)
{
    if (!gamepad || !gamepad->controller || !slot_ok(gamepad->gs_id)) {
        return;
    }
    SDL_AtomicLock(&g_pads_lock);
    pad_lightbar_t *p = &g_pads[gamepad->gs_id];
    const bool changed = !p->preview || p->preview_rgb != (rgb & 0xFFFFFFu);
    p->preview = true;
    p->preview_rgb = rgb & 0xFFFFFFu;
    SDL_AtomicUnlock(&g_pads_lock);
    if (changed) {
        apply(input, gamepad, false, false);
    }
}

void hid_pt_lightbar_preview_end(app_input_t *input, uint16_t skip_mask)
{
    bool any = false;
    SDL_AtomicLock(&g_pads_lock);
    for (int i = 0; i < LB_SLOTS; ++i) {
        any = any || g_pads[i].preview;
        g_pads[i].preview = false;
    }
    SDL_AtomicUnlock(&g_pads_lock);
    if (any) {
        hid_pt_lightbar_refresh(input, skip_mask);
    }
}

void hid_pt_lightbar_stream_ended(app_input_t *input)
{
    /* The game that owned a bar is gone with its stream: its colour must not
     * stay on the pad, nor be what the idle painter keeps repainting. Nor the
     * host's pads, whose colours are what Automatic put back; and a picker
     * still open is cancelled. */
    SDL_AtomicLock(&g_pads_lock);
    for (int i = 0; i < LB_SLOTS; ++i) {
        g_pads[i].game_owns = false;
        g_pads[i].host_seen = false;
        g_pads[i].on_host = false;
        g_pads[i].preview = false;
    }
    SDL_AtomicUnlock(&g_pads_lock);
    hid_pt_lightbar_refresh(input, 0);
}

#endif
