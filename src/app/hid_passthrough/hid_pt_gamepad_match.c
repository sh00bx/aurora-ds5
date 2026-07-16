#if defined(TARGET_WEBOS)

#define _GNU_SOURCE

#include "hid_pt_gamepad_match.h"

#include "hid_pt_device_prefs.h"
#include "ctm/ctm_state.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"
#include "stream/input/session_input.h"
#include "logging.h"

#include <SDL_version.h>
#include <ctype.h>
#include <string.h>

static bool mac_stable_ids_equal(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static bool vid_pid_equal_hex(const char *vid, const char *pid, uint16_t sdl_vid, uint16_t sdl_pid)
{
    if (!vid || !pid || !vid[0] || !pid[0]) {
        return false;
    }
    unsigned int hv = 0, hp = 0;
    if (sscanf(vid, "%x", &hv) != 1 || sscanf(pid, "%x", &hp) != 1) {
        return false;
    }
    return hv == sdl_vid && hp == sdl_pid;
}

static bool names_match_fuzzy(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    if (strcasecmp(a, b) == 0) {
        return true;
    }
    const char *shorter = strlen(a) <= strlen(b) ? a : b;
    const char *longer = shorter == a ? b : a;
    if (strlen(shorter) < 4) {
        return false;
    }
    return strcasestr(longer, shorter) != NULL;
}

static app_gamepad_state_t *gamepad_by_gs_id(app_input_t *input, int8_t gs_id)
{
    if (!input || gs_id < 0) {
        return NULL;
    }
    for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
        if (gp && gp->gs_id == gs_id) {
            return gp;
        }
    }
    return NULL;
}

static int count_gamepads_vid_pid(app_input_t *input, uint16_t vid, uint16_t pid, app_gamepad_state_t **only_out)
{
    int count = 0;
    app_gamepad_state_t *last = NULL;
    if (only_out) {
        *only_out = NULL;
    }
    for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
        if (!gp || !gp->controller) {
            continue;
        }
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(gp->controller);
        if ((uint16_t) SDL_JoystickGetVendor(joy) == vid && (uint16_t) SDL_JoystickGetProduct(joy) == pid) {
            count++;
            last = gp;
        }
    }
    if (only_out && count == 1) {
        *only_out = last;
    }
    return count;
}

bool hid_pt_match_gamepad_to_logical(const app_gamepad_state_t *gamepad,
                                     const logical_device_t *item)
{
    if (!gamepad || !gamepad->controller || !item) {
        return false;
    }

    char gamepad_id[96];
    char logical_id[96];
    hid_pt_stable_id_for_gamepad(gamepad, gamepad_id, sizeof(gamepad_id));
    hid_pt_stable_id_for_logical(item, logical_id, sizeof(logical_id));
    if (mac_stable_ids_equal(gamepad_id, logical_id)) {
        return true;
    }

    SDL_Joystick *joy = SDL_GameControllerGetJoystick(gamepad->controller);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char *serial = SDL_JoystickGetSerial(joy);
    if (serial && serial[0]) {
        char serial_id[96];
        hid_pt_stable_id_for_gamepad(gamepad, serial_id, sizeof(serial_id));
        if (serial_id[0] && strlen(serial_id) == 12 && mac_stable_ids_equal(serial_id, logical_id)) {
            return true;
        }
    }
#endif
    uint16_t vid = (uint16_t) SDL_JoystickGetVendor(joy);
    uint16_t pid = (uint16_t) SDL_JoystickGetProduct(joy);
    if (!vid_pid_equal_hex(item->vid, item->pid, vid, pid)) {
        return false;
    }

    const char *sdl_name = SDL_JoystickName(joy);
    if (names_match_fuzzy(sdl_name, item->name)) {
        return true;
    }

    return gamepad_id[0] && logical_id[0] && strcmp(gamepad_id, logical_id) == 0;
}

app_gamepad_state_t *hid_pt_find_gamepad_for_logical(app_input_t *input,
                                                     logical_device_t *item)
{
    if (!input || !item) {
        return NULL;
    }

    if (item->moonlight_gs_id >= 0) {
        app_gamepad_state_t *cached = gamepad_by_gs_id(input, item->moonlight_gs_id);
        if (cached) {
            return cached;
        }
        item->moonlight_gs_id = -1;
    }

    for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
        if (gp && hid_pt_match_gamepad_to_logical(gp, item)) {
            item->moonlight_gs_id = (int8_t) gp->gs_id;
            return gp;
        }
    }

    unsigned int vid = 0, pid = 0;
    if (item->vid[0] && item->pid[0] &&
        sscanf(item->vid, "%x", &vid) == 1 && sscanf(item->pid, "%x", &pid) == 1) {
        app_gamepad_state_t *only = NULL;
        if (count_gamepads_vid_pid(input, (uint16_t) vid, (uint16_t) pid, &only) == 1 && only) {
            item->moonlight_gs_id = (int8_t) only->gs_id;
            commons_log_info("HID-PT", "Matched %s to Moonlight slot %d by VID:PID",
                             item->name, only->gs_id);
            return only;
        }
    }

    if (app_input_get_gamepads_count(input) == 1) {
        for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
            app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
            if (gp) {
                item->moonlight_gs_id = (int8_t) gp->gs_id;
                commons_log_info("HID-PT", "Matched %s to sole Moonlight slot %d",
                                 item->name, gp->gs_id);
                return gp;
            }
        }
    }

    commons_log_warn("HID-PT", "No Moonlight gamepad match for HID device %s (vid=%s pid=%s)",
                     item->name, item->vid, item->pid);
    return NULL;
}

logical_device_t *hid_pt_find_logical_for_gamepad(app_input_t *input,
                                                  const app_gamepad_state_t *gamepad)
{
    (void) input;
    if (!gamepad) {
        return NULL;
    }
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        if (hid_pt_match_gamepad_to_logical(gamepad, item)) {
            item->moonlight_gs_id = (int8_t) gamepad->gs_id;
            return item;
        }
    }
    return NULL;
}

bool hid_pt_gamepad_is_autoplug(app_input_t *input, const app_gamepad_state_t *gamepad)
{
    if (!gamepad) {
        return false;
    }
    // Primary: pref keyed by the gamepad's live SDL serial/MAC stable-id.
    if (hid_pt_prefs_auto_plugin_for_gamepad(gamepad)) {
        return true;
    }
    // Fallback for the stream-churn / mid-session re-enumeration race: the SDL
    // serial can be transiently unreadable right after a (re)connect, so the
    // stable-id degrades to sdl:vid:pid:guid and misses the MAC-keyed pref.
    // Match to a known logical device (hid_pt_match_gamepad_to_logical also
    // matches by VID:PID + name, robust to a missing serial) and check the
    // auto-plug pref on the LOGICAL device (keyed by its own stored MAC). Without
    // this a bridged DS5 leaks a parallel ViGEm/Xbox pad to the host and the game
    // flaps between Xbox and DS5. Non-CTM pads have no auto-plug logical device.
    logical_device_t *item = hid_pt_find_logical_for_gamepad(input, gamepad);
    if (item && hid_pt_prefs_auto_plugin_for_logical(item)) {
        commons_log_info("HID-PT", "autoplug fallback: %s matched logical auto-plug (serial-less gamepad)",
                         item->name);
        return true;
    }
    return false;
}

uint16_t hid_pt_moonlight_excluded_mask_at_start(app_input_t *input)
{
    uint16_t mask = 0;
    if (!input) {
        return 0;
    }
    for (short i = 0; i < app_input_get_max_gamepads(input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
        if (!gp) {
            continue;
        }
        if (hid_pt_gamepad_is_autoplug(input, gp)) {
            mask |= (uint16_t) (1u << gp->gs_id);
            logical_device_t *item = hid_pt_find_logical_for_gamepad(input, gp);
            if (item) {
                item->moonlight_gs_id = (int8_t) gp->gs_id;
            }
        }
    }
    return mask;
}

bool hid_pt_gamepad_is_moonlight_excluded(const stream_input_t *input,
                                          const app_gamepad_state_t *gamepad)
{
    if (!input || !gamepad || gamepad->gs_id < 0) {
        return false;
    }
    return (input->moonlightExcludedMask & (1u << (unsigned) gamepad->gs_id)) != 0;
}

static void moonlight_exclude_gamepad(stream_input_t *input, app_gamepad_state_t *gp,
                                      logical_device_t *item)
{
    if (!input || !gp || gp->gs_id < 0) {
        return;
    }
    if (input->started) {
        stream_input_send_gamepad_remove(input, gp);
    }
    input->moonlightExcludedMask |= (uint16_t) (1u << gp->gs_id);
    if (item) {
        item->moonlight_gs_id = (int8_t) gp->gs_id;
    }
    commons_log_info("HID-PT", "Moonlight slot %d removed for HID bridge (%s)",
                     gp->gs_id, item ? item->name : "?");
}

static void moonlight_restore_gamepad(stream_input_t *input, app_gamepad_state_t *gp,
                                      logical_device_t *item)
{
    if (!input || !gp || gp->gs_id < 0) {
        return;
    }
    input->moonlightExcludedMask &= (uint16_t) ~(1u << gp->gs_id);
    if (input->started) {
        stream_input_send_gamepad_arrive(input, gp);
    }
    if (item) {
        item->moonlight_gs_id = (int8_t) gp->gs_id;
    }
    commons_log_info("HID-PT", "Moonlight slot %d restored after HID unplug (%s)",
                     gp->gs_id, item ? item->name : "?");
}

void hid_pt_moonlight_exclude(stream_input_t *input, logical_device_t *item)
{
    if (!input || !item) {
        return;
    }
    app_gamepad_state_t *gp = hid_pt_find_gamepad_for_logical(input->input, item);
    moonlight_exclude_gamepad(input, gp, item);
}

void hid_pt_moonlight_restore(stream_input_t *input, logical_device_t *item)
{
    if (!input || !item) {
        return;
    }
    app_gamepad_state_t *gp = hid_pt_find_gamepad_for_logical(input->input, item);
    moonlight_restore_gamepad(input, gp, item);
}

#endif /* TARGET_WEBOS */
