#include "controller_info.h"

#include "app.h"
#include "input/input_gamepad.h"
#include "stream/session.h"

#include <SDL.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(TARGET_WEBOS)

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include "ds5_hidfd.h"
#include "hid_passthrough/hid_passthrough_manager.h"

#endif

/* Sony DualSense family — the only pads whose battery byte lives at the fixed
 * offset decoded below. DS4/Xbox/puck use different layouts, so they go through
 * SDL's coarse power level instead. */
#define DS_VENDOR_SONY  0x054c
#define DS_PRODUCT_DS5  0x0ce6
#define DS_PRODUCT_EDGE 0x0df2

/* Battery moves slowly, so the hidraw reads are cached; a pad that connects
 * mid-stream shows up as soon as the cache turns over. */
#define DS_SCAN_INTERVAL_MS 5000

static bool is_dualsense(uint16_t vendor, uint16_t product) {
    return vendor == DS_VENDOR_SONY && (product == DS_PRODUCT_DS5 || product == DS_PRODUCT_EDGE);
}

/* MACs reach us in two spellings — sysfs writes "aa:bb:…", SDL writes "aa-bb-…" —
 * so both sides are reduced to bare lowercase hex before comparing. */
static void normalize_mac(const char *in, char *out, size_t outlen) {
    size_t pos = 0;
    for (const char *p = in; p && *p && pos + 1 < outlen; p++) {
        if (isxdigit((unsigned char) *p)) {
            out[pos++] = (char) tolower((unsigned char) *p);
        }
    }
    out[pos] = '\0';
}

static void power_set_exact(controller_info_t *info, int percent, bool charging) {
    info->power = CONTROLLER_POWER_EXACT;
    info->percent = percent;
    info->charging = charging;
    snprintf(info->power_text, sizeof(info->power_text), "%d%%", percent);
}

#if defined(TARGET_WEBOS)

#define DS_PATH_MAX 32
/* Distinct hidraw nodes one scan will touch. Higher than the node cap on
 * purpose: nodes that report no usable battery never become entries, but they
 * still have to be remembered so the scan does not probe them again. */
#define DS_MAX_PROBED 8

typedef struct {
    char path[DS_PATH_MAX];
    char uniq[20]; /* normalized MAC, empty when the node reports none */
    int percent;
    bool charging;
    bool claimed;
} ds_node_t;

static ds_node_t ds_nodes[CONTROLLER_INFO_MAX];
static int ds_node_count = 0;
static uint32_t ds_scan_ms = 0;
static bool ds_scanned = false;

/* The DualSense `status` byte sits at offset 52 of the common input-report struct.
 * The report is prefixed by the report id plus, over Bluetooth, a 1-byte seq tag:
 * +2 for BT report 0x31, +1 for USB report 0x01. Returns capacity 0..100 (and the
 * charging nibble), or -1 when the report id/length is not a full DualSense report.
 * The minimal 10-byte BT report (also id 0x01) carries no battery and is rejected
 * by the length check. */
static int ds_parse_battery(const uint8_t *buf, int len, int *charging) {
    int off;
    if (buf[0] == 0x31) {
        off = 2 + 52; /* Bluetooth full report */
    } else if (buf[0] == 0x01) {
        off = 1 + 52; /* USB full report */
    } else {
        return -1;
    }
    if (len <= off) {
        return -1;
    }
    uint8_t status = buf[off];
    int raw = status & 0x0f;        /* 0..10 capacity, or an error code (>10) */
    int chg = (status >> 4) & 0x0f; /* 0 discharging, 1 charging, 2 full */
    if (charging) {
        *charging = chg;
    }
    if (chg == 0x02) {
        return 100; /* fully charged */
    }
    if (raw > 10) {
        return -1; /* temperature/voltage error code, not a real level */
    }
    int pct = raw * 10 + 5;
    return pct > 100 ? 100 : pct;
}

/* The app jail's /dev is a static snapshot taken at jail-build time, so a pad that
 * hot-plugged onto a minor the snapshot never covered cannot be opened directly;
 * ds5_txd hands the fd over instead. A broker that never answers would stall the
 * LVGL thread for its whole timeout, so one failed handover disables the fallback
 * for the rest of the run — the direct-open path keeps working either way. */
static int ds_open_node(const char *node) {
    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        return fd;
    }
    static bool broker_dead = false;
    if (broker_dead) {
        return -1;
    }
    fd = ds5_hidfd_request(node);
    if (fd < 0) {
        broker_dead = true;
    }
    return fd;
}

/* Read one input report off a DualSense hidraw node and decode its battery. The
 * kernel fans each hidraw report out to every open fd, so this read steals nothing
 * from the usbip passthrough or SDL's HIDAPI driver holding the same node open. */
static int ds_read_battery_node(const char *node, int *charging) {
    int fd = ds_open_node(node);
    if (fd < 0) {
        return -1;
    }
    int result = -1;
    /* A DualSense in full-report mode streams ~250 reports/s, so a fresh one lands
     * within a few ms; the ~50ms cap only ever burns on a pad that reports nothing
     * usable, and that runs once per scan interval. */
    for (int tries = 0; tries < 4 && result < 0; tries++) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 12) <= 0) {
            continue;
        }
        uint8_t buf[96];
        int n = (int) read(fd, buf, sizeof(buf));
        if (n <= 0) {
            continue;
        }
        result = ds_parse_battery(buf, n, charging);
    }
    close(fd);
    return result;
}

/* Read a small sysfs attribute (id/vendor, uniq, …) into out, newline stripped. */
static bool read_sysfs_attr(const char *path, char *out, size_t outlen) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    bool ok = fgets(out, (int) outlen, f) != NULL;
    fclose(f);
    if (!ok) {
        return false;
    }
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

/* Resolve the hidraw node an input device belongs to: input<N>/device/hidraw/hidrawN. */
static bool input_hidraw_node(const char *input_name, char *out, size_t outlen) {
    char dir_path[128];
    snprintf(dir_path, sizeof(dir_path), "/sys/class/input/%s/device/hidraw", input_name);
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "hidraw", 6) == 0) {
            snprintf(out, outlen, "%s", entry->d_name);
            found = true;
        }
    }
    closedir(dir);
    return found;
}

/* Probe one hidraw node, at most once per scan — including when it says nothing.
 *
 * A DualSense exposes several /sys/class/input entries (pad, touchpad, motion
 * sensors) that all resolve to the same hidraw node, and a pad in the minimal
 * 10-byte report mode fails ds_parse_battery every time. The old dedupe ran
 * against ds_nodes[], which a probe only reaches after it succeeds, so those pads
 * were re-probed — and burned the full ~50 ms budget — once per input entry.
 *
 * Failures are remembered in `probed`, deliberately not in ds_nodes[]: ds_claim()
 * hands out the first unclaimed node when it has no path or MAC to match on, so a
 * failed probe parked in the node list would render as a confident "0%" on a pad
 * that is actually full. */
static void ds_probe_once(char probed[][DS_PATH_MAX], int *probed_count,
                          const char *dev_path, const char *mac) {
    for (int i = 0; i < *probed_count; i++) {
        if (strcmp(probed[i], dev_path) == 0) {
            return; /* one physical pad, several input devices */
        }
    }
    if (*probed_count >= DS_MAX_PROBED || ds_node_count >= CONTROLLER_INFO_MAX) {
        return;
    }
    snprintf(probed[(*probed_count)++], DS_PATH_MAX, "%s", dev_path);
    int chg = 0;
    int pct = ds_read_battery_node(dev_path, &chg);
    if (pct < 0) {
        return;
    }
    ds_node_t *node = &ds_nodes[ds_node_count++];
    snprintf(node->path, sizeof(node->path), "%s", dev_path);
    snprintf(node->uniq, sizeof(node->uniq), "%s", mac ? mac : "");
    node->percent = pct;
    node->charging = (chg == 0x01);
    node->claimed = false;
}

/* Rebuild the DualSense node cache off /sys/class/input.
 *
 * NOT off /sys/class/hidraw: the app runs jailed, and that jail's /sys/class holds
 * only `input` and `net` — the hidraw class is simply absent there, which is also
 * why the passthrough code resolves nodes the same way. Bridged pads may have no
 * input device left at all, so their node paths are passed in separately. */
static void ds_scan(const char *const *extra_paths, int extra_count) {
    ds_node_count = 0;
    char probed[DS_MAX_PROBED][DS_PATH_MAX];
    int probed_count = 0;

    DIR *dir = opendir("/sys/class/input");
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && probed_count < DS_MAX_PROBED &&
               ds_node_count < CONTROLLER_INFO_MAX) {
            if (strncmp(entry->d_name, "input", 5) != 0 || !isdigit((unsigned char) entry->d_name[5])) {
                continue;
            }
            char attr[128], value[32];
            snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", entry->d_name);
            if (!read_sysfs_attr(attr, value, sizeof(value))) {
                continue;
            }
            uint16_t vendor = (uint16_t) strtoul(value, NULL, 16);
            snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", entry->d_name);
            if (!read_sysfs_attr(attr, value, sizeof(value))) {
                continue;
            }
            uint16_t product = (uint16_t) strtoul(value, NULL, 16);
            if (!is_dualsense(vendor, product)) {
                continue;
            }

            char node_name[24];
            if (!input_hidraw_node(entry->d_name, node_name, sizeof(node_name))) {
                continue;
            }
            char mac[20] = {0};
            snprintf(attr, sizeof(attr), "/sys/class/input/%s/uniq", entry->d_name);
            if (read_sysfs_attr(attr, value, sizeof(value))) {
                normalize_mac(value, mac, sizeof(mac));
            }
            char dev_path[DS_PATH_MAX];
            snprintf(dev_path, sizeof(dev_path), "/dev/%s", node_name);
            ds_probe_once(probed, &probed_count, dev_path, mac);
        }
        closedir(dir);
    }

    for (int i = 0; i < extra_count; i++) {
        ds_probe_once(probed, &probed_count, extra_paths[i], NULL);
    }
}

/* Hand out the reading for a pad: by hidraw path for a bridged device, by MAC for
 * an SDL one, and failing both the first node nobody claimed yet — with a single
 * pad that last case is the common one, since SDL only exposes a serial for pads
 * its HIDAPI driver owns. */
static void ds_scan_refresh(const char *const *extra_paths, int extra_count) {
    uint32_t now = SDL_GetTicks();
    if (!ds_scanned || (now - ds_scan_ms) >= DS_SCAN_INTERVAL_MS) {
        ds_scan(extra_paths, extra_count);
        ds_scan_ms = now;
        ds_scanned = true;
    }
    for (int i = 0; i < ds_node_count; i++) {
        ds_nodes[i].claimed = false;
    }
}

static const ds_node_t *ds_claim(const char *path, const char *mac) {
    for (int i = 0; i < ds_node_count; i++) {
        if (ds_nodes[i].claimed) {
            continue;
        }
        if (path && path[0] && strcmp(ds_nodes[i].path, path) == 0) {
            ds_nodes[i].claimed = true;
            return &ds_nodes[i];
        }
        if (mac && mac[0] && ds_nodes[i].uniq[0] && strcmp(ds_nodes[i].uniq, mac) == 0) {
            ds_nodes[i].claimed = true;
            return &ds_nodes[i];
        }
    }
    if (path && path[0]) {
        return NULL; /* a bridged pad is addressed by node; never guess for it */
    }
    for (int i = 0; i < ds_node_count; i++) {
        if (!ds_nodes[i].claimed) {
            ds_nodes[i].claimed = true;
            return &ds_nodes[i];
        }
    }
    return NULL;
}

/* A passthrough device worth a row: plugged means the user asked for it to be
 * bridged, connected means the bridge session is actually live. */
typedef struct {
    char name[48];
    char path[32];
    char mac[20];
    uint16_t vendor, product;
    bool connected;
    bool dualsense;
} bridged_device_t;

/* Everything the passthrough manager is holding. `plugged` is only the user's
 * intent — a pad can sit plugged with no live session, in which case SDL is still
 * driving it — so the caller decides what to do with `connected`. */
static int collect_bridged_devices(app_t *app, bridged_device_t *out, int max) {
    if (!app->session) {
        return 0;
    }
    hid_passthrough_manager_t *mgr = session_get_hid_passthrough(app->session);
    if (!mgr || !hid_passthrough_manager_active(mgr)) {
        return 0;
    }
    int device_count = hid_passthrough_manager_device_count(mgr);
    int found = 0;
    for (int i = 0; i < device_count && found < max; i++) {
        hid_pt_device_info_t device;
        if (hid_passthrough_manager_get_device(mgr, i, &device) != 0 || !device.plugged) {
            continue;
        }
        bridged_device_t *item = &out[found++];
        memset(item, 0, sizeof(*item));
        snprintf(item->name, sizeof(item->name), "%s",
                 device.product[0] ? device.product : "Controller");
        snprintf(item->path, sizeof(item->path), "%s", device.path);
        item->vendor = device.vendor_id;
        item->product = device.product_id;
        item->connected = device.connected;
        item->dualsense = is_dualsense(device.vendor_id, device.product_id);
    }
    return found;
}

#endif /* TARGET_WEBOS */

/* SDL only buckets the level, so the row says so rather than inventing a
 * percentage. SDL_JOYSTICK_POWER_MAX is the enum's count, not a level. */
static bool sdl_power_apply(controller_info_t *info, SDL_JoystickPowerLevel level) {
    switch (level) {
        case SDL_JOYSTICK_POWER_EMPTY:
            info->percent = 5;
            break;
        case SDL_JOYSTICK_POWER_LOW:
            info->percent = 20;
            break;
        case SDL_JOYSTICK_POWER_MEDIUM:
            info->percent = 70;
            break;
        case SDL_JOYSTICK_POWER_FULL:
            info->percent = 100;
            break;
        case SDL_JOYSTICK_POWER_WIRED:
            info->power = CONTROLLER_POWER_WIRED;
            info->percent = 100;
            snprintf(info->power_text, sizeof(info->power_text), "Wired");
            return true;
        default:
            return false;
    }
    info->power = CONTROLLER_POWER_COARSE;
    if (info->percent >= 100) {
        snprintf(info->power_text, sizeof(info->power_text), "> 70%%");
    } else {
        snprintf(info->power_text, sizeof(info->power_text), "< %d%%", info->percent);
    }
    return true;
}

/* Trim the words every pad tacks onto its name: "DualSense Wireless Controller"
 * and "Sony DS5 Controller" both become what a person would call them. */
static void shorten_name(char *name) {
    static const char *tails[] = {" Controller", " Wireless", " Gamepad"};
    bool trimmed = true;
    while (trimmed) {
        trimmed = false;
        size_t len = strlen(name);
        for (size_t t = 0; t < sizeof(tails) / sizeof(tails[0]); t++) {
            size_t tail_len = strlen(tails[t]);
            if (len > tail_len && strcmp(name + len - tail_len, tails[t]) == 0) {
                name[len - tail_len] = '\0';
                trimmed = true;
                break;
            }
        }
    }
}

/* The pad's Bluetooth address, as SDL spells it, or "" when SDL has no serial for
 * it — only its HIDAPI drivers report one. */
static void sdl_pad_mac(SDL_Joystick *joystick, char *out, size_t outlen) {
    out[0] = '\0';
#if SDL_VERSION_ATLEAST(2, 0, 14)
    normalize_mac(SDL_JoystickGetSerial(joystick), out, outlen);
#else
    (void) joystick;
    (void) outlen;
#endif
}

int controller_info_collect(app_t *app, controller_info_t *out, int max) {
    if (!app || !out || max <= 0) {
        return 0;
    }
    int found = 0;
    /* MAC (or vendor/product as a fallback) of everything already listed, so one
     * physical pad cannot show up twice when both sources can see it. */
    char listed_mac[CONTROLLER_INFO_MAX][20];
    uint32_t listed_id[CONTROLLER_INFO_MAX];

#if defined(TARGET_WEBOS)
    bridged_device_t bridged[CONTROLLER_INFO_MAX];
    int bridged_count = collect_bridged_devices(app, bridged, CONTROLLER_INFO_MAX);

    const char *extra_paths[CONTROLLER_INFO_MAX];
    int extra_count = 0;
    for (int i = 0; i < bridged_count; i++) {
        if (bridged[i].dualsense && bridged[i].path[0]) {
            extra_paths[extra_count++] = bridged[i].path;
        }
    }
    ds_scan_refresh(extra_paths, extra_count);

    /* Only a live bridge earns a row here. A pad that is merely plugged is being
     * driven by SDL, and turns up in the pass below with the badge that matches. */
    for (int i = 0; i < bridged_count && found < max; i++) {
        if (!bridged[i].connected) {
            continue;
        }
        controller_info_t *info = &out[found];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", bridged[i].name);
        shorten_name(info->name);
        info->bridged = true;
        snprintf(info->power_text, sizeof(info->power_text), "-");

        listed_mac[found][0] = '\0';
        listed_id[found] = ((uint32_t) bridged[i].vendor << 16) | bridged[i].product;
        if (bridged[i].dualsense) {
            const ds_node_t *node = ds_claim(bridged[i].path, NULL);
            if (node) {
                power_set_exact(info, node->percent, node->charging);
                snprintf(listed_mac[found], sizeof(listed_mac[0]), "%s", node->uniq);
            }
        }
        found++;
    }
#endif

    /* Every pad SDL has open, i.e. everything the TV itself is driving. */
    app_input_t *input = &app->input;
    for (int i = 0; i < (int) app_input_get_max_gamepads(input) && found < max; i++) {
        app_gamepad_state_t *state = app_input_gamepad_state_by_index(input, i);
        if (state == NULL || state->controller == NULL) {
            continue;
        }
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(state->controller);
        if (joystick == NULL) {
            continue;
        }
        uint16_t vendor = 0, product = 0;
#if SDL_VERSION_ATLEAST(2, 0, 6)
        vendor = SDL_JoystickGetVendor(joystick);
        product = SDL_JoystickGetProduct(joystick);
#endif
        char mac[20];
        sdl_pad_mac(joystick, mac, sizeof(mac));

        bool duplicate = false;
        uint32_t id = ((uint32_t) vendor << 16) | product;
        for (int j = 0; j < found && !duplicate; j++) {
            if (mac[0] && listed_mac[j][0]) {
                duplicate = strcmp(mac, listed_mac[j]) == 0;
            } else {
                duplicate = id != 0 && id == listed_id[j];
            }
        }
        if (duplicate) {
            continue;
        }

        controller_info_t *info = &out[found];
        memset(info, 0, sizeof(*info));
        const char *name = SDL_GameControllerName(state->controller);
        snprintf(info->name, sizeof(info->name), "%s", name ? name : "Controller");
        shorten_name(info->name);
        info->bridged = false;
        snprintf(info->power_text, sizeof(info->power_text), "-");

        bool exact = false;
#if defined(TARGET_WEBOS)
        if (is_dualsense(vendor, product)) {
            const ds_node_t *node = ds_claim(NULL, mac);
            if (node) {
                power_set_exact(info, node->percent, node->charging);
                exact = true;
            }
        }
#endif
        if (!exact) {
            sdl_power_apply(info, SDL_JoystickCurrentPowerLevel(joystick));
        }
        snprintf(listed_mac[found], sizeof(listed_mac[0]), "%s", mac);
        listed_id[found] = id;
        found++;
    }
    return found;
}
