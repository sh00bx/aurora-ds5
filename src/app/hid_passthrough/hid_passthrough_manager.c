#include "hid_passthrough_manager.h"

#if defined(TARGET_WEBOS)

#include "ctm/ctm_state.h"
#include "logging.h"
#include "stream/input/session_input.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t parse_hex16(const char *text) {
    return (uint16_t) strtoul(text ? text : "0", NULL, 16);
}

static void hid_pt_sync_plugged_state(void) {
    for (int i = 0; i < g_devices.count; ++i) {
        g_devices.items[i].plugged = plug_key_is_set(g_devices.items[i].key);
    }
}

void hid_passthrough_manager_init(hid_passthrough_manager_t *manager) {
    memset(manager, 0, sizeof(*manager));
    manager->port = HID_PT_DEFAULT_PORT;
}

void hid_passthrough_manager_set_stream_input(hid_passthrough_manager_t *manager,
                                              stream_input_t *input) {
    if (manager) {
        manager->stream_input = input;
    }
}

void hid_passthrough_manager_deinit(hid_passthrough_manager_t *manager) {
    hid_passthrough_manager_stop(manager);
}

int hid_passthrough_manager_start(hid_passthrough_manager_t *manager, const char *host, int port,
                                  bool autoplug) {
    if (!manager || !host || !host[0]) {
        return -1;
    }
    strncpy(manager->host, host, sizeof(manager->host) - 1);
    manager->host[sizeof(manager->host) - 1] = '\0';
    manager->port = port > 0 ? port : HID_PT_DEFAULT_PORT;
    manager->autoplug = autoplug;

    ctm_bridge_set_agent_host(manager->host, manager->port);
    g_running = true;
    if (!g_stop_sniff_thread_started) {
        if (pthread_create(&g_stop_sniff_thread, NULL, stop_sniff_worker, NULL) != 0) {
            commons_log_warn("HidPassthrough", "stopSniff worker thread failed to start");
        } else {
            g_stop_sniff_thread_started = true;
        }
    }

    manager->running = true;
    hid_passthrough_manager_rescan(manager);
    return 0;
}

void hid_passthrough_manager_stop(hid_passthrough_manager_t *manager) {
    if (!manager || !manager->running) {
        return;
    }
    /* The stopSniff worker previously outlived every stop (nothing ever
     * cleared g_running), spawning luna-send-pub 2x/s per pad until app exit. */
    g_running = false;
    if (g_stop_sniff_thread_started) {
        pthread_join(g_stop_sniff_thread, NULL);
        g_stop_sniff_thread_started = false;
    }
    release_local_sessions_on_exit();
    g_plugged_key_count = 0;
    g_expanded_key_count = 0;
    autoplug_reset();
    hid_pt_sync_plugged_state();
    manager->stream_input = NULL;
    manager->running = false;
}

bool hid_passthrough_manager_active(const hid_passthrough_manager_t *manager) {
    return manager && manager->running;
}

int hid_passthrough_manager_device_count(hid_passthrough_manager_t *manager) {
    (void) manager;
    return g_devices.count;
}

int hid_passthrough_manager_get_device(hid_passthrough_manager_t *manager, int index,
                                       hid_pt_device_info_t *info) {
    (void) manager;
    if (!info || index < 0 || index >= g_devices.count) {
        return -1;
    }

    logical_device_t *item = &g_devices.items[index];
    int scan_index = first_scan_index_for_item(item);
    memset(info, 0, sizeof(*info));

    if (scan_index >= 0 && scan_index < g_scan.count) {
        const device_info_t *dev = &g_scan.devices[scan_index];
        snprintf(info->path, sizeof(info->path), "%s", dev->node);
        info->usage_page = dev->usage_page;
        info->usage = dev->usage;
    }

    snprintf(info->product, sizeof(info->product), "%s", item->name);
    info->vendor_id = parse_hex16(item->vid);
    info->product_id = parse_hex16(item->pid);
    info->plugged = item->plugged;

    int session_index = session_index_for_key(item->key);
    info->connected = false;
    if (session_index >= 0 && g_sessions[session_index].controller) {
        ctm_controller_status_t status;
        ctm_controller_get_status(g_sessions[session_index].controller, &status);
        info->connected = status.connected;
    }
    return 0;
}

void hid_passthrough_manager_rescan(hid_passthrough_manager_t *manager) {
    (void) manager;
    enumerate_devices(&g_scan);
    build_logical_devices(&g_scan, &g_devices);
    publish_bt_macs();

    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        const char *kind = bridge_kind_for_item(item);
        if (strcmp(kind, "puck") == 0) {
            composite_enum_capture(NULL, item->vid, item->pid, NULL, 0);
        } else if (strcmp(kind, "flydigi") == 0) {
            composite_enum_capture(item->usb_busid, item->vid, item->pid, NULL, 0);
        }
    }

    hid_pt_sync_plugged_state();
}

void hid_passthrough_manager_reconcile(hid_passthrough_manager_t *manager,
                                       stream_input_t *input) {
    if (!manager || !manager->running) {
        return;
    }
    hid_pt_autoplug_reconcile(input);
}

void hid_passthrough_manager_poll(hid_passthrough_manager_t *manager) {
    if (!manager || !manager->running) {
        return;
    }
    hid_passthrough_manager_rescan(manager);
    hid_pt_autoplug_reconcile(manager->stream_input);
}

#endif
