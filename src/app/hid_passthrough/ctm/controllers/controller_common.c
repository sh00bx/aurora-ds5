/* Controller pump + lifecycle + factory (D2 stage 2).
 *
 * One isolated bridging session per controller: its own reader + session
 * threads, HID fd, ctm_transport, settings, and per-MAC log. This is the merge
 * of tv_bridge_worker.c (DS pump: 2 threads, pacing, evdev grab, HELLO/
 * HOST_CONFIG handshake) and hidraw_bridge.c (verbatim relay). Per-type
 * behaviour is preserved via ops flags (needs_host_config / grab_evdev /
 * request_bt_mode) and the patch_output hook.
 *
 * What is left here is the lifecycle and the pump: device open, the handshake,
 * the primary input thread, the session loop and the plug-in/plug-out API. The
 * four subsystems that used to share this file's globals now own their state
 * behind an opaque pointer each — ctm_hid_io (the device handle + everything
 * written to it), ctm_composite (sibling HID interfaces), ctm_xpad_evdev (the
 * Flydigi XInput feeder) and ctm_feature_worker (off-thread feature ioctls).
 * They reach back only through the ctm_ctl_* services at the bottom of
 * ctm_controller_priv.h, which are implemented here. */

#define _GNU_SOURCE

#include "ctm_controller.h"
#include "ctm_controller_priv.h"
#include "ctm_composite.h"
#include "ctm_feature_worker.h"
#include "ctm_hid_io.h"
#include "ctm_xpad_evdev.h"
#include "ctm_hid.h"   /* read_report_descriptor, derive_report_lengths */
#include "ctm_state.h" /* flydigi mode probes, read_usb_identity_attrs */
#include "ctm_ds5_audio.h"
#include "ds5_acl_tx.h"
#include "ds5_hidfd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif

#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo { unsigned int bustype; short vendor; short product; };
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif
#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

#define MAX_REPORT_DESCRIPTOR 4096
#define MAX_EVDEV_GRABS 16
#define BUS_BLUETOOTH 0x05
#define BUS_USB 0x03

typedef struct { int fd; char path[64]; } evdev_grab_t;

/* The bridging counters the UI status panel shows.
 *
 * reports_in is incremented by every reader the controller runs: the primary
 * input thread, one thread per composite sibling interface (up to 15 for a
 * Steam puck) and the xpad evdev feeder — so on ARM32 a plain `x++` is a
 * load-add-store that interleaves and silently loses counts, which is exactly
 * the number an operator reads to decide whether a pad is feeding the host at
 * all. All four go through relaxed atomics: no ordering is implied or wanted,
 * they are pure statistics, and a relaxed fetch-add is a single ldrex/strex
 * pair with no barrier, no syscall and no lock on the input path. */
typedef struct {
    unsigned long reports_in;       /* input reports forwarded to the host */
    unsigned long reports_out;      /* output reports written to the device */
    unsigned long coalesced;        /* input reports dropped by per-burst coalescing */
    unsigned long ui_neutralized;   /* input reports blanked by the overlay gate */
    unsigned long stateless_in;     /* input reports dropped: no gamepad state in them */
} ctm_stats_t;

static inline void ctm_stat_add(unsigned long *v, unsigned long n)
{
    __atomic_fetch_add(v, n, __ATOMIC_RELAXED);
}

static inline unsigned long ctm_stat_get(unsigned long *v)
{
    return __atomic_load_n(v, __ATOMIC_RELAXED);
}


struct ctm_controller {
    const ctm_controller_ops_t *ops;
    ctm_controller_dev_t dev;
    unsigned int vid_num;
    unsigned int pid_num;
    char host[64];
    int port;

    ctm_transport_t xport;
    ctm_enet_client_t *enet;        /* process-owned client; borrowed by xport */
    ds5_acl_tx_t *acl_tx;          /* DS5 raw-ACL forwarder (NULL = hidraw only) */

    /* --- the extracted subsystems, one opaque handle each ------------------
     * Created in ctm_controller_create, destroyed in ctm_controller_destroy;
     * never NULL after a successful create. Each owns its own state; nothing
     * here reaches into it. */
    ctm_hid_io_t *io;              /* the primary hidraw fd + every write to it */
    ctm_composite_t *comp;         /* sibling HID interfaces (puck / dongle) */
    ctm_xpad_t *xpad;              /* Flydigi XInput evdev feeder */
    ctm_feature_worker_t *feat;    /* off-thread feature ioctls */

    /* DS BT audio concealment state + its 60s counters (ctm_ds5_audio.h).
     * Unlocked — see the caller contract in that header. The synth frames it
     * produces reach the pad through audio_synth_write_cb below. */
    ds5_audio_t audio;
    uint64_t plc_log_next_us;     /* next PLC/60s telemetry line */
    uint32_t last_pace_log_us;    /* last logged paced-drain interval (log-on-change) */
    /* Rumble slotting (hidraw path, audio active): 0x31/0x32 ride leftover air
     * slots as a 1-deep latest-wins state instead of jumping past the paced
     * audio queue into the BT stack FIFO — the one-outstanding wall serializes
     * EVERYTHING, so an unpaced rumble burst (up to 5/s measured, clustered in
     * combat) steals ~16ms air slots exactly when audio needs them. */
    uint8_t rumble_slot[2][CTM_MAX_REPORT]; /* [0]=0x31 [1]=0x32 */
    size_t rumble_slot_len[2];
    int rumble_rr;                /* round-robin cursor between the two slots */
    uint64_t last_rumble_write_us;
    uint32_t rumble_min_us;       /* min gap between rumble writes while audio active */
    unsigned long st_rumble_coal; /* rumble states overwritten latest-wins */
    /* Adaptive pad latency: extra ms over the slider while the link is hot
     * (fill bridging stalls). Slewed in the pump loop, applied per report by
     * ds5_patch_output via ctm_controller_adapt_latency_ms(). */
    int adapt_enabled;
    uint32_t adapt_lat_add_ms;   /* relaxed-atomic: read by the exported
                                  * ctm_controller_adapt_latency_ms() */
    uint64_t adapt_hot_until_us;
    uint64_t adapt_slew_next_us;
    /* Diagnostic: host->TV OUTPUT_REPORT arrival pattern (HOL-blocking probe).
     * The agent sends ALL its output (0x36 haptics, 0x31 rumble, feature
     * replies) RELIABLE on ONE ENet channel, so a WiFi loss stalls every later
     * output until the retransmit lands, then flushes them at once. Signature:
     * arrival gap >30ms followed by a catch-up burst (inter-arrival <2ms).
     * A gap WITHOUT a burst means loss/idle instead — that distinction is the
     * whole point of this probe. h->timestamp_us (agent send-time, host clock)
     * additionally gives per-packet transit delay over the window minimum,
     * which cancels the unknown clock offset. Logged as NET/60s. */
    uint64_t net_last_out_us;      /* arrival time of the previous OUTPUT report */
    unsigned net_burst_cur;        /* >0: inside the flush after a >30ms gap */
    uint64_t net_log_next_us;
    unsigned long st_net_out;      /* OUTPUT reports this window */
    unsigned long st_net_gaps30;   /* gaps 30ms..1s (stall-like) */
    unsigned long st_net_idle;     /* gaps >1s (stream idle, not counted as stall) */
    uint64_t st_net_gap_max_us;    /* largest stall-like gap (<=1s) */
    unsigned st_net_burst_max;     /* largest post-gap flush */
    int64_t st_net_skew_min;       /* min(now - host_ts): clock offset + best transit */
    int64_t st_net_skew_max;       /* max(now - host_ts) */
    int64_t st_net_skew_sum;
    /* Pump tunables resolved from the type's policy and the environment
     * (k_knobs indexes them by offsetof), then pushed into the subsystem that
     * uses them — ctm_hid_io — at session start. */
    int hid_wait_ms;
    uint8_t dedup_report_id;

    int wake_pipe[2];

    pthread_t session_thread;
    int session_started;
    _Atomic(int) session_finished;  /* session thread exited; reconcile re-plugs */
    pthread_t input_thread;
    int input_thread_started;
    /* Session flags, written by one thread and read by several others.
     * _Atomic rather than volatile so that a plain access anywhere in this file
     * is still an atomic one (C11 gives it sequential consistency), and the
     * three reads that sit on a per-report or per-poll path use an explicit
     * relaxed load below — none of these flags orders any data, each IS the
     * whole message, so relaxed is the correct strength and costs no barrier. */
    _Atomic(int) stop;
    _Atomic(int) link_down;         /* input thread saw a send failure; run_session
                                     * exits so session_main retries the connect */
    /* Liveness ticket for the input watchdog. The two readers that do NOT own
     * last_rx_us — the composite sibling threads and the xpad evdev feeder —
     * store 1 here per forwarded report; the watchdog (input_thread_main, on a
     * poll timeout) reads-and-clears it and treats a set ticket as activity.
     * The primary input thread needs no ticket: it updates last_rx_us itself.
     *
     * A flag rather than a timestamp on purpose: the composite sibling readers
     * and the xpad feeder run on their own threads, and a shared uint64_t
     * microsecond stamp neither stores atomically on 32-bit ARM nor is worth a
     * clock read per input report. A 32-bit relaxed store is one instruction
     * with no syscall, no lock and no ordering requirement — the watchdog only
     * needs to know "something arrived in the last 250 ms window", and a lost
     * race just defers the observation by one poll timeout, far inside the
     * multi-second timeout it feeds. */
    uint32_t rx_tick;

    pthread_mutex_t settings_mutex;
    tv_bridge_worker_settings_t settings;

    evdev_grab_t evdev_grabs[MAX_EVDEV_GRABS];
    int evdev_grab_count;

    FILE *log;

    /* Live status for the UI panel. connected/transport/last_event are written
     * and read under status_mutex; the counters are not, so they go through
     * ctm_stat_* (see ctm_stats_t). */
    pthread_mutex_t status_mutex;
    int st_connected;
    int st_transport_enet;
    ctm_stats_t stats;
    int ui_gated_logged;                   /* last logged overlay-gate state; written
                                            * by input_thread_main, zeroed by
                                            * session_state_reset (which the pump
                                            * runs before that thread is created) */
    char st_last_event[96];

    uint8_t battery_level;
    uint8_t battery_status;
    uint64_t battery_updated_us;
    uint8_t battery_raw_last;
    uint8_t battery_raw_stable;

    uint8_t *enum_payload;           /* composite: forwarded enumeration (CTMB_MSG_ENUM) */
    int enum_payload_len;
    _Atomic(int) comp_run;           /* gates the composite sibling readers and the
                                      * xpad feeder for the length of one session
                                      * (ctm_ctl_readers_run) */
    int flydigi_xinput_evdev_only;   /* gamepad via evdev only (no hidraw on bus) */
    int dummy_hid_pipe_wr;           /* write end of placeholder hid pipe */
};

/* ENet is process-global; init once for all controllers, never deinit. */
static pthread_once_t g_enet_once = PTHREAD_ONCE_INIT;
static int g_enet_ready = 0;
static void enet_global_init_once(void)
{
    if (enet_client_global_init() == 0) g_enet_ready = 1;
    else fprintf(stderr, "controller: enet_initialize failed; ENet disabled, TCP only\n");
}


/* Optional UI log sink, set by the app, so controller events also appear in the
 * on-screen console. NULL => file + stderr only. */
static void (*g_log_sink)(const char *line);

void ctm_controller_set_log_sink(void (*sink)(const char *line))
{
    g_log_sink = sink;
}

/* Per-controller log: writes to its MAC-named file (if open), stderr, and the
 * UI sink (if set), each line prefixed with the controller kind. When:
 * throughout a controller's lifetime (any of its threads, and any of the
 * subsystem modules). */
void ctm_ctl_log(ctm_controller_t *c, const char *fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    const char *kind = c->ops ? c->ops->kind : "ctl";
    fprintf(stderr, "[%s] %s\n", kind, body);
    if (c->log) {
        fprintf(c->log, "%s\n", body);
        fflush(c->log);
    }
    pthread_mutex_lock(&c->status_mutex);
    snprintf(c->st_last_event, sizeof(c->st_last_event), "%s", body);
    pthread_mutex_unlock(&c->status_mutex);
    if (g_log_sink) {
        char line[600];
        snprintf(line, sizeof(line), "%s: %s", kind, body);
        g_log_sink(line);
    }
}

static void ctl_set_rt_prio(ctm_controller_t *c, const char *who, int prio)
{
    const char *env = getenv("CTM_RT");
    if (env && strcmp(env, "0") == 0) {
        ctm_ctl_log(c, "%s RT disabled (CTM_RT=0)", who);
        return;
    }
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        ctm_ctl_log(c, "%s SCHED_FIFO prio=%d", who, prio);
    } else {
        int e = errno;
        (void)setpriority(PRIO_PROCESS, 0, -20);
        ctm_ctl_log(c, "%s SCHED_FIFO denied errno=%d -> nice -20", who, e);
    }
}

static void acl_log_cb(void *ctx, const char *msg)
{
    ctm_ctl_log((ctm_controller_t *)ctx, "[acl] %s", msg);
}

/* Send one framed CTMB message over this controller's transport. When: the
 * input thread (input reports), the session thread (HELLO + feature replies),
 * the composite sibling readers, the xpad feeder, the feature worker. */
int ctm_ctl_send(ctm_controller_t *c, uint16_t type, uint32_t flags,
                 uint32_t request_id, const void *payload, size_t len)
{
    return ctm_transport_send_msg(&c->xport, type, flags, request_id, payload, len);
}

/* Count one input report forwarded by a reader that does NOT own last_rx_us,
 * and hand the watchdog its liveness ticket. See rx_tick in the struct. */
void ctm_ctl_note_input_report(ctm_controller_t *c)
{
    ctm_stat_add(&c->stats.reports_in, 1);
    __atomic_store_n(&c->rx_tick, 1u, __ATOMIC_RELAXED);
}

/* Count one output report the device (or its injector) accepted. */
void ctm_ctl_note_output_report(ctm_controller_t *c)
{
    ctm_stat_add(&c->stats.reports_out, 1);
}

/* The session's non-primary readers keep looping while the session that started
 * them has not begun tearing down (comp_run, cleared before their joins) and
 * plug-out has not been requested. */
bool ctm_ctl_readers_run(ctm_controller_t *c)
{
    return __atomic_load_n(&c->comp_run, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&c->stop, __ATOMIC_RELAXED);
}

/* Pop one received message (1=got / 0=none / -1=dropped). When: the session
 * thread loop and the handshake wait. */
static int c_recv(ctm_controller_t *c, ctmb_header_t *h, uint8_t **payload)
{
    return ctm_transport_recv_msg(&c->xport, h, payload);
}


/* Sony feature-0x05 "full BT mode" probe. When: at HID open, DS only
 * (gated by ops->request_bt_mode). */
static void request_full_bt_mode(int fd)
{
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        fprintf(stderr, "controller: feature 0x05 failed errno=%d\n", errno);
    }
}

int ctm_controller_write_feature(ctm_controller_t *c, const uint8_t *feature, size_t len)
{
    if (!c) return -1;
    return ctm_hid_io_feature_set(c->io, feature, len);
}

void ctm_controller_update_battery(ctm_controller_t *c, uint8_t level, uint8_t status)
{
    if (!c) return;
    __atomic_store_n(&c->battery_level, level, __ATOMIC_RELEASE);
    __atomic_store_n(&c->battery_status, status, __ATOMIC_RELEASE);
    __atomic_store_n(&c->battery_updated_us, ctm_now_us(), __ATOMIC_RELEASE);
}

void ctm_controller_update_battery_raw(ctm_controller_t *c, uint8_t raw)
{
    if (!c) return;
    if (raw == c->battery_raw_last) {
        if (c->battery_raw_stable < 255) {
            c->battery_raw_stable++;
        }
    } else {
        c->battery_raw_last = raw;
        c->battery_raw_stable = 1;
    }
    /* Require three identical samples before updating (filters touchpad noise). */
    if (c->battery_raw_stable < 3) {
        return;
    }

    uint8_t level  = raw & 0x0Fu;
    uint8_t charge = (raw >> 4) & 0x0Fu;
    if (level > 10) {
        return;
    }
    uint8_t status = 0;
    if (charge == 1) {
        status = 1;
    } else if (charge == 2) {
        status = 2;
        if (level < 10) {
            level = 10;
        }
    } else if (charge != 0) {
        return;
    }
    ctm_controller_update_battery(c, level, status);
}

/* CRC32 (reflected, poly 0xedb88320) step. When: ctm_bt_sign_output only. */
static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) into the trailing 4
 * bytes. When: a DS patch_output hook, after rewriting a report. */
void ctm_bt_sign_output(uint8_t *data, size_t len)
{
    if (!data || len < 8) return;
    uint8_t seed = 0xa2;
    uint32_t crc = crc32_step(0xffffffffu, &seed, 1);
    crc = ~crc32_step(crc, data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xffu);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xffu);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xffu);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xffu);
}

static void flydigi_fill_caps_identity(const char *usb_busid, char *mfg, size_t mfg_len,
                                       char *product, size_t product_len);

/* Open the controller's hidraw node (ops->select_node or dev.path), validate
 * vid/pid, fill caps + report descriptor. When: once by session_main before
 * the connect loop. Returns the fd, or -1. */
static int open_hid(ctm_controller_t *c, ctmb_device_caps_t *caps,
                    uint8_t *report_desc, uint32_t *report_desc_len)
{
    char node[64];
    const char *path = c->dev.path;
    if (c->ops->select_node && c->ops->select_node(&c->dev, node, sizeof(node)) == 0 && node[0]) {
        path = node;
    }

    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fd = ds5_hidfd_request(path);
        if (fd < 0) {
            return -1;
        }
        ctm_ctl_log(c, "hid fd via root broker for %s (jail node absent)", path);
    }

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        close(fd);
        return -1;
    }
    unsigned int vid = (unsigned short)info.vendor;
    unsigned int pid = (unsigned short)info.product;
    if (!(c->ops && c->ops->composite) &&
        ((c->vid_num && vid != c->vid_num) || (c->pid_num && pid != c->pid_num))) {
        close(fd);
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = (uint16_t)vid;
    caps->product_id = (uint16_t)pid;
    caps->bus = info.bustype ? (uint16_t)info.bustype : BUS_BLUETOOTH;
    caps->input_report_len = 1024;
    caps->output_report_len = 1024;
    caps->feature_report_len = 64;
    caps->flags = 1;
    if (c->ops && strcmp(c->ops->kind, "ds5") == 0) {
        /* This client accepts batched 0x39 DS5 audio output reports (the
         * controller_ds5.c patch path handles the doubled sub-blocks), so tell
         * the host it may send them. */
        caps->flags |= CTMB_DEVCAP_DS5_AUDIO_0X39;
    }
    snprintf(caps->path, sizeof(caps->path), "%s", path);
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);
    snprintf(caps->manufacturer, sizeof(caps->manufacturer), "hidraw");
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(caps->product) - 1), caps->product) < 0 ||
        caps->product[0] == '\0') {
        snprintf(caps->product, sizeof(caps->product), "hidraw");
    }
    if (c->ops && c->ops->composite && strcmp(c->ops->kind, "flydigi") == 0 &&
        c->dev.usb_busid[0]) {
        flydigi_fill_caps_identity(c->dev.usb_busid, caps->manufacturer, sizeof(caps->manufacturer),
                                     caps->product, sizeof(caps->product));
        if (c->vid_num) caps->vendor_id = (uint16_t)c->vid_num;
        if (c->pid_num) caps->product_id = (uint16_t)c->pid_num;
    }

    *report_desc_len = read_report_descriptor(fd, report_desc, MAX_REPORT_DESCRIPTOR);
    if (*report_desc_len) {
        derive_report_lengths(report_desc, *report_desc_len, caps);
        if (caps->input_report_len < 1024) caps->input_report_len = 1024;
        if (caps->output_report_len < 1024) caps->output_report_len = 1024;
    }

    if (c->ops->request_bt_mode) request_full_bt_mode(fd);
    return fd;
}

static void flydigi_fill_caps_identity(const char *usb_busid, char *mfg, size_t mfg_len,
                                       char *product, size_t product_len)
{
    char sysfs_mfg[64] = {0};
    char sysfs_prod[64] = {0};
    if (usb_busid && usb_busid[0] &&
        read_usb_identity_attrs(usb_busid, sysfs_mfg, sizeof(sysfs_mfg),
                                sysfs_prod, sizeof(sysfs_prod)) == 0) {
        if (sysfs_mfg[0] && mfg && mfg_len) {
            snprintf(mfg, mfg_len, "%s", sysfs_mfg);
        }
        if (sysfs_prod[0] && product && product_len) {
            if (contains_ci(sysfs_prod, "apex")) {
                snprintf(product, product_len, "Apex 4");
            } else if (contains_ci(sysfs_prod, "vader")) {
                snprintf(product, product_len, "Vader3");
            } else {
                snprintf(product, product_len, "%s", sysfs_prod);
            }
            return;
        }
    }
    if (mfg && mfg_len && !mfg[0]) {
        snprintf(mfg, mfg_len, "Flydigi");
    }
    if (product && product_len && !product[0]) {
        snprintf(product, product_len, "Apex 4");
    }
}

/* Flydigi XInput with no hidraw on the USB bus: caps + synthetic descriptor,
 * placeholder pipe for the input thread; gamepad input comes from xpad evdev. */
static int flydigi_open_xinput_evdev_only(ctm_controller_t *c, ctmb_device_caps_t *caps,
                                          uint8_t *report_desc, uint32_t *report_desc_len)
{
    if (!c || !caps || !report_desc || !report_desc_len) {
        return -1;
    }
    if (!flydigi_is_xinput_evdev_only_for_busid(c->dev.usb_busid)) {
        return -1;
    }

    char evdev_path[64];
    if (ctm_xpad_find_evdev(c->dev.usb_busid, evdev_path, sizeof(evdev_path)) != 0) {
        ctm_ctl_log(c, "flydigi xinput evdev-only: xpad path missing busid=%s", c->dev.usb_busid);
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    caps->bus = BUS_USB;
    caps->input_report_len = 64;
    caps->output_report_len = 64;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);
    snprintf(caps->path, sizeof(caps->path), "%s", evdev_path);

    char usbdir[512];
    if (composite_usb_device_dir_by_busid(c->dev.usb_busid, usbdir, sizeof(usbdir)) == 0) {
        char path[600], v[16] = {0}, p[16] = {0};
        snprintf(path, sizeof(path), "%s/idVendor", usbdir);
        read_text_file(path, v, sizeof(v));
        snprintf(path, sizeof(path), "%s/idProduct", usbdir);
        read_text_file(path, p, sizeof(p));
        if (v[0]) {
            caps->vendor_id = (uint16_t)strtoul(v, NULL, 16);
        }
        if (p[0]) {
            caps->product_id = (uint16_t)strtoul(p, NULL, 16);
        }
    }
    if (!caps->vendor_id) {
        caps->vendor_id = (uint16_t)0x045e;
    }
    if (!caps->product_id) {
        caps->product_id = (uint16_t)0x028e;
    }

    flydigi_fill_caps_identity(c->dev.usb_busid, caps->manufacturer, sizeof(caps->manufacturer),
                                 caps->product, sizeof(caps->product));

    if (flydigi_xbox360_wired_rdesc_len > MAX_REPORT_DESCRIPTOR) {
        return -1;
    }
    memcpy(report_desc, flydigi_xbox360_wired_rdesc, flydigi_xbox360_wired_rdesc_len);
    *report_desc_len = (uint32_t)flydigi_xbox360_wired_rdesc_len;
    derive_report_lengths(report_desc, *report_desc_len, caps);
    if (caps->input_report_len < 64) {
        caps->input_report_len = 64;
    }
    if (caps->output_report_len < 64) {
        caps->output_report_len = 64;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        ctm_ctl_log(c, "flydigi xinput evdev-only: pipe failed errno=%d", errno);
        return -1;
    }
    (void)fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL, 0) | O_NONBLOCK);
    c->flydigi_xinput_evdev_only = 1;
    c->dummy_hid_pipe_wr = pipefd[1];
    ctm_ctl_log(c, "flydigi xinput evdev-only session busid=%s evdev=%s caps=%04x:%04x",
            c->dev.usb_busid, evdev_path, caps->vendor_id, caps->product_id);
    return pipefd[0];
}

/* Flydigi dongle in XInput mode with hidraw siblings: open a non-mouse hidraw
 * hidraw for the input thread and supply synthetic Xbox 360 wired caps/descriptor
 * for HELLO (identity still comes from sysfs). */
static int flydigi_open_xinput_handshake(ctm_controller_t *c, ctmb_device_caps_t *caps,
                                         uint8_t *report_desc, uint32_t *report_desc_len)
{
    if (!c || !caps || !report_desc || !report_desc_len) return -1;
    if (!flydigi_is_xinput_mode_for_busid(c->dev.usb_busid)) return -1;

    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = c->vid_num ? (uint16_t)c->vid_num : (uint16_t)0x04b4;
    caps->product_id = c->pid_num ? (uint16_t)c->pid_num : (uint16_t)0x2412;
    caps->bus = BUS_USB;
    caps->input_report_len = 64;
    caps->output_report_len = 64;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);

    flydigi_fill_caps_identity(c->dev.usb_busid, caps->manufacturer, sizeof(caps->manufacturer),
                                 caps->product, sizeof(caps->product));

    if (flydigi_xbox360_wired_rdesc_len > MAX_REPORT_DESCRIPTOR) return -1;
    memcpy(report_desc, flydigi_xbox360_wired_rdesc, flydigi_xbox360_wired_rdesc_len);
    *report_desc_len = (uint32_t)flydigi_xbox360_wired_rdesc_len;
    derive_report_lengths(report_desc, *report_desc_len, caps);
    if (caps->input_report_len < 64) caps->input_report_len = 64;
    if (caps->output_report_len < 64) caps->output_report_len = 64;

    char node[64] = {0};
    if (flydigi_handshake_hidraw_path_for_busid(c->dev.usb_busid, node, sizeof(node)) != 0 &&
        flydigi_hidraw_path_for_busid(c->dev.usb_busid, node, sizeof(node)) != 0) {
        ctm_ctl_log(c, "flydigi xinput mode: no hidraw for handshake (synthetic caps only)");
        return -1;
    }

    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ctm_ctl_log(c, "flydigi xinput handshake open failed path=%s errno=%d", node, errno);
        return -1;
    }
    snprintf(caps->path, sizeof(caps->path), "%s", node);
    ctm_ctl_log(c, "flydigi xinput mode: handshake via sysfs, gamepad via xpad evdev path=%s",
            node);
    return fd;
}

/* Open the device for a session, in the order the tiers can succeed:
 *
 *   1. open_hid            — the classified hidraw node (every type).
 *   2. flydigi_open_xinput_evdev_only — Flydigi XInput with NO hidraw on the
 *      bus: synthetic caps + a placeholder pipe, gamepad via xpad evdev.
 *   3. flydigi_open_xinput_handshake  — Flydigi XInput WITH hidraw siblings:
 *      any non-mouse-filtered node will do for the handshake, since the
 *      gamepad again comes from xpad evdev.
 *
 * Tier 3 can legitimately open a node tier 1 refuses: flydigi_select_node()
 * goes through flydigi_handshake_hidraw_pick(), which skips mice, while tier
 * 3's second chance goes through flydigi_hidraw_pick(), which does not.
 *
 * Used for the initial open AND for the post-link_down re-attach, which is the
 * point: they must agree, or a session can be established that can never be
 * re-established. Tier 2 sets flydigi_xinput_evdev_only and owns a pipe, so
 * both are cleared here before the ladder runs again.
 * When: session_main, once at start and once per link_down. Returns the fd. */
static int open_device_any_tier(ctm_controller_t *c, ctmb_device_caps_t *caps,
                                uint8_t *report_desc, uint32_t *report_desc_len)
{
    if (c->dummy_hid_pipe_wr >= 0) { close(c->dummy_hid_pipe_wr); c->dummy_hid_pipe_wr = -1; }
    c->flydigi_xinput_evdev_only = 0;

    int fd = open_hid(c, caps, report_desc, report_desc_len);
    const int is_flydigi = c->ops && c->ops->kind && strcmp(c->ops->kind, "flydigi") == 0;
    if (fd < 0 && is_flydigi && flydigi_is_xinput_evdev_only_for_busid(c->dev.usb_busid)) {
        fd = flydigi_open_xinput_evdev_only(c, caps, report_desc, report_desc_len);
    }
    if (fd < 0 && is_flydigi) {
        fd = flydigi_open_xinput_handshake(c, caps, report_desc, report_desc_len);
    }
    return fd;
}

/* EVIOCGRAB the device's evdev nodes so webOS doesn't double-consume input.
 * When: at session start, BT/DS only (gated by ops->grab_evdev). */
static void grab_matching_evdev(ctm_controller_t *c)
{
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char vendor_path[160], product_path[160], vendor[32] = {0}, product[32] = {0};
        snprintf(vendor_path, sizeof(vendor_path), "/sys/class/input/%s/id/vendor", ent->d_name);
        snprintf(product_path, sizeof(product_path), "/sys/class/input/%s/id/product", ent->d_name);
        if (read_text_file(vendor_path, vendor, sizeof(vendor)) != 0 ||
            read_text_file(product_path, product, sizeof(product)) != 0 ||
            !ctm_hex_equals(vendor, c->vid_num) || !ctm_hex_equals(product, c->pid_num)) {
            continue;
        }
        char input_dir[160];
        snprintf(input_dir, sizeof(input_dir), "/sys/class/input/%s", ent->d_name);
        DIR *input = opendir(input_dir);
        if (!input) continue;
        struct dirent *child;
        while ((child = readdir(input)) != NULL && c->evdev_grab_count < MAX_EVDEV_GRABS) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            char dev_path[64];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", child->d_name);
            int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                int idx = c->evdev_grab_count++;
                c->evdev_grabs[idx].fd = fd;
                snprintf(c->evdev_grabs[idx].path, sizeof(c->evdev_grabs[idx].path), "%s", dev_path);
                ctm_ctl_log(c, "grabbed %s", dev_path);
            } else {
                close(fd);
            }
        }
        closedir(input);
    }
    closedir(dir);
}

/* Un-grab + close the evdev nodes. When: each time a session ends. */
static void release_evdev_grabs(ctm_controller_t *c)
{
    for (int i = 0; i < c->evdev_grab_count; ++i) {
        if (c->evdev_grabs[i].fd >= 0) {
            ioctl(c->evdev_grabs[i].fd, EVIOCGRAB, 0);
            close(c->evdev_grabs[i].fd);
            c->evdev_grabs[i].fd = -1;
        }
    }
    c->evdev_grab_count = 0;
}


/* Thread-safe snapshot of the live settings. When: ctm_controller_get_settings
 * (so a patch_output hook can read current sliders without racing the UI). */
static tv_bridge_worker_settings_t copy_settings(ctm_controller_t *c)
{
    tv_bridge_worker_settings_t s;
    pthread_mutex_lock(&c->settings_mutex);
    s = c->settings;
    pthread_mutex_unlock(&c->settings_mutex);
    return s;
}

/* Write one synthesized audio frame to the pad, for ds5_audio_inject_synth.
 * When: the pump loop, via the audio module's write callback. */
static int audio_synth_write_cb(void *ctx, const uint8_t *data, size_t len)
{
    ctm_controller_t *c = (ctm_controller_t *)ctx;
    return ctm_hid_io_write_synth(c->io, data, len);
}

/* Extra pad-buffer ms the adaptive controller wants on top of the user's
 * latency slider right now. Read by ds5_patch_output per outbound report. */
uint32_t ctm_controller_adapt_latency_ms(ctm_controller_t *c)
{
    return c ? __atomic_load_n(&c->adapt_lat_add_ms, __ATOMIC_RELAXED) : 0;
}

/* Primary input thread: blocking-poll the bridged hidraw node and forward what
 * it reads to the transport. One per live session, started by run_session,
 * stopped via the wake pipe (or by setting stop / link_down).
 *
 * Per-burst input coalescing. The WiFi+BT combo chip starves BT scheduling under
 * heavy stream RX, so the DS5 buffers reports across a ~16-28ms stall then flushes
 * them back-to-back. Forwarding every stale report FIFO over usbip backlogs the
 * game's HID input queue and adds felt latency; SDL/Moonlight only ever consumed
 * the latest gamepad state, which is why the SDL path felt smoother. Match that:
 * within ONE readable burst keep only the most recent report per report-id, then
 * forward. In steady state a poll yields a single report, so this is a no-op. */
#define CTM_COAL_MAX_IDS 6
static void *input_thread_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    ctl_set_rt_prio(c, "ctm-input", 12);
    /* Allocated once (CTM_MAX_REPORT is 4096); per-thread so concurrent controllers
     * don't share. */
    static __thread uint8_t coal_buf[CTM_COAL_MAX_IDS][CTM_MAX_REPORT];
    size_t  coal_len[CTM_COAL_MAX_IDS];
    uint8_t coal_id[CTM_COAL_MAX_IDS];
    /* Jailed-hidraw disconnect detection: the app's static jail device node
     * (/var/palm/jail/.../dev/hidrawN) never raises POLLHUP when the DS5 drops
     * off BT — the underlying device vanishes but the static node stays, so a
     * lost link degenerates into silent 250ms poll timeouts forever. A connected
     * DS5 streams input reports continuously (~250/s even idle), so a multi-second
     * silence means the link is down. Signal link_down so run_session tears the
     * session down and session_main reconnects (same recovery path as the
     * send-failure case below), instead of spinning on a dead fd and wedging
     * the bridge (which also froze the whole app on a mid-session BT reconnect).
     *
     * That premise is type-specific, so the timeout comes from the type's pump
     * policy and 0 disables the check entirely — see ctm_pump_policy_t. And
     * because a composite type can be alive on a sibling interface or the xpad
     * feeder while THIS fd is quiet, activity from those readers counts too,
     * via the rx_tick ticket they set. */
    const ctm_pump_policy_t *policy = c->ops ? c->ops->policy : NULL;
    const uint64_t liveness_us = policy
        ? (uint64_t)policy->input_idle_timeout_ms * 1000ull : 0ull;
    uint64_t last_rx_us = ctm_now_us();
    /* Both are fixed for the length of this session: the fd is republished only
     * between sessions (session_main's re-attach), and the primary endpoint is
     * resolved before this thread starts. */
    const int hid_fd = ctm_hid_io_fd(c->io);
    const uint8_t in_ep = ctm_composite_primary_in_ep(c->comp);
    while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&c->link_down, __ATOMIC_RELAXED)) {
        struct pollfd pfds[2];
        pfds[0].fd = hid_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = c->wake_pipe[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
        /* hid_fd + wake_pipe cover every wake source (reports arrive on hid_fd;
         * stop setters write the wake pipe), so this timeout is only a belt-and-
         * braces backstop, not the input latency path. 1ms burned ~870 empty
         * wakeups/s; 250ms keeps a safety poll without the idle spin. */
        int pr = poll(pfds, 2, 250);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) {
            if (__atomic_exchange_n(&c->rx_tick, 0u, __ATOMIC_RELAXED)) {
                /* A sibling interface or the xpad feeder forwarded something
                 * since the last timeout: the device is alive, this fd just
                 * is not the one it talks on. */
                last_rx_us = ctm_now_us();
            } else if (liveness_us && ctm_now_us() - last_rx_us > liveness_us) {
                ctm_ctl_log(c, "no input for %llums, link presumed down",
                        (unsigned long long)((ctm_now_us() - last_rx_us) / 1000));
                c->link_down = 1;
                break;
            }
            continue;
        }
        if (pfds[1].revents & POLLIN) break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* Real hangup/error on the fd (delivered outside the jailed-node case):
             * signal link_down so run_session reconnects, rather than silently
             * killing input for the rest of the session. */
            c->link_down = 1;
            break;
        }
        if (!(pfds[0].revents & POLLIN)) continue;
        int coal_n = 0, drained = 0;
        for (;;) {
            uint8_t buf[CTM_MAX_REPORT];
            ssize_t n = read(hid_fd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                break;
            }
            if (c->ops->input_carries_state && !c->ops->input_carries_state(buf, (size_t) n)) {
                /* Not gamepad state. Dropped here rather than forwarded, because
                 * the coalescing below keys on the report id alone: a report
                 * that shares an id with the state reports would not sit beside
                 * the last real one, it would REPLACE it in the slot, and the
                 * host would repack whatever it carries as sticks and buttons.
                 * Still counts as traffic — the pad is plainly alive — so the
                 * liveness timeout below does not fire on a device that has gone
                 * quiet only in the sense this reader cares about. */
                unsigned long seen = ctm_stat_get(&c->stats.stateless_in) + 1;
                ctm_stat_add(&c->stats.stateless_in, 1);
                if (seen == 1 || seen % 500 == 0) {
                    ctm_ctl_log(c, "dropped %lu input report(s) carrying no pad state", seen);
                }
                drained++;
                continue;
            }
            uint8_t id = buf[0];
            int slot = -1;
            for (int i = 0; i < coal_n; ++i) if (coal_id[i] == id) { slot = i; break; }
            if (slot < 0) {
                slot = (coal_n < CTM_COAL_MAX_IDS) ? coal_n++ : (CTM_COAL_MAX_IDS - 1);
                coal_id[slot] = id;
            }
            memcpy(coal_buf[slot], buf, (size_t)n);
            coal_len[slot] = (size_t)n;
            drained++;
        }
        if (drained > 0) last_rx_us = ctm_now_us();
        /* Overlay gate: while the streaming overlay (or soft keyboard / HID
         * panel) owns the controller, its presses must not reach the game.
         * Reports are neutralized in place rather than dropped so the host
         * keeps seeing a live, untouched pad (no stuck buttons from a press
         * held across the gate edge, battery/seq keep flowing). Evaluated once
         * per burst; ops without a neutralizer forward unmodified (see
         * ctm_controller.h). */
        const bool ui_blocked = ui_should_block_input();
        if (c->ops->neutralize_input && ui_blocked != (c->ui_gated_logged != 0)) {
            c->ui_gated_logged = ui_blocked ? 1 : 0;
            ctm_ctl_log(c, "input %s by overlay gate", ui_blocked ? "neutralized" : "released");
        }
        for (int i = 0; i < coal_n && !__atomic_load_n(&c->stop, __ATOMIC_RELAXED); ++i) {
            if (ui_blocked && c->ops->neutralize_input) {
                c->ops->neutralize_input(c, coal_buf[i], coal_len[i]);
                ctm_stat_add(&c->stats.ui_neutralized, 1);
            }
            if (ctm_ctl_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, in_ep,
                       coal_buf[i], coal_len[i]) != 0) {
                /* Send failure = link down, NOT stop: stop=1 here races the
                 * session thread's own disconnect handling and permanently
                 * exits session_main's reconnect loop. Signal link_down so
                 * run_session tears down and session_main retries. */
                c->link_down = 1;
                break;
            }
            if (c->ops->on_input_report) {
                c->ops->on_input_report(c, coal_buf[i], coal_len[i]);
            }
            ctm_stat_add(&c->stats.reports_in, 1);
        }
        if (drained > coal_n)
            ctm_stat_add(&c->stats.coalesced, (unsigned long)(drained - coal_n));
    }
    return NULL;
}

/* Send HELLO (caps + HID report descriptor). When: first message of each
 * session, from handshake. */
static int send_hello(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                      const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_hid_descriptor_info_t desc_info;
    memset(&desc_info, 0, sizeof(desc_info));
    desc_info.report_descriptor_len = report_desc_len;
    size_t hello_len = sizeof(*caps) + sizeof(desc_info) + report_desc_len;
    uint8_t *hello = (uint8_t *)malloc(hello_len);
    if (!hello) return -1;
    memcpy(hello, caps, sizeof(*caps));
    memcpy(hello + sizeof(*caps), &desc_info, sizeof(desc_info));
    if (report_desc_len) {
        memcpy(hello + sizeof(*caps) + sizeof(desc_info), report_desc, report_desc_len);
    }
    int rc = ctm_ctl_send(c, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, hello_len);
    free(hello);
    return rc;
}

/* Dispatch one inbound message: OUTPUT (paced or direct write), FEATURE_GET/SET
 * (queued to the feature worker + async reply), HOST_CONFIG (pacing params).
 * When: per message decoded in the session loop. */
/* Track one host->TV OUTPUT_REPORT arrival for the NET/60s HOL probe (see the
 * struct fields for the theory). When: handle_message, before any dispatch. */
static void net_track_output(ctm_controller_t *c, const ctmb_header_t *h)
{
    uint64_t now = ctm_now_us();
    int64_t skew = (int64_t)now - (int64_t)h->timestamp_us;
    if (c->st_net_out == 0 || skew < c->st_net_skew_min) c->st_net_skew_min = skew;
    if (c->st_net_out == 0 || skew > c->st_net_skew_max) c->st_net_skew_max = skew;
    c->st_net_skew_sum += skew;
    c->st_net_out++;

    if (c->net_last_out_us != 0) {
        uint64_t gap = now - c->net_last_out_us;
        if (gap > 1000000ull) {
            /* Stream idle (no game audio/rumble), not a transport stall. */
            c->st_net_idle++;
            c->net_burst_cur = 0;
        } else if (gap > 30000ull) {
            c->st_net_gaps30++;
            if (gap > c->st_net_gap_max_us) c->st_net_gap_max_us = gap;
            c->net_burst_cur = 1;          /* this packet ends the gap = flush #1 */
            if (c->st_net_burst_max == 0) c->st_net_burst_max = 1;
        } else if (gap < 2000ull && c->net_burst_cur) {
            c->net_burst_cur++;            /* catch-up flush after the stall */
            if (c->net_burst_cur > c->st_net_burst_max)
                c->st_net_burst_max = c->net_burst_cur;
        } else {
            if (gap > c->st_net_gap_max_us) c->st_net_gap_max_us = gap;
            c->net_burst_cur = 0;          /* normal cadence resumed */
        }
    }
    c->net_last_out_us = now;
}

/* The window that decides "is this pad's audio stream live right now". Two
 * different timestamps feed it and the distinction matters: the rumble slotting
 * asks about the last audio report WRITTEN to the pad, the PLC fill asks about
 * the last real audio report the host DELIVERED. The subject stays explicit at
 * each call site; only the 150 ms window is shared.
 *
 * Unsigned by design: a zero timestamp (fresh session) underflows to a huge
 * difference and reads as "not live", which is what all five original copies
 * of this expression did (handle_message's rumble-slot branch, the rumble
 * drain, the PLC fill, and the two poll-timeout bounds). */
#define CTM_AUDIO_LIVE_US 150000ull

static inline bool audio_live_since(uint64_t now, uint64_t last_us)
{
    return now - last_us < CTM_AUDIO_LIVE_US;
}

static void handle_message(ctm_controller_t *c, ctmb_host_config_t *host_cfg,
                           ctm_paced_ring_t *paced,
                           const ctmb_header_t *h, uint8_t *payload)
{
    if (h->type == CTMB_MSG_OUTPUT_REPORT) {
        net_track_output(c, h);
        if (c->ops && c->ops->composite_evdev_gamepad && ctm_xpad_active(c->xpad)) {
            uint8_t ep = (uint8_t)h->request_id;
            if (ep == 0 || ep == ctm_xpad_out_ep(c->xpad) ||
                ep == ctm_composite_primary_out_ep(c->comp)) {
                ctm_xpad_apply_rumble(c->xpad, payload, h->payload_len);
                ctm_ctl_note_output_report(c);
                return;
            }
        }
        if (c->ops && c->ops->composite) {
            /* Route the host's OUT write to the interface that owns the OUT
             * endpoint it addressed (request_id = endpoint). Verbatim; pacing is
             * a BT-audio concern that does not apply to the composite puck. */
            int fd = ctm_composite_out_fd_for_ep(c->comp, (uint8_t)h->request_id,
                                                 ctm_hid_io_fd(c->io));
            (void)ctm_hid_io_write_fd_raw(c->io, fd, payload, h->payload_len);
        } else if (ctm_paced_should_pace(host_cfg, h, payload, h->payload_len)) {
            ctm_paced_queue(paced, payload, h->payload_len);
            /* Post-outage stale trim: after a long stall the burst of late
             * audio is history the pad already glitched through — playing it
             * all parks its length as PERMANENT extra speaker latency. Keep
             * ~85ms (4 x 0x39) and drop the oldest, with the fill's counter
             * anchor kept aligned (ds5_audio_note_dropped). */
            while (ctm_paced_count(paced) > 4) {
                const ctm_queued_report_t *r = ctm_paced_peek(paced);
                ds5_audio_note_dropped(&c->audio, r->data, r->len);
                ctm_paced_pop(paced);
            }
        } else if (!c->acl_tx && payload && h->payload_len >= 8 &&
                   h->payload_len <= CTM_MAX_REPORT &&
                   (payload[0] == 0x31 || payload[0] == 0x32) &&
                   audio_live_since(ctm_now_us(), ctm_hid_io_audio_last_us(c->io))) {
            /* Rumble/SetState while an audio stream is live on the hidraw
             * path: park in the 1-deep latest-wins slot instead of writing
             * immediately — a direct write jumps past the paced audio queue
             * into the BT stack FIFO and steals a ~16ms one-outstanding air
             * slot exactly when audio needs it (combat = rumble bursts = the
             * measured dropout windows). The pump loop writes the slot into
             * leftover air slots (see the drain block). Without live audio
             * (menus, DS4, 0x36/0x39 idle) this branch never taken -> direct
             * write, byte-identical to the old behavior. */
            int s = payload[0] == 0x31 ? 0 : 1;
            if (c->rumble_slot_len[s]) c->st_rumble_coal++;
            memcpy(c->rumble_slot[s], payload, h->payload_len);
            c->rumble_slot_len[s] = h->payload_len;
        } else {
            (void)ctm_hid_io_write(c->io, payload, h->payload_len);
        }
    } else if (h->type == CTMB_MSG_FEATURE_GET || h->type == CTMB_MSG_FEATURE_SET) {
        /* Async: the blocking ioctl runs on the feature worker so the input
         * pump keeps flowing; the worker sends the (success or failure) reply.
         * The target fd is resolved HERE because the composite interface map is
         * this thread's state. */
        int fd = ctm_composite_feature_fd(c->comp, c->ops && c->ops->composite,
                                          h->request_id, ctm_hid_io_fd(c->io));
        if (ctm_feature_worker_enqueue(c->feat, h->type, h->request_id,
                                       payload, h->payload_len, fd) != 0)
            (void)ctm_ctl_send(c, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0);
    } else if (h->type == CTMB_MSG_HOST_CONFIG && h->payload_len >= sizeof(*host_cfg)) {
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    }
}

static int handshake(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                     const uint8_t *report_desc, uint32_t report_desc_len,
                     ctmb_host_config_t *host_cfg)
{
    /* Composite (puck): forward the captured enumeration verbatim BEFORE HELLO so
     * the host can build the composite device from it. Identity passthrough. */
    if (c->enum_payload && c->enum_payload_len > 0 &&
        ctm_ctl_send(c, CTMB_MSG_ENUM, CTMB_FLAG_OK, 0, c->enum_payload, (size_t)c->enum_payload_len) != 0) {
        ctm_ctl_log(c, "ENUM send failed");
        return -1;
    }
    if (send_hello(c, caps, report_desc, report_desc_len) != 0) {
        ctm_ctl_log(c, "HELLO failed");
        return -1;
    }
    /* Relay types (puck/xbox/generic) do not require HOST_CONFIG — proceed
     * straight to the loop, which still applies HOST_CONFIG if it arrives. */
    if (!c->ops->needs_host_config) {
        host_cfg->bt_pace_us = 10667;
        return 0;
    }
    ctmb_header_t h;
    uint8_t *payload = NULL;
    uint64_t start = ctm_now_us();
    for (;;) {
        if (c->stop) return -1;
        if (c->xport.kind == CTM_TRANSPORT_ENET) {
            if (ctm_transport_service(&c->xport, 50) < 0) { ctm_ctl_log(c, "host config wait: link dropped"); return -1; }
        }
        int got = c_recv(c, &h, &payload);
        if (got < 0) { ctm_ctl_log(c, "host config receive failed"); return -1; }
        if (got == 0) {
            if (ctm_now_us() - start >= 5000000ull) { ctm_ctl_log(c, "host config timeout"); return -1; }
            continue;
        }
        if (h.type != CTMB_MSG_HOST_CONFIG || h.payload_len < sizeof(*host_cfg)) {
            ctm_ctl_log(c, "host config unexpected type=%u len=%u", h.type, h.payload_len);
            free(payload);
            return -1;
        }
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        free(payload);
        break;
    }
    if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    return 0;
}

/* Drop every scrap of the previous session. run_session is re-entered on each
 * reconnect and used to reset three things only; each of the rest survived on
 * an incidental guard that happened to hide it (a 150 ms activity window, the
 * 250 ms dedup TTL, an unsigned-underflow comparison) — and rumble_slot had no
 * guard at all, so the first tick of a reconnected session wrote the rumble
 * state the pad had when it fell off.
 *
 * Deliberately NOT reset: the ctm_stats_t counters (lifetime totals the UI
 * panel shows), and the adaptive-latency trio, which
 * is a controller with its own decay rather than leftover state — zeroing it
 * mid-stream would step the pad's jitter buffer, not clean anything up.
 * When: the top of run_session, before the handshake. */
static void session_state_reset(ctm_controller_t *c)
{
    ds5_audio_reset(&c->audio);
    /* Rumble: only the lengths gate a write, so a stale payload with len 0 is
     * unreachable. */
    c->rumble_slot_len[0] = c->rumble_slot_len[1] = 0;
    c->rumble_rr = 0;
    c->last_rumble_write_us = 0;
    /* The output side's own leftovers: the last-audio timestamp that gates
     * rumble slotting, the 0x31 dedup cache (a match against the pre-reconnect
     * frame would swallow the host's first rumble state of the new session) and
     * its telemetry window. */
    ctm_hid_io_session_reset(c->io);
    /* Telemetry windows restart with the session so a 60 s line never mixes
     * two of them. */
    c->st_rumble_coal = 0;
    c->plc_log_next_us = 0;
    c->last_pace_log_us = 0;
    c->net_last_out_us = 0;
    c->net_burst_cur = 0;
    c->net_log_next_us = 0;
    c->st_net_out = c->st_net_gaps30 = c->st_net_idle = 0;
    c->st_net_gap_max_us = 0;
    c->st_net_burst_max = 0;
    c->st_net_skew_min = c->st_net_skew_max = c->st_net_skew_sum = 0;
    /* Re-announce the overlay gate state on the first burst of the session. */
    c->ui_gated_logged = 0;
}

/* --- the pump's periodic work -----------------------------------------------
 * run_session's loop used to inline seven independent periodic tasks, each with
 * its own deadline variable and its own now_us() read, and then re-derive the
 * poll timeout from three of those deadlines by hand at the bottom. Each is a
 * named function on one context now, so a task's inputs are visible in its
 * signature and the loop body reads as the schedule it is.
 *
 * Each task also answers when it next wants to run, and the loop sleeps until
 * the earliest of those, so there is exactly one place that decides how long
 * the pump may sleep. */

/* One session's pump state: the controller plus the two objects that live on
 * run_session's stack, plus the rate-servo feedback cursor. */
typedef struct {
    ctm_controller_t *c;
    ctm_paced_ring_t *paced;
    const ctmb_host_config_t *host_cfg;   /* updated by handle_message */
    /* Rate-servo feedback (ds5_acl_tx_qstats -> the host's pacer). */
    int fb_enabled;
    uint64_t fb_next_us;
    uint32_t fb_last_seq;
    int fb_have_seq;
} ctm_pump_t;

/* One periodic pump task: do whatever is due at this instant, then return the
 * absolute time it next wants to run — or 0 for "nothing pending" (the feature
 * is off, or there is no work waiting). The loop's poll bound is the minimum of
 * the non-zero answers, capped at 50 ms. */
typedef uint64_t (*ctm_tick_t)(ctm_pump_t *p);

/* Release paced output on its schedule, at the pace the current output path can
 * actually sustain. */
static uint64_t tick_paced_drain(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    /* The ~10.6ms host pace (~94/s) was sized for the BT one-outstanding wall
     * on the hidraw write path. The raw-ACL injector bypasses that wall, but at
     * ~94/s the pace barely lags the ~100/s DS5 audio source, so the paced queue
     * parks near-full (up to CTM_PACED_QUEUE_CAP*pace ~= 340ms of feedback latency).
     * While the forwarder is actively injecting, tighten the pace to <=8ms so the
     * queue drains with margin and stays shallow; the hidraw fallback keeps the
     * conservative host pace. */
    uint32_t eff_pace_us = p->host_cfg->bt_pace_us;
    if (c->acl_tx) {
        long ij_ = 0, dp_ = 0;
        int rdy_ = 0;
        ds5_acl_tx_stats(c->acl_tx, &ij_, &dp_, &rdy_);
        if (rdy_ && eff_pace_us > 8000) eff_pace_us = 8000;
    } else {
        /* hidraw path: pace the drain at 3/4 of the host's ADVERTISED
         * cadence. The old hard 8ms clamp assumed "the hidraw write blocks
         * on BT slot anyway, so the kernel serializes for us" — false: the
         * fd is O_NONBLOCK and never pushes back (0 EAGAIN over 24k writes,
         * 08-03), so a backlog shoved in at 8ms just parks in the BT
         * stack's invisible one-outstanding queue where the PLC-fill gate
         * (an empty paced ring) cannot see it and would synth on top of it.
         * 3/4 of the legacy 10667 advert is exactly the old 8000, so a
         * pre-0x39 host is bit-identical; a batched host advertising 21334
         * drains at 16ms ~= the one-outstanding service rate. */
        uint32_t adv = eff_pace_us ? eff_pace_us : 10667;
        eff_pace_us = (adv * 3u) / 4u;
    }
    if (eff_pace_us != c->last_pace_log_us) {
        c->last_pace_log_us = eff_pace_us;
        ctm_ctl_log(c, "paced drain interval %u us (host advertises %u us)",
                    eff_pace_us, p->host_cfg->bt_pace_us);
    }
    ctm_paced_drain(c->io, p->paced, eff_pace_us);
    return ctm_paced_next_us(p->paced);   /* 0 once the ring is empty */
}

/* Rumble rides leftover air slots (stored latest-wins in handle_message): one
 * write per tick, never just before a due audio slot, and rate-capped while
 * audio is live — the ~62/s one-outstanding budget leaves ~15 slots/s beside
 * 46.9/s of 0x39, so a sustained rumble stream physically cannot go faster
 * without starving audio. Latest-wins makes the cap lossless for state (newest
 * rumble always wins); envelope steps in between coalesce. */
static void rumble_slot_run(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    uint64_t rnow = ctm_now_us();
    uint64_t min_gap = audio_live_since(rnow, ctm_hid_io_audio_last_us(c->io))
                           ? c->rumble_min_us : 16000ull;
    uint64_t paced_due = ctm_paced_next_us(p->paced);
    int audio_due_soon = ctm_paced_count(p->paced) > 0 && paced_due != 0 &&
                         paced_due <= rnow + 4000;
    if (audio_due_soon || rnow - c->last_rumble_write_us < min_gap) return;
    for (int k = 0; k < 2; k++) {
        int s = (c->rumble_rr + k) & 1;
        if (!c->rumble_slot_len[s]) continue;
        (void)ctm_hid_io_write(c->io, c->rumble_slot[s], c->rumble_slot_len[s]);
        c->rumble_slot_len[s] = 0;
        c->rumble_rr = s ^ 1;
        c->last_rumble_write_us = rnow;
        break;
    }
}

/* Wanted next at the earliest moment the rate cap allows another write. The
 * deadline can already be in the past when an audio slot is due within 4 ms —
 * that is the point: the drain gets the slot and this asks to be reconsidered
 * immediately afterwards. */
static uint64_t tick_rumble_slot(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    if (!c->rumble_slot_len[0] && !c->rumble_slot_len[1]) return 0;
    rumble_slot_run(p);
    if (!c->rumble_slot_len[0] && !c->rumble_slot_len[1]) return 0;
    uint64_t rnow = ctm_now_us();
    return c->last_rumble_write_us +
           (audio_live_since(rnow, ctm_hid_io_audio_last_us(c->io))
                ? c->rumble_min_us : 16000ull);
}

/* Adaptive pad latency: slider baseline while the link is clean; when the fill
 * has to bridge stalls (adapt_hot_until_us armed by tick_plc_fill), raise the
 * pad's jitter buffer by up to +40ms in ~1.25s, decay ~8ms/s after 10s of
 * quiet. Applied per outbound report by ds5_patch_output via
 * ctm_controller_adapt_latency_ms — the same live-patch path the manual slider
 * uses. */
static uint64_t tick_adapt_slew(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    if (!c->adapt_enabled) return 0;
    uint64_t anow = ctm_now_us();
    if (c->adapt_slew_next_us != 0 && anow < c->adapt_slew_next_us)
        return c->adapt_slew_next_us;
    c->adapt_slew_next_us = anow + 250000ull;
    uint32_t target = anow < c->adapt_hot_until_us ? 40u : 0u;
    uint32_t cur = __atomic_load_n(&c->adapt_lat_add_ms, __ATOMIC_RELAXED);
    if (cur < target) {
        cur = cur + 8 > target ? target : cur + 8;
    } else if (cur > target) {
        cur = cur >= 2 ? cur - 2 : 0;
    }
    if (cur != __atomic_load_n(&c->adapt_lat_add_ms, __ATOMIC_RELAXED)) {
        __atomic_store_n(&c->adapt_lat_add_ms, cur, __ATOMIC_RELAXED);
    }
    return c->adapt_slew_next_us;
}

/* Forward the daemon's inject-queue telemetry (ds5_acl_tx_qstats <- "<tmpl>.st")
 * to the host ~4/s so its pacer can shed TV-side backlog. Capability-gated on
 * HOST_CONFIG reserved[0] — a CTM host advertises nothing and never sees the
 * message type. */
static uint64_t tick_pace_feedback(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    if (!p->fb_enabled || !c->acl_tx) return 0;
    uint64_t fnow = ctm_now_us();
    if (p->fb_next_us == 0) {
        p->fb_next_us = fnow + 250000ull;
        return p->fb_next_us;
    }
    if (fnow < p->fb_next_us) return p->fb_next_us;
    p->fb_next_us = fnow + 250000ull;
    ds5_acl_qstats_t qs;
    /* Only a record the daemon advanced since our last send is news; a frozen
     * seq (unbound link, old daemon) sends nothing and the host falls back to
     * its static pace margin. */
    if (!ds5_acl_tx_qstats(c->acl_tx, &qs) ||
        (p->fb_have_seq && qs.seq == p->fb_last_seq)) {
        return p->fb_next_us;
    }
    p->fb_last_seq = qs.seq;
    p->fb_have_seq = 1;
    ctmb_pace_feedback_t fb;
    memset(&fb, 0, sizeof fb);
    fb.outstanding = qs.outstanding;
    fb.fifo_count = qs.fifo_count;
    fb.maxq = qs.maxq;
    fb.fifo_cap = qs.fifo_cap;
    fb.inj_total = qs.inj_total;
    fb.drop_total = qs.drop_total;
    ctm_ctl_send(c, CTMB_MSG_PACE_FEEDBACK, CTMB_FLAG_OK, 0, &fb, sizeof fb);
    return p->fb_next_us;
}

/* Timer-driven audio-loss concealment. While the game's audio stream is active
 * but a real 0x36 is overdue (the air dropped it) and none waits in the queue,
 * re-inject the last frame so the DS5's rate-matched speaker buffer stays
 * topped up instead of draining into a dropout. Idle >150 ms of real audio ->
 * disarm (genuine silence, not loss). */
static void plc_fill_run(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    uint64_t fnow = ctm_now_us();
    if (!audio_live_since(fnow, c->audio.plc_last_real_us)) {
        c->audio.plc_fill_next_us = 0;
        return;
    }
    if (c->audio.plc_fill_next_us == 0)
        c->audio.plc_fill_next_us = c->audio.plc_last_real_us +
                                    ds5_audio_fill_eff_us(&c->audio);
    int guard = 0;
    while (fnow >= c->audio.plc_fill_next_us && guard++ < 4) {
        if (!ds5_audio_inject_synth(&c->audio)) {
            /* Inject failed (send error during a blackout, or the repeat cap):
             * still advance the deadline. Leaving it in the past makes the
             * poll-timeout bound 0 and busy-spins this RT thread until the
             * 150ms activity window expires. */
            c->audio.plc_fill_next_us = fnow + ds5_audio_fill_eff_us(&c->audio);
            break;
        }
        ctm_ctl_note_output_report(c);
        /* A bridged stall = the pad buffer dipped: arm the adaptive latency
         * window for the next 10s (tick_adapt_slew moves towards it on its own
         * 250ms tick). */
        c->adapt_hot_until_us = ctm_now_us() + 10000000ull;
        c->audio.plc_fill_next_us += ds5_audio_fill_eff_us(&c->audio);
        if (c->audio.plc_fill_next_us + ds5_audio_fill_eff_us(&c->audio) < fnow)
            c->audio.plc_fill_next_us = fnow + ds5_audio_fill_eff_us(&c->audio);
        fnow = ctm_now_us();
    }
}

/* Wanted next at the fill's own deadline. The "is a fill armed" test here is
 * deliberately NOT gated on the paced ring being empty, unlike the run above:
 * the ring can drain during the sleep this bounds, and an unbounded sleep would
 * then miss the slot. That is the condition the hand-rolled bound used. */
static uint64_t tick_plc_fill(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    if (c->audio.plc_fill_enabled && c->audio.plc_have36 &&
        ctm_paced_count(p->paced) == 0) {
        plc_fill_run(p);
    }
    if (c->audio.plc_fill_enabled && c->audio.plc_have36 &&
        c->audio.plc_fill_next_us != 0 &&
        audio_live_since(ctm_now_us(), c->audio.plc_last_real_us)) {
        return c->audio.plc_fill_next_us;
    }
    return 0;
}

/* PLC/60s: what the concealment and the output path actually did this window. */
static uint64_t tick_plc_log(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    if (!c->audio.plc_enabled) return 0;
    uint64_t pnow = ctm_now_us();
    if (c->plc_log_next_us == 0) {
        c->plc_log_next_us = pnow + 60000000ull;
        return c->plc_log_next_us;
    }
    if (pnow < c->plc_log_next_us) return c->plc_log_next_us;

    long inj = 0, drp = 0;
    int rdy = 0;
    if (c->acl_tx) {
        ds5_acl_tx_stats(c->acl_tx, &inj, &drp, &rdy);
    }
    ctm_hid_io_stats_t io_st;
    ctm_hid_io_stats_take(c->io, &io_st);
    ctm_ctl_log(c, "PLC/60s: audio_omit=%lu conceal=%lu fill=%lu dupdrop=%lu fillskip=%lu capdrop=%lu | out36=%lu out39=%lu out31=%lu out32=%lu outX=%lu have36=%d | acl_ready=%d inj=%ld drop=%ld | hid ok=%lu eagain=%lu recov=%lu drop=%lu | dedup31=%lu | in=%lu coal=%lu | rcoal=%lu stale=%lu adapt=%u",
            c->audio.st_audio_omit, c->audio.st_audio_conceal, c->audio.st_audio_fill,
            c->audio.st_fill_dupdrop, c->audio.st_fill_skip, c->audio.st_audio_capdrop,
            io_st.out36, io_st.out39, io_st.out31, io_st.out32, io_st.out_other,
            c->audio.plc_have36,
            rdy, inj, drp,
            io_st.hid_ok, io_st.hid_eagain, io_st.hid_recovered, io_st.hid_dropped,
            io_st.dedup_skipped,
            ctm_stat_get(&c->stats.reports_in),
            ctm_stat_get(&c->stats.coalesced),
            c->st_rumble_coal, c->audio.st_stale_drop,
            (unsigned)__atomic_load_n(&c->adapt_lat_add_ms, __ATOMIC_RELAXED));
    c->audio.st_audio_omit = c->audio.st_audio_conceal = 0;
    c->audio.st_audio_capdrop = c->audio.st_audio_fill = 0;
    c->audio.st_fill_skip = 0;
    c->audio.st_fill_dupdrop = 0;
    c->st_rumble_coal = 0;
    c->audio.st_stale_drop = 0;
    c->plc_log_next_us = pnow + 60000000ull;
    return c->plc_log_next_us;
}

/* NET/60s: downlink arrival pattern + ENet link health (HOL probe, see
 * net_track_output). delay_* = transit over the window's best case (the skew
 * minimum cancels the TV<->host clock offset); drift between the two monotonic
 * clocks is ~ppm-scale, negligible per window against the >=30ms signal we are
 * looking for. */
static uint64_t tick_net_log(ctm_pump_t *p)
{
    ctm_controller_t *c = p->c;
    uint64_t nnow = ctm_now_us();
    if (c->net_log_next_us == 0) {
        c->net_log_next_us = nnow + 60000000ull;
        return c->net_log_next_us;
    }
    if (nnow < c->net_log_next_us) return c->net_log_next_us;

    if (c->st_net_out > 0) {
        int64_t davg = c->st_net_skew_sum / (int64_t)c->st_net_out - c->st_net_skew_min;
        int64_t dmax = c->st_net_skew_max - c->st_net_skew_min;
        ctm_enet_peer_stats_t ps;
        int have_ps = (c->xport.kind == CTM_TRANSPORT_ENET && c->enet &&
                       enet_client_peer_stats(c->enet, &ps) == 0);
        ctm_ctl_log(c, "NET/60s: out=%lu gaps30=%lu gap_max=%.1fms burst_max=%u idle=%lu delay_avg=%.1fms delay_max=%.1fms | rtt=%u+-%ums sent=%lu lost=%lu thr=%u/32",
                c->st_net_out, c->st_net_gaps30,
                (double)c->st_net_gap_max_us / 1000.0,
                c->st_net_burst_max, c->st_net_idle,
                (double)davg / 1000.0, (double)dmax / 1000.0,
                have_ps ? ps.rtt_ms : 0u,
                have_ps ? ps.rtt_variance_ms : 0u,
                have_ps ? (unsigned long)ps.packets_sent : 0ul,
                have_ps ? (unsigned long)ps.packets_lost : 0ul,
                have_ps ? ps.packet_throttle : 0u);
    }
    c->st_net_out = 0;
    c->st_net_gaps30 = 0;
    c->st_net_idle = 0;
    c->st_net_gap_max_us = 0;
    c->st_net_burst_max = 0;
    c->st_net_skew_min = c->st_net_skew_max = c->st_net_skew_sum = 0;
    c->net_log_next_us = nnow + 60000000ull;
    return c->net_log_next_us;
}

/* Run one connected session: handshake, start the input thread (plus the
 * composite sibling readers and the xpad feeder where the type uses them), then
 * the output/feature receive loop + paced drain until the link drops or stop.
 * Tears all of those down on exit. When: per successful connect, from
 * session_main. */
static void run_session(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                        const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_host_config_t host_cfg;
    ctm_paced_ring_t paced;
    memset(&host_cfg, 0, sizeof(host_cfg));
    ctm_paced_reset(&paced);
    c->link_down = 0;
    session_state_reset(c);

    if (handshake(c, caps, report_desc, report_desc_len, &host_cfg) != 0) return;

    { uint8_t drain[64]; while (read(c->wake_pipe[0], drain, sizeof(drain)) > 0) { /* discard */ } }

    /* Composite: open siblings + resolve the primary endpoints BEFORE the input
     * thread starts (it tags primary input with the primary IN endpoint). */
    if (c->ops->composite && !c->flydigi_xinput_evdev_only) {
        ctm_composite_open(c->comp);
    }
    c->comp_run = 1;   /* before the feeder starts: its loop gates on comp_run */
    if (c->ops->composite_evdev_gamepad) {
        if (ctm_xpad_start(c->xpad, c->comp, c->dev.usb_busid) != 0 &&
            c->ops->kind && strcmp(c->ops->kind, "flydigi") == 0 &&
            (flydigi_is_xinput_evdev_only_for_busid(c->dev.usb_busid) ||
             flydigi_is_xinput_mode_for_busid(c->dev.usb_busid))) {
            ctm_ctl_log(c, "xpad evdev feeder failed; aborting xinput session");
            c->comp_run = 0;
            ctm_composite_shutdown(c->comp);
            return;
        }
    }

    if (c->flydigi_xinput_evdev_only) {
        c->input_thread_started = 0;
    } else if (pthread_create(&c->input_thread, NULL, input_thread_main, c) == 0) {
        c->input_thread_started = 1;
    } else {
        ctm_ctl_log(c, "input thread failed errno=%d", errno);
        c->comp_run = 0;
        ctm_xpad_stop(c->xpad);
        ctm_composite_shutdown(c->comp);
        return;
    }
    if (c->ops->composite && !c->flydigi_xinput_evdev_only) {
        ctm_composite_start_readers(c->comp);
    }
    ctm_ctl_log(c, "active host=%s port=%d path=%s product=%s transport=%s",
         c->host, c->port, c->dev.path, caps->product,
         c->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 1;
    c->st_transport_enet = (c->xport.kind == CTM_TRANSPORT_ENET) ? 1 : 0;
    pthread_mutex_unlock(&c->status_mutex);

    ctm_pump_t pump;
    memset(&pump, 0, sizeof(pump));
    pump.c = c;
    pump.paced = &paced;
    pump.host_cfg = &host_cfg;
    pump.fb_enabled = (host_cfg.reserved[0] & CTMB_HOSTCFG_PACE_FEEDBACK) != 0;
    /* FIFO-depth gating: the deep (10) elastic FIFO is only safe under a
     * rate-servo host — it is the servo that bounds the parked latency. A
     * non-servo host (CTM, or a rolled-back Vibepollo) gets the shallow
     * depth explicitly, so a previous session's override never lingers. */
    if (c->acl_tx) {
        ds5_acl_tx_set_fifo_depth(c->acl_tx, pump.fb_enabled ? 10 : 3);
    }

    /* The periodic half of the pump, in the order it has always run. */
    static const ctm_tick_t k_ticks[] = {
        tick_paced_drain,
        tick_rumble_slot,
        tick_adapt_slew,
        tick_pace_feedback,
        tick_plc_fill,
        tick_plc_log,
        tick_net_log,
    };

    int link_alive = 1;
    while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&c->link_down, __ATOMIC_RELAXED) && link_alive) {
        /* Fire everything due, and remember the earliest thing that is not.
         * Nothing but this loop decides how long the pump may sleep: a task
         * that wants to run at T cannot be slept past T, and a task that wants
         * nothing cannot hold the sleep open. */
        uint64_t next_due_us = 0;
        for (size_t i = 0; i < sizeof(k_ticks) / sizeof(k_ticks[0]); ++i) {
            uint64_t due = k_ticks[i](&pump);
            if (due != 0 && (next_due_us == 0 || due < next_due_us)) next_due_us = due;
        }

        /* 50 ms ceiling: the ENet timers still need servicing on an idle link,
         * and a task that wants nothing must not park the pump indefinitely. */
        int timeout_ms = 50;
        if (next_due_us != 0) {
            uint64_t now = ctm_now_us();
            timeout_ms = next_due_us <= now ? 0 : (int)((next_due_us - now) / 1000u);
            if (timeout_ms > 50) timeout_ms = 50;
        }
        if (c->xport.kind == CTM_TRANSPORT_ENET) {
            /* Poll-driven pump: the reader thread wakes us via the client's
             * eventfd the instant it enqueues a report, so input no longer waits
             * for a blind 1ms tick; the timeout (paced deadline, capped <=10ms
             * inside) still bounds the idle sleep so ENet timers fire. All host
             * access stays on this session thread. */
            if (ctm_transport_service_wait(&c->xport, (unsigned int)timeout_ms) < 0) {
                ctm_ctl_log(c, "ENet link lost");
                link_alive = 0;
            } else {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                while (c_recv(c, &h, &payload) == 1) {
                    handle_message(c, &host_cfg, &paced, &h, payload);
                    free(payload);
                    payload = NULL;
                }
            }
        } else {
            struct pollfd pfd;
            /* Accessor, not c->xport.fd: the fd belongs to the transport and
             * only this (the owning session) thread may hold it, and only for
             * the length of the poll. The stop path no longer reaches in here —
             * it goes through ctm_transport_cancel. */
            pfd.fd = ctm_transport_pollfd(&c->xport); pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                link_alive = 0;
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                link_alive = 0;
            } else if (pfd.revents & POLLIN) {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                if (c_recv(c, &h, &payload) != 1) {
                    free(payload);
                    link_alive = 0;
                } else {
                    handle_message(c, &host_cfg, &paced, &h, payload);
                    free(payload);
                }
            }
        }
    }

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 0;
    pthread_mutex_unlock(&c->status_mutex);

    c->comp_run = 0;
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    ctm_feature_worker_stop(c->feat);   /* joins BEFORE the transport goes away below */
    ctm_xpad_stop(c->xpad);
    if (c->input_thread_started) {
        pthread_join(c->input_thread, NULL);
        c->input_thread_started = 0;
    }
    ctm_composite_shutdown(c->comp);
}

/* --- pump policy + env overrides ------------------------------------------
 * One env-overridable pump tunable. The DEFAULT is deliberately NOT in this
 * table: it comes from the type's ctm_pump_policy_t. That is also why lo/hi
 * are applied only to a value the environment actually supplied — clamping an
 * unset knob would drag a type that says 0 on purpose up to the DS5's floor. */
typedef enum {
    KNOB_INT,           /* number, clamped to [lo,hi], stored as int */
    KNOB_U32,           /* number, clamped to [lo,hi], stored as uint32_t */
    KNOB_FLAG_UNLESS_0, /* int: on unless the value is exactly "0" */
    KNOB_FLAG_IF_1,     /* int: on only if the value is exactly "1" */
} ctm_knob_kind_t;

typedef struct {
    const char *env;
    size_t off;                 /* offset into ctm_controller_t */
    ctm_knob_kind_t kind;
    long lo, hi;                /* KNOB_INT / KNOB_U32 only */
} ctm_env_knob_t;

static const ctm_env_knob_t k_knobs[] = {
    { "CTM_HID_WAIT_MS",       offsetof(ctm_controller_t, hid_wait_ms),
      KNOB_INT, 0, 20 },
    { "CTM_RUMBLE_MIN_US",     offsetof(ctm_controller_t, rumble_min_us),
      KNOB_U32, 8000, 200000 },
    { "CTM_AUDIO_PLC_FILL_US", offsetof(ctm_controller_t, audio.plc_fill_interval_us),
      KNOB_U32, 5000, 20000 },
    { "CTM_AUDIO_PLC",         offsetof(ctm_controller_t, audio.plc_enabled),
      KNOB_FLAG_UNLESS_0, 0, 0 },
    { "CTM_AUDIO_PLC_FILL",    offsetof(ctm_controller_t, audio.plc_fill_enabled),
      KNOB_FLAG_IF_1, 0, 0 },
    { "CTM_ADAPT_LATENCY",     offsetof(ctm_controller_t, adapt_enabled),
      KNOB_FLAG_IF_1, 0, 0 },
};

/* Seed the pump tunables from the type's policy, then let the environment
 * override them. Runs for EVERY type: the knobs used to live inside
 * `if (ops->raw_acl_output)`, so setting one had no effect on anything but a
 * DS5, and the fields simply stayed 0. The policy defaults reproduce those
 * zeros, so with no env set every type behaves exactly as before.
 * When: once per session thread, before the HID open. */
static void apply_pump_policy(ctm_controller_t *c)
{
    static const ctm_pump_policy_t k_policy_off = {0};
    const ctm_pump_policy_t *p = (c->ops && c->ops->policy) ? c->ops->policy : &k_policy_off;

    c->hid_wait_ms = p->hid_eagain_wait_ms;
    c->dedup_report_id = p->dedup_report_id;
    c->rumble_min_us = p->rumble_min_us;
    c->adapt_enabled = p->adaptive_latency ? 1 : 0;
    c->audio.plc_enabled = p->audio_plc ? 1 : 0;
    c->audio.plc_fill_enabled = p->audio_plc_fill ? 1 : 0;
    /* Not per type: the fill's slot period is a property of the DS5's ~100 Hz
     * audio clock, not of the controller. 16000 = 1.5x the 0x36 slot period
     * (10667; 0x39 doubles both via ds5_audio_fill_eff_us -> 32000 vs 21334).
     * The old 10000 default sat BELOW the real cadence, so every marginally
     * late real frame drew a synth — a duplicate-injection storm on a jittery
     * link. 1.5 slots means a synth fires only for a genuinely stalled slot,
     * still early enough to bridge a 60ms pad buffer. */
    c->audio.plc_fill_interval_us = 16000;

    for (size_t i = 0; i < sizeof(k_knobs) / sizeof(k_knobs[0]); ++i) {
        const ctm_env_knob_t *k = &k_knobs[i];
        const char *v = getenv(k->env);
        if (!v) continue;
        void *field = (char *)c + k->off;
        switch (k->kind) {
            case KNOB_INT:
            case KNOB_U32: {
                long n = atol(v);
                if (n < k->lo) n = k->lo;
                if (n > k->hi) n = k->hi;
                if (k->kind == KNOB_INT) *(int *)field = (int)n;
                else *(uint32_t *)field = (uint32_t)n;
                break;
            }
            case KNOB_FLAG_UNLESS_0:
                *(int *)field = strcmp(v, "0") != 0 ? 1 : 0;
                break;
            case KNOB_FLAG_IF_1:
                *(int *)field = strcmp(v, "1") == 0 ? 1 : 0;
                break;
        }
    }
    /* CTM_DEDUP is a kill switch over a report id, not a boolean field: only
     * "0" is meaningful; anything else keeps the type's own id. */
    const char *denv = getenv("CTM_DEDUP");
    if (denv && strcmp(denv, "0") == 0) c->dedup_report_id = 0;
    /* The fill is a mode of the PLC, not an independent feature. */
    if (!c->audio.plc_enabled) c->audio.plc_fill_enabled = 0;

    ctm_ctl_log(c, "pump policy: idle=%ums hidwait=%dms dedup=0x%02x rumble_min=%uus "
               "plc=%d fill=%d(%uus) adapt=%d",
            p->input_idle_timeout_ms, c->hid_wait_ms, c->dedup_report_id,
            c->rumble_min_us, c->audio.plc_enabled, c->audio.plc_fill_enabled,
            c->audio.plc_fill_interval_us, c->adapt_enabled);
}

/* Session thread body: open HID + wake pipe once, then the dual-probe
 * connect -> grab -> run_session -> release -> disconnect loop until plug_out.
 * When: the controller's own thread, started by plug_in. */
static void *session_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    ctl_set_rt_prio(c, "ctm-session", 15);
    ctmb_device_caps_t caps;
    uint8_t report_desc[MAX_REPORT_DESCRIPTOR];
    uint32_t report_desc_len = 0;

    if (c->ops && c->ops->raw_acl_output) {
        /* DEFAULT ON again since 1.3.0 — the daemon ships in this IPK.
         *
         * It was turned OFF in 1.0.60 for one reason only: ds5_txd had to be
         * hand-installed under /var/lib/webosbrew, so a stock install did not have
         * it and the default had to work without it. Batched 0x39 made that
         * viable (40.8 reports/s against the ~62/s one-outstanding ceiling), but
         * it did not make it better: hidraw still carries 30-67 ms of air jitter,
         * which is why a 60 ms pad buffer drops out on the daemon-free path and
         * needs the adaptive-latency ramp to stay clean.
         *
         * Now that the daemon travels with the app and elevates itself
         * (ds5_service.c), the injector is available on a stock install and gets
         * the default back. Turning it on unconditionally is safe: the forwarder
         * is inert until the daemon publishes a valid DS5T readiness template, and
         * ds5_acl_tx.c watches that template live — so a TV without Homebrew
         * Channel, or one where elevation failed, simply keeps running the
         * daemon-free hidraw path with the 1.0.61/62 fill, pacing and rumble-
         * slotting machinery, which stays in place untouched.
         *
         * CTM_RAW_ACL=0 forces the daemon-free path (the A/B handle, now inverted). */
        const char *env = getenv("CTM_RAW_ACL");
        if (!env || strcmp(env, "0") != 0) {
            /* Pass the controller's BT address so the forwarder tags each report
             * to THIS pad's inject link (multi-controller: each DS5 gets its own
             * daemon credit window / template). Empty for USB pads -> legacy
             * untagged wire (daemon primary link), unchanged single-pad path. */
            c->acl_tx = ds5_acl_tx_start(0, c->dev.mac, acl_log_cb, c);
            ctm_ctl_log(c, "raw-ACL output %s%s%s", c->acl_tx ? "enabled" : "unavailable (hidraw)",
                    (c->acl_tx && c->dev.mac[0]) ? " mac=" : "",
                    (c->acl_tx && c->dev.mac[0]) ? c->dev.mac : "");
        } else {
            ctm_ctl_log(c, "raw-ACL output disabled by CTM_RAW_ACL=0 (daemon-free hidraw path; "
                       "needs 0x39 host audio)");
        }
    }

    /* Pump policy: the type's measured defaults (ctm_pump_policy_t, defined
     * next to each ops table), then the env overrides on top. */
    apply_pump_policy(c);

    ctm_hid_io_set_tunables(c->io, c->hid_wait_ms, c->dedup_report_id);
    ctm_hid_io_set_acl_tx(c->io, c->acl_tx);

    int hid_fd = open_device_any_tier(c, &caps, report_desc, &report_desc_len);
    ctm_hid_io_set_fd(c->io, hid_fd);
    if (hid_fd < 0) {
        ctm_ctl_log(c, "hid open failed path=%s errno=%d", c->dev.path, errno);
        if (c->acl_tx) {
            ds5_acl_tx_stop(c->acl_tx);
            c->acl_tx = NULL;
            ctm_hid_io_set_acl_tx(c->io, NULL);
        }
        c->stop = 1;
        c->session_finished = 1;
        return NULL;
    }
    /* Composite primary endpoints are resolved by ctm_composite_open()
     * (reliable /sys/class/input path), at run_session start. */
    if (pipe(c->wake_pipe) != 0) {
        ctm_ctl_log(c, "wake pipe failed errno=%d", errno);
        if (c->acl_tx) {
            ds5_acl_tx_stop(c->acl_tx);
            c->acl_tx = NULL;
            ctm_hid_io_set_acl_tx(c->io, NULL);
        }
        c->stop = 1;
        c->session_finished = 1;
        return NULL;
    }
    (void)fcntl(c->wake_pipe[0], F_SETFL, fcntl(c->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(c->wake_pipe[1], F_SETFL, fcntl(c->wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    /* Reconnect pacing for a session that fails FAST. The 500ms sleep below is
     * inside the connect-failure condition, so a session that connects and then
     * aborts immediately (the flydigi abort path, where the xpad feeder cannot
     * start) re-ran connect + ENUM + HELLO with no delay at all: tens of full
     * handshakes per second, forever, from a SCHED_FIFO priority-15 thread,
     * each carrying the composite enumeration blob. Elapsed-time based rather
     * than failure-counting, so it cannot mistake a long, healthy session that
     * happens to end for a hot loop. Nothing is streaming while this sleeps —
     * the session is already down — and it aborts on stop within 50ms, so it
     * does not delay plug-out. */
    uint32_t backoff_ms = 0;

    while (!c->stop) {
        /* Re-entry after a dropped controller link (the input thread's liveness
         * timeout, or a POLLHUP, set link_down): the hid_fd still points at the
         * pad that fell off BT, so without this we'd re-run run_session on a dead
         * fd forever (input never recovers until a manual re-bridge). Reopen it
         * through the SAME tier ladder the initial open used — this used to call
         * open_hid() alone, which cannot reach the nodes tiers 2 and 3 reach, so
         * a session established on one of those could never re-attach and spun
         * here at 500 ms forever with the gamepad feeder already stopped. Tier 1
         * validates VID:PID (so it never grabs the wrong node) and falls back to
         * the root broker if the static jail node went stale; -1 just means the
         * device isn't back yet, so retry with a 500ms backoff until it
         * re-appears (or the stream stops). */
        if (c->link_down) {
            int old = ctm_hid_io_fd(c->io);
            if (old >= 0) { close(old); ctm_hid_io_set_fd(c->io, -1); }
            int announced = 0;
            while (!c->stop) {
                int fd = open_device_any_tier(c, &caps, report_desc, &report_desc_len);
                if (fd >= 0) {
                    ctm_hid_io_set_fd(c->io, fd);
                    ctm_ctl_log(c, "controller re-attached after link loss");
                    break;
                }
                if (!announced) { ctm_ctl_log(c, "controller dropped; waiting to re-attach"); announced = 1; }
                for (int s = 0; s < 500 && !c->stop; s += 50) usleep(50000);
            }
            if (c->stop) break;
            c->link_down = 0;
        }
        /* ENet budget >=1200ms: the vendored enet's initial RTO is 500ms, so the
         * old 400ms window could never retransmit a lost connect datagram - one
         * lost UDP packet silently pinned the whole session to TCP fallback. */
        while (!c->stop &&
               ctm_transport_connect_once(&c->xport, c->host, c->port, 1200) != 0) {
            for (int slept = 0; slept < 500 && !c->stop; slept += 50) usleep(50000);
        }
        if (c->stop) break;
        ctm_ctl_log(c, "connected via %s", c->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");
        if (c->xport.kind != CTM_TRANSPORT_ENET) {
            ctm_ctl_log(c, "WARN: TCP fallback transport (retransmit head-of-line blocking); check UDP path to host");
        }

        if (c->ops->grab_evdev) grab_matching_evdev(c);
        if (c->ops->on_plug_init) c->ops->on_plug_init(c, &c->xport);
        uint64_t session_start_us = ctm_now_us();
        run_session(c, &caps, report_desc, report_desc_len);
        uint64_t session_us = ctm_now_us() - session_start_us;
        release_evdev_grabs(c);

        ctm_transport_disconnect(&c->xport);
        if (!c->stop) ctm_ctl_log(c, "link lost; retrying probe loop");
        if (session_us < 1000000ull) {
            backoff_ms = backoff_ms ? (backoff_ms >= 2500 ? 5000 : backoff_ms * 2) : 500;
            if (!c->stop) {
                ctm_ctl_log(c, "session lasted %llums; waiting %ums before reconnecting",
                        (unsigned long long)(session_us / 1000), backoff_ms);
            }
            for (uint32_t slept = 0; slept < backoff_ms && !c->stop; slept += 50) {
                usleep(50000);
            }
        } else {
            backoff_ms = 0;
        }
    }
    if (c->acl_tx) {
        long inj = 0, drp = 0;
        ds5_acl_tx_stats(c->acl_tx, &inj, &drp, NULL);
        ctm_ctl_log(c, "raw-ACL output: injected=%ld dropped=%ld", inj, drp);
        ds5_acl_tx_stop(c->acl_tx);
        c->acl_tx = NULL;
        ctm_hid_io_set_acl_tx(c->io, NULL);
    }
    c->stop = 1;
    c->session_finished = 1;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

/* Build an idle controller for a detected device (factory picks its ops).
 * When: the UI/monitor decides to offer a device; before plug_in. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev)
{
    if (!dev) return NULL;
    ctm_controller_t *c = (ctm_controller_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->dev = *dev;
    c->ops = ctm_controller_ops_for(dev);
    c->vid_num = (unsigned int)strtoul(dev->vid, NULL, 16);
    c->pid_num = (unsigned int)strtoul(dev->pid, NULL, 16);
    c->wake_pipe[0] = -1;
    c->wake_pipe[1] = -1;
    c->flydigi_xinput_evdev_only = 0;
    c->dummy_hid_pipe_wr = -1;
    ds5_audio_init(&c->audio, audio_synth_write_cb, c);
    for (int i = 0; i < MAX_EVDEV_GRABS; ++i) c->evdev_grabs[i].fd = -1;
    pthread_mutex_init(&c->settings_mutex, NULL);
    pthread_mutex_init(&c->status_mutex, NULL);
    /* The four subsystems. Each is empty and inert until a session configures
     * it; a failure here means the controller cannot bridge at all, so it is
     * reported as a create failure rather than papered over per call site. */
    c->io = ctm_hid_io_create(c, c->ops, &c->audio);
    c->comp = ctm_composite_create(c, c->vid_num, c->pid_num, dev->usb_busid, dev->path);
    c->xpad = ctm_xpad_create(c);
    c->feat = ctm_feature_worker_create(c);
    if (!c->io || !c->comp || !c->xpad || !c->feat) {
        ctm_controller_destroy(c);
        return NULL;
    }
    return c;
}

/* Open this controller's per-MAC log file (/tmp/ctm-<mac>.log). When: plug_in. */
static void open_log(ctm_controller_t *c)
{
    char mac[64], name[128];
    size_t o = 0;
    for (size_t i = 0; c->dev.mac[i] && o + 1 < sizeof(mac); ++i) {
        char ch = c->dev.mac[i];
        if (ch != ':') mac[o++] = ch;
    }
    mac[o] = '\0';
    snprintf(name, sizeof(name), "/tmp/ctm-%s.log", mac[0] ? mac : c->ops->kind);
    c->log = fopen(name, "a");
}

/* Start bridging: open log, bring up ENet + transport, launch the session
 * thread. Returns once the thread is started (connect happens async). When: the
 * user clicks Plug in. */
/* Composite: hand the controller the forwarded enumeration payload (CTMB_MSG_ENUM),
 * which handshake sends verbatim before HELLO. Copies the bytes. When: app plug-in. */
void ctm_controller_set_enum_payload(ctm_controller_t *c, const uint8_t *payload, int len)
{
    if (!c) return;
    free(c->enum_payload);
    c->enum_payload = NULL;
    c->enum_payload_len = 0;
    if (payload && len > 0) {
        c->enum_payload = (uint8_t *)malloc((size_t)len);
        if (c->enum_payload) {
            memcpy(c->enum_payload, payload, (size_t)len);
            c->enum_payload_len = len;
        }
    }
}

int ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port)
{
    if (!c || !host || !host[0] || port <= 0) return -1;
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->stop = 0;
    c->link_down = 0;
    c->session_finished = 0;
    open_log(c);

    pthread_once(&g_enet_once, enet_global_init_once);
    if (g_enet_ready) {
        c->enet = enet_client_create();
        if (!c->enet) ctm_ctl_log(c, "enet client create failed; ENet disabled");
    }
    ctm_transport_init(&c->xport, c->enet);

    if (pthread_create(&c->session_thread, NULL, session_main, c) != 0) {
        ctm_ctl_log(c, "session thread failed errno=%d", errno);
        ctm_transport_destroy(&c->xport);
        if (c->enet) { enet_client_destroy(c->enet); c->enet = NULL; }
        if (c->log) { fclose(c->log); c->log = NULL; }
        return -1;
    }
    c->session_started = 1;
    return 0;
}

/* Stop bridging: signal stop, wake + join the session thread, then close the
 * HID fd, transport, evdev grabs, and log. When: the user clicks Plug out, or
 * the device disconnects. */
void ctm_controller_plug_out(ctm_controller_t *c)
{
    if (!c) return;
    c->stop = 1;
    /* Reaches the session thread wherever it is blocked — including inside the
     * TCP connect, which the old shutdown(c->xport.fd) could not touch because
     * the fd is not published until the connect returns. That gap is why a
     * plug-out against a powered-down host used to hang the join, and with it
     * the LVGL thread, for the kernel's whole SYN-retry budget. */
    ctm_transport_cancel(&c->xport);
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    if (c->session_started) {
        pthread_join(c->session_thread, NULL);
        c->session_started = 0;
    }
    if (c->acl_tx) { /* defensive: session_main normally stops it before exit */
        ds5_acl_tx_stop(c->acl_tx);
        c->acl_tx = NULL;
        ctm_hid_io_set_acl_tx(c->io, NULL);
    }
    ctm_transport_disconnect(&c->xport);
    ctm_transport_destroy(&c->xport);
    int hid_fd = ctm_hid_io_fd(c->io);
    if (hid_fd >= 0) { close(hid_fd); ctm_hid_io_set_fd(c->io, -1); }
    if (c->dummy_hid_pipe_wr >= 0) { close(c->dummy_hid_pipe_wr); c->dummy_hid_pipe_wr = -1; }
    if (c->wake_pipe[0] >= 0) { close(c->wake_pipe[0]); c->wake_pipe[0] = -1; }
    if (c->wake_pipe[1] >= 0) { close(c->wake_pipe[1]); c->wake_pipe[1] = -1; }
    if (c->enet) { enet_client_destroy(c->enet); c->enet = NULL; }
    release_evdev_grabs(c);
    if (c->log) { fclose(c->log); c->log = NULL; }
}

/* Push new UI settings to the controller (stored + forwarded to ops). When: a
 * detail-window slider/toggle changes while plugged in. */
void ctm_controller_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s)
{
    if (!c || !s) return;
    pthread_mutex_lock(&c->settings_mutex);
    c->settings = *s;
    pthread_mutex_unlock(&c->settings_mutex);
    if (c->ops->set_settings) c->ops->set_settings(c, s);
}

/* Read the controller's live settings (snapshot). When: UI refresh, or a
 * patch_output hook reading current values. */
void ctm_controller_get_settings(ctm_controller_t *c, tv_bridge_worker_settings_t *out)
{
    if (!c || !out) return;
    *out = copy_settings(c);
}

/* True once the session thread has exited while the controller is still
 * plugged (send-failure race, hid open failure). When: the autoplug reconcile,
 * to tear down + re-plug a zombie session the device-vanish reap can't see. */
bool ctm_controller_finished(ctm_controller_t *c)
{
    return c && c->session_started && c->session_finished;
}

/* Snapshot live bridging status for the UI panel. When: the UI status timer
 * (~500 ms). connected/transport/last_event are read under status_mutex; the
 * report counters come from ctm_stats_t, so each one is a single relaxed atomic
 * load against many writer threads (see the struct comment there). No lock and
 * no ordering: the panel wants a recent number, not a consistent set of four,
 * and the counters are statistics — advisory because of what they mean, not
 * because anything makes them stable while this reads them. */
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out)
{
    if (!c || !out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->status_mutex);
    out->connected = c->st_connected ? true : false;
    out->transport_enet = c->st_transport_enet ? true : false;
    snprintf(out->last_event, sizeof(out->last_event), "%s", c->st_last_event);
    pthread_mutex_unlock(&c->status_mutex);
    out->reports_in = ctm_stat_get(&c->stats.reports_in);
    out->reports_out = ctm_stat_get(&c->stats.reports_out);
    uint64_t now = ctm_now_us();
    uint64_t upd = __atomic_load_n(&c->battery_updated_us, __ATOMIC_ACQUIRE);
    out->battery_level  = __atomic_load_n(&c->battery_level,  __ATOMIC_ACQUIRE);
    out->battery_status = __atomic_load_n(&c->battery_status, __ATOMIC_ACQUIRE);
    out->battery_valid  = (upd != 0 && (now - upd) < 5000000ull);
}

/* Free an idle (already plugged-out) controller. When: the device is removed
 * from the list. */
void ctm_controller_destroy(ctm_controller_t *c)
{
    if (!c) return;
    pthread_mutex_destroy(&c->settings_mutex);
    pthread_mutex_destroy(&c->status_mutex);
    ctm_feature_worker_destroy(c->feat);
    ctm_xpad_destroy(c->xpad);
    ctm_composite_destroy(c->comp);
    ctm_hid_io_destroy(c->io);
    free(c->enum_payload);
    free(c);
}

/* --- factory: specific types first, generic last ------------------------- */

static const ctm_controller_ops_t *const k_registry[] = {
    &ctm_controller_flydigi_ops,
    &ctm_controller_steam_puck_ops,
    &ctm_controller_ds5_ops,
    &ctm_controller_ds4_ops,
    &ctm_controller_xbox_ops,
    &ctm_controller_generic_ops,
};

const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev)
{
    for (size_t i = 0; i < sizeof(k_registry) / sizeof(k_registry[0]); ++i) {
        const ctm_controller_ops_t *ops = k_registry[i];
        if (ops->matches && ops->matches(dev)) return ops;
    }
    return &ctm_controller_generic_ops;
}
