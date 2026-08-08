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

static void moonlight_exclude_gamepad(stream_input_t *input, app_gamepad_state_t *gp,
                                      logical_device_t *item);

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

    /* NO sole-slot fallback: returning "the only enumerated pad" for a device
     * that matched neither by identity nor by VID:PID binds a bridged pad to a
     * bystander controller (one Xbox pad open, a DS5 auto-plugging before SDL
     * enumerates it), and the exclusion then removes the Xbox pad from the host
     * for the whole session. Stage 2 of hid_pt_moonlight_reconcile_exclusions
     * covers the genuine late-enumeration case by VID:PID within one poll tick. */
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

logical_device_t *hid_pt_peek_logical_for_gamepad(const app_gamepad_state_t *gamepad)
{
    // Lookup WITHOUT the moonlight_gs_id binding side effect: predicates
    // (hid_pt_gamepad_is_autoplug) run for every pad, and letting a mere match
    // rebind another pad's logical device corrupts the multi-DS5 slot/rumble
    // bindings. Binding is committed only by hid_pt_find_logical_for_gamepad.
    if (!gamepad) {
        return NULL;
    }
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        if (hid_pt_match_gamepad_to_logical(gamepad, item)) {
            return item;
        }
    }
    return NULL;
}

bool hid_pt_gamepad_is_autoplug(app_input_t *input, const app_gamepad_state_t *gamepad)
{
    (void) input;
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
    //
    // ONLY for genuinely serial-less pads (stable-id degraded to the sdl:
    // vid:pid:guid form): with a readable serial the primary lookup above is
    // authoritative, and running the fuzzy VID:PID+name fallback anyway would
    // adopt another same-model pad's pref for a genuinely-new controller.
    // Matching-independent stage: a pad whose VID:PID matches an already
    // BRIDGED logical device never belongs in the Moonlight input path,
    // regardless of whether identity matching works right now (every
    // same-model pad in this setup gets auto-plugged). This is what stops the
    // announce when SDL enumerates the pad only after the bridge claimed it.
    if (gamepad->controller) {
        SDL_Joystick *gjoy = SDL_GameControllerGetJoystick(gamepad->controller);
        uint16_t gvid = (uint16_t) SDL_JoystickGetVendor(gjoy);
        uint16_t gpid = (uint16_t) SDL_JoystickGetProduct(gjoy);
        for (int d = 0; d < g_devices.count; ++d) {
            const logical_device_t *pl = &g_devices.items[d];
            if (pl->plugged && vid_pid_equal_hex(pl->vid, pl->pid, gvid, gpid)) {
                commons_log_info("HID-PT", "autoplug: pad matches bridged %s by VID:PID", pl->name);
                return true;
            }
        }
    }
    char sid[96];
    hid_pt_stable_id_for_gamepad(gamepad, sid, sizeof(sid));
    logical_device_t *item = hid_pt_peek_logical_for_gamepad(gamepad);
    if (!item || !hid_pt_prefs_auto_plugin_for_logical(item)) {
        return false;
    }
    if (sid[0] && strncmp(sid, "sdl:", 4) != 0) {
        // Readable serial but the gamepad-keyed pref above missed (fresh pref
        // store, store/serial formatting drift): trust the logical device's
        // auto-plug pref ONLY on an exact stable-id identity — same physical
        // pad beyond doubt. The fuzzy VID:PID+name match stays reserved for
        // serial-less pads, so another same-model controller can never adopt
        // this pad's pref.
        char lid[96];
        hid_pt_stable_id_for_logical(item, lid, sizeof(lid));
        if (!mac_stable_ids_equal(sid, lid)) {
            return false;
        }
        commons_log_info("HID-PT", "autoplug fallback: %s matched logical auto-plug (exact id)",
                         item->name);
        return true;
    }
    commons_log_info("HID-PT", "autoplug fallback: %s matched logical auto-plug (serial-less gamepad)",
                     item->name);
    return true;
}

void hid_pt_moonlight_reconcile_exclusions(stream_input_t *input)
{
    // Convergent sweep, driven by the 1 Hz auto-plug poll: any bridged pad
    // whose Moonlight slot is not yet excluded gets excluded here, regardless
    // of the order BT connect, SDL enumeration and the bridge claim happened
    // in. Both point-in-time guards have unavoidable miss windows — the
    // plug-time exclusion runs before SDL has opened a freshly connected pad
    // (JOYDEVICEADDED lags the BT link by seconds), and the arrival-time guard
    // depends on pref/device state that may not exist yet. Each miss used to
    // leak a parallel ViGEm pad on the host, flapping the game Xbox<->DS5
    // until an app restart; this sweep converges within one poll tick instead.
    if (!input) {
        return;
    }
    int plugged = 0;
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        if (!item->plugged) {
            continue;
        }
        plugged++;
        app_gamepad_state_t *gp = hid_pt_find_gamepad_for_logical(input->input, item);
        if (!gp || gp->gs_id < 0) {
            continue;
        }
        if (input->moonlightExcludedMask & (1u << (unsigned) gp->gs_id)) {
            continue;
        }
        moonlight_exclude_gamepad(input, gp, item);
    }
    if (plugged == 0) {
        return;
    }
    // Stage 2, matching-independent: identity matching above can fail outright
    // (unreadable SDL serial + name drift, duplicate enumeration of one
    // physical pad — observed as an 18 s announced X360 twin that stage 1
    // never caught). A pad MODEL that is bridged must never keep feeding
    // Moonlight, so drop every still-announced gamepad that shares a plugged
    // logical device's VID:PID. Every same-model pad in this setup gets
    // auto-plugged, so this cannot orphan a legitimate Moonlight pad.
    static unsigned diag_tick = 0;
    diag_tick++;
    for (short i = 0; i < app_input_get_max_gamepads(input->input); ++i) {
        app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input->input, i);
        if (!gp || !gp->controller || gp->gs_id < 0) {
            continue;
        }
        if (input->moonlightExcludedMask & (1u << (unsigned) gp->gs_id)) {
            continue;
        }
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(gp->controller);
        uint16_t vid = (uint16_t) SDL_JoystickGetVendor(joy);
        uint16_t pid = (uint16_t) SDL_JoystickGetProduct(joy);
        bool excluded = false;
        for (int d = 0; d < g_devices.count; ++d) {
            logical_device_t *item = &g_devices.items[d];
            if (!item->plugged || !vid_pid_equal_hex(item->vid, item->pid, vid, pid)) {
                continue;
            }
            commons_log_warn("HID-PT", "sweep: excluding Moonlight slot %d by VID:PID of bridged %s",
                             gp->gs_id, item->name);
            moonlight_exclude_gamepad(input, gp, item);
            excluded = true;
            break;
        }
        // Diagnostic heartbeat (~15 s): a live, unexcluded Moonlight pad while
        // devices are bridged means both sweep stages failed — log the raw
        // identities so the residual mismatch is visible in the field.
        if (!excluded && diag_tick % 15 == 0) {
            for (int d = 0; d < g_devices.count; ++d) {
                logical_device_t *item = &g_devices.items[d];
                if (!item->plugged) {
                    continue;
                }
                commons_log_warn("HID-PT",
                                 "sweep: slot %d (vid=%04x pid=%04x) not excluded; bridged %s has vid='%s' pid='%s'",
                                 gp->gs_id, vid, pid, item->name, item->vid, item->pid);
            }
        }
    }
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

/* The logical device -- other than `except` -- that still bridges this Moonlight
 * slot, or NULL. Identity matching is fuzzy enough for two logical devices to
 * land on one gs_id, and clearing the bit for one of them re-announces a pad the
 * other is still bridging: a duplicate ViGEm pad next to the real one. */
static const logical_device_t *moonlight_slot_other_owner(int gs_id, const logical_device_t *except)
{
    if (gs_id < 0) {
        return NULL;
    }
    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *other = &g_devices.items[i];
        if (other == except || !other->plugged || other->moonlight_gs_id != (int8_t) gs_id) {
            continue;
        }
        return other;
    }
    return NULL;
}

static void moonlight_restore_gamepad(stream_input_t *input, app_gamepad_state_t *gp,
                                      logical_device_t *item)
{
    if (!input || !gp || gp->gs_id < 0) {
        return;
    }
    const logical_device_t *owner = moonlight_slot_other_owner(gp->gs_id, item);
    if (owner) {
        commons_log_info("HID-PT", "Moonlight slot %d stays excluded: still bridged by %s",
                         gp->gs_id, owner->name);
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

void hid_pt_moonlight_restore_slot(stream_input_t *input, int gs_id)
{
    if (!input || gs_id < 0) {
        return;
    }
    app_gamepad_state_t *gp = gamepad_by_gs_id(input->input, (int8_t) gs_id);
    if (gp) {
        moonlight_restore_gamepad(input, gp, NULL);
        return;
    }
    /* The pad left with the bridge (SDL closed it), so there is nobody to
     * announce — but the exclusion bit MUST still go. gs_ids are recycled
     * (app_input_gamepad_state_deinit keeps the id in the freed slot), so a bit
     * left behind silently kills the next controller that inherits the slot. */
    const logical_device_t *owner = moonlight_slot_other_owner(gs_id, NULL);
    if (owner) {
        commons_log_info("HID-PT", "Moonlight slot %d stays excluded: still bridged by %s",
                         gs_id, owner->name);
        return;
    }
    input->moonlightExcludedMask &= (uint16_t) ~(1u << (unsigned) gs_id);
    commons_log_info("HID-PT", "Moonlight slot %d released after HID bridge ended", gs_id);
}

#endif /* TARGET_WEBOS */
