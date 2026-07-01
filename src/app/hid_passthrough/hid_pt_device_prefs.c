#if defined(TARGET_WEBOS)

#define _GNU_SOURCE

#include "hid_pt_device_prefs.h"

#include "ctm/ctm_state.h"
#include "input/app_input.h"

#include "app.h"
#include "app_settings.h"
#include "ini_writer.h"
#include "util/ini_ext.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HID_PT_PREFS_MAX 32
#define HID_PT_STABLE_ID_LEN 96

typedef struct {
    char id[HID_PT_STABLE_ID_LEN];
    bool auto_plugin;
} hid_pt_pref_entry_t;

static hid_pt_pref_entry_t g_hid_pt_prefs[HID_PT_PREFS_MAX];
static int g_hid_pt_pref_count;

static void normalize_mac_to_stable_id(const char *src, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!src || !src[0]) {
        return;
    }
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < out_len; ++p) {
        if (*p == ':' || *p == '-' || *p == ' ') {
            continue;
        }
        out[o++] = (char) tolower((unsigned char) *p);
    }
    out[o] = '\0';
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
    hid_pt_pref_entry_t *e = pref_find(stable_id);
    if (e) {
        return e;
    }
    if (g_hid_pt_pref_count >= HID_PT_PREFS_MAX || !stable_id || !stable_id[0]) {
        return NULL;
    }
    e = &g_hid_pt_prefs[g_hid_pt_pref_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", stable_id);
    return e;
}

void hid_pt_prefs_init(void)
{
    g_hid_pt_pref_count = 0;
}

void hid_pt_prefs_clear(void)
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
    if (valid_bt_address(item->mac)) {
        normalize_mac_to_stable_id(item->mac, out, out_len);
        return;
    }
    if (item->mac[0]) {
        normalize_mac_to_stable_id(item->mac, out, out_len);
        if (out[0]) {
            return;
        }
    }
    if (item->key[0]) {
        snprintf(out, out_len, "%s", item->key);
    }
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
        normalize_mac_to_stable_id(serial, out, out_len);
        if (strlen(out) == 12) {
            return;
        }
        snprintf(out, out_len, "%s", serial);
        return;
    }
#endif
    char guidstr[33];
    SDL_JoystickGetGUIDString(gamepad->guid, guidstr, sizeof(guidstr));
    uint16_t vid = (uint16_t) SDL_JoystickGetVendor(joy);
    uint16_t pid = (uint16_t) SDL_JoystickGetProduct(joy);
    snprintf(out, out_len, "sdl:%04x:%04x:%s", vid, pid, guidstr);
}

bool hid_pt_prefs_get_auto_plugin(const char *stable_id)
{
    const hid_pt_pref_entry_t *e = pref_find(stable_id);
    return e ? e->auto_plugin : false;
}

void hid_pt_prefs_set_auto_plugin(const char *stable_id, bool enabled)
{
    hid_pt_pref_entry_t *e = pref_upsert(stable_id);
    if (!e) {
        return;
    }
    e->auto_plugin = enabled;
    hid_pt_prefs_flush();
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
    hid_pt_pref_entry_t *e = pref_upsert(name);
    if (!e) {
        return 1;
    }
    e->auto_plugin = INI_IS_TRUE(value);
    return 1;
}

void hid_pt_prefs_write_section(FILE *fp)
{
    if (!fp || g_hid_pt_pref_count <= 0) {
        return;
    }
    ini_write_section(fp, "hid_pt_devices");
    for (int i = 0; i < g_hid_pt_pref_count; ++i) {
        ini_write_bool(fp, g_hid_pt_prefs[i].id, g_hid_pt_prefs[i].auto_plugin);
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
