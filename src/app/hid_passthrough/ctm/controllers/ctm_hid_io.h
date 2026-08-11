#ifndef CTM_HID_IO_H
#define CTM_HID_IO_H

/* The controller's output side: the device handle, everything written to it,
 * and the pacing ring that feeds it.
 *
 * One object per controller owns the primary hidraw fd, the mutex that
 * serializes writes to it, the 0x31 dedup cache, the EAGAIN retry policy and
 * the per-report-id telemetry counters. Every byte this app sends to a bridged
 * pad goes through ctm_hid_io_write() (patch -> PLC -> dedup -> raw-ACL or
 * hidraw), so "what actually reached the device" is answerable in one place
 * instead of five.
 *
 * Threads: every write path is reached from the controller's session thread
 * (the pump's rumble drain, the paced drain, handle_message) — but nothing
 * here relies on that: the writes are serialised by this object's own mutex
 * and the counters are relaxed atomics, so a future caller on another thread
 * costs accuracy nowhere. The fd is additionally read by the input thread's
 * poll and dup()'d by the feature worker; ctm_hid_io_fd() exists for those. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_controller_priv.h"
#include "ctm_ds5_audio.h"
#include "ds5_acl_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- paced output ring ------------------------------------------------------
 * Host output the pad cannot swallow at wire speed (DS BT audio) is queued here
 * and released on a schedule instead of written on arrival. Lives on the
 * session thread's stack for the length of one session. */

#define CTM_PACED_QUEUE_CAP 32

typedef struct { uint8_t data[CTM_MAX_REPORT]; size_t len; } ctm_queued_report_t;

typedef struct {
    ctm_queued_report_t q[CTM_PACED_QUEUE_CAP];
    int head;
    int count;
    uint64_t next_us;   /* absolute due time of the head entry; 0 = nothing due */
} ctm_paced_ring_t;

typedef struct ctm_hid_io ctm_hid_io_t;

/* Empty the ring. When: the top of every session. */
void ctm_paced_reset(ctm_paced_ring_t *r);
/* Append one report, dropping the oldest when full. */
void ctm_paced_queue(ctm_paced_ring_t *r, const uint8_t *data, size_t len);
int ctm_paced_count(const ctm_paced_ring_t *r);
uint64_t ctm_paced_next_us(const ctm_paced_ring_t *r);
/* Oldest queued report, or NULL when empty. Valid until the next ring call. */
const ctm_queued_report_t *ctm_paced_peek(const ctm_paced_ring_t *r);
/* Discard the oldest report without writing it. */
void ctm_paced_pop(ctm_paced_ring_t *r);
/* Write queued reports as their schedule comes due. When: every pump tick. */
void ctm_paced_drain(ctm_hid_io_t *io, ctm_paced_ring_t *r, uint32_t pace_us);
/* Whether an outbound report must be rate-limited (PACED flag, or a report id
 * the host's HOST_CONFIG listed). When: per OUTPUT report. */
bool ctm_paced_should_pace(const ctmb_host_config_t *cfg, const ctmb_header_t *h,
                           const uint8_t *payload, size_t len);

/* --- the device handle ----------------------------------------------------- */

/* `ops` supplies the per-type patch_output hook, `audio` the DS concealment
 * state both the write path and the pump share. Both must outlive the object.
 * Returns NULL only on allocation failure. */
ctm_hid_io_t *ctm_hid_io_create(ctm_controller_t *owner, const ctm_controller_ops_t *ops,
                                ds5_audio_t *audio);
void ctm_hid_io_destroy(ctm_hid_io_t *io);

/* Publish (or retire, with -1) the primary hidraw fd for this session. The
 * caller owns the descriptor's lifetime; this module never closes it. */
void ctm_hid_io_set_fd(ctm_hid_io_t *io, int fd);
int ctm_hid_io_fd(const ctm_hid_io_t *io);

/* Borrow the DS5 raw-ACL injector for the session, or NULL to fall back to
 * hidraw. */
void ctm_hid_io_set_acl_tx(ctm_hid_io_t *io, ds5_acl_tx_t *tx);

/* Pump policy: ms to wait for POLLOUT after an EAGAIN (0 = don't), and the
 * output report id eligible for dedup (0 = off). */
void ctm_hid_io_set_tunables(ctm_hid_io_t *io, int eagain_wait_ms, uint8_t dedup_report_id);

/* Patch (via the type's hook) then write one report to the device.
 * 0 = written or deliberately consumed, 2 = dropped as a late duplicate (no air
 * slot was used), -1 = not delivered. */
int ctm_hid_io_write(ctm_hid_io_t *io, const uint8_t *data, size_t len);

/* Write a report verbatim to a specific sibling fd — no patching, no pacing:
 * the composite is an identity passthrough. */
int ctm_hid_io_write_fd_raw(ctm_hid_io_t *io, int fd, const uint8_t *data, size_t len);

/* Write an already-prepared report to the primary: injector first, hidraw
 * fallback, no patch and no PLC. For the audio module's synth frames, which are
 * verbatim copies of reports that already went through both. 1 if it was sent. */
int ctm_hid_io_write_synth(ctm_hid_io_t *io, const uint8_t *data, size_t len);

/* HIDIOCSFEATURE on the primary fd, serialized against the write path.
 * 0 on success. */
int ctm_hid_io_feature_set(ctm_hid_io_t *io, const uint8_t *feature, size_t len);

/* Time of the last audio report (0x36/0x39) written to the device. The pump
 * gates rumble slotting on how recently that was. */
uint64_t ctm_hid_io_audio_last_us(const ctm_hid_io_t *io);

/* Drop the dedup cache and zero the telemetry window. When: session start —
 * a dedup match against the pre-reconnect frame would swallow the host's first
 * rumble state of the new session. */
void ctm_hid_io_session_reset(ctm_hid_io_t *io);

/* One telemetry window's counters, for the PLC/60s line. */
typedef struct {
    unsigned long out36, out39, out31, out32, out_other;
    unsigned long hid_ok, hid_eagain, hid_recovered, hid_dropped;
    unsigned long dedup_skipped;
} ctm_hid_io_stats_t;

/* Read the counters and start a new window. */
void ctm_hid_io_stats_take(ctm_hid_io_t *io, ctm_hid_io_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CTM_HID_IO_H */
