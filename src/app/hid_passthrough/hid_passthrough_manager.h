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

/* Rescan + per-device auto-plug reconcile. LVGL/main thread only. */
void hid_passthrough_manager_poll(hid_passthrough_manager_t *manager);

void hid_passthrough_manager_reconcile(hid_passthrough_manager_t *manager,
                                        struct stream_input_t *input);

int hid_passthrough_manager_device_count(hid_passthrough_manager_t *manager);

int hid_passthrough_manager_get_device(hid_passthrough_manager_t *manager, int index,
                                       hid_pt_device_info_t *info);

void hid_passthrough_manager_rescan(hid_passthrough_manager_t *manager);

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

static inline void hid_passthrough_manager_reconcile(hid_passthrough_manager_t *manager, void *input) {
    (void) manager;
    (void) input;
}

static inline void hid_passthrough_manager_rescan(hid_passthrough_manager_t *manager) {
    (void) manager;
}

#endif

#endif
