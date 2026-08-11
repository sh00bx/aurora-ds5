/* What a scanned device is, and what to call it.
 *
 * The pad predicates, the interface scoring that decides which HID interface of
 * a device is the gamepad one, and the key and display name a logical device is
 * built from. These functions do not scan and hold no state of their own; the
 * ones that need a device's USB manufacturer/product strings get them from
 * read_usb_identity_attrs() in ctm_dev_composite.c, which does cache.
 *
 * Every rule that IS a VID:PID fact is a row in ctm_pad_table.h. What stays
 * here is everything that is not: the name sniffing (Gulikit, a Microsoft pad
 * known only by its product string) and the Flydigi sysfs heuristics.
 *
 * Split out of ui_devices.c; the code is unchanged. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ctm_state.h"
#include "ctm_devices_internal.h"
#include "ctm_pad_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *bus_label(const char *bus)
{
    if (strcmp(bus, "0003") == 0 || strcmp(bus, "3") == 0) return "USB";
    if (strcmp(bus, "0005") == 0 || strcmp(bus, "5") == 0) return "BT";
    return bus[0] ? bus : "-";
}

static bool flydigi_identity_text(const char *mfg, const char *prod)
{
    if (mfg && contains_ci(mfg, "flydigi")) return true;
    if (prod && (contains_ci(prod, "flydigi") || contains_ci(prod, "apex") ||
                 contains_ci(prod, "vader"))) {
        return true;
    }
    return false;
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

int gamepad_iface_score_for_device(const device_info_t *dev)
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
