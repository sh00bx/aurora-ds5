#pragma once

#include <SDL_keycode.h>
#include <SDL_thread.h>

typedef struct session_t session_t;

typedef struct session_evkbd_t {
    session_t *session;
    SDL_mutex *lock;
    SDL_cond *cond;
    SDL_Thread *thread;
    struct evkbd_t *dev;
    SDL_bool started;
    SDL_bool disabled;
    /* Sticky record of an interrupt, so one issued while the worker is still
     * inside evkbd_open_default() -- when there is no evkbd_t to interrupt yet --
     * is not lost, which used to leave the listener running with nothing able to
     * stop it and SDL_WaitThread() blocking the LVGL thread forever. */
    SDL_bool interrupted;
    /* Modifier state rebuilt from the raw key stream: with the device grabbed SDL
     * never sees these keys, so SDL_GetModState() would report nothing. */
    SDL_Keymod mods;
} session_evkbd_t;

void session_evkbd_init(session_evkbd_t *kbd, session_t *session);

void session_evkbd_deinit(session_evkbd_t *kbd);

void session_evkbd_interrupt(session_evkbd_t *kbd);

/* Hands the keyboard back to webOS so an in-app overlay can be driven through the
 * ordinary SDL path, and stops forwarding to the host until re-enabled. */
void session_evkbd_disable(session_evkbd_t *kbd);

void session_evkbd_enable(session_evkbd_t *kbd);

/* True while a physical keyboard is open AND exclusively grabbed, i.e. while SDL
 * cannot see that keyboard's keys at all. Callers use this to tell "webOS handed us
 * a key the compositor folded onto a shared scancode" apart from "this key had its
 * own route and cannot be the one that just arrived". */
SDL_bool session_evkbd_is_grabbing(session_evkbd_t *kbd);
