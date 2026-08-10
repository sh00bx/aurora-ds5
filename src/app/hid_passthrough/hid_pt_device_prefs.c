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

typedef struct {
    char id[HID_PT_STABLE_ID_LEN];
    bool auto_plugin;
} hid_pt_pref_entry_t;

static hid_pt_pref_entry_t g_hid_pt_prefs[HID_PT_PREFS_MAX];
static int g_hid_pt_pref_count;

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

static hid_pt_pref_entry_t *pref_upsert(const char *stable_id)
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
        /* Full. An entry with auto_plugin == false and a missing entry are the
         * same answer to every reader (pref_find -> NULL -> false), so the
         * opted-out slot carries no information and is the one to reuse. This is
         * what keeps the table from wedging: it used to be append-only for the
         * app's whole lifetime, and once 32 controllers had ever been toggled,
         * enabling auto-plug on the 33rd silently did nothing. */
        for (int i = 0; i < g_hid_pt_pref_count; ++i) {
            if (!g_hid_pt_prefs[i].auto_plugin) {
                e = &g_hid_pt_prefs[i];
                break;
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
        e = pref_upsert(stable_id);
    }
    if (!e) {
        commons_log_warn("HID-PT",
                         "auto-plug pref for %s NOT stored: all %d slots hold opted-in devices",
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

int hid_pt_prefs_ini_handler(const char *section, const char *name, const char *value)
{
    if (!section || strcmp(section, "hid_pt_devices") != 0) {
        return 0;
    }
    if (!name || !name[0]) {
        return 0;
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
    hid_pt_pref_entry_t *e = pref_upsert(id);
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
    /* Only opted-in devices go to disk. A `= false` line said exactly what its
     * absence says, and writing them back made the section grow by one entry per
     * controller that had ever been toggled, forever. */
    bool wrote_header = false;
    for (int i = 0; i < g_hid_pt_pref_count; ++i) {
        if (!g_hid_pt_prefs[i].auto_plugin) {
            continue;
        }
        if (!wrote_header) {
            ini_write_section(fp, "hid_pt_devices");
            wrote_header = true;
        }
        ini_write_bool(fp, g_hid_pt_prefs[i].id, true);
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
                in_section = (strcmp(sec, "hid_pt_devices") == 0);
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
