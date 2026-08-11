/* Device enumeration, classification, and the logical-device model for the
 * app. Moved verbatim out of lvgl_ui.c; functions de-static'd and prototyped
 * in ui_common.h. Pure relocation, no behavior change. */

#define _GNU_SOURCE

#include "ctm_state.h"
#include "ctm_hid.h"
#include "ctm_bridge_protocol.h"
#include "ctm_pad_table.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Xbox 360 wired-style HID report descriptor (matches flydigi_apex4_usb.profile). */
const uint8_t flydigi_xbox360_wired_rdesc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x81, 0x02, 0x05, 0x01, 0x09,
    0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95,
    0x01, 0x81, 0x42, 0x75, 0x08, 0x95, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x5B, 0x81, 0x02, 0xC0,
    0xC0
};
const unsigned flydigi_xbox360_wired_rdesc_len = sizeof(flydigi_xbox360_wired_rdesc);

device_info_t *find_or_add_device(scan_result_t *result, const char *hidraw)
{
    for (int i = 0; i < result->count; ++i) {
        if (strcmp(result->devices[i].hidraw, hidraw) == 0) {
            return &result->devices[i];
        }
    }
    if (result->count >= MAX_DEVICES) {
        return NULL;
    }

    device_info_t *dev = &result->devices[result->count++];
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->hidraw, sizeof(dev->hidraw), "%s", hidraw);
    snprintf(dev->node, sizeof(dev->node), "/dev/%s", hidraw);
    return dev;
}

device_info_t *find_or_add_input_device(scan_result_t *result,
                                               const char *input_name,
                                               const char *usb_busid)
{
    for (int i = 0; i < result->count; ++i) {
        device_info_t *dev = &result->devices[i];
        if (dev->hidraw[0]) {
            continue;
        }
        if (usb_busid && usb_busid[0] && strcmp(dev->usb_busid, usb_busid) == 0) {
            return dev;
        }
        if (input_name && input_name[0] && strstr(dev->inputs, input_name)) {
            return dev;
        }
    }
    if (result->count >= MAX_DEVICES) {
        return NULL;
    }

    device_info_t *dev = &result->devices[result->count++];
    memset(dev, 0, sizeof(*dev));
    if (usb_busid && usb_busid[0]) {
        snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", usb_busid);
    }
    return dev;
}

void usb_busid_from_input_path(const char *input_path, char *out, size_t out_len)
{
    out[0] = '\0';
    char device_link[PATH_MAX];
    char real[PATH_MAX];
    snprintf(device_link, sizeof(device_link), "%s/device", input_path);
    if (!realpath(device_link, real)) {
        return;
    }

    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", real);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strchr(part, '-')) {
            continue;
        }
        if (strchr(part, ':')) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s", part);
            char *colon = strchr(tmp, ':');
            if (colon) *colon = '\0';
            snprintf(out, out_len, "%s", tmp);
            return;
        }
        if (part[0] >= '0' && part[0] <= '9') {
            snprintf(out, out_len, "%s", part);
        }
    }
}

void inspect_hidraw(device_info_t *dev)
{
    if (!dev || !dev->node[0]) {
        return;
    }

    /* Idempotent: enumerate_devices inspects every hidraw node once in the
     * /dev loop and again in the all-devices loop. Once the identifying fields
     * are populated the re-open/ioctl work is redundant, so skip it. Devices
     * not yet fully populated (e.g. jail /dev-gap nodes only seen via sysfs)
     * still fall through and get inspected normally. */
    if (dev->vid[0] && dev->pid[0] && dev->iface[0]) {
        return;
    }

    struct stat st;
    if (stat(dev->node, &st) != 0) {
        return;
    }

    int fd = open(dev->node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        dev->readable = true;
        dev->writable = true;
    } else {
        fd = open(dev->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            dev->readable = true;
        }
    }
    if (fd < 0) {
        return;
    }

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        if (!dev->bus[0]) snprintf(dev->bus, sizeof(dev->bus), "%04x", info.bustype);
        if (!dev->vid[0]) snprintf(dev->vid, sizeof(dev->vid), "%04x", (unsigned short)info.vendor);
        if (!dev->pid[0]) snprintf(dev->pid, sizeof(dev->pid), "%04x", (unsigned short)info.product);
    }

    char raw_name[TEXT_LEN] = {0};
    if (!dev->name[0] && ioctl(fd, HIDIOCGRAWNAME(sizeof(raw_name) - 1), raw_name) >= 0) {
        snprintf(dev->name, sizeof(dev->name), "%s", raw_name);
    }

    char raw_phys[TEXT_LEN] = {0};
    if (!dev->phys[0] && ioctl(fd, HIDIOCGRAWPHYS(sizeof(raw_phys) - 1), raw_phys) >= 0) {
        snprintf(dev->phys, sizeof(dev->phys), "%s", raw_phys);
    }

    char raw_uniq[64] = {0};
    if (!dev->mac[0] && ioctl(fd, HIDIOCGRAWUNIQ(sizeof(raw_uniq) - 1), raw_uniq) >= 0) {
        snprintf(dev->mac, sizeof(dev->mac), "%s", raw_uniq);
    }

    int desc_size = 0;
    if (dev->report_descriptor_bytes <= 0 && ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) == 0 && desc_size > 0) {
        dev->report_descriptor_bytes = desc_size;
    }

    /* Classify the interface from its report descriptor (top-level usage), so
     * composite devices (Steam Controller: keyboard/mouse/vendor) are legible
     * and we can pick the gamepad-bearing (vendor) interface. */
    if (!dev->iface[0]) {
        uint8_t desc[4096];
        uint32_t dlen = read_report_descriptor(fd, desc, sizeof(desc));
        if (dlen) {
            uint16_t up = 0, us = 0;
            uint32_t mb = 0;
            ctm_hid_top_usage(desc, dlen, &up, &us, &mb);
            dev->usage_page = up;
            dev->usage = us;
            snprintf(dev->iface, sizeof(dev->iface), "%s %uB",
                     ctm_hid_usage_label(up, us), (unsigned)mb);
        }
    }

    close(fd);
}

const char *bus_label(const char *bus)
{
    if (strcmp(bus, "0003") == 0 || strcmp(bus, "3") == 0) return "USB";
    if (strcmp(bus, "0005") == 0 || strcmp(bus, "5") == 0) return "BT";
    return bus[0] ? bus : "-";
}

/* ---- port-path cache lifetime -------------------------------------------- */

/* Who is in this USB device dir right now: idVendor:idProduct:serial, folded to
 * 64 bits. 0 means "sysfs would not say" — a missing idVendor/idProduct, or a
 * dir that no longer exists — and 0 is never equal to a captured identity, so
 * every such case is a cache miss. serial is included when the device has one
 * and simply absent when it does not, which is why two indistinguishable
 * devices of the same model hash the same: they are interchangeable for every
 * consumer of these caches. */
uint64_t hid_pt_usb_identity_hash(const char *usbdir)
{
    if (!usbdir || !usbdir[0]) {
        return 0;
    }
    char vendor[16] = {0};
    char product[16] = {0};
    char serial[64] = {0};
    char path[320];
    snprintf(path, sizeof(path), "%s/idVendor", usbdir);
    read_text_file(path, vendor, sizeof(vendor));
    snprintf(path, sizeof(path), "%s/idProduct", usbdir);
    read_text_file(path, product, sizeof(product));
    if (!vendor[0] || !product[0]) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s/serial", usbdir);
    read_text_file(path, serial, sizeof(serial));

    uint64_t hash = 1469598103934665603ULL;
    const char *parts[3] = {vendor, product, serial};
    for (int i = 0; i < 3; ++i) {
        for (const unsigned char *b = (const unsigned char *)parts[i]; *b; ++b) {
            hash ^= *b;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xffu;             /* separator, so "ab"+"c" != "a"+"bc" */
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;        /* 0 is reserved for "unknown" */
}

/* Still the same device in that port? Re-read at most once per model rebuild:
 * three small sysfs reads, against a re-capture that walks /sys/class/input. */
static bool port_cache_entry_still_current(hid_pt_port_cache_t *entry)
{
    if (entry->checked_generation == g_devices.generation && entry->identity != 0) {
        return true;
    }
    entry->checked_generation = g_devices.generation;
    uint64_t now = hid_pt_usb_identity_hash(entry->usbdir);
    return now != 0 && now == entry->identity;
}

/* A live, identity-checked entry for @p key, or NULL. An entry whose port now
 * holds a different device (or nothing) is dropped here rather than returned. */
static hid_pt_port_cache_t *port_cache_find(void *base, size_t stride, int max, const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    char *bytes = (char *)base;
    for (int i = 0; i < max; ++i) {
        hid_pt_port_cache_t *entry = (hid_pt_port_cache_t *)(bytes + (size_t)i * stride);
        if (!entry->valid || strcmp(entry->key, key) != 0) {
            continue;
        }
        if (port_cache_entry_still_current(entry)) {
            return entry;
        }
        memset(entry, 0, stride);
        return NULL;
    }
    return NULL;
}

/* An emptied entry for @p key, ready to be filled by the caller. Reuses the
 * entry already holding the key, else a free slot, else evicts — preferring a
 * slot whose device is gone, and only then the oldest claim. Refusing instead
 * (which is what the old fixed tables did once full) permanently broke every
 * later plug: "Composite enum missing" for the rest of the app run. */
static hid_pt_port_cache_t *port_cache_claim(void *base, size_t stride, int max,
                                             const char *key, const char *usbdir,
                                             uint64_t identity)
{
    static uint32_t seq;
    if (!key || !key[0] || max <= 0) {
        return NULL;
    }
    char *bytes = (char *)base;
    hid_pt_port_cache_t *slot = NULL;
    hid_pt_port_cache_t *oldest = NULL;
    for (int i = 0; i < max; ++i) {
        hid_pt_port_cache_t *entry = (hid_pt_port_cache_t *)(bytes + (size_t)i * stride);
        if (!entry->valid) {
            if (!slot) slot = entry;
            continue;
        }
        if (strcmp(entry->key, key) == 0) {
            slot = entry;
            break;
        }
        if (!oldest || entry->insert_seq < oldest->insert_seq) {
            oldest = entry;
        }
    }
    if (!slot) {
        for (int i = 0; i < max && !slot; ++i) {
            hid_pt_port_cache_t *entry = (hid_pt_port_cache_t *)(bytes + (size_t)i * stride);
            if (entry->valid && !port_cache_entry_still_current(entry)) {
                slot = entry;
            }
        }
    }
    if (!slot) {
        slot = oldest;
    }
    if (!slot) {
        return NULL;
    }
    memset(slot, 0, stride);
    slot->valid = 1;
    snprintf(slot->key, sizeof(slot->key), "%s", key);
    snprintf(slot->usbdir, sizeof(slot->usbdir), "%s", usbdir ? usbdir : "");
    slot->identity = identity;
    slot->checked_generation = g_devices.generation;
    slot->insert_seq = ++seq;
    return slot;
}

#define USB_IDENTITY_CACHE_MAX 16

typedef struct {
    hid_pt_port_cache_t hdr;
    char manufacturer[TEXT_LEN];
    char product[TEXT_LEN];
} usb_identity_cache_t;

_Static_assert(offsetof(usb_identity_cache_t, hdr) == 0,
               "port cache helpers cast the entry address to hid_pt_port_cache_t *");
_Static_assert(offsetof(composite_enum_t, hdr) == 0,
               "port cache helpers cast the entry address to hid_pt_port_cache_t *");

static usb_identity_cache_t g_usb_identity_cache[USB_IDENTITY_CACHE_MAX];

static bool flydigi_identity_text(const char *mfg, const char *prod)
{
    if (mfg && contains_ci(mfg, "flydigi")) return true;
    if (prod && (contains_ci(prod, "flydigi") || contains_ci(prod, "apex") ||
                 contains_ci(prod, "vader"))) {
        return true;
    }
    return false;
}

int read_usb_identity_attrs(const char *usb_busid, char *mfg, size_t mfg_len,
                            char *prod, size_t prod_len)
{
    if (mfg && mfg_len) mfg[0] = '\0';
    if (prod && prod_len) prod[0] = '\0';
    if (!usb_busid || !usb_busid[0]) return -1;

    /* The hit test used to be "manufacturer[0] is set", which meant every
     * device without a manufacturer string — most of them — re-walked
     * /sys/class/input on every call. The entry's own validity answers it now. */
    usb_identity_cache_t *cached = (usb_identity_cache_t *)port_cache_find(
        g_usb_identity_cache, sizeof(g_usb_identity_cache[0]), USB_IDENTITY_CACHE_MAX, usb_busid);
    if (cached) {
        if (mfg && mfg_len) snprintf(mfg, mfg_len, "%s", cached->manufacturer);
        if (prod && prod_len) snprintf(prod, prod_len, "%s", cached->product);
        return 0;
    }

    char usbdir[256];
    if (composite_usb_device_dir_by_busid(usb_busid, usbdir, sizeof(usbdir)) != 0) {
        return -1;
    }

    char manufacturer[TEXT_LEN] = {0};
    char product[TEXT_LEN] = {0};
    char path[320];
    snprintf(path, sizeof(path), "%s/manufacturer", usbdir);
    read_text_file(path, manufacturer, sizeof(manufacturer));
    snprintf(path, sizeof(path), "%s/product", usbdir);
    read_text_file(path, product, sizeof(product));

    /* Only cache what can be invalidated later: without an identity there is no
     * way to notice the next device in this port, and that entry is exactly the
     * one that would misclassify it as a Flydigi. Answer this call, cache
     * nothing. */
    uint64_t identity = hid_pt_usb_identity_hash(usbdir);
    if (identity) {
        usb_identity_cache_t *slot = (usb_identity_cache_t *)port_cache_claim(
            g_usb_identity_cache, sizeof(g_usb_identity_cache[0]), USB_IDENTITY_CACHE_MAX,
            usb_busid, usbdir, identity);
        if (slot) {
            snprintf(slot->manufacturer, sizeof(slot->manufacturer), "%s", manufacturer);
            snprintf(slot->product, sizeof(slot->product), "%s", product);
        }
    }
    if (mfg && mfg_len) snprintf(mfg, mfg_len, "%s", manufacturer);
    if (prod && prod_len) snprintf(prod, prod_len, "%s", product);
    return 0;
}

bool is_flydigi_usb_busid(const char *usb_busid)
{
    if (!usb_busid || !usb_busid[0]) return false;
    char mfg[TEXT_LEN] = {0};
    char prod[TEXT_LEN] = {0};
    if (read_usb_identity_attrs(usb_busid, mfg, sizeof(mfg), prod, sizeof(prod)) != 0) {
        return false;
    }
    return flydigi_identity_text(mfg, prod);
}

static void usb_busid_from_sysfs_realpath(const char *real, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!real || !real[0]) return;
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", real);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strchr(part, '-')) continue;
        if (strchr(part, ':')) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s", part);
            char *colon = strchr(tmp, ':');
            if (colon) *colon = '\0';
            snprintf(out, out_len, "%s", tmp);
            return;
        }
        if (part[0] >= '0' && part[0] <= '9') {
            snprintf(out, out_len, "%s", part);
        }
    }
}

static void usb_busid_from_sysfs_path(const char *path, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!path || !path[0]) return;
    char real[PATH_MAX];
    if (realpath(path, real)) {
        usb_busid_from_sysfs_realpath(real, out, out_len);
        if (out[0]) return;
    }
    char linkbuf[PATH_MAX];
    ssize_t n = readlink(path, linkbuf, sizeof(linkbuf) - 1);
    if (n > 0) {
        linkbuf[n] = '\0';
        if (linkbuf[0] != '/') {
            char base[PATH_MAX];
            snprintf(base, sizeof(base), "%s", path);
            char *slash = strrchr(base, '/');
            if (slash) *slash = '\0';
            snprintf(real, sizeof(real), "%s/%s", base, linkbuf);
        } else {
            snprintf(real, sizeof(real), "%s", linkbuf);
        }
        usb_busid_from_sysfs_realpath(real, out, out_len);
    }
}

static int hidraw_sysfs_device_path(const char *hidraw, char *out, size_t out_len)
{
    if (!hidraw || !hidraw[0] || !out || out_len == 0) {
        return -1;
    }
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/class/hidraw/%s/device", hidraw);
    if (realpath(link, out)) {
        return 0;
    }
    DIR *d = opendir("/sys/class/input");
    if (!d) {
        return -1;
    }
    struct dirent *ent;
    int rc = -1;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) {
            continue;
        }
        char hp[PATH_MAX];
        snprintf(hp, sizeof(hp), "/sys/class/input/%s/device/hidraw/%s",
                 ent->d_name, hidraw);
        if (realpath(hp, out)) {
            rc = 0;
            break;
        }
    }
    closedir(d);
    return rc;
}

void usb_busid_from_hidraw_name(const char *hidraw, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!hidraw || !hidraw[0]) {
        return;
    }
    char real[PATH_MAX];
    if (hidraw_sysfs_device_path(hidraw, real, sizeof(real)) == 0) {
        usb_busid_from_sysfs_realpath(real, out, out_len);
        if (out[0]) {
            return;
        }
    }
    char node[64];
    snprintf(node, sizeof(node), "/dev/%s", hidraw);
    int fd = open(node, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    char phys[TEXT_LEN] = {0};
    if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys) >= 0 && phys[0]) {
        DIR *d = opendir("/sys/class/input");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, "input", 5) != 0) {
                    continue;
                }
                char input_path[PATH_MAX];
                char input_phys[TEXT_LEN] = {0};
                snprintf(input_path, sizeof(input_path), "/sys/class/input/%s/phys", ent->d_name);
                read_text_file(input_path, input_phys, sizeof(input_phys));
                if (!input_phys[0] || strcmp(input_phys, phys) != 0) {
                    continue;
                }
                snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", ent->d_name);
                usb_busid_from_input_path(input_path, out, out_len);
                if (out[0]) {
                    break;
                }
            }
            closedir(d);
        }
    }
    close(fd);
}

static void logical_device_merge_fields(logical_device_t *item, const device_info_t *dev)
{
    if (!item || !dev) return;
    if (!item->usb_busid[0] && dev->usb_busid[0]) {
        snprintf(item->usb_busid, sizeof(item->usb_busid), "%s", dev->usb_busid);
    }
    if (!item->mac[0] && dev->mac[0]) {
        snprintf(item->mac, sizeof(item->mac), "%s", dev->mac);
    }
    if (!item->bus[0] && dev->bus[0]) {
        snprintf(item->bus, sizeof(item->bus), "%s", dev->bus);
    }
    if ((!item->vid[0] || !item->pid[0]) && dev->vid[0] && dev->pid[0]) {
        if (!item->vid[0]) snprintf(item->vid, sizeof(item->vid), "%s", dev->vid);
        if (!item->pid[0]) snprintf(item->pid, sizeof(item->pid), "%s", dev->pid);
    }
}

static void enrich_scan_usb_busids(scan_result_t *result)
{
    for (int i = 0; i < result->count; ++i) {
        device_info_t *dev = &result->devices[i];
        if (dev->usb_busid[0]) continue;
        if (dev->hidraw[0]) {
            usb_busid_from_hidraw_name(dev->hidraw, dev->usb_busid, sizeof(dev->usb_busid));
        } else if (dev->inputs[0]) {
            char link[PATH_MAX];
            snprintf(link, sizeof(link), "/sys/class/input/%s/device", dev->inputs);
            usb_busid_from_sysfs_path(link, dev->usb_busid, sizeof(dev->usb_busid));
        }
    }
    for (int i = 0; i < result->count; ++i) {
        device_info_t *anchor = &result->devices[i];
        if (!anchor->usb_busid[0] || !is_flydigi_usb_busid(anchor->usb_busid)) {
            continue;
        }
        for (int j = 0; j < result->count; ++j) {
            if (i == j) {
                continue;
            }
            device_info_t *dev = &result->devices[j];
            if (dev->usb_busid[0] || !dev->hidraw[0]) {
                continue;
            }
            char peer[64] = {0};
            usb_busid_from_hidraw_name(dev->hidraw, peer, sizeof(peer));
            if (peer[0] && strcmp(peer, anchor->usb_busid) == 0) {
                snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", anchor->usb_busid);
            }
        }
    }
}

static void tag_xpad_flydigi_candidates(scan_result_t *result)
{
    for (int i = 0; i < result->count; ++i) {
        device_info_t *dev = &result->devices[i];
        if (!dev->usb_busid[0] || dev->hidraw[0]) continue;
        if (!is_xpad_compatible_pid(dev->vid, dev->pid)) continue;
        if (!is_flydigi_usb_busid(dev->usb_busid)) continue;
        char mfg[TEXT_LEN] = {0};
        char prod[TEXT_LEN] = {0};
        read_usb_identity_attrs(dev->usb_busid, mfg, sizeof(mfg), prod, sizeof(prod));
        if (prod[0] && !contains_ci(dev->name, "flydigi") && !contains_ci(dev->name, "apex") &&
            !contains_ci(dev->name, "vader")) {
            snprintf(dev->name, sizeof(dev->name), "%s", prod);
        }
    }
}

bool is_steam_puck_device(const device_info_t *dev)
{
    return dev && ctm_pad_kind_str(dev->vid, dev->pid) == CTM_PAD_KIND_PUCK;
}

bool is_xpad_only_scan_device(const device_info_t *dev)
{
    return dev && !dev->hidraw[0] && dev->usb_busid[0] &&
           is_xpad_compatible_pid(dev->vid, dev->pid);
}

bool is_flydigi_composite_device(const device_info_t *dev)
{
    if (!dev) return false;
    if (ctm_pad_has_flag_str(dev->vid, dev->pid, CTM_PAD_COMPOSITE)) return true;
    if (dev->usb_busid[0] && is_flydigi_usb_busid(dev->usb_busid)) return true;
    if (dev->usb_busid[0] &&
        (contains_ci(dev->name, "flydigi") || contains_ci(dev->name, "vader") ||
         contains_ci(dev->name, "apex"))) {
        return true;
    }
    return false;
}

bool is_flydigi_logical_device(const logical_device_t *item)
{
    return item && starts_with(item->key, "flydigi:");
}

bool is_ds5_device(const device_info_t *dev)
{
    return dev && ctm_pad_kind_str(dev->vid, dev->pid) == CTM_PAD_KIND_DS5;
}

bool is_ds4_device(const device_info_t *dev)
{
    return dev && ctm_pad_kind_str(dev->vid, dev->pid) == CTM_PAD_KIND_DS4;
}

bool is_xbox_pid(const char *pid)
{
    return ctm_pad_pid_is_xbox(pid);
}

bool is_xbox_device(const device_info_t *dev)
{
    if (!dev) return false;
    if (ctm_pad_kind_str(dev->vid, dev->pid) == CTM_PAD_KIND_XBOX) return true;
    /* Not a VID:PID fact, so not a table row: Microsoft ships pads this table
     * does not list, and for those the product string is the only evidence. */
    return ctm_pad_hex16(dev->vid) == CTM_PAD_VENDOR_MICROSOFT && contains_ci(dev->name, "xbox");
}

bool is_xpad_compatible_pid(const char *vid, const char *pid)
{
    return ctm_pad_has_flag_str(vid, pid, CTM_PAD_XPAD_COMPATIBLE);
}

bool is_gulikit_named_device(const char *name)
{
    return contains_ci(name, "gulikit") || contains_ci(name, "guli");
}

bool is_xpad_input_only_candidate(const char *bus, const char *vid,
                                         const char *pid, const char *name,
                                         const char *usb_busid)
{
    return usb_busid && usb_busid[0] &&
           (strcmp(bus_label(bus), "USB") == 0) &&
           (is_xpad_compatible_pid(vid, pid) ||
            is_gulikit_named_device(name) ||
            contains_ci(name, "xinput") ||
            contains_ci(name, "x-box") ||
            contains_ci(name, "xbox"));
}

void steam_root_from_phys(const char *phys, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", phys && phys[0] ? phys : "unknown");
    char *input = strstr(out, "/input");
    if (input) {
        *input = '\0';
    }
}

void logical_key_for_device(const device_info_t *dev, char *out, size_t out_len)
{
    if (is_steam_puck_device(dev)) {
        snprintf(out, out_len, "steam:%s:%s", dev->vid, dev->pid);
    } else if (is_flydigi_composite_device(dev) && dev->usb_busid[0]) {
        snprintf(out, out_len, "flydigi:%s", dev->usb_busid);
    } else if (dev && !dev->hidraw[0] && dev->usb_busid[0]) {
        snprintf(out, out_len, "usb:%s", dev->usb_busid);
    } else if (dev && !dev->hidraw[0] && dev->inputs[0]) {
        snprintf(out, out_len, "input:%s", dev->inputs);
    } else {
        snprintf(out, out_len, "hid:%s", dev->hidraw);
    }
}

void logical_name_for_device(const device_info_t *dev, char *out, size_t out_len)
{
    /* The display name for everything the table knows by id. The branches below
     * that do NOT read it are the ones whose condition is not a VID:PID fact:
     * the Flydigi sysfs heuristics, Gulikit by name, and a Microsoft pad
     * recognised only by its product string. */
    const ctm_pad_desc_t *pad = dev ? ctm_pad_desc_find_str(dev->vid, dev->pid) : NULL;

    if (is_steam_puck_device(dev)) {
        snprintf(out, out_len, "%s", ctm_pad_display_name(pad, "Steam Controller Puck"));
    } else if (is_flydigi_composite_device(dev)) {
        char mfg[TEXT_LEN] = {0};
        char prod[TEXT_LEN] = {0};
        if (dev->usb_busid[0]) {
            read_usb_identity_attrs(dev->usb_busid, mfg, sizeof(mfg), prod, sizeof(prod));
        }
        if (is_xpad_compatible_pid(dev->vid, dev->pid) &&
            (contains_ci(prod, "flydigi") || contains_ci(mfg, "flydigi"))) {
            snprintf(out, out_len, "Flydigi Apex 4");
        } else if (contains_ci(prod, "apex") || contains_ci(dev->name, "apex")) {
            snprintf(out, out_len, "Flydigi Apex 4");
        } else if (contains_ci(prod, "vader") || contains_ci(dev->name, "vader")) {
            snprintf(out, out_len, "Flydigi Vader3");
        } else if (contains_ci(prod, "flydigi") || contains_ci(dev->name, "flydigi")) {
            snprintf(out, out_len, "%s", prod[0] ? prod : "Flydigi Controller");
        } else if (dev->name[0]) {
            snprintf(out, out_len, "%s", dev->name);
        } else {
            snprintf(out, out_len, "Flydigi Controller");
        }
    } else if (is_ds5_device(dev) || is_ds4_device(dev)) {
        snprintf(out, out_len, "%s", ctm_pad_display_name(pad, "Sony Controller"));
    } else if (is_gulikit_named_device(dev->name)) {
        snprintf(out, out_len, "Gulikit Gamepad");
    } else if (is_xbox_device(dev)) {
        /* Only a row that the table itself calls an Xbox controller may name
         * this. A 360-era pad (kind XINPUT) whose product string happens to say
         * "xbox" reaches this branch too, and has always been shown as the
         * generic Microsoft name rather than its own row's. */
        snprintf(out, out_len, "%s",
                 ctm_pad_display_name((pad && pad->kind == CTM_PAD_KIND_XBOX) ? pad : NULL,
                                      "Microsoft Xbox Controller"));
    } else if (dev && !dev->hidraw[0] && is_xpad_compatible_pid(dev->vid, dev->pid)) {
        snprintf(out, out_len, "%s", ctm_pad_display_name(pad, "XInput-compatible Gamepad"));
    } else {
        snprintf(out, out_len, "%s", dev->name[0] ? dev->name : "Unnamed HID device");
    }
}

bool device_should_list_in_ui(const device_info_t *dev)
{
    if (!dev) {
        return false;
    }
    char name[TEXT_LEN];
    logical_name_for_device(dev, name, sizeof(name));
    if (strcmp(name, "Unnamed HID device") == 0) {
        return false;
    }
    return true;
}

bool plug_key_is_set(const char *key)
{
    for (int i = 0; i < g_plugged_key_count; ++i) {
        if (strcmp(g_plugged_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

void set_plug_key(const char *key, bool plugged)
{
    for (int i = 0; i < g_plugged_key_count; ++i) {
        if (strcmp(g_plugged_keys[i], key) == 0) {
            if (!plugged) {
                memmove(&g_plugged_keys[i], &g_plugged_keys[i + 1],
                        (size_t)(g_plugged_key_count - i - 1) * sizeof(g_plugged_keys[0]));
                g_plugged_key_count--;
            }
            return;
        }
    }
    if (plugged && g_plugged_key_count < MAX_DEVICES) {
        snprintf(g_plugged_keys[g_plugged_key_count++], sizeof(g_plugged_keys[0]), "%s", key);
    }
}

bool expand_key_is_set(const char *key)
{
    for (int i = 0; i < g_expanded_key_count; ++i) {
        if (strcmp(g_expanded_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

void set_expand_key(const char *key, bool expanded)
{
    for (int i = 0; i < g_expanded_key_count; ++i) {
        if (strcmp(g_expanded_keys[i], key) == 0) {
            if (!expanded) {
                memmove(&g_expanded_keys[i], &g_expanded_keys[i + 1],
                        (size_t)(g_expanded_key_count - i - 1) * sizeof(g_expanded_keys[0]));
                g_expanded_key_count--;
            }
            return;
        }
    }
    if (expanded && g_expanded_key_count < MAX_DEVICES) {
        snprintf(g_expanded_keys[g_expanded_key_count++], sizeof(g_expanded_keys[0]), "%s", key);
    }
}

bool logical_device_can_expand(const logical_device_t *item)
{
    return item &&
           ((starts_with(item->key, "steam:") || starts_with(item->key, "flydigi:")) &&
            item->device_count > 1);
}

/* The one sanctioned way to get a logical device from outside the model: by the
 * key it is identified by, resolved against the CURRENT g_devices. Returns NULL
 * when the device is gone — callers must handle that rather than fall back to a
 * remembered index, which after a rebuild names a different device. */
logical_device_t *logical_device_by_key(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (int i = 0; i < g_devices.count; ++i) {
        if (strcmp(g_devices.items[i].key, key) == 0) {
            return &g_devices.items[i];
        }
    }
    return NULL;
}

logical_device_t *find_or_add_logical_device(logical_result_t *logical, const device_info_t *dev, int scan_index)
{
    char key[96];
    logical_key_for_device(dev, key, sizeof(key));

    for (int i = 0; i < logical->count; ++i) {
        if (strcmp(logical->items[i].key, key) == 0) {
            logical_device_t *item = &logical->items[i];
            logical_device_merge_fields(item, dev);
            if (item->device_count < MAX_DEVICES) {
                item->device_indices[item->device_count++] = scan_index;
            }
            return item;
        }
    }

    if (logical->count >= MAX_DEVICES) {
        return NULL;
    }

    logical_device_t *item = &logical->items[logical->count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->key, sizeof(item->key), "%s", key);
    logical_name_for_device(dev, item->name, sizeof(item->name));
    snprintf(item->bus, sizeof(item->bus), "%s", dev->bus);
    snprintf(item->vid, sizeof(item->vid), "%s", dev->vid);
    snprintf(item->pid, sizeof(item->pid), "%s", dev->pid);
    snprintf(item->mac, sizeof(item->mac), "%s", dev->mac);
    snprintf(item->usb_busid, sizeof(item->usb_busid), "%s", dev->usb_busid);
    item->plugged = plug_key_is_set(item->key);
    item->moonlight_gs_id = -1;
    item->device_indices[item->device_count++] = scan_index;
    return item;
}

void build_logical_devices(const scan_result_t *scan, logical_result_t *logical)
{
    uint32_t generation = logical->generation;
    memset(logical, 0, sizeof(*logical));
    logical->generation = generation + 1;
    for (int i = 0; i < scan->count; ++i) {
        if (!device_should_list_in_ui(&scan->devices[i])) {
            continue;
        }
        find_or_add_logical_device(logical, &scan->devices[i], i);
    }
    finalize_logical_devices(logical);
}

void finalize_logical_devices(logical_result_t *logical)
{
    if (!logical) return;
    for (int i = 0; i < logical->count; ++i) {
        logical_device_t *item = &logical->items[i];
        for (int j = 0; j < item->device_count; ++j) {
            int idx = item->device_indices[j];
            if (idx < 0 || idx >= g_scan.count) continue;
            logical_device_merge_fields(item, &g_scan.devices[idx]);
        }
        if (!item->usb_busid[0] && starts_with(item->key, "flydigi:")) {
            const char *from_key = item->key + strlen("flydigi:");
            if (from_key[0]) {
                snprintf(item->usb_busid, sizeof(item->usb_busid), "%s", from_key);
            }
        }
        if (starts_with(item->key, "flydigi:") && item->usb_busid[0]) {
            char usbdir[512];
            if (composite_usb_device_dir_by_busid(item->usb_busid, usbdir, sizeof(usbdir)) == 0) {
                char path[600];
                char v[16] = {0};
                char p[16] = {0};
                snprintf(path, sizeof(path), "%s/idVendor", usbdir);
                read_text_file(path, v, sizeof(v));
                snprintf(path, sizeof(path), "%s/idProduct", usbdir);
                read_text_file(path, p, sizeof(p));
                if (v[0]) snprintf(item->vid, sizeof(item->vid), "%s", v);
                if (p[0]) snprintf(item->pid, sizeof(item->pid), "%s", p);
            }
            if (item->device_count > 0) {
                int idx = item->device_indices[0];
                if (idx >= 0 && idx < g_scan.count) {
                    logical_name_for_device(&g_scan.devices[idx], item->name, sizeof(item->name));
                }
            }
            {
                char mfg[64] = {0};
                char prod[64] = {0};
                if (read_usb_identity_attrs(item->usb_busid, mfg, sizeof(mfg), prod, sizeof(prod)) == 0 &&
                    prod[0]) {
                    if (contains_ci(prod, "apex")) {
                        snprintf(item->name, sizeof(item->name), "Flydigi Apex 4");
                    } else if (contains_ci(prod, "vader")) {
                        snprintf(item->name, sizeof(item->name), "Flydigi Vader3");
                    }
                }
            }
        }
    }
    int kept = 0;
    for (int i = 0; i < logical->count; ++i) {
        if (strcmp(logical->items[i].name, "Unnamed HID device") == 0) {
            continue;
        }
        if (kept != i) {
            logical->items[kept] = logical->items[i];
        }
        kept++;
    }
    logical->count = kept;
}

/* --- Steam Puck USB-interface enumeration (shared by the list + detail) -----
 * Resolve the puck's USB device dir and enumerate its interfaces straight from
 * sysfs, so both the expanded list and the detail panel see the FULL set —
 * including the CDC interfaces and the input-less Service interface a
 * /sys/class/input scan can't reach. Paths are derived (host controller varies:
 * xhci/vhci); never hardcoded, never via /sys/bus (absent in the jail). */

/* The /dev node for a USB interface dir: a hidraw (HID) or ttyACMx (CDC). */
static void iface_node(const char *ifdir, char *out, size_t outlen)
{
    out[0] = '\0';
    DIR *d = opendir(ifdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && out[0] == '\0') {
        if (e->d_name[0] == '.') continue;
        if (strncmp(e->d_name, "ttyACM", 6) == 0) {
            snprintf(out, outlen, "/dev/%s", e->d_name);
            break;
        }
        if (strcmp(e->d_name, "tty") == 0) {
            char tdir[PATH_MAX];
            snprintf(tdir, sizeof(tdir), "%s/tty", ifdir);
            DIR *t = opendir(tdir);
            if (t) {
                struct dirent *te;
                while ((te = readdir(t)) != NULL) {
                    if (te->d_name[0] == '.') continue;
                    snprintf(out, outlen, "/dev/%s", te->d_name);
                    break;
                }
                closedir(t);
            }
            continue;
        }
        char hdir[PATH_MAX];
        snprintf(hdir, sizeof(hdir), "%s/%s/hidraw", ifdir, e->d_name);
        DIR *h = opendir(hdir);
        if (h) {
            struct dirent *he;
            while ((he = readdir(h)) != NULL) {
                if (strncmp(he->d_name, "hidraw", 6) == 0) {
                    snprintf(out, outlen, "/dev/%s", he->d_name);
                    break;
                }
            }
            closedir(h);
        }
    }
    closedir(d);
}

int puck_usb_device_dir(const char *vid, const char *pid, char *out, size_t out_len)
{
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (strncmp(e->d_name, "input", 5) != 0) continue;
        char attr[PATH_MAX], v[16] = {0}, p[16] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", e->d_name);
        read_text_file(attr, v, sizeof(v));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", e->d_name);
        read_text_file(attr, p, sizeof(p));
        if (strcmp(v, vid) != 0 || strcmp(p, pid) != 0) continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", e->d_name);
        if (!realpath(link, real)) continue;
        while (real[0]) {
            char idf[PATH_MAX];
            struct stat st;
            snprintf(idf, sizeof(idf), "%s/idVendor", real);
            if (stat(idf, &st) == 0) {
                snprintf(out, out_len, "%s", real);
                rc = 0;
                break;
            }
            char *s = strrchr(real, '/');
            if (!s || s == real) break;
            *s = '\0';
        }
    }
    closedir(d);
    return rc;
}

int composite_usb_device_dir_by_busid(const char *usb_busid, char *out, size_t out_len)
{
    if (!usb_busid || !usb_busid[0]) return -1;
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (strncmp(e->d_name, "input", 5) != 0) continue;
        char input_path[PATH_MAX], busid[64] = {0};
        snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", e->d_name);
        usb_busid_from_input_path(input_path, busid, sizeof(busid));
        if (strcmp(busid, usb_busid) != 0) continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "%s/device", input_path);
        if (!realpath(link, real)) continue;
        while (real[0]) {
            char idf[PATH_MAX];
            struct stat st;
            snprintf(idf, sizeof(idf), "%s/idVendor", real);
            if (stat(idf, &st) == 0) {
                snprintf(out, out_len, "%s", real);
                rc = 0;
                break;
            }
            char *s = strrchr(real, '/');
            if (!s || s == real) break;
            *s = '\0';
        }
    }
    closedir(d);
    return rc;
}

int composite_enumerate_ifaces(const char *usbdir, composite_if_t *out, int max)
{
    const char *base = strrchr(usbdir, '/');
    base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    int n = 0;
    DIR *d = opendir(usbdir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[PATH_MAX], attr[PATH_MAX], num[16] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(attr, sizeof(attr), "%s/bInterfaceClass", ifdir);
        if (read_text_file(attr, out[n].cls, sizeof(out[n].cls)) != 0) continue;
        snprintf(attr, sizeof(attr), "%s/bInterfaceNumber", ifdir);
        read_text_file(attr, num, sizeof(num));
        out[n].num = (int)strtol(num, NULL, 16);
        iface_node(ifdir, out[n].node, sizeof(out[n].node));
        snprintf(out[n].dir, sizeof(out[n].dir), "%s", e->d_name);
        n++;
    }
    closedir(d);
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            if (out[b].num < out[a].num) {
                composite_if_t t = out[a];
                out[a] = out[b];
                out[b] = t;
            }
        }
    }
    return n;
}

/* Raw (binary) sysfs read — for `descriptors` and `report_descriptor` blobs
 * (read_text_file would trim/stop at NULs). Returns bytes read. */
static int read_binary_file(const char *path, uint8_t *out, int max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int total = 0, n;
    while (total < max && (n = (int)read(fd, out + total, (size_t)(max - total))) > 0) {
        total += n;
    }
    close(fd);
    return total;
}

/* The HID report descriptor for one interface dir (<usbdir>/<dir>/<hid>/report_descriptor). */
static int read_iface_rdesc(const char *usbdir, const char *dir, uint8_t *out, int max)
{
    char ifdir[300];
    snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, dir);
    DIR *d = opendir(ifdir);
    if (!d) return 0;
    struct dirent *e;
    int len = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char rd[400];
        snprintf(rd, sizeof(rd), "%s/%s/report_descriptor", ifdir, e->d_name);
        len = read_binary_file(rd, out, max);
        if (len > 0) break;
        len = 0;
    }
    closedir(d);
    return len;
}

static uint8_t read_usb_full_speed_flag(const char *usbdir)
{
    char speed[16] = {0};
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/speed", usbdir);
    read_text_file(path, speed, sizeof(speed));
    return (strcmp(speed, "12") == 0 || strcmp(speed, "1.5") == 0) ? 1 : 0;
}

const composite_enum_t *composite_enum_lookup(const char *key)
{
    /* Identity-checked, like every other read of this table: a plug that
     * resolves an enumeration captured from a device that has since left the
     * port would hand the host the wrong descriptors. */
    return (const composite_enum_t *)port_cache_find(
        g_composite_enums, sizeof(g_composite_enums[0]), COMPOSITE_ENUM_MAX_CACHE, key);
}

static const char *composite_enum_key_for_device(const char *usb_busid, const char *usbdir)
{
    static char key[64];
    if (usb_busid && usb_busid[0]) {
        snprintf(key, sizeof(key), "%s", usb_busid);
        return key;
    }
    const char *base = strrchr(usbdir, '/');
    snprintf(key, sizeof(key), "%s", base ? base + 1 : usbdir);
    return key;
}

int composite_enum_capture(const char *usb_busid, const char *vid, const char *pid,
                           char *out_key, size_t out_key_len)
{
    if (out_key && out_key_len) out_key[0] = '\0';
    char usbdir[256];
    if (usb_busid && usb_busid[0] &&
        composite_usb_device_dir_by_busid(usb_busid, usbdir, sizeof(usbdir)) == 0) {
        /* resolved by bus id (Flydigi Xbox-emulation mode) */
    } else if (vid && pid && puck_usb_device_dir(vid, pid, usbdir, sizeof(usbdir)) == 0) {
        /* resolved by VID/PID (Steam Puck, native Flydigi) */
    } else {
        return -1;
    }

    const char *key = composite_enum_key_for_device(usb_busid, usbdir);
    /* The freshness test used to be usbdir == usbdir, i.e. port == port, which
     * is true for every device that ever occupies that port: a Flydigi flipped
     * from D-Input to XInput re-enumerates on the same port with a different
     * VID/PID and a different interface layout, and the cached D-Input
     * descriptors were forwarded to the Windows agent verbatim. Compare the
     * device, not the socket it is in. */
    uint64_t identity = hid_pt_usb_identity_hash(usbdir);
    const composite_enum_t *live = composite_enum_lookup(key);
    if (live && live->hdr.identity == identity && strcmp(live->hdr.usbdir, usbdir) == 0) {
        if (out_key && out_key_len) snprintf(out_key, out_key_len, "%s", key);
        return 0;
    }
    if (!identity) {
        /* No idVendor/idProduct under usbdir: an entry captured now could never
         * be told apart from the next device in this port, and a cache that
         * cannot be invalidated is what this whole change is removing. */
        return -1;
    }

    composite_enum_t *cache = (composite_enum_t *)port_cache_claim(
        g_composite_enums, sizeof(g_composite_enums[0]), COMPOSITE_ENUM_MAX_CACHE,
        key, usbdir, identity);
    if (!cache) return -1;
    cache->full_speed = read_usb_full_speed_flag(usbdir);

    char attr[300];
    snprintf(attr, sizeof(attr), "%s/descriptors", usbdir);
    cache->descriptors_len = read_binary_file(attr, cache->descriptors, COMPOSITE_ENUM_MAX_DESC);
    snprintf(attr, sizeof(attr), "%s/serial", usbdir);
    read_text_file(attr, cache->serial, sizeof(cache->serial));

    composite_if_t ifs[16];
    int nif = composite_enumerate_ifaces(usbdir, ifs, 16);
    bool xinput_ff_enum = false;
    if (usb_busid && usb_busid[0] && is_flydigi_usb_busid(usb_busid)) {
        char xpad_probe[64];
        xinput_ff_enum = flydigi_xpad_evdev_path_for_busid(usb_busid, xpad_probe,
                                                            sizeof(xpad_probe)) == 0 &&
                         !flydigi_has_hidraw_for_busid(usb_busid);
    }
    for (int i = 0; i < nif && cache->if_count < COMPOSITE_ENUM_MAX_IF; ++i) {
        if (xinput_ff_enum && strncmp(ifs[i].cls, "ff", 2) == 0) {
            char ifdir[PATH_MAX], protof[PATH_MAX], proto[8] = {0};
            snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, ifs[i].dir);
            snprintf(protof, sizeof(protof), "%s/bInterfaceProtocol", ifdir);
            read_text_file(protof, proto, sizeof(proto));
            composite_enum_if_t *de = &cache->ifs[cache->if_count++];
            de->num = ifs[i].num;
            snprintf(de->cls, sizeof(de->cls), "%s", ifs[i].cls);
            snprintf(de->node, sizeof(de->node), "%s", ifs[i].node);
            de->rdesc_len = 0;
            if (strtoul(proto, NULL, 16) == 0x01u &&
                flydigi_xbox360_wired_rdesc_len <= COMPOSITE_ENUM_MAX_RDESC) {
                memcpy(de->rdesc, flydigi_xbox360_wired_rdesc, flydigi_xbox360_wired_rdesc_len);
                de->rdesc_len = (int)flydigi_xbox360_wired_rdesc_len;
                log_append("enum: iface %d xpad ff synthetic rdesc (%dB)", de->num, de->rdesc_len);
            }
            continue;
        }
        if (strncmp(ifs[i].cls, "03", 2) != 0) {
            continue;
        }
        composite_enum_if_t *de = &cache->ifs[cache->if_count++];
        de->num = ifs[i].num;
        snprintf(de->cls, sizeof(de->cls), "%s", ifs[i].cls);
        snprintf(de->node, sizeof(de->node), "%s", ifs[i].node);
        de->rdesc_len = read_iface_rdesc(usbdir, ifs[i].dir, de->rdesc, COMPOSITE_ENUM_MAX_RDESC);
        if (de->rdesc_len == 0 && !de->node[0] && usb_busid && usb_busid[0]) {
            char xpad_probe[64];
            if (flydigi_xpad_evdev_path_for_busid(usb_busid, xpad_probe, sizeof(xpad_probe)) == 0 &&
                flydigi_xbox360_wired_rdesc_len <= COMPOSITE_ENUM_MAX_RDESC) {
                memcpy(de->rdesc, flydigi_xbox360_wired_rdesc, flydigi_xbox360_wired_rdesc_len);
                de->rdesc_len = (int)flydigi_xbox360_wired_rdesc_len;
                log_append("enum: iface %d xpad synthetic rdesc (%dB)", de->num, de->rdesc_len);
            }
        }
    }
    log_append("composite enum: %s key=%s desc=%dB ifaces=%d full_speed=%u serial=%s",
               usbdir, key, cache->descriptors_len, cache->if_count, cache->full_speed, cache->serial);
    for (int i = 0; i < cache->if_count; ++i) {
        log_append("  if 1.%d cls=%s rdesc=%dB %s", cache->ifs[i].num,
                   cache->ifs[i].cls, cache->ifs[i].rdesc_len,
                   cache->ifs[i].node[0] ? cache->ifs[i].node : "-");
    }
    if (out_key && out_key_len) snprintf(out_key, out_key_len, "%s", key);
    return 0;
}

#define LG_VENDOR_ID 0x005du

static bool gamepad_iface_candidate(uint16_t vendor_id, uint16_t usage_page, uint16_t usage)
{
    if (vendor_id == LG_VENDOR_ID) {
        return false;
    }
    if (usage_page == 0x01 && (usage == 0x04 || usage == 0x05)) {
        return true;
    }
    if (usage_page == 0x01 && (usage == 0x02 || usage == 0x06)) {
        return false;
    }
    if (usage_page >= 0xff00) {
        return true;
    }
    if (usage_page == 0x01 && usage == 0x08) {
        return true;
    }
    if (usage_page == 0 && usage == 0) {
        /* No usage at all: the vendor is the last evidence there is, and the
         * allow-list is the same table that answers every other VID:PID
         * question. */
        return ctm_pad_vendor_makes_gamepads(vendor_id);
    }
    return false;
}

static int gamepad_iface_score(uint16_t vendor_id, uint16_t usage_page, uint16_t usage)
{
    if (!gamepad_iface_candidate(vendor_id, usage_page, usage)) {
        return -1;
    }
    if (usage_page == 0x01 && usage == 0x05) return 100;
    if (usage_page == 0x01 && usage == 0x04) return 90;
    if (usage_page >= 0xff00) return 50;
    if (usage_page == 0x01 && usage == 0x08) return 40;
    (void) vendor_id;
    return 10;
}

static int gamepad_iface_score_for_device(const device_info_t *dev)
{
    if (!dev) return -1;
    if (!dev->hidraw[0]) {
        if (is_xpad_only_scan_device(dev) && dev->usb_busid[0] &&
            is_flydigi_usb_busid(dev->usb_busid)) {
            return 120;
        }
        return -1;
    }
    if (dev->iface[0]) {
        if (contains_ci(dev->iface, "mouse")) return -1;
        if (contains_ci(dev->iface, "keyboard")) return -1;
    }
    uint16_t vid = (uint16_t)strtoul(dev->vid, NULL, 16);
    int score = gamepad_iface_score(vid, dev->usage_page, dev->usage);
    if (dev->usb_busid[0] && is_flydigi_usb_busid(dev->usb_busid)) {
        if (score < 0) score = 60;
        else score += 15;
    }
    if (dev->name[0] &&
        (contains_ci(dev->name, "flydigi") || contains_ci(dev->name, "vader") ||
         contains_ci(dev->name, "apex"))) {
        if (score < 0) score = 60;
        else score += 15;
    }
    if (dev->iface[0] && contains_ci(dev->iface, "vendor")) {
        if (score < 0) score = 55;
        else score += 10;
    }
    return score;
}

int best_scan_index_for_item(const logical_device_t *item)
{
    if (!item || item->device_count <= 0) return -1;
    int best = -1;
    int best_score = -1;
    int first_hidraw = -1;
    int xpad_only = -1;
    for (int i = 0; i < item->device_count; ++i) {
        int idx = item->device_indices[i];
        if (idx < 0 || idx >= g_scan.count) continue;
        const device_info_t *dev = &g_scan.devices[idx];
        if (dev->hidraw[0]) {
            if (first_hidraw < 0) first_hidraw = idx;
            int score = gamepad_iface_score_for_device(dev);
            if (score > best_score) {
                best_score = score;
                best = idx;
            }
        } else if (is_xpad_only_scan_device(dev)) {
            if (xpad_only < 0) xpad_only = idx;
            int score = gamepad_iface_score_for_device(dev);
            if (score > best_score) {
                best_score = score;
                best = idx;
            }
        }
    }
    if (best >= 0) return best;
    if (first_hidraw >= 0) return first_hidraw;
    if (xpad_only >= 0) return xpad_only;
    return item->device_indices[0];
}

static int flydigi_hidraw_pick(const char *usb_busid, const logical_device_t *only_item)
{
    int best_idx = -1;
    int best_score = -1;
    int first_idx = -1;
    const int loops = only_item ? only_item->device_count : g_scan.count;
    for (int p = 0; p < loops; ++p) {
        int i = only_item ? only_item->device_indices[p] : p;
        if (i < 0 || i >= g_scan.count) continue;
        const device_info_t *dev = &g_scan.devices[i];
        if (!dev->hidraw[0]) continue;

        char peer_busid[64] = {0};
        snprintf(peer_busid, sizeof(peer_busid), "%s", dev->usb_busid);
        if (!peer_busid[0]) {
            usb_busid_from_hidraw_name(dev->hidraw, peer_busid, sizeof(peer_busid));
        }
        if (usb_busid && usb_busid[0]) {
            if (!peer_busid[0] || strcmp(peer_busid, usb_busid) != 0) continue;
        } else if (peer_busid[0]) {
            if (!is_flydigi_usb_busid(peer_busid)) continue;
        } else if (!is_flydigi_composite_device(dev)) {
            continue;
        }

        if (first_idx < 0) first_idx = i;
        int score = gamepad_iface_score_for_device(dev);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    if (first_idx < 0 && only_item) {
        return flydigi_hidraw_pick(usb_busid, NULL);
    }
    return best_idx >= 0 ? best_idx : first_idx;
}

static bool flydigi_hidraw_is_mouse(const device_info_t *dev)
{
    if (!dev) return false;
    if (dev->iface[0] && contains_ci(dev->iface, "mouse")) return true;
    if (dev->name[0] && contains_ci(dev->name, "mouse")) return true;
    return false;
}

static bool flydigi_has_gamepad_hidraw_for_busid(const char *usb_busid)
{
    if (!usb_busid || !usb_busid[0]) return false;
    for (int i = 0; i < g_scan.count; ++i) {
        const device_info_t *dev = &g_scan.devices[i];
        if (!dev->hidraw[0]) continue;

        char peer_busid[64] = {0};
        snprintf(peer_busid, sizeof(peer_busid), "%s", dev->usb_busid);
        if (!peer_busid[0]) {
            usb_busid_from_hidraw_name(dev->hidraw, peer_busid, sizeof(peer_busid));
        }
        if (!peer_busid[0] || strcmp(peer_busid, usb_busid) != 0) continue;
        if (gamepad_iface_score_for_device(dev) >= 90) return true;
    }
    return false;
}

bool flydigi_is_xinput_mode_for_busid(const char *usb_busid)
{
    char xpad_path[64];
    if (!usb_busid || !usb_busid[0]) return false;
    if (flydigi_xpad_evdev_path_for_busid(usb_busid, xpad_path, sizeof(xpad_path)) != 0) {
        return false;
    }
    return !flydigi_has_gamepad_hidraw_for_busid(usb_busid);
}

bool flydigi_is_xinput_mode(const logical_device_t *item)
{
    if (!item) return false;
    char busid[64] = {0};
    if (item->usb_busid[0]) {
        snprintf(busid, sizeof(busid), "%s", item->usb_busid);
    } else if (starts_with(item->key, "flydigi:")) {
        snprintf(busid, sizeof(busid), "%s", item->key + strlen("flydigi:"));
    }
    return flydigi_is_xinput_mode_for_busid(busid);
}

bool flydigi_has_hidraw_for_busid(const char *usb_busid)
{
    if (!usb_busid || !usb_busid[0]) {
        return false;
    }
    for (int i = 0; i < g_scan.count; ++i) {
        const device_info_t *dev = &g_scan.devices[i];
        if (!dev->hidraw[0]) {
            continue;
        }
        char peer_busid[64] = {0};
        snprintf(peer_busid, sizeof(peer_busid), "%s", dev->usb_busid);
        if (!peer_busid[0]) {
            usb_busid_from_hidraw_name(dev->hidraw, peer_busid, sizeof(peer_busid));
        }
        if (peer_busid[0] && strcmp(peer_busid, usb_busid) == 0) {
            return true;
        }
    }
    return false;
}

bool flydigi_is_xinput_evdev_only_for_busid(const char *usb_busid)
{
    char xpad_path[64];
    if (!usb_busid || !usb_busid[0] || !is_flydigi_usb_busid(usb_busid)) {
        return false;
    }
    if (flydigi_xpad_evdev_path_for_busid(usb_busid, xpad_path, sizeof(xpad_path)) != 0) {
        return false;
    }
    return !flydigi_has_hidraw_for_busid(usb_busid);
}

bool flydigi_is_xinput_evdev_only(const logical_device_t *item)
{
    if (!item) {
        return false;
    }
    char busid[64] = {0};
    if (item->usb_busid[0]) {
        snprintf(busid, sizeof(busid), "%s", item->usb_busid);
    } else if (starts_with(item->key, "flydigi:")) {
        snprintf(busid, sizeof(busid), "%s", item->key + strlen("flydigi:"));
    }
    return flydigi_is_xinput_evdev_only_for_busid(busid);
}

int flydigi_xpad_scan_index_for_item(const logical_device_t *item)
{
    if (!item) {
        return -1;
    }
    char busid[64] = {0};
    if (item->usb_busid[0]) {
        snprintf(busid, sizeof(busid), "%s", item->usb_busid);
    } else if (starts_with(item->key, "flydigi:")) {
        snprintf(busid, sizeof(busid), "%s", item->key + strlen("flydigi:"));
    }
    if (!flydigi_is_xinput_evdev_only_for_busid(busid)) {
        return -1;
    }
    for (int p = 0; p < item->device_count; ++p) {
        int idx = item->device_indices[p];
        if (idx < 0 || idx >= g_scan.count) {
            continue;
        }
        const device_info_t *dev = &g_scan.devices[idx];
        if (dev->hidraw[0]) {
            continue;
        }
        if (dev->usb_busid[0] && strcmp(dev->usb_busid, busid) == 0) {
            return idx;
        }
    }
    for (int i = 0; i < g_scan.count; ++i) {
        const device_info_t *dev = &g_scan.devices[i];
        if (dev->hidraw[0] || !dev->usb_busid[0]) {
            continue;
        }
        if (strcmp(dev->usb_busid, busid) != 0) {
            continue;
        }
        if (is_xpad_only_scan_device(dev) || is_xpad_compatible_pid(dev->vid, dev->pid)) {
            return i;
        }
    }
    return -1;
}

static int flydigi_handshake_hidraw_pick(const char *usb_busid, const logical_device_t *only_item)
{
    int first_idx = -1;
    const int loops = only_item ? only_item->device_count : g_scan.count;
    for (int p = 0; p < loops; ++p) {
        int i = only_item ? only_item->device_indices[p] : p;
        if (i < 0 || i >= g_scan.count) continue;
        const device_info_t *dev = &g_scan.devices[i];
        if (!dev->hidraw[0] || flydigi_hidraw_is_mouse(dev)) continue;

        char peer_busid[64] = {0};
        snprintf(peer_busid, sizeof(peer_busid), "%s", dev->usb_busid);
        if (!peer_busid[0]) {
            usb_busid_from_hidraw_name(dev->hidraw, peer_busid, sizeof(peer_busid));
        }
        if (usb_busid && usb_busid[0]) {
            if (!peer_busid[0] || strcmp(peer_busid, usb_busid) != 0) continue;
        } else if (peer_busid[0]) {
            if (!is_flydigi_usb_busid(peer_busid)) continue;
        } else if (!is_flydigi_composite_device(dev)) {
            continue;
        }

        if (first_idx < 0) first_idx = i;
    }
    if (first_idx < 0 && only_item) {
        return flydigi_handshake_hidraw_pick(usb_busid, NULL);
    }
    return first_idx;
}

int flydigi_handshake_hidraw_path_for_busid(const char *usb_busid, char *out, size_t out_len)
{
    if (!out || out_len == 0) return -1;
    int pick = flydigi_handshake_hidraw_pick(usb_busid, NULL);
    if (pick < 0) return -1;
    snprintf(out, out_len, "%s", g_scan.devices[pick].node);
    return 0;
}

int flydigi_handshake_hidraw_path_for_item(const logical_device_t *item, char *out, size_t out_len)
{
    if (!item || !out || out_len == 0) return -1;
    char busid[64] = {0};
    if (item->usb_busid[0]) {
        snprintf(busid, sizeof(busid), "%s", item->usb_busid);
    } else if (starts_with(item->key, "flydigi:")) {
        snprintf(busid, sizeof(busid), "%s", item->key + strlen("flydigi:"));
    }
    int pick = flydigi_handshake_hidraw_pick(busid[0] ? busid : NULL, item);
    if (pick < 0) return -1;
    snprintf(out, out_len, "%s", g_scan.devices[pick].node);
    return 0;
}

int flydigi_hidraw_path_for_busid(const char *usb_busid, char *out, size_t out_len)
{
    if (!out || out_len == 0) return -1;
    int pick = flydigi_hidraw_pick(usb_busid, NULL);
    if (pick < 0) return -1;
    snprintf(out, out_len, "%s", g_scan.devices[pick].node);
    return 0;
}

int flydigi_hidraw_path_for_item(const logical_device_t *item, char *out, size_t out_len)
{
    if (!item || !out || out_len == 0) return -1;
    char busid[64] = {0};
    if (item->usb_busid[0]) {
        snprintf(busid, sizeof(busid), "%s", item->usb_busid);
    } else if (starts_with(item->key, "flydigi:")) {
        snprintf(busid, sizeof(busid), "%s", item->key + strlen("flydigi:"));
    }
    int pick = flydigi_hidraw_pick(busid[0] ? busid : NULL, item);
    if (pick < 0) return -1;
    snprintf(out, out_len, "%s", g_scan.devices[pick].node);
    return 0;
}

static bool hex_id_matches(const char *text, unsigned int value)
{
    unsigned int parsed = 0;
    if (!text || !text[0]) {
        return false;
    }
    if (sscanf(text, "%x", &parsed) != 1) {
        return false;
    }
    return parsed == value;
}

int flydigi_xpad_evdev_path_for_busid(const char *usb_busid, char *out, size_t out_len)
{
    if (!usb_busid || !usb_busid[0] || !out || out_len == 0) {
        return -1;
    }
    DIR *d = opendir("/sys/class/input");
    if (!d) {
        return -1;
    }
    struct dirent *ent;
    int rc = -1;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) {
            continue;
        }
        char input_path[PATH_MAX], busid[64] = {0};
        snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", ent->d_name);
        usb_busid_from_input_path(input_path, busid, sizeof(busid));
        if (strcmp(busid, usb_busid) != 0) {
            continue;
        }

        char vendor[16] = {0}, product[16] = {0}, name[128] = {0};
        char attr[PATH_MAX];
        snprintf(attr, sizeof(attr), "%s/id/vendor", input_path);
        read_text_file(attr, vendor, sizeof(vendor));
        snprintf(attr, sizeof(attr), "%s/id/product", input_path);
        read_text_file(attr, product, sizeof(product));
        snprintf(attr, sizeof(attr), "%s/name", input_path);
        read_text_file(attr, name, sizeof(name));
        if (!hex_id_matches(vendor, 0x045e) || !hex_id_matches(product, 0x028e)) {
            if (!contains_ci(name, "x-box") && !contains_ci(name, "xbox")) {
                continue;
            }
        }

        DIR *input = opendir(input_path);
        if (!input) {
            continue;
        }
        struct dirent *child;
        while ((child = readdir(input)) != NULL) {
            if (strncmp(child->d_name, "event", 5) != 0) {
                continue;
            }
            snprintf(out, out_len, "/dev/input/%s", child->d_name);
            rc = 0;
            break;
        }
        closedir(input);
        if (rc == 0) {
            break;
        }
    }
    closedir(d);
    return rc;
}

int flydigi_xpad_evdev_path_for_item(const logical_device_t *item, char *out, size_t out_len)
{
    if (!item || !out || out_len == 0) {
        return -1;
    }
    char busid[64] = {0};
    if (item->usb_busid[0]) {
        snprintf(busid, sizeof(busid), "%s", item->usb_busid);
    } else if (starts_with(item->key, "flydigi:")) {
        snprintf(busid, sizeof(busid), "%s", item->key + strlen("flydigi:"));
    }
    if (!busid[0]) {
        return -1;
    }
    return flydigi_xpad_evdev_path_for_busid(busid, out, out_len);
}

bool flydigi_has_pluggable_path(const logical_device_t *item)
{
    char path[64];
    if (flydigi_is_xinput_evdev_only(item)) {
        return flydigi_xpad_evdev_path_for_item(item, path, sizeof(path)) == 0;
    }
    if (flydigi_handshake_hidraw_path_for_item(item, path, sizeof(path)) == 0) {
        return true;
    }
    if (flydigi_is_xinput_mode(item) &&
        flydigi_xpad_evdev_path_for_item(item, path, sizeof(path)) == 0) {
        return true;
    }
    if (!flydigi_is_xinput_mode(item) &&
        flydigi_hidraw_path_for_item(item, path, sizeof(path)) == 0) {
        return true;
    }
    return false;
}

uint8_t *build_composite_enum_payload(const char *key, int *out_len)
{
    const composite_enum_t *cache = composite_enum_lookup(key);
    if (!cache || !out_len) return NULL;
    int size = (int)sizeof(ctmb_enum_info_t) + cache->descriptors_len;
    for (int i = 0; i < cache->if_count; ++i) {
        size += (int)sizeof(ctmb_enum_iface_t) + cache->ifs[i].rdesc_len;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) return NULL;
    int off = 0;
    ctmb_enum_info_t info;
    memset(&info, 0, sizeof(info));
    info.descriptors_len = (uint16_t)cache->descriptors_len;
    info.iface_count = (uint8_t)cache->if_count;
    info.full_speed = cache->full_speed ? 1 : 0;
    memcpy(buf + off, &info, sizeof(info)); off += (int)sizeof(info);
    memcpy(buf + off, cache->descriptors, (size_t)cache->descriptors_len);
    off += cache->descriptors_len;
    for (int i = 0; i < cache->if_count; ++i) {
        ctmb_enum_iface_t ie;
        memset(&ie, 0, sizeof(ie));
        ie.interface_number = (uint8_t)cache->ifs[i].num;
        ie.iface_class = (uint8_t)strtol(cache->ifs[i].cls, NULL, 16);
        ie.report_desc_len = (uint16_t)cache->ifs[i].rdesc_len;
        memcpy(buf + off, &ie, sizeof(ie)); off += (int)sizeof(ie);
        memcpy(buf + off, cache->ifs[i].rdesc, (size_t)cache->ifs[i].rdesc_len);
        off += cache->ifs[i].rdesc_len;
    }
    *out_len = off;
    return buf;
}

void enumerate_devices(scan_result_t *result)
{
    memset(result, 0, sizeof(*result));

    DIR *input_dir = opendir("/sys/class/input");
    if (input_dir) {
        struct dirent *input_ent;
        while ((input_ent = readdir(input_dir)) != NULL) {
            if (!starts_with(input_ent->d_name, "input")) {
                continue;
            }
            result->input_count++;

            char input_path[PATH_MAX];
            snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", input_ent->d_name);

            char name[TEXT_LEN] = {0};
            char phys[TEXT_LEN] = {0};
            char bus[16] = {0};
            char vid[16] = {0};
            char pid[16] = {0};
            char version[16] = {0};
            char uniq[64] = {0};
            char usb_busid[64] = {0};
            char child_path[PATH_MAX];
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/name", input_path), child_path), name, sizeof(name));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/phys", input_path), child_path), phys, sizeof(phys));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/bustype", input_path), child_path), bus, sizeof(bus));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/vendor", input_path), child_path), vid, sizeof(vid));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/product", input_path), child_path), pid, sizeof(pid));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/version", input_path), child_path), version, sizeof(version));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/uniq", input_path), child_path), uniq, sizeof(uniq));
            usb_busid_from_input_path(input_path, usb_busid, sizeof(usb_busid));

            char event_names[TEXT_LEN] = {0};
            char input_node[64] = {0};
            DIR *one_input = opendir(input_path);
            if (one_input) {
                struct dirent *child;
                while ((child = readdir(one_input)) != NULL) {
                    if (starts_with(child->d_name, "js")) {
                        append_unique(event_names, sizeof(event_names), child->d_name);
                        snprintf(input_node, sizeof(input_node), "/dev/input/%s", child->d_name);
                    } else if (starts_with(child->d_name, "event")) {
                        append_unique(event_names, sizeof(event_names), child->d_name);
                        if (!input_node[0]) {
                            snprintf(input_node, sizeof(input_node), "/dev/input/%s", child->d_name);
                        }
                    }
                }
                closedir(one_input);
            }

            char hidraw_path[PATH_MAX];
            snprintf(hidraw_path, sizeof(hidraw_path), "%s/device/hidraw", input_path);
            DIR *hidraw_dir = opendir(hidraw_path);
            if (!hidraw_dir) {
                if (is_xpad_input_only_candidate(bus, vid, pid, name, usb_busid)) {
                    device_info_t *dev = find_or_add_input_device(result, input_ent->d_name, usb_busid);
                    if (dev) {
                        if (!dev->node[0]) {
                            snprintf(dev->node, sizeof(dev->node), "%s",
                                     input_node[0] ? input_node : input_path);
                        }
                        if (!dev->name[0]) snprintf(dev->name, sizeof(dev->name), "%s", name);
                        if (!dev->phys[0]) snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
                        if (!dev->bus[0]) snprintf(dev->bus, sizeof(dev->bus), "%s", bus);
                        if (!dev->vid[0]) snprintf(dev->vid, sizeof(dev->vid), "%s", vid);
                        if (!dev->pid[0]) snprintf(dev->pid, sizeof(dev->pid), "%s", pid);
                        if (!dev->version[0]) snprintf(dev->version, sizeof(dev->version), "%s", version);
                        if (!dev->mac[0]) snprintf(dev->mac, sizeof(dev->mac), "%s", uniq);
                        if (!dev->usb_busid[0]) snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", usb_busid);
                        append_unique(dev->inputs, sizeof(dev->inputs), input_ent->d_name);
                        append_unique(dev->events, sizeof(dev->events), event_names);
                    }
                }
                continue;
            }

            struct dirent *hidraw_ent;
            while ((hidraw_ent = readdir(hidraw_dir)) != NULL) {
                if (!starts_with(hidraw_ent->d_name, "hidraw")) {
                    continue;
                }
                device_info_t *dev = find_or_add_device(result, hidraw_ent->d_name);
                if (!dev) {
                    continue;
                }
                if (!dev->name[0]) snprintf(dev->name, sizeof(dev->name), "%s", name);
                if (!dev->phys[0]) snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
                if (!dev->bus[0]) snprintf(dev->bus, sizeof(dev->bus), "%s", bus);
                if (!dev->vid[0]) snprintf(dev->vid, sizeof(dev->vid), "%s", vid);
                if (!dev->pid[0]) snprintf(dev->pid, sizeof(dev->pid), "%s", pid);
                if (!dev->version[0]) snprintf(dev->version, sizeof(dev->version), "%s", version);
                if (!dev->mac[0]) snprintf(dev->mac, sizeof(dev->mac), "%s", uniq);
                if (!dev->usb_busid[0]) snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", usb_busid);
                append_unique(dev->inputs, sizeof(dev->inputs), input_ent->d_name);
                append_unique(dev->events, sizeof(dev->events), event_names);

                snprintf(child_path, sizeof(child_path), "%s/device/report_descriptor", input_path);
                struct stat report_st;
                if (dev->report_descriptor_bytes <= 0 && stat(child_path, &report_st) == 0) {
                    dev->report_descriptor_bytes = (int)report_st.st_size;
                }
            }
            closedir(hidraw_dir);
        }
        closedir(input_dir);
    }

    for (int i = 0; i < 64; ++i) {
        char hidraw[32];
        char node[64];
        snprintf(hidraw, sizeof(hidraw), "hidraw%d", i);
        snprintf(node, sizeof(node), "/dev/%s", hidraw);
        struct stat st;
        if (stat(node, &st) != 0) {
            continue;
        }
        device_info_t *dev = find_or_add_device(result, hidraw);
        if (dev) {
            inspect_hidraw(dev);
        }
    }

    /* Catch-all for devices not covered by the /dev loop above (e.g. hidraw
     * nodes only visible via sysfs when the jail /dev is missing them). This is
     * now cheap: inspect_hidraw early-returns for devices already populated. */
    for (int i = 0; i < result->count; ++i) {
        inspect_hidraw(&result->devices[i]);
    }

    enrich_scan_usb_busids(result);
    tag_xpad_flydigi_candidates(result);
}

