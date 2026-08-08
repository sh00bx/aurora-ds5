#include "session_evkbd.h"

#include "evkbd.h"
#include "session_input.h"
#include "stream/session_priv.h"

#include <SDL.h>
#include <linux/input.h>

#include "logging.h"

static int kbd_worker(session_evkbd_t *kbd);

static void kbd_listener(const evkbd_event_t *event, void *userdata);

/*
 * evdev keycode -> SDL scancode.
 *
 * Deliberately translating into SDL's vocabulary rather than straight to VK
 * codes: it lets the grabbed keys re-enter stream_input_handle_key() exactly as
 * SDL-delivered ones do, so the special-combo detection, the pressed-key
 * bookkeeping, the view-only guard and the webOS remote interception all keep
 * working with no second implementation to drift out of sync.
 */
static SDL_Scancode scancode_from_evdev(uint16_t code) {
    switch (code) {
        case KEY_ESC: return SDL_SCANCODE_ESCAPE;
        case KEY_1: return SDL_SCANCODE_1;
        case KEY_2: return SDL_SCANCODE_2;
        case KEY_3: return SDL_SCANCODE_3;
        case KEY_4: return SDL_SCANCODE_4;
        case KEY_5: return SDL_SCANCODE_5;
        case KEY_6: return SDL_SCANCODE_6;
        case KEY_7: return SDL_SCANCODE_7;
        case KEY_8: return SDL_SCANCODE_8;
        case KEY_9: return SDL_SCANCODE_9;
        case KEY_0: return SDL_SCANCODE_0;
        case KEY_MINUS: return SDL_SCANCODE_MINUS;
        case KEY_EQUAL: return SDL_SCANCODE_EQUALS;
        case KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
        case KEY_TAB: return SDL_SCANCODE_TAB;
        case KEY_Q: return SDL_SCANCODE_Q;
        case KEY_W: return SDL_SCANCODE_W;
        case KEY_E: return SDL_SCANCODE_E;
        case KEY_R: return SDL_SCANCODE_R;
        case KEY_T: return SDL_SCANCODE_T;
        case KEY_Y: return SDL_SCANCODE_Y;
        case KEY_U: return SDL_SCANCODE_U;
        case KEY_I: return SDL_SCANCODE_I;
        case KEY_O: return SDL_SCANCODE_O;
        case KEY_P: return SDL_SCANCODE_P;
        case KEY_LEFTBRACE: return SDL_SCANCODE_LEFTBRACKET;
        case KEY_RIGHTBRACE: return SDL_SCANCODE_RIGHTBRACKET;
        case KEY_ENTER: return SDL_SCANCODE_RETURN;
        case KEY_LEFTCTRL: return SDL_SCANCODE_LCTRL;
        case KEY_A: return SDL_SCANCODE_A;
        case KEY_S: return SDL_SCANCODE_S;
        case KEY_D: return SDL_SCANCODE_D;
        case KEY_F: return SDL_SCANCODE_F;
        case KEY_G: return SDL_SCANCODE_G;
        case KEY_H: return SDL_SCANCODE_H;
        case KEY_J: return SDL_SCANCODE_J;
        case KEY_K: return SDL_SCANCODE_K;
        case KEY_L: return SDL_SCANCODE_L;
        case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
        case KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
        case KEY_GRAVE: return SDL_SCANCODE_GRAVE;
        case KEY_LEFTSHIFT: return SDL_SCANCODE_LSHIFT;
        case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
        case KEY_Z: return SDL_SCANCODE_Z;
        case KEY_X: return SDL_SCANCODE_X;
        case KEY_C: return SDL_SCANCODE_C;
        case KEY_V: return SDL_SCANCODE_V;
        case KEY_B: return SDL_SCANCODE_B;
        case KEY_N: return SDL_SCANCODE_N;
        case KEY_M: return SDL_SCANCODE_M;
        case KEY_COMMA: return SDL_SCANCODE_COMMA;
        case KEY_DOT: return SDL_SCANCODE_PERIOD;
        case KEY_SLASH: return SDL_SCANCODE_SLASH;
        case KEY_RIGHTSHIFT: return SDL_SCANCODE_RSHIFT;
        case KEY_KPASTERISK: return SDL_SCANCODE_KP_MULTIPLY;
        case KEY_LEFTALT: return SDL_SCANCODE_LALT;
        case KEY_SPACE: return SDL_SCANCODE_SPACE;
        case KEY_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
        case KEY_F1: return SDL_SCANCODE_F1;
        case KEY_F2: return SDL_SCANCODE_F2;
        case KEY_F3: return SDL_SCANCODE_F3;
        case KEY_F4: return SDL_SCANCODE_F4;
        case KEY_F5: return SDL_SCANCODE_F5;
        case KEY_F6: return SDL_SCANCODE_F6;
        case KEY_F7: return SDL_SCANCODE_F7;
        case KEY_F8: return SDL_SCANCODE_F8;
        case KEY_F9: return SDL_SCANCODE_F9;
        case KEY_F10: return SDL_SCANCODE_F10;
        case KEY_NUMLOCK: return SDL_SCANCODE_NUMLOCKCLEAR;
        case KEY_SCROLLLOCK: return SDL_SCANCODE_SCROLLLOCK;
        case KEY_KP7: return SDL_SCANCODE_KP_7;
        case KEY_KP8: return SDL_SCANCODE_KP_8;
        case KEY_KP9: return SDL_SCANCODE_KP_9;
        case KEY_KPMINUS: return SDL_SCANCODE_KP_MINUS;
        case KEY_KP4: return SDL_SCANCODE_KP_4;
        case KEY_KP5: return SDL_SCANCODE_KP_5;
        case KEY_KP6: return SDL_SCANCODE_KP_6;
        case KEY_KPPLUS: return SDL_SCANCODE_KP_PLUS;
        case KEY_KP1: return SDL_SCANCODE_KP_1;
        case KEY_KP2: return SDL_SCANCODE_KP_2;
        case KEY_KP3: return SDL_SCANCODE_KP_3;
        case KEY_KP0: return SDL_SCANCODE_KP_0;
        case KEY_KPDOT: return SDL_SCANCODE_KP_PERIOD;
        case KEY_102ND: return SDL_SCANCODE_NONUSBACKSLASH;
        case KEY_F11: return SDL_SCANCODE_F11;
        case KEY_F12: return SDL_SCANCODE_F12;
        case KEY_KPENTER: return SDL_SCANCODE_KP_ENTER;
        case KEY_RIGHTCTRL: return SDL_SCANCODE_RCTRL;
        case KEY_KPSLASH: return SDL_SCANCODE_KP_DIVIDE;
        case KEY_SYSRQ: return SDL_SCANCODE_PRINTSCREEN;
        case KEY_RIGHTALT: return SDL_SCANCODE_RALT;
        case KEY_HOME: return SDL_SCANCODE_HOME;
        case KEY_UP: return SDL_SCANCODE_UP;
        case KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
        case KEY_LEFT: return SDL_SCANCODE_LEFT;
        case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
        case KEY_END: return SDL_SCANCODE_END;
        case KEY_DOWN: return SDL_SCANCODE_DOWN;
        case KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
        case KEY_INSERT: return SDL_SCANCODE_INSERT;
        case KEY_DELETE: return SDL_SCANCODE_DELETE;
        case KEY_MUTE: return SDL_SCANCODE_MUTE;
        case KEY_VOLUMEDOWN: return SDL_SCANCODE_VOLUMEDOWN;
        case KEY_VOLUMEUP: return SDL_SCANCODE_VOLUMEUP;
        case KEY_PAUSE: return SDL_SCANCODE_PAUSE;
        case KEY_KPEQUAL: return SDL_SCANCODE_KP_EQUALS;
        case KEY_LEFTMETA: return SDL_SCANCODE_LGUI;
        case KEY_RIGHTMETA: return SDL_SCANCODE_RGUI;
        case KEY_COMPOSE: return SDL_SCANCODE_APPLICATION;
        case KEY_F13: return SDL_SCANCODE_F13;
        case KEY_F14: return SDL_SCANCODE_F14;
        case KEY_F15: return SDL_SCANCODE_F15;
        case KEY_F16: return SDL_SCANCODE_F16;
        case KEY_F17: return SDL_SCANCODE_F17;
        case KEY_F18: return SDL_SCANCODE_F18;
        case KEY_F19: return SDL_SCANCODE_F19;
        case KEY_F20: return SDL_SCANCODE_F20;
        case KEY_F21: return SDL_SCANCODE_F21;
        case KEY_F22: return SDL_SCANCODE_F22;
        case KEY_F23: return SDL_SCANCODE_F23;
        case KEY_F24: return SDL_SCANCODE_F24;
        case KEY_NEXTSONG: return SDL_SCANCODE_AUDIONEXT;
        case KEY_PLAYPAUSE: return SDL_SCANCODE_AUDIOPLAY;
        case KEY_PREVIOUSSONG: return SDL_SCANCODE_AUDIOPREV;
        case KEY_STOPCD: return SDL_SCANCODE_AUDIOSTOP;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

static SDL_Keymod modifier_bit(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_LCTRL: return KMOD_LCTRL;
        case SDL_SCANCODE_RCTRL: return KMOD_RCTRL;
        case SDL_SCANCODE_LSHIFT: return KMOD_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return KMOD_RSHIFT;
        case SDL_SCANCODE_LALT: return KMOD_LALT;
        case SDL_SCANCODE_RALT: return KMOD_RALT;
        case SDL_SCANCODE_LGUI: return KMOD_LGUI;
        case SDL_SCANCODE_RGUI: return KMOD_RGUI;
        default: return KMOD_NONE;
    }
}

void session_evkbd_init(session_evkbd_t *kbd, session_t *session) {
    kbd->session = session;
    kbd->lock = SDL_CreateMutex();
    kbd->cond = SDL_CreateCond();
    kbd->dev = NULL;
    kbd->started = SDL_FALSE;
    kbd->disabled = SDL_FALSE;
    kbd->interrupted = SDL_FALSE;
    kbd->mods = KMOD_NONE;
    kbd->thread = SDL_CreateThread((SDL_ThreadFunction) kbd_worker, "sessevkbd", kbd);
    kbd->started = SDL_TRUE;
}

void session_evkbd_deinit(session_evkbd_t *kbd) {
    if (!kbd->started) {
        return;
    }
    session_evkbd_interrupt(kbd);
    if (kbd->thread != NULL) {
        SDL_WaitThread(kbd->thread, NULL);
        kbd->thread = NULL;
    }
    SDL_DestroyMutex(kbd->lock);
    kbd->lock = NULL;
    SDL_DestroyCond(kbd->cond);
    kbd->cond = NULL;
    kbd->started = SDL_FALSE;
}

void session_evkbd_interrupt(session_evkbd_t *kbd) {
    if (!kbd->started) {
        return;
    }
    SDL_LockMutex(kbd->lock);
    kbd->interrupted = SDL_TRUE;
    if (kbd->dev != NULL) {
        evkbd_interrupt(kbd->dev);
    }
    SDL_UnlockMutex(kbd->lock);
}

void session_evkbd_disable(session_evkbd_t *kbd) {
    if (!kbd->started) {
        return;
    }
    SDL_LockMutex(kbd->lock);
    if (!kbd->disabled) {
        commons_log_info("Session", "EvKbd release keyboard to webOS");
        kbd->disabled = SDL_TRUE;
        if (kbd->dev != NULL) {
            evkbd_set_grab(kbd->dev, false);
        }
        kbd->mods = KMOD_NONE;
    }
    SDL_UnlockMutex(kbd->lock);
}

void session_evkbd_enable(session_evkbd_t *kbd) {
    if (!kbd->started) {
        return;
    }
    SDL_LockMutex(kbd->lock);
    if (kbd->disabled) {
        commons_log_info("Session", "EvKbd capture keyboard");
        kbd->disabled = SDL_FALSE;
        if (kbd->dev != NULL) {
            evkbd_set_grab(kbd->dev, true);
        }
        kbd->mods = KMOD_NONE;
    }
    SDL_UnlockMutex(kbd->lock);
}

static void set_dev(session_evkbd_t *kbd, evkbd_t *dev) {
    SDL_LockMutex(kbd->lock);
    kbd->dev = dev;
    SDL_CondSignal(kbd->cond);
    SDL_UnlockMutex(kbd->lock);
}

static int kbd_worker(session_evkbd_t *kbd) {
    evkbd_t *dev = evkbd_open_default();
    set_dev(kbd, dev);
    if (dev == NULL) {
        commons_log_info("Session", "No physical keyboard to capture");
        return 0;
    }
    SDL_LockMutex(kbd->lock);
    SDL_bool interrupted = kbd->interrupted;
    SDL_UnlockMutex(kbd->lock);
    if (!interrupted) {
        commons_log_info("Session", "EvKbd captured %d keyboard(s)", evkbd_device_count(dev));
        evkbd_listen(dev, kbd_listener, kbd);
    }
    set_dev(kbd, NULL);
    evkbd_close(dev);
    commons_log_info("Session", "EvKbd released");
    return 0;
}

static void kbd_listener(const evkbd_event_t *event, void *userdata) {
    session_evkbd_t *kbd = userdata;
    SDL_LockMutex(kbd->lock);
    session_t *session = kbd->session;
    if (kbd->disabled || !session_accepting_input(session)) {
        SDL_UnlockMutex(kbd->lock);
        return;
    }

    SDL_Scancode scancode = scancode_from_evdev(event->code);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        commons_log_debug("Session", "EvKbd unmapped evdev code %u", event->code);
        SDL_UnlockMutex(kbd->lock);
        return;
    }

    SDL_Keymod bit = modifier_bit(scancode);
    if (bit != KMOD_NONE) {
        if (event->pressed) {
            kbd->mods |= bit;
        } else {
            kbd->mods &= ~bit;
        }
    }

    SDL_KeyboardEvent synthetic = {
            .type = event->pressed ? SDL_KEYDOWN : SDL_KEYUP,
            .timestamp = SDL_GetTicks(),
            .windowID = STREAM_INPUT_EVKBD_WINDOW_ID,
            .state = event->pressed ? SDL_PRESSED : SDL_RELEASED,
            .repeat = event->repeat ? 1 : 0,
            .keysym = {
                    .scancode = scancode,
                    .sym = SDL_GetKeyFromScancode(scancode),
                    .mod = (Uint16) kbd->mods,
            },
    };
    SDL_UnlockMutex(kbd->lock);

    /* Dispatched with kbd->lock released. stream_input_handle_key() can reach
     * session_interrupt() through the quit combo, and that takes session->mutex
     * and then comes back for kbd->lock via session_input_interrupt() -- holding
     * kbd->lock across this call is the AB-BA that hung the app.
     *
     * The key bookkeeping this lands in is shared with the SDL main thread (the
     * Magic Remote is never grabbed, so its keys keep arriving there); it carries
     * its own lock. */
    stream_input_handle_key(&session->input, &synthetic);
}
