#ifndef CTM_STATE_H
#define CTM_STATE_H

/* Headless shared state, types and core API for the CTM bridge (no LVGL / SDL).
 * Split out of ui_common.h so the enumeration/classification (ui_devices.c), the
 * agent/session/plug logic (ui_bridge.c) and the helpers below can compile and
 * link into a host app (e.g. moonlight-tv) that owns its own LVGL/SDL. The UI
 * half (display, widgets, per-controller detail windows) stays in ui_common.h,
 * which includes this header. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif

#include "ctm_controller.h"   /* controller lifecycle + ctm_settings types */

struct stream_input_t;

/* ---- HID ioctl fallbacks (sysroot may predate these) -------------------- */
#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo {
    unsigned int bustype;
    short vendor;
    short product;
};
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif

#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif

#ifndef HIDIOCGRAWPHYS
#define HIDIOCGRAWPHYS(len) _IOC(_IOC_READ, 'H', 0x05, len)
#endif

#ifndef HIDIOCGRAWUNIQ
#define HIDIOCGRAWUNIQ(len) _IOC(_IOC_READ, 'H', 0x08, len)
#endif

#ifndef HIDIOCGRDESCSIZE
#define HIDIOCGRDESCSIZE _IOR('H', 0x01, int)
#endif

#define MAX_DEVICES 64
#define MAX_SESSIONS 32
#define TEXT_LEN 256
#define LOG_LEN 8192
#define CTM_AGENT_PORT 48054
#define CTM_BRIDGE_BASE_PORT 48055

typedef struct {
    char hidraw[32];
    char node[64];
    char name[TEXT_LEN];
    char phys[TEXT_LEN];
    char bus[16];
    char vid[16];
    char pid[16];
    char version[16];
    char mac[64];
    char usb_busid[64];
    char inputs[TEXT_LEN];
    char events[TEXT_LEN];
    int report_descriptor_bytes;
    uint16_t usage_page;     /* top-level HID usage page (interface class) */
    uint16_t usage;          /* top-level HID usage */
    char iface[20];          /* human label, e.g. "vendor 64B" / "keyboard 8B" */
    bool readable;
    bool writable;
} device_info_t;

typedef struct {
    device_info_t devices[MAX_DEVICES];
    int count;
    int input_count;
} scan_result_t;

typedef struct {
    char key[96];
    char name[TEXT_LEN];
    char bus[16];
    char vid[16];
    char pid[16];
    char mac[64];
    char usb_busid[64];
    int device_indices[MAX_DEVICES];
    int device_count;
    bool plugged;
    int8_t moonlight_gs_id; /* cached Moonlight slot after match; -1 = unknown */
} logical_device_t;

typedef struct {
    logical_device_t items[MAX_DEVICES];
    int count;
    /* Incremented by every build_logical_devices() rebuild. items[] is rebuilt
     * from scratch in readdir order and then compacted, so an index taken at
     * generation N addresses a different device at generation N+1. Anything
     * that outlives one rebuild (an LVGL widget's user data, a remembered
     * selection) must carry the key and re-resolve it, or carry the generation
     * it was stamped at and check it. */
    uint32_t generation;
} logical_result_t;

typedef struct {
    char key[96];
    char busid[32];
    int port;
    ctm_controller_t *controller;   /* owns the in-process bridging session */
    uint64_t plug_ms;               /* CLOCK_MONOTONIC ms at plug: lets the dead-
                                     * session reconcile tell an instant death
                                     * from a session that ran for a while */
    int8_t moonlight_gs_id;         /* Moonlight slot excluded for this bridge;
                                     * -1 = none. Survives the device leaving
                                     * g_devices, so the teardown paths can still
                                     * give the slot back. */
} bridge_session_t;

typedef struct {
    char key[96];
    tv_bridge_worker_settings_t settings;
    unsigned int headset_volume_percent;
    unsigned int speaker_volume_percent;
} ui_device_settings_t;

/* One USB interface of a composite device (from the device-dir sysfs walk). */
typedef struct { int num; char cls[8]; char node[64]; char dir[32]; } composite_if_t;

/* Cached composite USB enumeration — captured once per usb_busid and forwarded
 * verbatim to Windows at bridge plug (CTMB_MSG_ENUM). */
#define COMPOSITE_ENUM_MAX_IF    12
#define COMPOSITE_ENUM_MAX_DESC  4096
#define COMPOSITE_ENUM_MAX_RDESC 1024
#define COMPOSITE_ENUM_MAX_CACHE 8

typedef struct {
    int num;
    char cls[8];
    char node[64];
    int rdesc_len;
    uint8_t rdesc[COMPOSITE_ENUM_MAX_RDESC];
} composite_enum_if_t;

/* Lifetime bookkeeping shared by every cache keyed on a USB PORT PATH.
 *
 * A usb_busid ("1-1") names a port, not a device: whatever is plugged in there
 * next inherits the key. An entry therefore records the identity of the device
 * it was captured from and is re-checked against sysfs; a mismatch is a miss.
 * `identity` 0 means "sysfs could not say", which compares equal to nothing, so
 * an unreadable or vanished device dir always misses. That direction is the
 * cheap one — a miss costs a re-capture, a wrong hit ships one device's USB
 * descriptors to the host as another's.
 *
 * Entries live in fixed tables whose first member must be this struct; the
 * helpers in ui_devices.c enforce that with a _Static_assert rather than a
 * comment. */
typedef struct {
    int valid;
    char key[64];                 /* usb_busid, or the usbdir basename when the
                                   * bus id could not be resolved */
    char usbdir[256];             /* sysfs device dir the entry was taken from */
    uint64_t identity;            /* idVendor:idProduct:serial hash; 0 = unknown */
    uint32_t checked_generation;  /* g_devices.generation of the last re-check */
    uint32_t insert_seq;          /* claim order, for oldest-first eviction */
} hid_pt_port_cache_t;

uint64_t hid_pt_usb_identity_hash(const char *usbdir);

typedef struct {
    hid_pt_port_cache_t hdr;
    char serial[64];
    int descriptors_len;
    uint8_t descriptors[COMPOSITE_ENUM_MAX_DESC];
    int if_count;
    composite_enum_if_t ifs[COMPOSITE_ENUM_MAX_IF];
    uint8_t full_speed;                        /* 1 = USB full-speed (12 Mbps) */
} composite_enum_t;

/* Slots are claimed anywhere in the table (a claim may evict), so there is no
 * "count": iterate the whole array and test hdr.valid. */
extern composite_enum_t g_composite_enums[COMPOSITE_ENUM_MAX_CACHE];

/* ---- headless global state (defined in ctm_state.c) --------------------- */
extern scan_result_t g_scan;
extern logical_result_t g_devices;
extern int g_selected_index;
extern char g_selected_key[96];
extern char g_plugged_keys[MAX_DEVICES][96];
extern int g_plugged_key_count;
extern char g_expanded_keys[MAX_DEVICES][96];
extern int g_expanded_key_count;
extern bridge_session_t g_sessions[MAX_SESSIONS];
extern int g_session_count;
extern ui_device_settings_t g_settings[MAX_DEVICES];
extern int g_settings_count;
extern char g_agent_host[64];
extern int g_agent_port;
extern bool g_agent_online;
extern bool g_running;
extern pthread_t g_stop_sniff_thread;
extern bool g_stop_sniff_thread_started;
extern pthread_mutex_t g_bt_mac_mutex;
extern char g_bt_macs[MAX_DEVICES][64];
extern int g_bt_mac_count;

/* ---- auto-plug reconcile state (ui_bridge.c) ----------------------------- */
typedef enum {
    AUTOPLUG_PENDING = 0,   /* recognized controller, not yet bridged: eligible */
    AUTOPLUG_DONE,          /* bridged by us, or the user took manual control */
    AUTOPLUG_GIVEUP         /* repeated plug failures: stop retrying until reconnect */
} autoplug_state_t;

typedef struct {
    char key[96];
    autoplug_state_t state;
    int fail_count;
} autoplug_entry_t;

extern autoplug_entry_t g_autoplug[MAX_DEVICES];
extern int g_autoplug_count;

extern char g_log[LOG_LEN];
extern size_t g_log_used;
extern pthread_mutex_t g_log_mutex;   /* guards g_log + g_log_dirty */
extern bool g_log_dirty;              /* set by log_append, cleared by ctm_ui_log_flush */

void ctm_set_plug_error(const char *fmt, ...);
void ctm_clear_plug_error(void);
const char *ctm_last_plug_error(void);

/* ---- ctm_state.c: log + generic string/file helpers ---- */
void log_append(const char *fmt, ...);
int count_dir_entries(const char *path);
int read_text_file(const char *path, char *out, size_t out_len);
void join_path(char *out, size_t out_len, const char *a, const char *b);
bool starts_with(const char *text, const char *prefix);
char ascii_lower(char c);
bool contains_ci(const char *text, const char *needle);
bool valid_bt_address(const char *s);
void append_unique(char *dst, size_t dst_len, const char *value);

/* ---- ui_devices.c: enumeration + classification + logical model ---- */
device_info_t *find_or_add_device(scan_result_t *result, const char *hidraw);
device_info_t *find_or_add_input_device(scan_result_t *result, const char *input_name,
                                        const char *usb_busid);
void usb_busid_from_input_path(const char *input_path, char *out, size_t out_len);
void inspect_hidraw(device_info_t *dev);
const char *bus_label(const char *bus);
bool is_steam_puck_device(const device_info_t *dev);
bool is_flydigi_composite_device(const device_info_t *dev);
bool is_flydigi_logical_device(const logical_device_t *item);
bool is_flydigi_usb_busid(const char *usb_busid);
int read_usb_identity_attrs(const char *usb_busid, char *mfg, size_t mfg_len,
                            char *prod, size_t prod_len);
bool device_should_list_in_ui(const device_info_t *dev);
bool is_xpad_only_scan_device(const device_info_t *dev);
void usb_busid_from_hidraw_name(const char *hidraw, char *out, size_t out_len);
int flydigi_hidraw_path_for_busid(const char *usb_busid, char *out, size_t out_len);
int flydigi_hidraw_path_for_item(const logical_device_t *item, char *out, size_t out_len);
int flydigi_handshake_hidraw_path_for_busid(const char *usb_busid, char *out, size_t out_len);
int flydigi_handshake_hidraw_path_for_item(const logical_device_t *item, char *out, size_t out_len);
int flydigi_xpad_evdev_path_for_busid(const char *usb_busid, char *out, size_t out_len);
int flydigi_xpad_evdev_path_for_item(const logical_device_t *item, char *out, size_t out_len);
bool flydigi_is_xinput_mode(const logical_device_t *item);
bool flydigi_is_xinput_mode_for_busid(const char *usb_busid);
bool flydigi_is_xinput_evdev_only(const logical_device_t *item);
bool flydigi_is_xinput_evdev_only_for_busid(const char *usb_busid);
bool flydigi_has_hidraw_for_busid(const char *usb_busid);
int flydigi_xpad_scan_index_for_item(const logical_device_t *item);
bool flydigi_has_pluggable_path(const logical_device_t *item);
extern const uint8_t flydigi_xbox360_wired_rdesc[];
extern const unsigned flydigi_xbox360_wired_rdesc_len;
void finalize_logical_devices(logical_result_t *logical);
bool is_ds5_device(const device_info_t *dev);
bool is_ds4_device(const device_info_t *dev);
bool is_xbox_pid(const char *pid);
bool is_xbox_device(const device_info_t *dev);
bool is_xpad_compatible_pid(const char *vid, const char *pid);
bool is_gulikit_named_device(const char *name);
bool is_xpad_input_only_candidate(const char *bus, const char *vid, const char *pid,
                                  const char *name, const char *usb_busid);
void steam_root_from_phys(const char *phys, char *out, size_t out_len);
void logical_key_for_device(const device_info_t *dev, char *out, size_t out_len);
void logical_name_for_device(const device_info_t *dev, char *out, size_t out_len);
bool plug_key_is_set(const char *key);
void set_plug_key(const char *key, bool plugged);
bool expand_key_is_set(const char *key);
void set_expand_key(const char *key, bool expanded);
bool logical_device_can_expand(const logical_device_t *item);
logical_device_t *find_or_add_logical_device(logical_result_t *logical,
                                             const device_info_t *dev, int scan_index);
/* Resolve a logical device by key against the current g_devices; NULL if it is
 * no longer there. Use this instead of holding an index across a rebuild. */
logical_device_t *logical_device_by_key(const char *key);
void build_logical_devices(const scan_result_t *scan, logical_result_t *logical);
void enumerate_devices(scan_result_t *result);
/* Resolve the puck's USB device dir (e.g. .../usb5/5-1) via /sys/class/input ->
 * /sys/devices (host controller derived, never hardcoded). Returns 0 on success.
 * Enumerate its interfaces (sorted by interface number) into out[]. Shared by the
 * expanded list (controller/service slots) and the detail panel. */
int puck_usb_device_dir(const char *vid, const char *pid, char *out, size_t out_len);
int composite_usb_device_dir_by_busid(const char *usb_busid, char *out, size_t out_len);
int composite_enumerate_ifaces(const char *usbdir, composite_if_t *out, int max);
int composite_enum_capture(const char *usb_busid, const char *vid, const char *pid);
int puck_enum_capture(const char *vid, const char *pid);
const composite_enum_t *composite_enum_lookup(const char *key);
uint8_t *build_composite_enum_payload(const char *key, int *out_len);
int best_scan_index_for_item(const logical_device_t *item);

/* ---- ui_bridge.c: settings store + agent client + sessions + plug ---- */
tv_bridge_worker_settings_t default_settings_for_item(const logical_device_t *item);
ui_device_settings_t *ui_record_for_item(const logical_device_t *item);
tv_bridge_worker_settings_t *settings_for_item(const logical_device_t *item);
void apply_settings_to_session(const logical_device_t *item);
void hid_pt_sync_auto_plugin_pref(const logical_device_t *item);
int run_child_wait(char *const argv[]);
void stop_sniff_once(const char *mac);
void *stop_sniff_worker(void *arg);
void publish_bt_macs(void);
bool discover_agent_once(void);
int send_agent_command(const char *command, char *response, size_t response_len);
int session_index_for_key(const char *key);
int next_bridge_port(void);
int first_scan_index_for_item(const logical_device_t *item);
void make_bridge_busid(const logical_device_t *item, char *out, size_t out_len);
bool add_session(const char *key, const char *busid, ctm_controller_t *controller, int port);
void stop_session(const char *key);
void release_local_sessions_on_exit(void);
const char *bridge_kind_for_item(const logical_device_t *item);
bool plug_in_item(logical_device_t *item);
bool plug_in_node(logical_device_t *item, int scan_index);   /* bridge ONE chosen hidraw */

/* Auto-plug reconcile: bridge every connected, recognized controller that is not
 * already plugged and is still auto-managed. Idempotent; cheap when nothing new. */
void hid_pt_autoplug_reconcile(struct stream_input_t *input);
/* Mark a device key as user-/manager-owned so the reconcile leaves it alone (e.g.
 * after a manual plug-out). Cleared when the device disconnects. */
void autoplug_mark_done(const char *key);
/* Re-arm a device key for auto-plug (PENDING + fail_count reset) so the reconcile re-bridges it
 * via its proven retry path — e.g. when returning from forced-Xbox mode to native CTM. */
void autoplug_mark_pending(const char *key);
/* Forget all auto-plug bookkeeping (called when the passthrough session stops). */
void autoplug_reset(void);
void node_session_key(const logical_device_t *item, int scan_index, char *out, size_t out_len);

/* Host app (moonlight-tv): pin the Windows CTM agent to the streaming PC IP. */
void ctm_bridge_set_agent_host(const char *host, int port);

#endif /* CTM_STATE_H */
