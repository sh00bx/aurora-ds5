#ifndef CTM_BRIDGE_PROTOCOL_H
#define CTM_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define CTMB_MAGIC 0x54424d43u
#define CTMB_VERSION 1u
#define CTMB_FLAG_OK 0x00000001u
#define CTMB_FLAG_PACED 0x00000002u
#define CTMB_MAX_PAYLOAD 65536u

enum ctmb_message_type {
    CTMB_MSG_HELLO = 1,
    CTMB_MSG_HOST_CONFIG = 2,
    CTMB_MSG_INPUT_REPORT = 3,
    CTMB_MSG_OUTPUT_REPORT = 4,
    CTMB_MSG_FEATURE_GET = 5,
    CTMB_MSG_FEATURE_REPORT = 6,
    CTMB_MSG_LOG = 7,
    CTMB_MSG_ERROR = 8,
    CTMB_MSG_FEATURE_SET = 9,
    CTMB_MSG_ENUM = 10,          /* forwarded composite USB enumeration (puck) */
    CTMB_MSG_PACE_FEEDBACK = 11  /* TV -> host inject-queue telemetry (rate servo).
                                    Sent ONLY when HOST_CONFIG advertised
                                    CTMB_HOSTCFG_PACE_FEEDBACK, so a CTM host
                                    never sees the type. */
};

/* ctmb_host_config_t.reserved[0] capability bits (0 on a CTM host). */
#define CTMB_HOSTCFG_PACE_FEEDBACK 0x01u

/* ctmb_device_caps_t.flags capability bits (TV -> host, in HELLO).
 * Bit 0 is the pre-existing always-set "1".
 *   DS5_AUDIO_0X39 — this client accepts batched 0x39 DS5 audio output
 *   reports (two Opus frames + two coil blocks per report). */
#define CTMB_DEVCAP_DS5_AUDIO_0X39 0x0002u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t request_id;
    uint32_t payload_len;
} ctmb_header_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    uint16_t bus;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
    uint16_t flags;
    char path[64];
    char serial[64];
    char product[64];
    char manufacturer[64];
} ctmb_device_caps_t;

typedef struct {
    uint32_t report_descriptor_len;
    uint8_t reserved[28];
} ctmb_hid_descriptor_info_t;

typedef struct {
    uint32_t bt_pace_us;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
    uint8_t paced_report_count;
    uint8_t paced_report_ids[16];
    uint8_t reserved[31];
} ctmb_host_config_t;

/* CTMB_MSG_PACE_FEEDBACK payload: snapshot of ds5_txd's raw-ACL inject queue
 * for this pad's link (from the "<tmpl>.<mac>.st" record), forwarded ~4/s.
 * The host pacer's rate servo keys on fifo_count (true backlog: frames parked
 * behind a FULL credit window) and drop_total deltas. */
typedef struct {
    uint8_t outstanding;   /* in-flight TX (NOCP credit window occupancy) */
    uint8_t fifo_count;    /* parked behind the window (elastic FIFO depth) */
    uint16_t maxq;         /* credit window cap */
    uint16_t fifo_cap;     /* elastic FIFO cap */
    uint16_t reserved0;
    uint32_t inj_total;    /* daemon lifetime counters (monotonic) */
    uint32_t drop_total;
    uint8_t reserved[16];
} ctmb_pace_feedback_t;

/* CTMB_MSG_ENUM payload (puck composite): the device's OWN enumeration, read
 * from sysfs on the TV and forwarded verbatim. Windows replays it (no parsing
 * on the TV). Layout:
 *   [ctmb_enum_info_t]
 *   [descriptors blob: descriptors_len bytes]   (device + config + ifaces + eps)
 *   iface_count x ( [ctmb_enum_iface_t] [report_desc: report_desc_len bytes] )
 * full_speed=1 tells the host to present the virtual device as full-speed so the
 * 64-byte CDC bulk endpoints are legal (the device really is full-speed). */
typedef struct {
    uint16_t descriptors_len;
    uint8_t  iface_count;
    uint8_t  full_speed;
    uint8_t  reserved[28];
} ctmb_enum_info_t;

typedef struct {
    uint8_t  interface_number;
    uint8_t  iface_class;
    uint16_t report_desc_len;
} ctmb_enum_iface_t;
#pragma pack(pop)

#endif
