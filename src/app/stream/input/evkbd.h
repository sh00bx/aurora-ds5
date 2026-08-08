#pragma once

/*
 * Exclusive evdev keyboard capture.
 *
 * webOS routes a USB keyboard through the compositor, which keeps some keys for
 * itself: F12 adjusts TV volume and never reaches us, and the only app-facing
 * lever (wl_webos_shell_surface::set_key_mask) has no bit for function or volume
 * keys, so no amount of asking politely will surface them. Reading the evdev node
 * directly and grabbing it takes the keyboard away from the compositor entirely,
 * which is what makes a complete passthrough possible.
 *
 * The jailed app may do this on its own: webOS grants it the `compositor`
 * supplementary group at launch, and /dev/input/event* is root:compositor 0660.
 * No root helper is involved.
 */

#include <SDL_mutex.h>
#include <SDL_stdinc.h>

#include <stdbool.h>
#include <stdint.h>

/* One physical keyboard is the norm; the cap only bounds the scan. */
#define EVKBD_MAX_FDS 4

typedef struct evkbd_t evkbd_t;

typedef struct evkbd_event_t {
    uint16_t code;      /* evdev KEY_* code */
    bool pressed;
    bool repeat;        /* kernel auto-repeat rather than a fresh press */
} evkbd_event_t;

typedef void (*evkbd_listener_t)(const evkbd_event_t *event, void *userdata);

/* Opens every attached physical keyboard and grabs it. NULL when none qualifies,
 * which is the normal case on a TV with no keyboard plugged in. */
evkbd_t *evkbd_open_default(void);

void evkbd_close(evkbd_t *kbd);

/* Blocks until evkbd_interrupt(), dispatching key transitions to the listener. */
void evkbd_listen(evkbd_t *kbd, evkbd_listener_t listener, void *userdata);

void evkbd_interrupt(evkbd_t *kbd);

/* Hands the keyboard back to webOS (false) or takes it again (true). Used when an
 * in-app overlay needs the keys to arrive through SDL the ordinary way. */
void evkbd_set_grab(evkbd_t *kbd, bool grab);

int evkbd_device_count(const evkbd_t *kbd);
