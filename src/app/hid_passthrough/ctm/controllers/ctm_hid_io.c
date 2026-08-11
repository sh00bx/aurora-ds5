/* Controller output path + paced ring. See ctm_hid_io.h. */

#include "ctm_hid_io.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif

/* 0x31 dedup match lifetime: bounds a stuck rumble from an invisible daemon-side
 * drop to one refresh interval; ~1 extra 0x31 per 250ms is negligible vs 94/s audio. */
#define DEDUP31_TTL_US 250000

struct ctm_hid_io {
    ctm_controller_t *owner;
    const ctm_controller_ops_t *ops;   /* patch_output hook */
    ds5_audio_t *audio;                /* borrowed; owned by the controller */
    ds5_acl_tx_t *acl_tx;              /* borrowed; NULL = hidraw only */

    pthread_mutex_t mutex;             /* serializes writes/ioctls on fd */
    int fd;                            /* primary hidraw; -1 when closed */
    int eagain_wait_ms;
    uint8_t dedup_report_id;           /* output report id eligible for dedup; 0 = off */

    size_t last31_len;
    uint8_t last31[80];
    uint64_t last31_ts_us;             /* when the cached report was last actually sent */

    uint64_t audio_last_us;            /* last 0x36/0x39 write: gates rumble slotting */

    /* Per-report-id output histogram + hidraw outcome counters (what actually
     * flows out). Read and zeroed once per telemetry window. */
    unsigned long st_out_36, st_out_31, st_out_32, st_out_other;
    unsigned long st_out_39;           /* batched audio/haptic report: counted
                                        * separately so the telemetry line shows
                                        * WHICH audio form is on air. */
    unsigned long st_hid_ok, st_hid_eagain, st_hid_recovered, st_hid_dropped;
    unsigned long st_dedup_skipped;
};

/* --- paced ring ----------------------------------------------------------- */

void ctm_paced_reset(ctm_paced_ring_t *r)
{
    if (r) memset(r, 0, sizeof(*r));
}

void ctm_paced_queue(ctm_paced_ring_t *r, const uint8_t *data, size_t len)
{
    if (!r || len > CTM_MAX_REPORT) return;
    if (r->count >= CTM_PACED_QUEUE_CAP) {
        r->head = (r->head + 1) % CTM_PACED_QUEUE_CAP;
        r->count--;
    }
    int idx = (r->head + r->count) % CTM_PACED_QUEUE_CAP;
    memcpy(r->q[idx].data, data, len);
    r->q[idx].len = len;
    r->count++;
}

int ctm_paced_count(const ctm_paced_ring_t *r) { return r ? r->count : 0; }
uint64_t ctm_paced_next_us(const ctm_paced_ring_t *r) { return r ? r->next_us : 0; }

const ctm_queued_report_t *ctm_paced_peek(const ctm_paced_ring_t *r)
{
    if (!r || r->count <= 0) return NULL;
    return &r->q[r->head];
}

void ctm_paced_pop(ctm_paced_ring_t *r)
{
    if (!r || r->count <= 0) return;
    r->head = (r->head + 1) % CTM_PACED_QUEUE_CAP;
    r->count--;
}

bool ctm_paced_should_pace(const ctmb_host_config_t *cfg, const ctmb_header_t *h,
                           const uint8_t *payload, size_t len)
{
    if ((h->flags & CTMB_FLAG_PACED) != 0) return true;
    if (!payload || len == 0) return false;
    for (int i = 0; i < cfg->paced_report_count && i < 16; i++) {
        if (payload[0] == cfg->paced_report_ids[i]) return true;
    }
    return false;
}

void ctm_paced_drain(ctm_hid_io_t *io, ctm_paced_ring_t *r, uint32_t pace_us)
{
    uint64_t now = ctm_now_us();
    if (r->count <= 0) { r->next_us = 0; return; }
    if (r->next_us == 0) r->next_us = now;
    while (r->count > 0 && now >= r->next_us) {
        ctm_queued_report_t *e = &r->q[r->head];
        int rc = ctm_hid_io_write(io, e->data, e->len);
        r->head = (r->head + 1) % CTM_PACED_QUEUE_CAP;
        r->count--;
        if (rc == 2) {
            /* Dup-dropped stale frame: no air slot consumed — don't advance
             * the pace grid, so a run of stale frames flushes immediately and
             * the fresh audio behind it isn't delayed by phantom slots. */
            now = ctm_now_us();
            continue;
        }
        if (pace_us == 0) pace_us = 10667;
        r->next_us += pace_us;
        if (r->next_us + pace_us < now) r->next_us = now + pace_us;
        now = ctm_now_us();
    }
    if (r->count <= 0) r->next_us = 0;
}

/* --- the device handle ---------------------------------------------------- */

ctm_hid_io_t *ctm_hid_io_create(ctm_controller_t *owner, const ctm_controller_ops_t *ops,
                                ds5_audio_t *audio)
{
    ctm_hid_io_t *io = (ctm_hid_io_t *)calloc(1, sizeof(*io));
    if (!io) return NULL;
    io->owner = owner;
    io->ops = ops;
    io->audio = audio;
    io->fd = -1;
    pthread_mutex_init(&io->mutex, NULL);
    return io;
}

void ctm_hid_io_destroy(ctm_hid_io_t *io)
{
    if (!io) return;
    pthread_mutex_destroy(&io->mutex);
    free(io);
}

void ctm_hid_io_set_fd(ctm_hid_io_t *io, int fd) { if (io) io->fd = fd; }
int ctm_hid_io_fd(const ctm_hid_io_t *io) { return io ? io->fd : -1; }
void ctm_hid_io_set_acl_tx(ctm_hid_io_t *io, ds5_acl_tx_t *tx) { if (io) io->acl_tx = tx; }

void ctm_hid_io_set_tunables(ctm_hid_io_t *io, int eagain_wait_ms, uint8_t dedup_report_id)
{
    if (!io) return;
    io->eagain_wait_ms = eagain_wait_ms;
    io->dedup_report_id = dedup_report_id;
}

uint64_t ctm_hid_io_audio_last_us(const ctm_hid_io_t *io) { return io ? io->audio_last_us : 0; }

void ctm_hid_io_session_reset(ctm_hid_io_t *io)
{
    if (!io) return;
    io->last31_len = 0;
    io->last31_ts_us = 0;
    io->audio_last_us = 0;
    io->st_out_36 = io->st_out_39 = io->st_out_31 = io->st_out_32 = io->st_out_other = 0;
    io->st_hid_ok = io->st_hid_eagain = io->st_hid_recovered = io->st_hid_dropped = 0;
    io->st_dedup_skipped = 0;
}

void ctm_hid_io_stats_take(ctm_hid_io_t *io, ctm_hid_io_stats_t *out)
{
    if (!io || !out) return;
    out->out36 = io->st_out_36;
    out->out39 = io->st_out_39;
    out->out31 = io->st_out_31;
    out->out32 = io->st_out_32;
    out->out_other = io->st_out_other;
    out->hid_ok = io->st_hid_ok;
    out->hid_eagain = io->st_hid_eagain;
    out->hid_recovered = io->st_hid_recovered;
    out->hid_dropped = io->st_hid_dropped;
    out->dedup_skipped = io->st_dedup_skipped;
    io->st_out_36 = io->st_out_39 = io->st_out_31 = io->st_out_32 = io->st_out_other = 0;
    io->st_hid_ok = io->st_hid_eagain = io->st_hid_recovered = io->st_hid_dropped = 0;
    io->st_dedup_skipped = 0;
}

int ctm_hid_io_write(ctm_hid_io_t *io, const uint8_t *data, size_t len)
{
    if (!io || io->fd < 0 || !data || len == 0) return -1;
    switch (data[0]) {
        case 0x36: io->st_out_36++; io->audio_last_us = ctm_now_us(); break;
        case 0x39: io->st_out_39++; io->audio_last_us = ctm_now_us(); break;
        case 0x31: io->st_out_31++; break;
        case 0x32: io->st_out_32++; break;
        default:   io->st_out_other++; break;
    }
    uint8_t patched[CTM_MAX_REPORT];
    if (len > sizeof(patched)) return -1;
    memcpy(patched, data, len);
    size_t patched_len = len;
    if (io->ops && io->ops->patch_output &&
        io->ops->patch_output(io->owner, patched, &patched_len)) {
        return 0;
    }
    if (ds5_audio_plc(io->audio, patched, patched_len)) {
        /* Late real for a slot the fill already bridged: dropping it keeps the
         * pad's rate-matched buffer level (the synth already fed that slot).
         * rc 2 tells ctm_paced_drain no air slot was consumed, so it flushes a
         * stale run at loop speed instead of burning 16ms pace ticks on it. */
        return 2;
    }
    ds5_audio_stamp_tx_seq(io->audio, patched, patched_len);
    if (io->dedup_report_id && patched_len >= 8 && patched[0] == io->dedup_report_id) {
        /* TTL on the dedup match: DS5_ACL_TX_SENT only means the datagram reached
         * the root daemon; the daemon can still drop the frame latest-wins on a
         * full credit window with no feedback channel. Without a TTL one such
         * invisible drop of a rumble OFF strands the pad buzzing forever (host
         * resends differ only in seq+CRC, both outside the memcmp window). The
         * TTL bounds that to one resend interval while still deduping the
         * high-rate identical resends. */
        size_t cmp = patched_len - 4;
        if (patched_len == io->last31_len &&
            memcmp(patched + 2, io->last31 + 2, cmp - 2) == 0 &&
            ctm_now_us() - io->last31_ts_us < DEDUP31_TTL_US) {
            io->st_dedup_skipped++;
            return 0;
        }
        if (patched_len <= sizeof(io->last31)) {
            memcpy(io->last31, patched, patched_len);
            io->last31_len = patched_len;
            io->last31_ts_us = ctm_now_us();
        }
    }
    if (io->acl_tx && patched_len > 0 && ds5_acl_is_injectable(patched[0])) {
        int rc = ds5_acl_tx_send(io->acl_tx, patched, patched_len);
        if (rc == DS5_ACL_TX_SENT) {
            ctm_ctl_note_output_report(io->owner);
            return 0;
        }
        if (rc == DS5_ACL_TX_DROP) {
            /* Transient injector congestion: skip this frame rather than falling
             * back to the slow flow-controlled hidraw path, which would head-of-line
             * block subsequent rumble/trigger/LED writes precisely when congested.
             * The dedup cache was filled BEFORE this send attempt — invalidate it,
             * or the host's identical resends of this very frame (a rumble OFF, a
             * trigger effect) would all dedup-match a frame that never reached the
             * pad: rumble stuck on until a byte-different 0x31 happens to arrive.
             * Scoped to the deduped id: the cache only ever holds that report,
             * and a dropped 0x36 audio frame says nothing about the delivered
             * rumble state — clearing on it would defeat dedup exactly under
             * congestion. */
            if (io->dedup_report_id && patched[0] == io->dedup_report_id) io->last31_len = 0;
            io->st_hid_dropped++;
            return -1;
        }
        /* DS5_ACL_TX_HIDRAW: injector not ready/disabled — fall through to the
         * hidraw write below (which also seeds template capture). */
    }
    pthread_mutex_lock(&io->mutex);
    ssize_t n = write(io->fd, patched, patched_len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        io->st_hid_eagain++;
        if (io->eagain_wait_ms > 0) {
            struct pollfd pf;
            pf.fd = io->fd;
            pf.events = POLLOUT;
            pf.revents = 0;
            if (poll(&pf, 1, io->eagain_wait_ms) > 0 && (pf.revents & POLLOUT)) {
                n = write(io->fd, patched, patched_len);
                if (n == (ssize_t)patched_len) {
                    io->st_hid_recovered++;
                }
            }
        }
    }
    pthread_mutex_unlock(&io->mutex);
    if (n == (ssize_t)patched_len) {
        io->st_hid_ok++;
        ctm_ctl_note_output_report(io->owner);
        return 0;
    }
    /* frame not delivered: never let it satisfy future dedup (scoped to the
     * deduped id, the only report the cache ever holds) */
    if (io->dedup_report_id && patched[0] == io->dedup_report_id) io->last31_len = 0;
    io->st_hid_dropped++;
    return -1;
}

int ctm_hid_io_write_fd_raw(ctm_hid_io_t *io, int fd, const uint8_t *data, size_t len)
{
    if (!io || fd < 0 || !data || len == 0 || len > CTM_MAX_REPORT) return -1;
    pthread_mutex_lock(&io->mutex);
    ssize_t n = write(fd, data, len);
    pthread_mutex_unlock(&io->mutex);
    if (n == (ssize_t)len) { ctm_ctl_note_output_report(io->owner); return 0; }
    return -1;
}

int ctm_hid_io_write_synth(ctm_hid_io_t *io, const uint8_t *data, size_t len)
{
    if (!io) return 0;
    int sent = 0;
    int skip_hidraw = 0;
    if (io->acl_tx && ds5_acl_is_injectable(data[0])) {
        int rc = ds5_acl_tx_send(io->acl_tx, data, len);
        if (rc == DS5_ACL_TX_SENT) sent = 1;
        else if (rc == DS5_ACL_TX_DROP) skip_hidraw = 1; /* congested — don't HOL-block hidraw */
    }
    if (!sent && !skip_hidraw && io->fd >= 0) {
        pthread_mutex_lock(&io->mutex);
        ssize_t w = write(io->fd, data, len);
        pthread_mutex_unlock(&io->mutex);
        if (w == (ssize_t)len) sent = 1;
    }
    return sent;
}

int ctm_hid_io_feature_set(ctm_hid_io_t *io, const uint8_t *feature, size_t len)
{
    if (!io || io->fd < 0 || !feature || len == 0 || len > 4096) {
        return -1;
    }
    uint8_t buf[4096];
    memcpy(buf, feature, len);
    pthread_mutex_lock(&io->mutex);
    int rc = ioctl(io->fd, HIDIOCSFEATURE((int)len), buf);
    pthread_mutex_unlock(&io->mutex);
    return rc >= 0 ? 0 : -1;
}
