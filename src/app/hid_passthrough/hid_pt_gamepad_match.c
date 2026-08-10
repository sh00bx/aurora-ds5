#if defined(TARGET_WEBOS)

#define _GNU_SOURCE

#include "hid_pt_gamepad_match.h"

#include "hid_pt_device_prefs.h"
#include "ctm/ctm_state.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"
#include "stream/input/session_input.h"
#include "logging.h"

#include <string.h>

static void moonlight_exclude_gamepad(stream_input_t *input, app_gamepad_state_t *gp,
                                      logical_device_t *item);

/* How much evidence a caller demands of a match: the floor it passes to a
 * resolver, which then ignores every tier below it. Destructive callers -- the
 * ones whose match takes a pad away from the host for the rest of the stream --
 * ask for more than diagnostic ones. */
#define HID_PT_CONF_FUZZY  30
#define HID_PT_CONF_VIDPID 60
#define HID_PT_CONF_EXACT  100

/* What a resolver found: the peer, the name of the tier that decided (for the
 * log -- a DS5 that resolves by anything other than EXACT_ID is a symptom), and
 * that tier's confidence. `gamepad`/`item` NULL means nothing resolved. */
typedef struct {
    app_gamepad_state_t *gamepad;
    const char *tier;
    int confidence;
} hid_pt_match_t;

typedef struct {
    logical_device_t *item;
    const char *tier;
    int confidence;
} hid_pt_logical_match_t;

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

static bool gamepad_vid_pid(const app_gamepad_state_t *gamepad, uint16_t *vid, uint16_t *pid)
{
    if (!gamepad || !gamepad->controller) {
        return false;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(gamepad->controller);
    *vid = (uint16_t) SDL_JoystickGetVendor(joy);
    *pid = (uint16_t) SDL_JoystickGetProduct(joy);
    return true;
}

/* ---- identity tiers -----------------------------------------------------
 *
 * One ordered table answers "is this Moonlight pad that bridged device?". Each
 * row is a PURE predicate over the (pad, logical device) pair; everything that
 * is not a property of the pair -- ordering, ambiguity, which slots are already
 * spoken for, whether a match may be cached -- belongs to the resolvers below
 * and is written exactly once there.
 *
 * The plan this table came from listed a SERIAL_MAC row between EXACT_ID and
 * the VID:PID rows. It is deliberately absent: it would have fired when the
 * pad's SDL serial normalised to the logical device's MAC, and since
 * hid_pt_stable_id() became the single normalisation for both sides (commit
 * "let a pad with an odd serial find its own auto-plug pref again"), that
 * condition IS tier_exact_id(). A separate row could never have been reached.
 */
typedef struct {
    const char *name;
    int confidence;
    bool (*match)(const app_gamepad_state_t *gamepad, const logical_device_t *item);
    /* May a match at this tier be written into item->moonlight_gs_id? Only
     * identity-grade evidence may: the cached slot survives across polls, so a
     * guess cached here would keep answering for the rest of the session. */
    bool binds;
    /* Evidence that only holds when nothing else could be meant: the resolver
     * refuses this tier unless exactly one candidate matches it. */
    bool requires_unique;
} hid_pt_tier_t;

static bool tier_exact_id(const app_gamepad_state_t *gamepad, const logical_device_t *item)
{
    char gid[HID_PT_STABLE_ID_LEN];
    char lid[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_gamepad(gamepad, gid, sizeof(gid));
    hid_pt_stable_id_for_logical(item, lid, sizeof(lid));
    return gid[0] && lid[0] && strcmp(gid, lid) == 0;
}

static bool tier_vidpid(const app_gamepad_state_t *gamepad, const logical_device_t *item)
{
    uint16_t vid = 0, pid = 0;
    if (!gamepad_vid_pid(gamepad, &vid, &pid)) {
        return false;
    }
    return vid_pid_equal_hex(item->vid, item->pid, vid, pid);
}

static bool tier_vidpid_name_fuzzy(const app_gamepad_state_t *gamepad, const logical_device_t *item)
{
    if (!tier_vidpid(gamepad, item)) {
        return false;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(gamepad->controller);
    return names_match_fuzzy(SDL_JoystickName(joy), item->name);
}

static const hid_pt_tier_t g_tiers[] = {
    {"EXACT_ID",          HID_PT_CONF_EXACT,  tier_exact_id,          true,  false},
    {"VIDPID_UNIQUE",     HID_PT_CONF_VIDPID, tier_vidpid,            true,  true},
    {"VIDPID_NAME_FUZZY", HID_PT_CONF_FUZZY,  tier_vidpid_name_fuzzy, false, false},
};

#define HID_PT_TIER_COUNT ((int) (sizeof(g_tiers) / sizeof(g_tiers[0])))

/* True when a PLUGGED logical device other than `except` already answers for
 * this Moonlight slot. One device, one slot: two same-model bridges otherwise
 * both resolve to the first pad, the sibling keeps moonlight_gs_id == -1, and on
 * teardown moonlight_slot_other_owner() sees the slot as unowned, clears the
 * exclusion bit and announces an arrival for a pad the other bridge still holds.
 * Stage 2 of the sweep has carried this rule for a while; the resolver is where
 * it belongs. */
static bool slot_claimed_by_other(short gs_id, const logical_device_t *except)
{
    if (gs_id < 0) {
        return false;
    }
    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *d = &g_devices.items[i];
        if (d == except || !d->plugged) {
            continue;
        }
        if (d->moonlight_gs_id == (int8_t) gs_id) {
            return true;
        }
    }
    return false;
}

/* Which Moonlight pad IS this logical device, using only tiers at or above
 * min_confidence. Tier-major: the strongest kind of evidence decides across ALL
 * pads before a weaker kind is consulted anywhere. The old cascade was pad-major
 * and so let a low-numbered pad that matched only by fuzzy name beat a
 * high-numbered pad that matched by identity. */
static hid_pt_match_t hid_pt_resolve(app_input_t *input, logical_device_t *item, int min_confidence)
{
    hid_pt_match_t result = {NULL, NULL, 0};
    if (!input || !item) {
        return result;
    }

    /* The pad behind the cached slot is gone, so the slot number means nothing
     * now. Below, a still-live cached slot is only ever used to break a tie
     * between pads that match a tier anyway -- it is never returned unchecked,
     * which is what let one bad match stick for a whole session. */
    if (item->moonlight_gs_id >= 0 && !gamepad_by_gs_id(input, item->moonlight_gs_id)) {
        item->moonlight_gs_id = -1;
    }

    const short max_pads = app_input_get_max_gamepads(input);
    for (int t = 0; t < HID_PT_TIER_COUNT; ++t) {
        const hid_pt_tier_t *tier = &g_tiers[t];
        if (tier->confidence < min_confidence) {
            continue;
        }
        int matched = 0;
        app_gamepad_state_t *pick = NULL;
        for (short i = 0; i < max_pads; ++i) {
            app_gamepad_state_t *gp = app_input_gamepad_state_by_index(input, i);
            if (!gp || !gp->controller || !tier->match(gp, item)) {
                continue;
            }
            matched++;
            if (slot_claimed_by_other(gp->gs_id, item)) {
                continue;
            }
            if (!pick || (item->moonlight_gs_id >= 0 && gp->gs_id == item->moonlight_gs_id)) {
                pick = gp;
            }
        }
        if (tier->requires_unique && matched != 1) {
            continue;
        }
        if (!pick) {
            continue;
        }
        if (tier->binds && pick->gs_id >= 0) {
            item->moonlight_gs_id = (int8_t) pick->gs_id;
        }
        result.gamepad = pick;
        result.tier = tier->name;
        result.confidence = tier->confidence;
        return result;
    }
    /* NO sole-slot fallback: "the only enumerated pad" for a device that matched
     * on no tier binds a bridged pad to a bystander controller (one Xbox pad
     * open, a DS5 auto-plugging before SDL enumerates it), and the exclusion
     * then removes the Xbox pad from the host for the whole session. Stage 2 of
     * hid_pt_moonlight_reconcile_exclusions covers the genuine late-enumeration
     * case by VID:PID within one poll tick. */
    return result;
}

/* The mirror of hid_pt_resolve: which bridged device IS this pad. `bind` asks
 * for the match to be cached; it is honoured only for tiers that carry
 * identity-grade evidence, so a predicate that runs over every pad can never
 * rebind another pad's logical device and corrupt the slot/rumble routing. */
static hid_pt_logical_match_t resolve_logical(const app_gamepad_state_t *gamepad,
                                              int min_confidence, bool bind)
{
    hid_pt_logical_match_t result = {NULL, NULL, 0};
    if (!gamepad) {
        return result;
    }
    for (int t = 0; t < HID_PT_TIER_COUNT; ++t) {
        const hid_pt_tier_t *tier = &g_tiers[t];
        if (tier->confidence < min_confidence) {
            continue;
        }
        int matched = 0;
        logical_device_t *pick = NULL;
        for (int d = 0; d < g_devices.count; ++d) {
            logical_device_t *item = &g_devices.items[d];
            if (!tier->match(gamepad, item)) {
                continue;
            }
            matched++;
            if (!pick) {
                pick = item;
            }
        }
        if (tier->requires_unique && matched != 1) {
            continue;
        }
        if (!pick) {
            continue;
        }
        if (bind && tier->binds && gamepad->gs_id >= 0) {
            pick->moonlight_gs_id = (int8_t) gamepad->gs_id;
        }
        result.item = pick;
        result.tier = tier->name;
        result.confidence = tier->confidence;
        return result;
    }
    return result;
}

bool hid_pt_gamepad_is_autoplug(app_input_t *input, const app_gamepad_state_t *gamepad)
{
    (void) input;
    if (!gamepad) {
        return false;
    }
    /* Primary: the pref keyed by this pad's own stable id. */
    if (hid_pt_prefs_auto_plugin_for_gamepad(gamepad)) {
        return true;
    }
    /* Matching-independent: a pad whose VID:PID matches an already BRIDGED
     * logical device never belongs in the Moonlight input path, whether or not
     * identity matching works right now (every same-model pad in this setup gets
     * auto-plugged). This is what stops the announce when SDL enumerates the pad
     * only after the bridge has claimed it. */
    uint16_t gvid = 0, gpid = 0;
    if (gamepad_vid_pid(gamepad, &gvid, &gpid)) {
        for (int d = 0; d < g_devices.count; ++d) {
            const logical_device_t *pl = &g_devices.items[d];
            if (pl->plugged && vid_pid_equal_hex(pl->vid, pl->pid, gvid, gpid)) {
                commons_log_info("HID-PT", "autoplug: pad matches bridged %s by VID:PID", pl->name);
                return true;
            }
        }
    }
    /* Fallback for the stream-churn / mid-session re-enumeration race: the SDL
     * serial can be transiently unreadable right after a (re)connect, so this
     * pad's stable id degrades to the synthetic per-model form and misses the
     * MAC-keyed pref above. Resolve it to a known logical device instead and
     * read the auto-plug pref there. Without this a bridged DS5 leaks a parallel
     * ViGEm/Xbox pad to the host and the game flaps between Xbox and DS5.
     *
     * The confidence floor is the whole "only for serial-less pads" rule: with a
     * readable serial only EXACT_ID is trusted, because a weaker match would
     * hand a genuinely-new controller the pref of another same-model pad. A pad
     * with no usable serial has nothing better available, so it may use the
     * VID:PID tiers. No binding is requested -- this predicate runs for every
     * pad, and a mere match must not rebind anything. */
    char sid[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_gamepad(gamepad, sid, sizeof(sid));
    const int floor_confidence =
        hid_pt_stable_id_is_synthetic(sid) ? HID_PT_CONF_FUZZY : HID_PT_CONF_EXACT;
    hid_pt_logical_match_t m = resolve_logical(gamepad, floor_confidence, false);
    if (!m.item || !hid_pt_prefs_auto_plugin_for_logical(m.item)) {
        return false;
    }
    commons_log_info("HID-PT", "autoplug fallback: %s matched logical auto-plug (%s)",
                     m.item->name, m.tier);
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
        /* Destructive: whatever this resolves gets removed from the host.
         * Identity, or a VID:PID that can only mean this device -- never a
         * name that merely looks similar. */
        hid_pt_match_t m = hid_pt_resolve(input->input, item, HID_PT_CONF_VIDPID);
        if (!m.gamepad || m.gamepad->gs_id < 0) {
            continue;
        }
        if (input->moonlightExcludedMask & (1u << (unsigned) m.gamepad->gs_id)) {
            continue;
        }
        commons_log_info("HID-PT", "sweep: %s resolved to Moonlight slot %d via %s",
                         item->name, m.gamepad->gs_id, m.tier);
        moonlight_exclude_gamepad(input, m.gamepad, item);
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
            /* One device, one slot. Without this a second same-model bridge never
             * gets a slot recorded: both pads match the FIRST plugged device, whose
             * moonlight_gs_id is simply overwritten, and the sibling stays at -1.
             * moonlight_slot_other_owner() only recognises an owner through that
             * field, so on teardown it would see the slot as unowned, clear the bit
             * and announce an arrival for a pad another live bridge still holds.
             * Stage 1 walks the tier table from scratch every tick and clears a
             * cached slot whose pad is gone -- it never hands back a cached
             * binding without re-matching it -- so a stale id cannot pin a
             * device here. */
            if (item->moonlight_gs_id >= 0 && item->moonlight_gs_id != (int8_t) gp->gs_id) {
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
            /* Record which bridged device owns the slot we are about to take
             * away, so the teardown paths can give it back to the right one.
             * resolve_logical commits the binding itself, for the tiers whose
             * evidence may be cached -- which is why the floor here costs
             * nothing: the tier it excludes could not have bound anyway. */
            resolve_logical(gp, HID_PT_CONF_VIDPID, true);
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
 * slot, or NULL. Clearing the exclusion bit while another device still bridges
 * that slot re-announces a pad the other is holding: a duplicate ViGEm pad next
 * to the real one. Both resolvers and stage 2 of the sweep now refuse a slot
 * another plugged device claims, so two devices should not reach one gs_id in
 * the first place -- this is the check that does not depend on that holding. */
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
    /* Destructive: same floor as the sweep. */
    hid_pt_match_t m = hid_pt_resolve(input->input, item, HID_PT_CONF_VIDPID);
    if (!m.gamepad) {
        commons_log_warn("HID-PT", "No Moonlight gamepad match for HID device %s (vid=%s pid=%s)",
                         item->name, item->vid, item->pid);
        return;
    }
    commons_log_info("HID-PT", "%s resolved to Moonlight slot %d via %s",
                     item->name, m.gamepad->gs_id, m.tier);
    moonlight_exclude_gamepad(input, m.gamepad, item);
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
