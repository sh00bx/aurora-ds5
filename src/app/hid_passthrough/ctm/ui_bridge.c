/* Agent control client (discovery + commands), bridge sessions, process
 * spawning, the per-device settings store, and plug-in/out orchestration.
 * Moved verbatim out of lvgl_ui.c; de-static'd and prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ctm_state.h"
#include "ctm_bridge_protocol.h"
#include "hid_pt_device_prefs.h"
#include "hid_pt_gamepad_match.h"
#include "stream/input/session_input.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
tv_bridge_worker_settings_t default_settings_for_item(const logical_device_t *item)
{
    tv_bridge_worker_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.kind = TV_BRIDGE_KIND_HID;
    settings.audio_mode = TV_BRIDGE_AUDIO_AUTO;
    settings.latency_ms = 48;
    settings.haptics_gain_centi = 100;
    settings.headset_volume_percent = 100;
    settings.speaker_volume_percent = 100;
    settings.ds5_patch_high_nibble = 0xf;
    settings.ds5_patch_low_nibble = 0xd;
    settings.ds5_patch2_high_nibble = 0xf;
    settings.ds5_patch2_low_nibble = 0x7;
    settings.auto_plugin = false;

    if (is_flydigi_logical_device(item) ||
        (item && item->usb_busid[0] && is_flydigi_usb_busid(item->usb_busid)) ||
        (item && strcmp(item->vid, "04b4") == 0 && strcmp(item->pid, "2412") == 0) ||
        (item && (contains_ci(item->name, "flydigi") || contains_ci(item->name, "vader") ||
                  contains_ci(item->name, "apex")))) {
        settings.composite_passthrough = true;
    }

    const char *kind = bridge_kind_for_item(item);
    if (strcmp(kind, "ds5") == 0) {
        settings.kind = TV_BRIDGE_KIND_DS5;
        settings.headset_volume_percent = 0x4d;
        settings.speaker_volume_percent = 0x41;
        settings.block_bt_audio_sink = true;
    } else if (strcmp(kind, "ds4") == 0) {
        settings.kind = TV_BRIDGE_KIND_DS4;
        settings.haptics_gain_centi = 0;
    }
    if (item) {
        settings.auto_plugin = hid_pt_prefs_auto_plugin_for_logical(item);
    }
    return settings;
}

ui_device_settings_t *ui_record_for_item(const logical_device_t *item)
{
    if (!item) return NULL;
    for (int i = 0; i < g_settings_count; ++i) {
        if (strcmp(g_settings[i].key, item->key) == 0) {
            return &g_settings[i];
        }
    }
    if (g_settings_count >= MAX_DEVICES) {
        return NULL;
    }
    snprintf(g_settings[g_settings_count].key, sizeof(g_settings[0].key), "%s", item->key);
    g_settings[g_settings_count].settings = default_settings_for_item(item);
    g_settings[g_settings_count].headset_volume_percent =
        g_settings[g_settings_count].settings.headset_volume_percent;
    g_settings[g_settings_count].speaker_volume_percent =
        g_settings[g_settings_count].settings.speaker_volume_percent;
    return &g_settings[g_settings_count++];
}

tv_bridge_worker_settings_t *settings_for_item(const logical_device_t *item)
{
    ui_device_settings_t *record = ui_record_for_item(item);
    return record ? &record->settings : NULL;
}

void apply_settings_to_session(const logical_device_t *item)
{
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) return;
    int session = session_index_for_key(item->key);
    if (session >= 0 && g_sessions[session].controller) {
        ctm_controller_set_settings(g_sessions[session].controller, settings);
    }
}

void hid_pt_sync_auto_plugin_pref(const logical_device_t *item)
{
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) {
        return;
    }
    char stable_id[96];
    hid_pt_stable_id_for_logical(item, stable_id, sizeof(stable_id));
    if (stable_id[0]) {
        hid_pt_prefs_set_auto_plugin(stable_id, settings->auto_plugin);
    }
}

int run_child_wait(char *const argv[])
{
    /* posix_spawn (clone/vfork semantics) instead of fork()+exec. fork() copies
     * the parent's page tables — cheap in the small standalone app, but expensive
     * and a recurring stall inside the large moonlight process (video buffers).
     * The stopSniff worker calls this every 500 ms, so the fork stall was starving
     * the bridge -> BT sniff mode (~115 Hz) + added latency. posix_spawn does not
     * copy the address space, so its cost is independent of process size. */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_RDWR, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_RDWR, 0);

    extern char **environ;
    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void stop_sniff_once(const char *mac)
{
    if (!valid_bt_address(mac)) {
        return;
    }
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"address\":\"%s\"}", mac);
    char *const argv[] = {
        "luna-send-pub",
        "-n",
        "1",
        "-f",
        "luna://com.webos.service.bluetooth2/device/internal/stopSniff",
        payload,
        NULL
    };
    (void)run_child_wait(argv);
}

void *stop_sniff_worker(void *arg)
{
    (void)arg;
    while (g_running) {
        char macs[MAX_DEVICES][64];
        int count = 0;
        pthread_mutex_lock(&g_bt_mac_mutex);
        count = g_bt_mac_count;
        if (count > MAX_DEVICES) count = MAX_DEVICES;
        for (int i = 0; i < count; ++i) {
            snprintf(macs[i], sizeof(macs[i]), "%s", g_bt_macs[i]);
        }
        pthread_mutex_unlock(&g_bt_mac_mutex);

        for (int i = 0; i < count; ++i) {
            stop_sniff_once(macs[i]);
        }
        usleep(500000);
    }
    return NULL;
}

void publish_bt_macs(void)
{
    char macs[MAX_DEVICES][64];
    int count = 0;
    for (int i = 0; i < g_devices.count && count < MAX_DEVICES; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        if (strcmp(bus_label(item->bus), "BT") != 0 || !valid_bt_address(item->mac)) {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(macs[j], item->mac) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            snprintf(macs[count++], sizeof(macs[0]), "%s", item->mac);
        }
    }

    pthread_mutex_lock(&g_bt_mac_mutex);
    g_bt_mac_count = count;
    for (int i = 0; i < count; ++i) {
        snprintf(g_bt_macs[i], sizeof(g_bt_macs[0]), "%s", macs[i]);
    }
    pthread_mutex_unlock(&g_bt_mac_mutex);
}

void ctm_bridge_set_agent_host(const char *host, int port)
{
    if (host && host[0]) {
        snprintf(g_agent_host, sizeof(g_agent_host), "%s", host);
    } else {
        g_agent_host[0] = '\0';
    }
    g_agent_port = port > 0 ? port : CTM_AGENT_PORT;
    g_agent_online = g_agent_host[0] != '\0';
}

bool discover_agent_once(void)
{
    if (g_agent_host[0]) {
        g_agent_online = true;
        return true;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 180000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CTM_AGENT_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char probe[] = "CTM_DISCOVER_V1";
    sendto(fd, probe, sizeof(probe) - 1, 0, (struct sockaddr *)&addr, sizeof(addr));

    char buf[256];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    if (!starts_with(buf, "CTM_AGENT_V1")) {
        return false;
    }
    const char *port = strstr(buf, "port=");
    g_agent_port = port ? atoi(port + 5) : CTM_AGENT_PORT;
    if (g_agent_port <= 0 || g_agent_port > 65535) {
        g_agent_port = CTM_AGENT_PORT;
    }
    const char *ip = inet_ntoa(from.sin_addr);
    snprintf(g_agent_host, sizeof(g_agent_host), "%s", ip ? ip : "");
    g_agent_online = g_agent_host[0] != '\0';
    return g_agent_online;
}

static bool agent_tcp_reachable(void)
{
    if (!g_agent_host[0] && !discover_agent_once()) {
        return false;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_agent_port);
    if (inet_aton(g_agent_host, &addr.sin_addr) == 0 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        g_agent_online = false;
        return false;
    }
    close(fd);
    g_agent_online = true;
    return true;
}

int send_agent_command(const char *command, char *response, size_t response_len)
{
    if (!g_agent_host[0] && !discover_agent_once()) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_agent_port);
    if (inet_aton(g_agent_host, &addr.sin_addr) == 0) {
        close(fd);
        g_agent_online = false;
        return -1;
    }
    /* Non-blocking connect with a bounded timeout: SO_SNDTIMEO does NOT cap the
     * TCP connect phase, so a host that silently drops SYNs (firewall) would hang
     * the caller for tens of seconds. These commands now also run on a stream-time
     * poll on the UI thread, so a stall must never happen. */
    int sock_flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, sock_flags | O_NONBLOCK);
    int crc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (crc != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            g_agent_online = false;
            return -1;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        /* The agent is the stream host we're already connected to — a healthy LAN
         * connect completes in a few ms. Keep this short so a host that black-holes
         * SYNs can't stall the UI thread (these run on the auto-plug poll). */
        struct timeval ctv;
        ctv.tv_sec = 0;
        ctv.tv_usec = 300000;
        if (select(fd + 1, NULL, &wfds, NULL, &ctv) <= 0) {
            close(fd);
            g_agent_online = false;
            return -1;
        }
        int soerr = 0;
        socklen_t soerr_len = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) != 0 || soerr != 0) {
            close(fd);
            g_agent_online = false;
            return -1;
        }
    }
    fcntl(fd, F_SETFL, sock_flags);   /* restore blocking; send/recv bounded by SO_*TIMEO */

    char line[512];
    snprintf(line, sizeof(line), "%s\n", command);
    if (send(fd, line, strlen(line), 0) < 0) {
        close(fd);
        return -1;
    }
    ssize_t n = recv(fd, response, response_len > 0 ? response_len - 1 : 0, 0);
    close(fd);
    if (response_len > 0) {
        response[n > 0 ? n : 0] = '\0';
    }
    return n > 0 && response && starts_with(response, "OK") ? 0 : -1;
}

int session_index_for_key(const char *key)
{
    for (int i = 0; i < g_session_count; ++i) {
        if (strcmp(g_sessions[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

int next_bridge_port(void)
{
    int port = CTM_BRIDGE_BASE_PORT;
    for (;;) {
        bool used = false;
        for (int i = 0; i < g_session_count; ++i) {
            if (g_sessions[i].port == port) {
                used = true;
                break;
            }
        }
        if (!used) {
            return port;
        }
        ++port;
    }
}

int first_scan_index_for_item(const logical_device_t *item)
{
    if (!item || item->device_count <= 0) return -1;
    int index = item->device_indices[0];
    return index >= 0 && index < g_scan.count ? index : -1;
}

void make_bridge_busid(const logical_device_t *item, char *out, size_t out_len)
{
    static unsigned seq;
    const char *kind = bridge_kind_for_item(item);
    snprintf(out, out_len, "ctm-%s-%u", kind, ++seq);
}

bool add_session(const char *key, const char *busid, ctm_controller_t *controller, int port)
{
    int index = session_index_for_key(key);
    if (index >= 0) {
        if (g_sessions[index].controller && g_sessions[index].controller != controller) {
            ctm_controller_plug_out(g_sessions[index].controller);
            ctm_controller_destroy(g_sessions[index].controller);
        }
        snprintf(g_sessions[index].busid, sizeof(g_sessions[index].busid), "%s", busid ? busid : "");
        g_sessions[index].port = port;
        g_sessions[index].controller = controller;
        return true;
    }
    if (g_session_count >= MAX_SESSIONS) {
        return false;
    }
    snprintf(g_sessions[g_session_count].key, sizeof(g_sessions[0].key), "%s", key);
    snprintf(g_sessions[g_session_count].busid, sizeof(g_sessions[0].busid), "%s", busid ? busid : "");
    g_sessions[g_session_count].port = port;
    g_sessions[g_session_count].controller = controller;
    g_session_count++;
    return true;
}

void stop_session(const char *key)
{
    int index = session_index_for_key(key);
    if (index < 0) {
        return;
    }
    if (g_sessions[index].controller) {
        ctm_controller_plug_out(g_sessions[index].controller);
        ctm_controller_destroy(g_sessions[index].controller);
        g_sessions[index].controller = NULL;
    }
    /* Tell the host to drop the bridge — but skip the (blocking) round-trip when
     * the agent is already known unreachable: it would only time out, and the
     * first failure flips g_agent_online, so a mass-disconnect against a dead host
     * costs at most one connect timeout on the UI thread (not one per device). The
     * host GCs orphan virtual controllers when it comes back anyway. */
    if (g_sessions[index].port > 0 && g_agent_online) {
        char cmd[160];
        char response[256];
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", g_sessions[index].busid);
        (void)send_agent_command(cmd, response, sizeof(response));
    }
    memmove(&g_sessions[index], &g_sessions[index + 1],
            (size_t)(g_session_count - index - 1) * sizeof(g_sessions[0]));
    g_session_count--;
}

void release_local_sessions_on_exit(void)
{
    for (int i = 0; i < g_session_count; ++i) {
        if (g_sessions[i].controller) {
            ctm_controller_plug_out(g_sessions[i].controller);
            ctm_controller_destroy(g_sessions[i].controller);
            g_sessions[i].controller = NULL;
        }
    }
    g_session_count = 0;
}

const char *bridge_kind_for_item(const logical_device_t *item)
{
    if (!item) return "hid";
    if (strcmp(item->vid, "054c") == 0 && strcmp(item->pid, "0ce6") == 0) return "ds5";
    if (strcmp(item->vid, "054c") == 0 &&
        (strcmp(item->pid, "09cc") == 0 || strcmp(item->pid, "05c4") == 0)) return "ds4";
    if (strcmp(item->vid, "045e") == 0 &&
        (is_xbox_pid(item->pid) || contains_ci(item->name, "xbox"))) return "xbox";
    if (strcmp(item->vid, "28de") == 0 && strcmp(item->pid, "1304") == 0) return "puck";
    if (is_flydigi_logical_device(item) ||
        (item->usb_busid[0] && is_flydigi_usb_busid(item->usb_busid)) ||
        (strcmp(item->vid, "04b4") == 0 && strcmp(item->pid, "2412") == 0) ||
        contains_ci(item->name, "flydigi") || contains_ci(item->name, "vader") ||
        contains_ci(item->name, "apex")) {
        for (int i = 0; i < g_settings_count; ++i) {
            if (strcmp(g_settings[i].key, item->key) == 0) {
                return g_settings[i].settings.composite_passthrough ? "flydigi" : "hid";
            }
        }
        return "flydigi";
    }
    return "hid";
}

/* Session key for ONE specific hidraw node of a logical device, so each
 * interface can be plugged/unplugged independently. When: per-node plug + its
 * button state. */
void node_session_key(const logical_device_t *item, int scan_index, char *out, size_t out_len)
{
    const char *tag = "node";
    if (scan_index >= 0 && scan_index < g_scan.count && g_scan.devices[scan_index].hidraw[0]) {
        tag = g_scan.devices[scan_index].hidraw;
    }
    snprintf(out, out_len, "%s#%s", item ? item->key : "", tag);
}

/* Core plug: start the host bridge, build a controller from one chosen scan
 * node, plug it in, and register the session under session_key. Shared by the
 * whole-device plug (plug_in_item) and the per-interface plug (plug_in_node).
 * When: any Plug button. */
/* Serialize cached composite enumeration into CTMB_MSG_ENUM payload. */
static void enum_lookup_key_for_item(const logical_device_t *item, char *out, size_t out_len)
{
    if (item && item->usb_busid[0]) {
        snprintf(out, out_len, "%s", item->usb_busid);
        return;
    }
    if (item && strcmp(item->vid, "28de") == 0 && strcmp(item->pid, "1304") == 0) {
        composite_enum_capture(NULL, item->vid, item->pid);
        for (int i = 0; i < g_composite_enum_count; ++i) {
            if (g_composite_enums[i].valid) {
                snprintf(out, out_len, "%s", g_composite_enums[i].key);
                return;
            }
        }
    }
    out[0] = '\0';
}

static bool plug_in_scan_index(logical_device_t *item, int scan_index, const char *session_key)
{
    if (!item || scan_index < 0 || scan_index >= g_scan.count) {
        ctm_set_plug_error("Invalid device selection");
        return false;
    }
    if (!g_agent_host[0] && !discover_agent_once()) {
        ctm_set_plug_error("Windows agent not found");
        return false;
    }
    if (!agent_tcp_reachable()) {
        ctm_set_plug_error("Cannot reach agent at %s:%d", g_agent_host, g_agent_port);
        return false;
    }

    const device_info_t *dev = &g_scan.devices[scan_index];
    char response[512];
    int port = next_bridge_port();
    char cmd[256];
    char busid[32];
    const char *kind = bridge_kind_for_item(item);
    make_bridge_busid(item, busid, sizeof(busid));
    if (strcmp(kind, "flydigi") == 0) {
        char busid_key[64] = {0};
        if (item->usb_busid[0]) {
            snprintf(busid_key, sizeof(busid_key), "%s", item->usb_busid);
        } else if (starts_with(item->key, "flydigi:")) {
            snprintf(busid_key, sizeof(busid_key), "%s", item->key + strlen("flydigi:"));
        }
        if (busid_key[0]) {
            composite_enum_capture(busid_key, item->vid, item->pid);
        }
    }
    snprintf(cmd, sizeof(cmd), "BRIDGE_START %s %d %s", kind, port, busid);
    if (send_agent_command(cmd, response, sizeof(response)) != 0) {
        ctm_set_plug_error("Agent bridge start failed: %s", response[0] ? response : "no response");
        return false;
    }

    /* Build the neutral descriptor and hand the device to a controller — one
     * mechanism for DS5/DS4/xbox/puck/generic (factory picks the ops). */
    ctm_controller_dev_t cdev;
    memset(&cdev, 0, sizeof(cdev));
    snprintf(cdev.vid, sizeof(cdev.vid), "%s", item->vid);
    snprintf(cdev.pid, sizeof(cdev.pid), "%s", item->pid);
    snprintf(cdev.bus, sizeof(cdev.bus), "%s", bus_label(item->bus));
    snprintf(cdev.name, sizeof(cdev.name), "%s", item->name);
    if (is_flydigi_logical_device(item)) {
        if (flydigi_is_xinput_evdev_only(item)) {
            if (flydigi_xpad_evdev_path_for_item(item, cdev.path, sizeof(cdev.path)) != 0) {
                ctm_set_plug_error("Flydigi XInput: xpad evdev not found (busid=%s)",
                                   item->usb_busid[0] ? item->usb_busid : "-");
                snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
                (void)send_agent_command(cmd, response, sizeof(response));
                return false;
            }
        } else if (flydigi_handshake_hidraw_path_for_item(item, cdev.path, sizeof(cdev.path)) != 0) {
            cdev.path[0] = '\0';
        }
        if (!flydigi_has_pluggable_path(item)) {
            ctm_set_plug_error("Flydigi: no handshake hidraw or xpad path (busid=%s)",
                               item->usb_busid[0] ? item->usb_busid : "-");
            snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
            (void)send_agent_command(cmd, response, sizeof(response));
            return false;
        }
        if (flydigi_is_xinput_mode(item)) {
            char xpad_path[64];
            if (flydigi_xpad_evdev_path_for_item(item, xpad_path, sizeof(xpad_path)) != 0) {
                ctm_set_plug_error("Flydigi XInput: xpad evdev not found (busid=%s)",
                                   item->usb_busid[0] ? item->usb_busid : "-");
                snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
                (void)send_agent_command(cmd, response, sizeof(response));
                return false;
            }
            if (cdev.path[0] == '\0') {
                snprintf(cdev.path, sizeof(cdev.path), "%s", xpad_path);
            }
        } else if (cdev.path[0] == '\0') {
            ctm_set_plug_error("Flydigi D-Input: no hidraw path (busid=%s)",
                               item->usb_busid[0] ? item->usb_busid : "-");
            snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
            (void)send_agent_command(cmd, response, sizeof(response));
            return false;
        }
    } else {
        snprintf(cdev.path, sizeof(cdev.path), "%s", dev->node);
    }
    snprintf(cdev.mac, sizeof(cdev.mac), "%s", item->mac);
    snprintf(cdev.usb_busid, sizeof(cdev.usb_busid), "%s", item->usb_busid);

    ctm_controller_t *controller = ctm_controller_create(&cdev);
    if (!controller) {
        ctm_set_plug_error("Controller create failed");
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (settings) {
        ctm_controller_set_settings(controller, settings);
    }
    /* Composite: forward cached USB enumeration so the host builds the device. */
    if (strcmp(kind, "puck") == 0 || strcmp(kind, "flydigi") == 0) {
        char enum_key[64];
        enum_lookup_key_for_item(item, enum_key, sizeof(enum_key));
        int elen = 0;
        uint8_t *epl = build_composite_enum_payload(enum_key, &elen);
        if (epl) {
            ctm_controller_set_enum_payload(controller, epl, elen);
            free(epl);
            log_append("forwarding composite enumeration to host (%d bytes, key=%s)", elen, enum_key);
        } else {
            ctm_set_plug_error("Composite enum missing for %s (tap Refresh)", enum_key);
            ctm_controller_destroy(controller);
            snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
            (void)send_agent_command(cmd, response, sizeof(response));
            return false;
        }
    }
    if (ctm_controller_plug_in(controller, g_agent_host, port) != 0) {
        ctm_set_plug_error("Controller plug-in failed");
        ctm_controller_destroy(controller);
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    add_session(session_key, busid, controller, port);
    ctm_clear_plug_error();
    log_append("controller started kind=%s node=%s busid=%s host=%s port=%d",
               kind, cdev.path, busid, g_agent_host, port);
    return true;
}

/* Plug the whole device using its first hidraw node (the default). When: the
 * logical row's Plug button. */
bool plug_in_item(logical_device_t *item)
{
    if (!item) {
        ctm_set_plug_error("No device selected");
        return false;
    }
    int scan_index = flydigi_xpad_scan_index_for_item(item);
    if (scan_index < 0) {
        scan_index = best_scan_index_for_item(item);
    }
    if (scan_index < 0) {
        scan_index = first_scan_index_for_item(item);
    }
    if (scan_index < 0) {
        ctm_set_plug_error("No device node for %s", item->name);
        return false;
    }
    if (is_flydigi_logical_device(item) && !flydigi_has_pluggable_path(item)) {
        ctm_set_plug_error("Flydigi: no hidraw/xpad for %s", item->name);
        return false;
    }
    const device_info_t *dev = &g_scan.devices[scan_index];
    if (!is_flydigi_logical_device(item) && !dev->hidraw[0]) {
        ctm_set_plug_error("No hidraw node for %s", item->name);
        return false;
    }
    return plug_in_scan_index(item, scan_index, item->key);
}

/* Plug ONE chosen hidraw interface (keyed per node, independent of the whole-
 * device plug). When: a sub-row Plug button — pick exactly which interface of a
 * composite device to bridge. */
bool plug_in_node(logical_device_t *item, int scan_index)
{
    if (!item || scan_index < 0 || scan_index >= g_scan.count) {
        return false;
    }
    char key[96];
    node_session_key(item, scan_index, key, sizeof(key));
    return plug_in_scan_index(item, scan_index, key);
}

/* ---- auto-plug reconcile ------------------------------------------------- */

#define AUTOPLUG_MAX_FAILS 3
/* After a failed plug attempt (a blocking agent round-trip that just stalled the
 * UI thread), skip plug attempts for this many polls so an unreachable/slow host
 * can't stall the main loop every tick. */
#define AUTOPLUG_FAIL_COOLDOWN_POLLS 4
static int g_autoplug_plug_cooldown;

static autoplug_entry_t *autoplug_entry_for(const char *key, bool create)
{
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < g_autoplug_count; ++i) {
        if (strcmp(g_autoplug[i].key, key) == 0) {
            return &g_autoplug[i];
        }
    }
    if (!create || g_autoplug_count >= MAX_DEVICES) return NULL;
    autoplug_entry_t *e = &g_autoplug[g_autoplug_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->state = AUTOPLUG_PENDING;
    return e;
}

void autoplug_mark_done(const char *key)
{
    autoplug_entry_t *e = autoplug_entry_for(key, true);
    if (e) {
        e->state = AUTOPLUG_DONE;
        e->fail_count = 0;
    }
}

void autoplug_mark_pending(const char *key)
{
    autoplug_entry_t *e = autoplug_entry_for(key, true);
    if (e) {
        e->state = AUTOPLUG_PENDING;
        e->fail_count = 0;
    }
}

void autoplug_reset(void)
{
    g_autoplug_count = 0;
    g_autoplug_plug_cooldown = 0;
}

/* A device's "plugged" flag is persisted only in g_plugged_keys, which is NOT
 * pruned when the device physically disappears. Without this, a controller that
 * sleeps / disconnects and reconnects mid-stream would come back wearing a stale
 * "plugged" flag and the reconcile would never re-bridge it. Reap any plugged key
 * whose device is gone: tear down the now-dead local session (stop_session also
 * BRIDGE_STOPs the host so it doesn't accumulate orphan virtual controllers) and
 * clear the flag, so the device is auto-plugged afresh when it returns. */
static void autoplug_reap_vanished(void)
{
    for (int i = 0; i < g_plugged_key_count;) {
        bool present = false;
        for (int j = 0; j < g_devices.count; ++j) {
            if (strcmp(g_devices.items[j].key, g_plugged_keys[i]) == 0) {
                present = true;
                break;
            }
        }
        if (present) {
            ++i;
            continue;
        }
        /* Absent. Copy the key first: stop_session/set_plug_key mutate g_sessions /
         * g_plugged_keys (the latter shrinks via memmove), so do not advance i. */
        char key[96];
        snprintf(key, sizeof(key), "%s", g_plugged_keys[i]);
        stop_session(key);
        set_plug_key(key, false);
    }
}

/* Drop bookkeeping for devices no longer present so a controller that
 * disconnects and reconnects (e.g. a DualSense re-paired over Bluetooth) is
 * auto-plugged afresh, and a previously "given up" device gets a clean retry. */
static void autoplug_prune(void)
{
    for (int i = 0; i < g_autoplug_count;) {
        bool present = false;
        for (int j = 0; j < g_devices.count; ++j) {
            if (strcmp(g_devices.items[j].key, g_autoplug[i].key) == 0) {
                present = true;
                break;
            }
        }
        if (present) {
            ++i;
        } else {
            memmove(&g_autoplug[i], &g_autoplug[i + 1],
                    (size_t)(g_autoplug_count - i - 1) * sizeof(g_autoplug[0]));
            g_autoplug_count--;
        }
    }
}

/* Defensive secondary gate for AUTO-plug ONLY (manual plug stays unrestricted):
 * never auto-bridge an interface that identifies itself as a keyboard / keypad /
 * mouse / pointer / consumer-control, even if it somehow carried a controller
 * VID/PID. A DualSense's gamepad interface reports Generic-Desktop/Gamepad, so
 * this never blocks it; an undetermined usage (0) falls through to the VID/PID
 * gate. This is what keeps e.g. a Bluetooth keyboard or media remote from ever
 * being passed to the host. */
static bool usage_is_input_peripheral(uint16_t up, uint16_t us)
{
    if (up == 0x0c) return true;            /* Consumer Control (media keys/remote) */
    if (up == 0x01) {                       /* Generic Desktop */
        return us == 0x01 ||                /*   Pointer   */
               us == 0x02 ||                /*   Mouse     */
               us == 0x06 ||                /*   Keyboard  */
               us == 0x07;                  /*   Keypad    */
    }
    return false;
}

static bool autoplug_node_is_peripheral(const logical_device_t *item)
{
    int idx = first_scan_index_for_item(item);
    if (idx < 0 || idx >= g_scan.count) {
        return false;
    }
    return usage_is_input_peripheral(g_scan.devices[idx].usage_page,
                                     g_scan.devices[idx].usage);
}

void hid_pt_autoplug_reconcile(stream_input_t *input)
{
    autoplug_reap_vanished();   /* drop stale plugged state for devices that left */
    autoplug_prune();           /* drop bookkeeping for devices that left */

    /* Backing off after a recent plug failure: don't issue any (blocking) agent
     * round-trips this tick, but still record manually/already-plugged devices so a
     * deliberate plug-out keeps being respected. */
    if (g_autoplug_plug_cooldown > 0) {
        g_autoplug_plug_cooldown--;
        for (int i = 0; i < g_devices.count; ++i) {
            if (g_devices.items[i].plugged) {
                autoplug_mark_done(g_devices.items[i].key);
            }
        }
        return;
    }

    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        if (item->plugged) {
            /* Already bridged (by us or manually): remember it so a later manual
             * plug-out is respected instead of being re-plugged on the next poll. */
            autoplug_mark_done(item->key);
            continue;
        }
        tv_bridge_worker_settings_t *settings = settings_for_item(item);
        if (!settings || !settings->auto_plugin) {
            continue;
        }
        const char *kind = bridge_kind_for_item(item);
        if (g_xbox_suppress_ds5 && strcmp(kind, "ds5") == 0) {
            continue;   /* forced-Xbox mode: DS5 is intentionally released to SDL, never re-grab */
        }
        if (strcmp(kind, "hid") == 0) {
            continue;   /* only known controllers (ds5/ds4/xbox/puck), never generic HID */
        }
        /* The puck is a composite device whose first interface may be keyboard/
         * mouse; its dedicated handling picks the gamepad interface, so don't apply
         * the peripheral guard to it. For the simple pads, refuse anything that
         * presents as a keyboard/mouse/consumer-control interface. */
        if (strcmp(kind, "puck") != 0 && autoplug_node_is_peripheral(item)) {
            continue;
        }
        autoplug_entry_t *e = autoplug_entry_for(item->key, true);
        if (!e || e->state != AUTOPLUG_PENDING) {
            continue;   /* DONE (user-managed) or GIVEUP */
        }
        if (plug_in_item(item)) {
            item->plugged = true;
            set_plug_key(item->key, true);
            e->state = AUTOPLUG_DONE;
            e->fail_count = 0;
            if (input) {
                hid_pt_moonlight_exclude(input, item);
            }
            log_append("auto-plugged %s (%s)", item->name, kind);
            /* success is cheap; keep going so a second controller plugs the same tick */
        } else {
            if (++e->fail_count >= AUTOPLUG_MAX_FAILS) {
                e->state = AUTOPLUG_GIVEUP;
                log_append("auto-plug: giving up on %s after %d attempts", item->name, e->fail_count);
            }
            /* A failure means a blocking agent round-trip just stalled the UI thread.
             * Stop after the first failure this tick and back off, so an unreachable
             * or slow host can't freeze the main loop on every poll. */
            g_autoplug_plug_cooldown = AUTOPLUG_FAIL_COOLDOWN_POLLS;
            break;
        }
    }
}

