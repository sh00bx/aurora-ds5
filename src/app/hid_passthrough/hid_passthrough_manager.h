#ifndef HID_PASSTHROUGH_MANAGER_H
#define HID_PASSTHROUGH_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#if defined(TARGET_WEBOS)

#define HID_PT_PATH_LEN 64
#define HID_PT_DEFAULT_PORT 48054

struct stream_input_t;

struct hid_passthrough_manager {
    bool running;
    bool autoplug;
    char host[128];
    int port;
    struct stream_input_t *stream_input;
};

typedef struct hid_passthrough_manager hid_passthrough_manager_t;

typedef struct {
    char path[HID_PT_PATH_LEN];
    char product[64];
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usage_page;
    uint16_t usage;
    bool plugged;
    bool connected;
} hid_pt_device_info_t;

void hid_passthrough_manager_init(hid_passthrough_manager_t *manager);

void hid_passthrough_manager_deinit(hid_passthrough_manager_t *manager);

int hid_passthrough_manager_start(hid_passthrough_manager_t *manager, const char *host, int port,
                                  bool autoplug);

void hid_passthrough_manager_stop(hid_passthrough_manager_t *manager);

bool hid_passthrough_manager_active(const hid_passthrough_manager_t *manager);

void hid_passthrough_manager_set_stream_input(hid_passthrough_manager_t *manager,
                                              struct stream_input_t *input);

/* The stream-time poll tick: request_rescan() with the stored stream input.
 * LVGL/main thread only. */
void hid_passthrough_manager_poll(hid_passthrough_manager_t *manager);

/* Ask for the device model to be rebuilt now, and auto-plug reconciled against
 * @p input (NULL = the input this manager was given).
 *
 * This module's only entry point for enumeration, and the only caller of the
 * underlying primitives in the tree: the SDL hotplug handlers, session start
 * and the panel's Refresh button all come through here, so one component
 * decides when sysfs is walked and when g_devices.generation moves. It runs the
 * scan immediately and never defers — a hotplug that arrived must not wait for
 * the next tick. LVGL/main thread only. */
void hid_passthrough_manager_request_rescan(hid_passthrough_manager_t *manager,
                                            struct stream_input_t *input);

int hid_passthrough_manager_device_count(hid_passthrough_manager_t *manager);

int hid_passthrough_manager_get_device(hid_passthrough_manager_t *manager, int index,
                                       hid_pt_device_info_t *info);

#else

typedef struct hid_passthrough_manager {
    int unused;
} hid_passthrough_manager_t;

typedef struct {
    char path[64];
    char product[64];
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usage_page;
    uint16_t usage;
    bool plugged;
    bool connected;
} hid_pt_device_info_t;

static inline void hid_passthrough_manager_init(hid_passthrough_manager_t *manager) {
    (void) manager;
}

static inline void hid_passthrough_manager_deinit(hid_passthrough_manager_t *manager) {
    (void) manager;
}

static inline int hid_passthrough_manager_start(hid_passthrough_manager_t *manager, const char *host, int port,
                                                bool autoplug) {
    (void) manager;
    (void) host;
    (void) port;
    (void) autoplug;
    return -1;
}

static inline void hid_passthrough_manager_stop(hid_passthrough_manager_t *manager) {
    (void) manager;
}

static inline bool hid_passthrough_manager_active(const hid_passthrough_manager_t *manager) {
    (void) manager;
    return false;
}

static inline void hid_passthrough_manager_set_stream_input(hid_passthrough_manager_t *manager,
                                                            void *input) {
    (void) manager;
    (void) input;
}

static inline void hid_passthrough_manager_poll(hid_passthrough_manager_t *manager) {
    (void) manager;
}

static inline void hid_passthrough_manager_request_rescan(hid_passthrough_manager_t *manager,
                                                          void *input) {
    (void) manager;
    (void) input;
}

#endif

#endif
