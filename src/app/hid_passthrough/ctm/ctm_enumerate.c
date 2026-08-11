/* The device scan: walk /sys/class/input and /dev/hidraw*, ask each node what
 * it is (inspect_hidraw), and resolve each device's USB bus id.
 *
 * Everything here reads the system and fills in a scan_result_t. It decides
 * nothing about what a device IS (ctm_classify.c) and builds no logical devices
 * (ctm_model.c) — it calls into both only to name the rows it keeps.
 *
 * Split out of ui_devices.c; the code is unchanged apart from one comment that
 * pointed at a function which now lives in ctm_classify.c. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ctm_state.h"
#include "ctm_hid.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* Bluetooth hidraw nodes that have no USB bus id, so the two /sys/class/input
 * walks below are not repeated for them.
 *
 * The hazard is that hidraw minors are recycled: a USB pad that later becomes
 * hidraw3 must not inherit a Bluetooth pad's "no bus id". What blocks that is
 * the bus gate in hidraw_busid_memoisable() below — only devices the kernel
 * puts on the Bluetooth bus are written to the memo, and only those consult it,
 * so a USB pad taking over the minor never reaches this table at all; and a
 * device the kernel calls Bluetooth cannot grow a USB bus id afterwards.
 *
 * The key is (hidraw name, phys) rather than the name alone, but phys is only
 * a cheap extra discriminator here, NOT an identity: for Bluetooth HID the
 * kernel sets phys to the local adapter address, which every BT pad on this
 * adapter shares (and steam_root_from_phys(), in ctm_classify.c, deliberately
 * trims phys to a root several interfaces share). Among BT devices the pair
 * therefore degenerates to the hidraw name. If the bus gate is ever relaxed —
 * memoising USB negatives, or a device whose bus string is blank — phys will
 * not carry the guarantee and the recycled-minor hazard comes straight back. A
 * device with no phys at all is not memoised either.
 *
 * A USB device that failed to resolve (the jail sysfs gap) is deliberately NOT
 * memoised for a second reason: that one could plausibly resolve on a later
 * pass, and hiding a bus id the device really has is the expensive direction.
 *
 * Only negatives are cached. A wrong positive would attribute another device's
 * port to this one, which is the failure this phase removes everywhere else. */
#define HIDRAW_NO_BUSID_MAX 32

typedef struct {
    char hidraw[32];
    char phys[TEXT_LEN];
    uint32_t seq;             /* 0 = free slot; otherwise write order */
} hidraw_no_busid_t;

static hidraw_no_busid_t g_hidraw_no_busid[HIDRAW_NO_BUSID_MAX];

static bool hidraw_busid_memoisable(const device_info_t *dev)
{
    return dev && dev->hidraw[0] && dev->phys[0] && strcmp(bus_label(dev->bus), "BT") == 0;
}

static bool hidraw_busid_known_absent(const device_info_t *dev)
{
    if (!hidraw_busid_memoisable(dev)) {
        return false;
    }
    for (int i = 0; i < HIDRAW_NO_BUSID_MAX; ++i) {
        if (g_hidraw_no_busid[i].seq &&
            strcmp(g_hidraw_no_busid[i].hidraw, dev->hidraw) == 0 &&
            strcmp(g_hidraw_no_busid[i].phys, dev->phys) == 0) {
            return true;
        }
    }
    return false;
}

static void hidraw_note_no_busid(const device_info_t *dev)
{
    static uint32_t seq;
    if (!hidraw_busid_memoisable(dev)) {
        return;
    }
    hidraw_no_busid_t *slot = &g_hidraw_no_busid[0];
    for (int i = 0; i < HIDRAW_NO_BUSID_MAX; ++i) {
        if (!g_hidraw_no_busid[i].seq) {
            slot = &g_hidraw_no_busid[i];
            break;
        }
        if (g_hidraw_no_busid[i].seq < slot->seq) {
            slot = &g_hidraw_no_busid[i];   /* oldest, if nothing is free */
        }
    }
    snprintf(slot->hidraw, sizeof(slot->hidraw), "%s", dev->hidraw);
    snprintf(slot->phys, sizeof(slot->phys), "%s", dev->phys);
    slot->seq = ++seq;
}

static void usb_busid_resolve_uncached(const char *hidraw, char *out, size_t out_len)
{
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

void usb_busid_for_scan_device(const device_info_t *dev, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!dev || !dev->hidraw[0]) {
        return;
    }
    if (hidraw_busid_known_absent(dev)) {
        return;
    }
    usb_busid_resolve_uncached(dev->hidraw, out, out_len);
    if (!out[0]) {
        hidraw_note_no_busid(dev);
    }
}

static void enrich_scan_usb_busids(scan_result_t *result)
{
    for (int i = 0; i < result->count; ++i) {
        device_info_t *dev = &result->devices[i];
        if (dev->usb_busid[0]) continue;
        if (dev->hidraw[0]) {
            usb_busid_for_scan_device(dev, dev->usb_busid, sizeof(dev->usb_busid));
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
            usb_busid_for_scan_device(dev, peer, sizeof(peer));
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
