/* What is left of ui_devices.c while it is being split: the logical-device
 * model, which moves to ctm_model.c next and empties this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ctm_state.h"
#include "ctm_devices_internal.h"

#include <stdio.h>
#include <string.h>

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

/* Resolve a device by the key it is identified by, against the CURRENT
 * g_devices. Returns NULL when the device is gone, and callers must handle that
 * rather than fall back to a remembered index.
 *
 * Use this for any index that OUTLIVES the call that took it — widget user
 * data, a remembered selection, anything crossing a rebuild. Walking
 * g_devices.items by index within one call is fine and several callers do it
 * (hid_pt_gamepad_match.c, the panel's render loop); nothing enforces the
 * distinction, it is about lifetime. */
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
