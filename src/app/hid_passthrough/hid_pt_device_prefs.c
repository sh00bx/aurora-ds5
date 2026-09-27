#if defined(TARGET_WEBOS)

#define _GNU_SOURCE

#include "hid_pt_device_prefs.h"

#include "ctm/ctm_state.h"
#include "input/app_input.h"

#include "app.h"
#include "app_settings.h"
#include "ini_writer.h"
#include "logging.h"
#include "util/ini_ext.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HID_PT_PREFS_MAX 32

/* Prefix of the synthetic identity used for a pad with no usable serial. Chosen
 * so hid_pt_stable_id() leaves it alone: '_' is inside the allowed set, ':'
 * would have been stripped and the prefix would then be indistinguishable from
 * a serial that happens to start with "sdl". */
#define HID_PT_SYNTHETIC_PREFIX "sdl_"

/* Key suffix of the SDL type pref:
 * `<stable_id>.sdl_type = xbox|playstation|dualsense|switch|auto`.
 * A separate key rather than a new value for the auto-plug line, so a file
 * written by this build still reads correctly in an older one: there the whole
 * key normalises to an id no device has, and its value is not "true", so the
 * old parser drops it exactly like an opted-out entry. */
#define HID_PT_SDL_TYPE_SUFFIX ".sdl_type"

/* Key suffixes of the lightbar pref, same reasoning as the SDL type's: one
 * colour per mode, `<stable_id>.lightbar.<mode> = auto|off|rrggbb` with <mode>
 * one of hid|x360|ds4|ds5|switch, and, only for colours the game may NOT
 * change, `<stable_id>.lightbar_game = 0` -- one switch per controller.
 * 1.7.30 kept one colour per controller as `<stable_id>.lightbar`: still read,
 * as the colour of every mode without a key of its own, never written again
 * but kept, so its owner keeps their colour wherever they have not chosen
 * another. */
#define HID_PT_LIGHTBAR_SUFFIX ".lightbar"
#define HID_PT_LIGHTBAR_GAME_SUFFIX ".lightbar_game"

typedef struct {
    char id[HID_PT_STABLE_ID_LEN];
    bool auto_plugin;
    /* An SDL type was chosen for this id, and sdl_type is that choice -- AUTO
     * included. An explicit AUTO is not the same as no choice: a CTM device's
     * id is read before the pad's own (hid_pt_gamepad_sdl_type()), so
     * "Automatic" picked while the pad was mounted has to be able to outvote an
     * older Xbox stored under the pad's SDL serial. */
    bool sdl_type_set;
    gamepad_type_pref_t sdl_type;
    /* The lightbar colour chosen for this id per mode, indexed by
     * gamepad_mode_t; [GAMEPAD_MODE_NONE] is 1.7.30's one colour, the fallback
     * of every mode without its own. `set` includes an explicit Automatic --
     * the same device-first lookup as sdl_type_set, for the same reason. */
    struct {
        bool set;
        bool automatic;
        uint32_t rgb;
    } lightbar[GAMEPAD_MODE_COUNT];
    /* Stored inverted, so a zeroed entry means the default: the game may. One
     * for every mode's colour. */
    bool lightbar_game_off;
} hid_pt_pref_entry_t;

static hid_pt_pref_entry_t g_hid_pt_prefs[HID_PT_PREFS_MAX];
static int g_hid_pt_pref_count;

#define HID_PT_APP_MODES_SECTION "controller_app_modes"
/* Games with a fixed mode. A lock is a deliberate act per game, so this fills
 * slowly and a full table refuses rather than evicting someone's lock. */
#define HID_PT_APP_MODES_MAX 32

typedef struct {
    char key[HID_PT_STABLE_ID_LEN];   /* the app name through hid_pt_stable_id() */
    gamepad_mode_t mode;              /* never NONE: removing a lock drops the entry */
} hid_pt_app_mode_entry_t;

static hid_pt_app_mode_entry_t g_app_modes[HID_PT_APP_MODES_MAX];
static int g_app_mode_count;
/* The app the running session streams: its key ("" = none) and its name. */
static char g_current_app_key[HID_PT_STABLE_ID_LEN];
static char g_current_app_name[128];

/* Any mode's lightbar stored -- with @p colour_only, a colour rather than an
 * explicit Automatic. */
static bool pref_has_lightbar(const hid_pt_pref_entry_t *e, bool colour_only)
{
    for (int m = 0; m < GAMEPAD_MODE_COUNT; ++m) {
        if (e->lightbar[m].set && !(colour_only && e->lightbar[m].automatic)) {
            return true;
        }
    }
    return false;
}

/* An entry every reader would answer exactly as it answers a missing one: no
 * auto-plug, no SDL type chosen, no lightbar. Such an entry carries no
 * information, which is what makes it the one a full table may reuse and the
 * one the writer may skip. */
static bool pref_is_default(const hid_pt_pref_entry_t *e)
{
    return !e->auto_plugin && !e->sdl_type_set && !pref_has_lightbar(e, false);
}

/* Holds nothing but explicit "Automatic"s (SDL type, lightbar or both). Worth
 * keeping (see sdl_type_set), but the least of what a full table holds: it is
 * taken only when no default entry is left, and never to make room for another
 * one of its kind. */
static bool pref_is_explicit_auto_only(const hid_pt_pref_entry_t *e)
{
    const bool sdl_auto = !e->sdl_type_set || e->sdl_type == GAMEPAD_TYPE_PREF_AUTO;
    return !e->auto_plugin && sdl_auto && !pref_has_lightbar(e, true) && !pref_is_default(e);
}

static const char *sdl_type_ini_value(gamepad_type_pref_t type)
{
    return gamepad_type_pref_word(type);
}

void hid_pt_stable_id(const char *raw, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw) {
        return;
    }
    size_t o = 0;
    for (const char *p = raw; *p && o + 1 < out_len; ++p) {
        unsigned char c = (unsigned char) *p;
        if (c == ':' || c == '-' || c == ' ') {
            continue; /* MAC separators: aa:bb:cc, aa-bb-cc and "aa bb cc" are
                       * the same address and must produce the same id. */
        }
        c = (unsigned char) tolower(c);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '.')) {
            /* Serials and product strings are device-controlled and land in an
             * ini key position; '=', '[', ';' or a newline in one would rewrite
             * the config file's structure on the next flush. */
            c = '_';
        }
        out[o++] = (char) c;
    }
    out[o] = '\0';
}

/* A normalised id that carries no identity. Firmware that leaves the serial or
 * the sysfs `uniq` field unset reports "", "0" or "00:00:00:00:00:00", and a
 * string of unprintables normalises to nothing but '_'. Every such device would
 * otherwise share ONE pref entry, so one pad's auto-plug setting would apply to
 * all of them. */
static bool stable_id_is_blank(const char *id)
{
    if (!id || !id[0]) {
        return true;
    }
    for (const char *p = id; *p; ++p) {
        if (*p != '0' && *p != '_' && *p != '.') {
            return false;
        }
    }
    return true;
}

bool hid_pt_stable_id_is_synthetic(const char *stable_id)
{
    return stable_id && strncmp(stable_id, HID_PT_SYNTHETIC_PREFIX,
                                strlen(HID_PT_SYNTHETIC_PREFIX)) == 0;
}

static hid_pt_pref_entry_t *pref_find(const char *stable_id)
{
    if (!stable_id || !stable_id[0]) {
        return NULL;
    }
    for (int i = 0; i < g_hid_pt_pref_count; ++i) {
        if (strcmp(g_hid_pt_prefs[i].id, stable_id) == 0) {
            return &g_hid_pt_prefs[i];
        }
    }
    return NULL;
}

static hid_pt_pref_entry_t *pref_upsert(const char *stable_id, bool may_evict_explicit_auto)
{
    if (!stable_id || !stable_id[0]) {
        return NULL;
    }
    hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (e) {
        return e;
    }
    if (g_hid_pt_pref_count < HID_PT_PREFS_MAX) {
        e = &g_hid_pt_prefs[g_hid_pt_pref_count++];
    } else {
        /* Full. A default entry and a missing entry are the same answer to
         * every reader (pref_find -> NULL -> false / AUTO), so the all-default
         * slot carries no information and is the one to reuse. This is what
         * keeps the table from wedging: it used to be append-only for the app's
         * whole lifetime, and once 32 controllers had ever been toggled,
         * enabling auto-plug on the 33rd silently did nothing. An entry that
         * only holds an SDL type is NOT default and must not be taken. */
        for (int i = 0; i < g_hid_pt_pref_count; ++i) {
            if (pref_is_default(&g_hid_pt_prefs[i])) {
                e = &g_hid_pt_prefs[i];
                break;
            }
        }
        for (int i = 0; i < g_hid_pt_pref_count && !e && may_evict_explicit_auto; ++i) {
            if (pref_is_explicit_auto_only(&g_hid_pt_prefs[i])) {
                commons_log_info("HID-PT", "pref table full: dropping the explicit Automatic choices of %s",
                                 g_hid_pt_prefs[i].id);
                e = &g_hid_pt_prefs[i];
            }
        }
        if (!e) {
            return NULL;
        }
    }
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", stable_id);
    return e;
}

void hid_pt_prefs_init(void)
{
    g_hid_pt_pref_count = 0;
    g_app_mode_count = 0;
}

void hid_pt_stable_id_for_logical(const logical_device_t *item, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!item) {
        return;
    }
    if (item->mac[0]) {
        hid_pt_stable_id(item->mac, out, out_len);
        if (!stable_id_is_blank(out)) {
            return;
        }
        out[0] = '\0';
    }
    hid_pt_stable_id(item->key, out, out_len);
}

void hid_pt_stable_id_for_gamepad(const app_gamepad_state_t *gamepad, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!gamepad || !gamepad->controller) {
        return;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(gamepad->controller);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char *serial = SDL_JoystickGetSerial(joy);
    if (serial && serial[0]) {
        hid_pt_stable_id(serial, out, out_len);
        if (!stable_id_is_blank(out)) {
            return;
        }
        out[0] = '\0';
    }
#endif
    char guidstr[33];
    SDL_JoystickGetGUIDString(gamepad->guid, guidstr, sizeof(guidstr));
    uint16_t vid = (uint16_t) SDL_JoystickGetVendor(joy);
    uint16_t pid = (uint16_t) SDL_JoystickGetProduct(joy);
    snprintf(out, out_len, HID_PT_SYNTHETIC_PREFIX "%04x%04x_%s", vid, pid, guidstr);
}

bool hid_pt_prefs_get_auto_plugin(const char *stable_id)
{
    const hid_pt_pref_entry_t *e = pref_find(stable_id);
    return e ? e->auto_plugin : false;
}

bool hid_pt_prefs_set_auto_plugin(const char *stable_id, bool enabled)
{
    if (!stable_id || !stable_id[0]) {
        commons_log_warn("HID-PT", "auto-plug pref dropped: device has no stable id");
        return false;
    }
    hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (!e && !enabled) {
        /* Nothing stored and nothing worth storing: a missing entry already
         * reads as "no auto-plug". Do not consume a slot to record a default. */
        return true;
    }
    if (!e) {
        e = pref_upsert(stable_id, true);
    }
    if (!e) {
        commons_log_warn("HID-PT",
                         "auto-plug pref for %s NOT stored: all %d slots hold non-default prefs",
                         stable_id, HID_PT_PREFS_MAX);
        return false;
    }
    e->auto_plugin = enabled;
    hid_pt_prefs_flush();
    return true;
}

bool hid_pt_prefs_auto_plugin_for_logical(const logical_device_t *item)
{
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_logical(item, id, sizeof(id));
    return hid_pt_prefs_get_auto_plugin(id);
}

bool hid_pt_prefs_auto_plugin_for_gamepad(const app_gamepad_state_t *gamepad)
{
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_gamepad(gamepad, id, sizeof(id));
    return hid_pt_prefs_get_auto_plugin(id);
}

gamepad_type_pref_t hid_pt_prefs_get_sdl_type(const char *stable_id)
{
    gamepad_type_pref_t type = GAMEPAD_TYPE_PREF_AUTO;
    hid_pt_prefs_lookup_sdl_type(stable_id, &type);
    return type;
}

bool hid_pt_prefs_lookup_sdl_type(const char *stable_id, gamepad_type_pref_t *out)
{
    const hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (!e || !e->sdl_type_set) {
        return false;
    }
    if (out) {
        *out = e->sdl_type;
    }
    return true;
}

bool hid_pt_prefs_set_sdl_type(const char *stable_id, gamepad_type_pref_t type, bool keep_auto)
{
    if (!stable_id || !stable_id[0]) {
        commons_log_warn("HID-PT", "SDL type pref dropped: device has no stable id");
        return false;
    }
    if ((unsigned) type >= GAMEPAD_TYPE_PREF_COUNT) {
        return false;
    }
    hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (type == GAMEPAD_TYPE_PREF_AUTO && !keep_auto) {
        /* No choice at all: the same answer as a missing entry, so it never
         * needs a slot and only has to clear one that holds a type. */
        if (e && e->sdl_type_set) {
            e->sdl_type_set = false;
            e->sdl_type = GAMEPAD_TYPE_PREF_AUTO;
            hid_pt_prefs_flush();
        }
        return true;
    }
    if (!e) {
        /* Another explicit Automatic may give up its slot for a real type, but
         * not for one more of its own kind. */
        e = pref_upsert(stable_id, type != GAMEPAD_TYPE_PREF_AUTO);
    }
    if (!e) {
        commons_log_warn("HID-PT",
                         "SDL type pref for %s NOT stored: all %d slots hold non-default prefs",
                         stable_id, HID_PT_PREFS_MAX);
        return false;
    }
    if (e->sdl_type_set && e->sdl_type == type) {
        return true;
    }
    e->sdl_type_set = true;
    e->sdl_type = type;
    hid_pt_prefs_flush();
    return true;
}

gamepad_type_pref_t hid_pt_prefs_sdl_type_for_logical(const logical_device_t *item)
{
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_logical(item, id, sizeof(id));
    return hid_pt_prefs_get_sdl_type(id);
}

gamepad_type_pref_t hid_pt_prefs_sdl_type_for_gamepad(const app_gamepad_state_t *gamepad)
{
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_gamepad(gamepad, id, sizeof(id));
    return hid_pt_prefs_get_sdl_type(id);
}

bool hid_pt_prefs_lookup_lightbar(const char *stable_id, gamepad_mode_t mode, lightbar_pref_t *out)
{
    const hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (!e || (unsigned) mode >= GAMEPAD_MODE_COUNT) {
        return false;
    }
    /* The mode's own colour, else the one 1.7.30 kept for every mode. */
    const int m = e->lightbar[mode].set ? (int) mode : GAMEPAD_MODE_NONE;
    if (!e->lightbar[m].set) {
        return false;
    }
    if (out) {
        out->automatic = e->lightbar[m].automatic;
        out->rgb = e->lightbar[m].rgb & 0xFFFFFFu;
        out->game = !e->lightbar_game_off;
    }
    return true;
}

bool hid_pt_prefs_lookup_lightbar_game(const char *stable_id, bool *game)
{
    const hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (!e) {
        return false;
    }
    if (game) {
        *game = !e->lightbar_game_off;
    }
    return true;
}

bool hid_pt_prefs_set_lightbar(const char *stable_id, gamepad_mode_t mode, const lightbar_pref_t *lb, bool keep_auto)
{
    if (!stable_id || !stable_id[0] || !lb) {
        commons_log_warn("HID-PT", "lightbar pref dropped: device has no stable id");
        return false;
    }
    if (mode == GAMEPAD_MODE_NONE || (unsigned) mode >= GAMEPAD_MODE_COUNT) {
        /* 1.7.30's one colour is read, never written. */
        return false;
    }
    hid_pt_pref_entry_t *e = pref_find(stable_id);
    /* Automatic with nothing else to outvote is no choice: what a missing key
     * says, so it only clears. Not over 1.7.30's colour, which the mode would
     * fall back to -- there it is stored, as it is where the caller keeps it. */
    const bool legacy_colour = e && e->lightbar[GAMEPAD_MODE_NONE].set &&
                               !e->lightbar[GAMEPAD_MODE_NONE].automatic;
    if (lb->automatic && !keep_auto && !legacy_colour) {
        if (e && e->lightbar[mode].set) {
            memset(&e->lightbar[mode], 0, sizeof(e->lightbar[mode]));
            hid_pt_prefs_flush();
        }
        return true;
    }
    if (!e) {
        e = pref_upsert(stable_id, !lb->automatic);
    }
    if (!e) {
        commons_log_warn("HID-PT", "lightbar pref for %s NOT stored: all %d slots hold non-default prefs",
                         stable_id, HID_PT_PREFS_MAX);
        return false;
    }
    const uint32_t rgb = lb->automatic ? 0 : (lb->rgb & 0xFFFFFFu);
    /* The game switch is the controller's, one for every mode: a colour sets
     * it, Automatic (which has no switch) leaves it as the other modes have
     * it. */
    const bool game_off = lb->automatic ? e->lightbar_game_off : !lb->game;
    if (e->lightbar[mode].set && e->lightbar[mode].automatic == lb->automatic && e->lightbar[mode].rgb == rgb &&
        e->lightbar_game_off == game_off) {
        return true;
    }
    e->lightbar[mode].set = true;
    e->lightbar[mode].automatic = lb->automatic;
    e->lightbar[mode].rgb = rgb;
    e->lightbar_game_off = game_off;
    hid_pt_prefs_flush();
    return true;
}

lightbar_pref_t hid_pt_prefs_lightbar_for_logical(const logical_device_t *item, gamepad_mode_t mode)
{
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id_for_logical(item, id, sizeof(id));
    lightbar_pref_t lb = lightbar_pref_automatic();
    if (!hid_pt_prefs_lookup_lightbar(id, mode, &lb)) {
        hid_pt_prefs_lookup_lightbar_game(id, &lb.game);
    }
    return lb;
}

/* ---- per-game mode ------------------------------------------------------ */

static const char *app_mode_ini_value(gamepad_mode_t mode)
{
    switch (mode) {
        case GAMEPAD_MODE_HID:
            return "hid";
        case GAMEPAD_MODE_X360:
            return "x360";
        case GAMEPAD_MODE_DS4:
            return "ds4";
        case GAMEPAD_MODE_DS5:
            return "ds5";
        case GAMEPAD_MODE_SWITCH:
            return "switch";
        case GAMEPAD_MODE_NONE:
        default:
            return NULL;
    }
}

/* `.lightbar.<mode>` for a mode, 1.7.30's `.lightbar` for NONE. */
static void lightbar_mode_suffix(gamepad_mode_t mode, char *out, size_t len)
{
    const char *word = app_mode_ini_value(mode);
    if (word) {
        snprintf(out, len, HID_PT_LIGHTBAR_SUFFIX ".%s", word);
    } else {
        snprintf(out, len, HID_PT_LIGHTBAR_SUFFIX);
    }
}

static hid_pt_app_mode_entry_t *app_mode_find(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (int i = 0; i < g_app_mode_count; ++i) {
        if (strcmp(g_app_modes[i].key, key) == 0) {
            return &g_app_modes[i];
        }
    }
    return NULL;
}

/* Store @p mode under @p key, or drop the entry for NONE. False only for a
 * full table. Does not flush. */
static bool app_mode_put(const char *key, gamepad_mode_t mode)
{
    hid_pt_app_mode_entry_t *e = app_mode_find(key);
    if (mode == GAMEPAD_MODE_NONE) {
        if (e) {
            /* Order carries nothing; the last entry fills the hole. */
            *e = g_app_modes[--g_app_mode_count];
        }
        return true;
    }
    if (!e) {
        if (g_app_mode_count >= HID_PT_APP_MODES_MAX) {
            return false;
        }
        e = &g_app_modes[g_app_mode_count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->mode = mode;
    return true;
}

void hid_pt_prefs_set_current_app(const char *app_name)
{
    g_current_app_key[0] = '\0';
    g_current_app_name[0] = '\0';
    if (!app_name) {
        return;
    }
    hid_pt_stable_id(app_name, g_current_app_key, sizeof(g_current_app_key));
    if (stable_id_is_blank(g_current_app_key)) {
        /* A name of nothing but punctuation would share one lock with every
         * other such name. */
        g_current_app_key[0] = '\0';
        return;
    }
    snprintf(g_current_app_name, sizeof(g_current_app_name), "%s", app_name);
    const hid_pt_app_mode_entry_t *e = app_mode_find(g_current_app_key);
    if (e) {
        commons_log_info("HID-PT", "%s fixes every controller to mode %s", app_name, app_mode_ini_value(e->mode));
    }
}

bool hid_pt_prefs_current_app_name(char *buf, size_t len)
{
    if (!g_current_app_key[0]) {
        return false;
    }
    if (buf && len > 0) {
        snprintf(buf, len, "%s", g_current_app_name);
    }
    return true;
}

gamepad_mode_t hid_pt_prefs_current_app_mode(void)
{
    const hid_pt_app_mode_entry_t *e = app_mode_find(g_current_app_key);
    return e ? e->mode : GAMEPAD_MODE_NONE;
}

bool hid_pt_prefs_set_current_app_mode(gamepad_mode_t mode)
{
    if (!g_current_app_key[0]) {
        return false;
    }
    if (hid_pt_prefs_current_app_mode() == mode) {
        return true;
    }
    if (!app_mode_put(g_current_app_key, mode)) {
        commons_log_warn("HID-PT", "mode for %s NOT stored: all %d game slots are taken", g_current_app_name,
                         HID_PT_APP_MODES_MAX);
        return false;
    }
    commons_log_info("HID-PT", "%s: every controller %s", g_current_app_name,
                     mode == GAMEPAD_MODE_NONE ? "back in its own mode" : app_mode_ini_value(mode));
    hid_pt_prefs_flush();
    return true;
}

bool hid_pt_prefs_effective_hid(bool own_hid)
{
    const gamepad_mode_t lock = hid_pt_prefs_current_app_mode();
    if (lock == GAMEPAD_MODE_HID) {
        return true;
    }
    return gamepad_mode_is_sdl(lock) ? false : own_hid;
}

gamepad_type_pref_t hid_pt_prefs_effective_sdl_type(gamepad_type_pref_t own)
{
    const gamepad_type_pref_t forced = gamepad_mode_sdl_type(hid_pt_prefs_current_app_mode());
    return forced != GAMEPAD_TYPE_PREF_AUTO ? forced : own;
}

/* `<app> = hid|x360|ds4|ds5|switch`. The key is normalised again, so a
 * hand-edited name with spaces or capitals still finds its app. */
static void app_mode_ini_entry(const char *name, const char *value)
{
    char key[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id(name, key, sizeof(key));
    if (stable_id_is_blank(key) || !value) {
        return;
    }
    gamepad_mode_t mode = GAMEPAD_MODE_NONE;
    for (int m = GAMEPAD_MODE_NONE + 1; m < GAMEPAD_MODE_COUNT; ++m) {
        const char *word = app_mode_ini_value((gamepad_mode_t) m);
        if (word && strcmp(value, word) == 0) {
            mode = (gamepad_mode_t) m;
        }
    }
    if (mode == GAMEPAD_MODE_NONE) {
        /* A word from a newer build reads as no lock rather than as a guess. */
        return;
    }
    if (!app_mode_put(key, mode)) {
        commons_log_warn("HID-PT", "mode for %s dropped on load: table full", key);
    }
}

/* `<stable_id>.sdl_type = xbox|playstation|dualsense|switch|auto`. Split
 * BEFORE normalising: '.' is inside the id alphabet, so the suffix would
 * otherwise just become part of an id no device has. */
static void sdl_type_ini_entry(const char *name, size_t id_len, const char *value)
{
    char raw[HID_PT_STABLE_ID_LEN];
    if (id_len >= sizeof(raw)) {
        id_len = sizeof(raw) - 1;
    }
    memcpy(raw, name, id_len);
    raw[id_len] = '\0';
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id(raw, id, sizeof(id));
    if (!id[0] || !value) {
        return;
    }
    int type = -1;
    for (int t = 0; t < GAMEPAD_TYPE_PREF_COUNT; ++t) {
        const char *word = sdl_type_ini_value((gamepad_type_pref_t) t);
        if (word && strcmp(value, word) == 0) {
            type = t;
        }
    }
    if (type < 0) {
        /* An unknown word from a newer build reads as no choice rather than as
         * a guess. */
        return;
    }
    hid_pt_pref_entry_t *e = pref_upsert(id, type != GAMEPAD_TYPE_PREF_AUTO);
    if (!e) {
        commons_log_warn("HID-PT", "SDL type pref for %s dropped on load: table full", id);
        return;
    }
    e->sdl_type_set = true;
    e->sdl_type = (gamepad_type_pref_t) type;
}

/* The id part of `<id><suffix>`, normalised, into @p id. Split BEFORE
 * normalising, as sdl_type_ini_entry() explains. False for an empty id. */
static bool suffixed_ini_id(const char *name, size_t id_len, char id[HID_PT_STABLE_ID_LEN])
{
    char raw[HID_PT_STABLE_ID_LEN];
    if (id_len >= sizeof(raw)) {
        id_len = sizeof(raw) - 1;
    }
    memcpy(raw, name, id_len);
    raw[id_len] = '\0';
    hid_pt_stable_id(raw, id, HID_PT_STABLE_ID_LEN);
    return id[0] != '\0';
}

/* `<stable_id>.lightbar.<mode> = auto|off|rrggbb`, or 1.7.30's
 * `<stable_id>.lightbar` for @p mode NONE. */
static void lightbar_ini_entry(const char *name, size_t id_len, gamepad_mode_t mode, const char *value)
{
    char id[HID_PT_STABLE_ID_LEN];
    if (!suffixed_ini_id(name, id_len, id) || !value) {
        return;
    }
    bool automatic = false;
    uint32_t rgb = 0;
    if (strcmp(value, "auto") == 0) {
        automatic = true;
    } else if (strcmp(value, "off") != 0) {
        char *end = NULL;
        const unsigned long v = strtoul(value, &end, 16);
        if (strlen(value) != 6 || !end || *end != '\0' || v > 0xFFFFFFul) {
            /* Not a colour: no choice rather than a guess. */
            return;
        }
        rgb = (uint32_t) v;
    }
    hid_pt_pref_entry_t *e = pref_upsert(id, !automatic);
    if (!e) {
        commons_log_warn("HID-PT", "lightbar pref for %s dropped on load: table full", id);
        return;
    }
    e->lightbar[mode].set = true;
    e->lightbar[mode].automatic = automatic;
    e->lightbar[mode].rgb = rgb;
}

/* `<stable_id>.lightbar_game = 0|1`. Only 0 carries anything; a line for an id
 * whose colour line was dropped leaves an entry the writer skips again. */
static void lightbar_game_ini_entry(const char *name, size_t id_len, const char *value)
{
    char id[HID_PT_STABLE_ID_LEN];
    if (!suffixed_ini_id(name, id_len, id) || !value || strcmp(value, "0") != 0) {
        return;
    }
    hid_pt_pref_entry_t *e = pref_upsert(id, false);
    if (e) {
        e->lightbar_game_off = true;
    }
}

/* Whether @p name ends in @p suffix with something in front; *id_len = that. */
static bool ini_name_has_suffix(const char *name, const char *suffix, size_t *id_len)
{
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    if (name_len <= suffix_len || strcmp(name + name_len - suffix_len, suffix) != 0) {
        return false;
    }
    *id_len = name_len - suffix_len;
    return true;
}

int hid_pt_prefs_ini_handler(const char *section, const char *name, const char *value)
{
    if (section && name && name[0] && strcmp(section, HID_PT_APP_MODES_SECTION) == 0) {
        app_mode_ini_entry(name, value);
        return 1;
    }
    if (!section || strcmp(section, "hid_pt_devices") != 0) {
        return 0;
    }
    if (!name || !name[0]) {
        return 0;
    }
    size_t id_len = 0;
    if (ini_name_has_suffix(name, HID_PT_SDL_TYPE_SUFFIX, &id_len)) {
        sdl_type_ini_entry(name, id_len, value);
        return 1;
    }
    for (int m = GAMEPAD_MODE_NONE + 1; m < GAMEPAD_MODE_COUNT; ++m) {
        char suffix[32];
        lightbar_mode_suffix((gamepad_mode_t) m, suffix, sizeof(suffix));
        if (ini_name_has_suffix(name, suffix, &id_len)) {
            lightbar_ini_entry(name, id_len, (gamepad_mode_t) m, value);
            return 1;
        }
    }
    if (ini_name_has_suffix(name, HID_PT_LIGHTBAR_SUFFIX, &id_len)) {
        lightbar_ini_entry(name, id_len, GAMEPAD_MODE_NONE, value);
        return 1;
    }
    if (ini_name_has_suffix(name, HID_PT_LIGHTBAR_GAME_SUFFIX, &id_len)) {
        lightbar_game_ini_entry(name, id_len, value);
        return 1;
    }
    /* Normalise on load, so keys written by an older build in one of the two
     * pre-unification forms (a raw SDL serial, or a verbatim `hid:hidraw3` /
     * `flydigi:1-1.2` enumeration key) resolve to the id the running code now
     * derives for the same device, and get rewritten in that form on the next
     * flush. Two legacy keys can normalise to one id; the later one wins, which
     * is the same resolution a duplicate key already had. */
    char id[HID_PT_STABLE_ID_LEN];
    hid_pt_stable_id(name, id, sizeof(id));
    if (!id[0]) {
        return 1;
    }
    if (!INI_IS_TRUE(value)) {
        /* Opted out reads the same as absent, so an older file's `= false`
         * entries do not get to consume the table before the devices that
         * actually opted in are parsed. */
        return 1;
    }
    hid_pt_pref_entry_t *e = pref_upsert(id, true);
    if (!e) {
        commons_log_warn("HID-PT", "auto-plug pref for %s dropped on load: table full", id);
        return 1;
    }
    e->auto_plugin = true;
    return 1;
}

void hid_pt_prefs_write_section(FILE *fp)
{
    if (!fp) {
        return;
    }
    /* Only non-default prefs go to disk. A `= false` line said exactly what its
     * absence says, and writing them back made the section grow by one entry per
     * controller that had ever been toggled, forever. */
    bool wrote_header = false;
    for (int i = 0; i < g_hid_pt_pref_count; ++i) {
        const hid_pt_pref_entry_t *e = &g_hid_pt_prefs[i];
        if (pref_is_default(e)) {
            continue;
        }
        if (!wrote_header) {
            ini_write_section(fp, "hid_pt_devices");
            wrote_header = true;
        }
        if (e->auto_plugin) {
            ini_write_bool(fp, e->id, true);
        }
        const char *sdl_type = e->sdl_type_set ? sdl_type_ini_value(e->sdl_type) : NULL;
        if (sdl_type) {
            char key[HID_PT_STABLE_ID_LEN + sizeof(HID_PT_SDL_TYPE_SUFFIX)];
            snprintf(key, sizeof(key), "%s" HID_PT_SDL_TYPE_SUFFIX, e->id);
            ini_write_string(fp, key, sdl_type);
        }
        for (int m = 0; m < GAMEPAD_MODE_COUNT; ++m) {
            if (!e->lightbar[m].set) {
                continue;
            }
            char suffix[32];
            char key[HID_PT_STABLE_ID_LEN + sizeof(suffix)];
            char value[8];
            if (e->lightbar[m].automatic) {
                snprintf(value, sizeof(value), "auto");
            } else if (e->lightbar[m].rgb == 0) {
                snprintf(value, sizeof(value), "off");
            } else {
                snprintf(value, sizeof(value), "%06x", (unsigned) (e->lightbar[m].rgb & 0xFFFFFFu));
            }
            lightbar_mode_suffix((gamepad_mode_t) m, suffix, sizeof(suffix));
            snprintf(key, sizeof(key), "%s%s", e->id, suffix);
            ini_write_string(fp, key, value);
        }
        if (pref_has_lightbar(e, true) && e->lightbar_game_off) {
            char key[HID_PT_STABLE_ID_LEN + sizeof(HID_PT_LIGHTBAR_GAME_SUFFIX)];
            snprintf(key, sizeof(key), "%s" HID_PT_LIGHTBAR_GAME_SUFFIX, e->id);
            ini_write_string(fp, key, "0");
        }
    }
    for (int i = 0; i < g_app_mode_count; ++i) {
        const char *mode = app_mode_ini_value(g_app_modes[i].mode);
        if (!mode) {
            continue;
        }
        if (i == 0) {
            ini_write_section(fp, HID_PT_APP_MODES_SECTION);
        }
        ini_write_string(fp, g_app_modes[i].key, mode);
    }
}

void hid_pt_prefs_flush(void)
{
    if (!app_configuration || !app_configuration->ini_path) {
        return;
    }
    FILE *fp = fopen(app_configuration->ini_path, "r");
    if (!fp) {
        return;
    }
    char line[512];
    char **lines = NULL;
    size_t line_count = 0;
    size_t line_cap = 0;
    int in_section = 0;
    int skip_section = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                const char *sec = line + 1;
                /* Both sections this store owns are rewritten below. */
                in_section = (strcmp(sec, "hid_pt_devices") == 0 || strcmp(sec, HID_PT_APP_MODES_SECTION) == 0);
                *end = ']'; /* restore: line is stored verbatim below */
                skip_section = in_section;
                if (in_section) {
                    continue;
                }
            }
        }
        if (skip_section) {
            if (line[0] == '[') {
                skip_section = 0;
            } else {
                continue;
            }
        }
        if (line_count >= line_cap) {
            line_cap = line_cap ? line_cap * 2 : 64;
            char **n = realloc(lines, line_cap * sizeof(char *));
            if (!n) {
                break;
            }
            lines = n;
        }
        lines[line_count] = strdup(line);
        if (lines[line_count]) {
            line_count++;
        }
    }
    fclose(fp);

    fp = fopen(app_configuration->ini_path, "w");
    if (!fp) {
        for (size_t i = 0; i < line_count; ++i) {
            free(lines[i]);
        }
        free(lines);
        return;
    }
    for (size_t i = 0; i < line_count; ++i) {
        fputs(lines[i], fp);
        free(lines[i]);
    }
    free(lines);

    hid_pt_prefs_write_section(fp);
    fclose(fp);
}

#endif /* TARGET_WEBOS */
