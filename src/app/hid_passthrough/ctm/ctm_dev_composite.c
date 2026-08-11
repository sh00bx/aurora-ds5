/* Composite USB devices, device side: the sysfs device dir behind a Steam Puck
 * or a Flydigi, the interface enumeration that is captured there and forwarded
 * to the Windows agent at plug time, and the port-keyed caches both of those
 * read. The Flydigi mode probes (XInput vs D-Input, which hidraw or evdev node
 * to bridge) live here too, because every one of them is a question about that
 * device dir.
 *
 * The file is ctm_dev_composite.c and not ctm_composite.c because
 * ctm/controllers/ctm_composite.c already exists and is a different thing: the
 * runtime reader for a bridged controller's sibling hidraw nodes. The two share
 * no state and no naming.
 *
 * Split out of ui_devices.c; the code is unchanged. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ctm_state.h"
#include "ctm_devices_internal.h"
#include "ctm_bridge_protocol.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Still the same device in that port? @p reverify reads sysfs now; without it
 * the answer is re-read at most once per model rebuild and the last answer
 * stands for the rest of it — three small sysfs reads per caller per scan,
 * against a re-capture that walks /sys/class/input. The memo is for the
 * per-scan callers only: g_devices.generation advances once per
 * hid_pt_rescan_model(), so anything that must not act on a stale answer
 * between rescans (a plug lands off that cadence) has to pass reverify. */
static bool port_cache_entry_still_current(hid_pt_port_cache_t *entry, bool reverify)
{
    if (!reverify && entry->checked_generation == g_devices.generation && entry->identity != 0) {
        return true;
    }
    entry->checked_generation = g_devices.generation;
    uint64_t now = hid_pt_usb_identity_hash(entry->usbdir);
    return now != 0 && now == entry->identity;
}

/* A live, identity-checked entry for @p key, or NULL. An entry whose port now
 * holds a different device (or nothing) is dropped here rather than returned —
 * decided by reading sysfs now when @p reverify, otherwise by the identity
 * check this model rebuild already made (port_cache_entry_still_current). */
static hid_pt_port_cache_t *port_cache_find(void *base, size_t stride, int max, const char *key,
                                            bool reverify)
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
        if (port_cache_entry_still_current(entry, reverify)) {
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
            /* Eviction preference only: a memoised "still current" here costs
             * at worst the oldest entry instead of a departed one, so this walk
             * does not force a sysfs re-read of every slot. */
            if (entry->valid && !port_cache_entry_still_current(entry, false)) {
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
        g_usb_identity_cache, sizeof(g_usb_identity_cache[0]), USB_IDENTITY_CACHE_MAX, usb_busid,
        false);   /* per-scan caller: the once-per-rebuild identity check is the point */
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
    /* Identity re-read from sysfs on every call (reverify=true), not memoised
     * per model rebuild the way the per-scan identity cache is: this table is
     * read at plug time, and a plug happens when it happens, not on the rescan
     * cadence — so the memoised answer can be a whole poll interval stale. An
     * enumeration captured from a device that has since left the port would
     * hand the host the wrong descriptors, and three sysfs reads per plug is a
     * cheap way to not do that. */
    return (const composite_enum_t *)port_cache_find(
        g_composite_enums, sizeof(g_composite_enums[0]), COMPOSITE_ENUM_MAX_CACHE, key, true);
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
            usb_busid_for_scan_device(dev, peer_busid, sizeof(peer_busid));
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
            usb_busid_for_scan_device(dev, peer_busid, sizeof(peer_busid));
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
            usb_busid_for_scan_device(dev, peer_busid, sizeof(peer_busid));
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
            usb_busid_for_scan_device(dev, peer_busid, sizeof(peer_busid));
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
