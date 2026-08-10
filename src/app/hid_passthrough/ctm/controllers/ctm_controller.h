#ifndef CTM_CONTROLLER_H
#define CTM_CONTROLLER_H

/* Controller abstraction (D2). One mechanism per detected controller: each
 * type supplies an ops vtable; the factory picks the right ops for a device.
 *
 * STAGE 1 (scaffold, this commit): the interface + classification (matches) +
 * the factory. The shared byte pump in controller_common.c and the wiring into
 * plug_in_item are STAGE 2 — until then these ops do not yet drive live
 * sessions; the proven tv_bridge_worker / ctm_hidraw_bridge paths still run.
 *
 * The layer is UI-independent: the app fills a neutral ctm_controller_dev_t
 * from its logical_device_t, so controllers/ does not depend on app/ types. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_transport.h"
#include "ctm_settings.h"       /* tv_bridge_worker_settings_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char vid[8];
    char pid[8];
    char bus[8];      /* "USB" / "BT" */
    char name[128];
    char path[64];    /* /dev/hidrawN */
    char mac[64];
    char usb_busid[64]; /* USB port id (e.g. "5-1") for composite sibling resolve */
} ctm_controller_dev_t;

typedef struct ctm_controller ctm_controller_t;   /* opaque; defined in stage 2 */

/* Per-type pump policy: the knobs the shared pump used to hardcode for the
 * DualSense. Every ops table carries one, so a type's behaviour is readable in
 * one literal instead of spread over conditionals in controller_common.c.
 *
 * These are DEFAULTS. controller_common.c's k_knobs[] table lets an env var
 * override each of them at session start — uniformly for every type, which is
 * new: the getenv() block used to sit inside `if (ops->raw_acl_output)`, so
 * five of these were silently inert for everything but the DS5. A type that
 * wants a feature off must therefore say 0 here, not rely on never reaching
 * the code. */
typedef struct {
    /* Declare the link down after this many ms with no input report and force
     * a session teardown + reconnect. 0 = no watchdog.
     *
     * Only sound for a type whose bridged hidraw node genuinely streams while
     * the pad is connected — a DS5/DS4 over BT emits ~250 reports/s even idle,
     * so silence really does mean the link died (and the app's static jail
     * hidraw node never raises POLLHUP when it does, which is why the timeout
     * exists at all). For a type whose bridged node is silent by design — the
     * Flydigi XInput handshake node, whose gamepad data arrives via the xpad
     * evdev feeder, or any relay type that only reports on user action — this
     * MUST stay 0: the watchdog would tear the session down every 2 s forever,
     * killing gamepad input on each cycle. */
    uint32_t input_idle_timeout_ms;
    /* ms to wait for POLLOUT after a hidraw write returns EAGAIN before giving
     * up on the report. 0 = don't wait. */
    int      hid_eagain_wait_ms;
    /* Output report id that may be skipped when byte-identical to the last one
     * actually sent (bounded by DEDUP31_TTL_US). 0 = no dedup. */
    uint8_t  dedup_report_id;
    /* Minimum gap between rumble writes while an audio stream is live. Read
     * only on the hidraw audio path (see the rumble-slot drain in the pump). */
    uint32_t rumble_min_us;
    /* DS BT audio concealment (ctm_ds5_audio.c). audio_plc = splice a cached
     * audio block into a 0x36 that arrived without one; audio_plc_fill =
     * additionally re-inject the last frame when a real one is overdue;
     * adaptive_latency = raise the pad's jitter buffer while the fill bridges.
     * The last two are OFF by design on every type — see the commentary at the
     * DS5 literal for the measurements behind that. */
    bool     audio_plc;
    bool     audio_plc_fill;
    bool     adaptive_latency;
} ctm_pump_policy_t;

typedef struct {
    const char *kind;   /* "ds5" / "ds4" / "xbox" / "steam_puck" / "generic" */

    /* Behaviour flags preserving each path's proven semantics in the shared
     * pump. DS (worker) = all true; puck/xbox/generic (relay) = all false. */
    bool needs_host_config;   /* block for HOST_CONFIG after HELLO (DS pacing) */
    bool grab_evdev;          /* EVIOCGRAB the device's evdev nodes (BT/DS) */
    bool request_bt_mode;     /* send the Sony feature-0x05 full-BT-mode probe */
    bool composite;           /* forward EVERY HID interface, each tagged by its IN
                               * endpoint (puck); host plugs the whole composite. */
    bool composite_evdev_gamepad; /* Flydigi XInput: feed xpad/js evdev as HID on the
                                   * gamepad interface IN endpoint (no hidraw). */
    bool raw_acl_output;          /* DS5: inject high-rate output via raw HCI-ACL
                                   * forwarder; falls back to hidraw on failure. */

    /* Pump tunables for this type. All six ops tables in this tree set one;
     * apply_pump_policy() reads a NULL policy as all-zero (every feature off,
     * no watchdog), so a type added without one gets the safe default rather
     * than the DualSense's. */
    const ctm_pump_policy_t *policy;

    /* Does this type claim the device? Factory tries specific types first,
     * generic last. */
    bool (*matches)(const ctm_controller_dev_t *dev);

    /* Choose which /dev/hidrawN to bridge for composite devices. NULL => the
     * app default (dev->path). steam_puck overrides to pick the gamepad
     * interface (fixes the §6a "first hidraw wins" bug). */
    int (*select_node)(const ctm_controller_dev_t *dev, char *out, size_t out_len);

    /* Optional one-shot init after plug, before the pump starts (xbox GIP
     * handshake, puck lizard-mode exit). NULL => none. */
    int (*on_plug_init)(ctm_controller_t *c, ctm_transport_t *t);

    /* Patch an outbound report in place before it reaches the device (DS audio
     * route / volume / CRC). Returns nonzero to DROP the report (suppress the
     * write), 0 to write `*len` bytes. NULL => verbatim forward. */
    int (*patch_output)(ctm_controller_t *c, uint8_t *buf, size_t *len);

    /* Live UI settings update (DS sliders). NULL => ignored. */
    void (*set_settings)(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);

    /* Optional per-input-report hook (DS5 battery, etc.). NULL => none. */
    void (*on_input_report)(ctm_controller_t *c, const uint8_t *data, size_t len);

    /* Neutralize the gamepad state of an input report IN PLACE (sticks
     * centered, buttons/triggers/touch released, gyro zeroed) — used while the
     * streaming overlay owns the controller so UI navigation stops leaking
     * into the game. Reports keep flowing at the native rate (battery, seq and
     * timestamps stay live), which avoids stuck-input edge cases that dropping
     * reports would create. NULL => reports forward unmodified (types without
     * a known input layout keep today's leaky behavior on purpose: a wrongly
     * guessed offset or a dropped release event is worse than the leak). */
    void (*neutralize_input)(ctm_controller_t *c, uint8_t *buf, size_t len);
} ctm_controller_ops_t;

/* Live bridging status — read-only snapshot for the UI status panel. */
typedef struct {
    bool connected;              /* a session is up (handshake done, looping) */
    bool transport_enet;         /* true = ENet/UDP, false = TCP */
    unsigned long reports_in;    /* input reports forwarded to the host */
    unsigned long reports_out;   /* output reports written to the device */
    char last_event[96];         /* most recent controller log line */
    uint8_t battery_level;       /* 0-10; 255 = unknown */
    uint8_t battery_status;      /* 0=discharging, 1=charging, 2=full */
    bool battery_valid;          /* true if updated within the last 5 s */
} ctm_controller_status_t;

/* --- lifecycle (controller_common.c) ----------------------------------------
 * Each controller runs in isolation: its own pump (reader + session threads),
 * HID fd, transport, settings, and per-MAC log file. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev);
int  ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port);
void ctm_controller_plug_out(ctm_controller_t *c);
/* Composite: forwarded enumeration payload (CTMB_MSG_ENUM), sent before HELLO. */
void ctm_controller_set_enum_payload(ctm_controller_t *c, const uint8_t *payload, int len);
void ctm_controller_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);
void ctm_controller_get_settings(ctm_controller_t *c, tv_bridge_worker_settings_t *out);
/* Adaptive pad latency: extra ms the congestion controller wants on top of the
 * latency slider right now (0 when the link is clean or the feature is off).
 * Read by the DS5 patch_output hook per outbound report. */
uint32_t ctm_controller_adapt_latency_ms(ctm_controller_t *c);
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out);
/* Session thread exited while still plugged (zombie) — reconcile re-plugs. */
bool ctm_controller_finished(ctm_controller_t *c);
int  ctm_controller_write_feature(ctm_controller_t *c, const uint8_t *feature, size_t len);
void ctm_controller_update_battery(ctm_controller_t *c, uint8_t level, uint8_t status);
void ctm_controller_update_battery_raw(ctm_controller_t *c, uint8_t raw);
void ctm_controller_destroy(ctm_controller_t *c);

/* Register a sink for controller log lines (e.g. the app's on-screen console).
 * NULL = file + stderr only. The sink must be thread-safe — controllers log
 * from their own threads. When: app startup. */
void ctm_controller_set_log_sink(void (*sink)(const char *line));

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) to the trailing 4
 * bytes. When: a DS patch_output hook after rewriting a report. Defined in
 * controller_common.c so DS4 + DS5 share it. */
void ctm_bt_sign_output(uint8_t *data, size_t len);

/* Per-type ops tables, defined in controller_<kind>.c. */
extern const ctm_controller_ops_t ctm_controller_ds5_ops;
extern const ctm_controller_ops_t ctm_controller_ds4_ops;
extern const ctm_controller_ops_t ctm_controller_xbox_ops;
extern const ctm_controller_ops_t ctm_controller_flydigi_ops;
extern const ctm_controller_ops_t ctm_controller_steam_puck_ops;
extern const ctm_controller_ops_t ctm_controller_generic_ops;

/* Pick ops for a device: specific types first (puck/ds5/ds4/xbox), generic
 * fallback. Never returns NULL. */
const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* CTM_CONTROLLER_H */
