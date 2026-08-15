/* Raw-ACL output for DS5 BT reports — FORWARDER mode. See ds5_acl_tx.h. */

#define _GNU_SOURCE

#include "ds5_acl_tx.h"
#include "input/ds5_idle_lightbar.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#define ACL_MAX_REPORT 4096

/* Tagged-datagram framing (multi-controller): [0xA5][kind][addr LSB-first][report].
 * Byte 0 is not a DS5 output report id (0x31/0x32/0x36), so the daemon tells a
 * tagged datagram from a legacy untagged one by inspecting it alone.
 *   ACL_TAG_INJECT — forward onto that pad's link (sent only while ready).
 *   ACL_TAG_ASSERT — identity assertion, NEVER injected: sent alongside the hidraw
 *                    seed while NOT ready so the daemon can match the on-air bytes
 *                    and learn handle->bdaddr (daemon restarted mid-session = no
 *                    HCI connect event). Un-injectable by design, so the readiness
 *                    flip cannot put the same frame on air twice (daemon inject +
 *                    our hidraw write). */
#define ACL_TAG_M0      0xA5
#define ACL_TAG_INJECT  0x5A
#define ACL_TAG_ASSERT  0x5B
#define ACL_TAG_CTRL    0x5C   /* [A5][5C][code][value] — daemon control */
#define ACL_CTRL_FIFO_DEPTH 0x01
#define ACL_TAG_LEN     8

struct ds5_acl_tx {
    ds5_acl_log_fn log_fn;
    void *log_ctx;
    int unixfd;
    struct sockaddr_un daddr;
    char tmpl_path[256];

    int tagged;
    uint8_t tag[ACL_TAG_LEN];

    pthread_t poll_thread;
    int poll_started;
    int lb_owned_held;      /* this session holds one idle-lightbar claim */
    int wake_pipe[2];       /* stop() -> mon thread: wake the blocking poll() */
    volatile int running;
    volatile int ready;

    long injected;
    long dropped;
};

/* "aa:bb:cc:dd:ee:ff" -> 6 bytes LSB-first (HCI order, matches the daemon's
 * captured bound_addr); out_hex = colon-stripped lowercase (per-address readiness
 * filename). Returns 1 on a valid 6-octet address, else 0 (caller stays legacy). */
static int parse_bt_mac(const char *s, uint8_t out[6], char out_hex[13])
{
    if (!s || !s[0]) {
        return 0;
    }
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return 0;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) {
            return 0;
        }
        out[5 - i] = (uint8_t)v[i];
    }
    for (int i = 0; i < 6; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", v[i]);
    }
    return 1;
}

static void acl_log(ds5_acl_tx_t *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (t && t->log_fn) {
        t->log_fn(t->log_ctx, buf);
    } else {
        fprintf(stderr, "[ds5-acl] %s\n", buf);
    }
}

static void *acl_mon_thread(void *arg)
{
    ds5_acl_tx_t *t = (ds5_acl_tx_t *)arg;
    acl_log(t, "forwarder readiness watching %s", t->tmpl_path);

    /* inotify on the template's directory (review S5): a valid<->invalid flip
     * used to cost up to 200ms of discarded output (daemon drops INJECTs, the
     * client skips hidraw) on the old fixed 200ms poll. Watch for the daemon's
     * writes/renames and re-check immediately; the 1s poll() timeout keeps the
     * old polling as a fallback when inotify is unavailable in the jail. */
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0) {
        char dir[sizeof t->tmpl_path];
        snprintf(dir, sizeof dir, "%s", t->tmpl_path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
        } else {
            snprintf(dir, sizeof dir, "/tmp");
        }
        if (inotify_add_watch(ifd, dir, IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE) < 0) {
            acl_log(t, "inotify watch on %s failed errno=%d -> 1s poll fallback", dir, errno);
            close(ifd);
            ifd = -1;
        }
    }

    int last = -1;
    while (t->running) {
        int valid = 0;
        int fd = open(t->tmpl_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            uint8_t rec[16];
            ssize_t n = read(fd, rec, sizeof rec);
            close(fd);
            if (n == 16 && rec[0] == 'D' && rec[1] == 'S' && rec[2] == '5' && rec[3] == 'T' &&
                rec[4] == 1 && (rec[5] & 1)) {
                valid = 1;
            }
        }
        if (valid != last) {
            __atomic_store_n(&t->ready, valid, __ATOMIC_RELEASE);
            acl_log(t, valid ? "daemon template ready -> raw-ACL forward ACTIVE"
                             : "daemon template not ready -> hidraw seeding");
            last = valid;
        }
        if (ifd >= 0) {
            /* Wake pipe in the set: without it stop()'s pthread_join stalls up
             * to the full poll timeout on every controller teardown (plug-out,
             * session exit) — the old 50ms-sliced loop honored running quickly. */
            struct pollfd pf[2] = {
                {.fd = ifd, .events = POLLIN, .revents = 0},
                {.fd = t->wake_pipe[0], .events = POLLIN, .revents = 0},
            };
            int pr = poll(pf, t->wake_pipe[0] >= 0 ? 2 : 1, 1000);
            if (pr > 0 && (pf[0].revents & POLLIN)) {
                /* Drain everything queued; the loop re-reads the template. */
                char evbuf[512];
                while (read(ifd, evbuf, sizeof evbuf) > 0) {
                }
            }
        } else {
            for (int i = 0; i < 4 && t->running; i++) {
                usleep(50000);
            }
        }
    }
    if (ifd >= 0) {
        close(ifd);
    }
    return NULL;
}

ds5_acl_tx_t *ds5_acl_tx_start(int hci_dev, const char *bt_mac,
                               ds5_acl_log_fn log_fn, void *log_ctx)
{
    (void)hci_dev;
    ds5_acl_tx_t *t = (ds5_acl_tx_t *)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->log_fn = log_fn;
    t->log_ctx = log_ctx;
    t->unixfd = -1;
    t->running = 1;
    if (pipe2(t->wake_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        t->wake_pipe[0] = t->wake_pipe[1] = -1;
    }

    uint8_t addr[6];
    char machex[13];
    t->tagged = parse_bt_mac(bt_mac, addr, machex);
    if (t->tagged) {
        t->tag[0] = ACL_TAG_M0;   /* tag[1] = kind, stamped per send */
        memcpy(t->tag + 2, addr, 6);
    }

    const char *tp = getenv("DS5_ACL_TMPL");
    const char *base = (tp && tp[0]) ? tp : "/tmp/ds5_acl_tmpl";
    if (t->tagged) {
        snprintf(t->tmpl_path, sizeof t->tmpl_path, "%s.%s", base, machex);
    } else {
        snprintf(t->tmpl_path, sizeof t->tmpl_path, "%s", base);
    }
    const char *sp = getenv("DS5_ACL_SOCK");
    const char *sock = (sp && sp[0]) ? sp : "/tmp/ds5_acl.sock";

    t->unixfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (t->unixfd < 0) {
        acl_log(t, "unix socket failed errno=%d (staying on hidraw)", errno);
        goto fail;
    }
    int fl = fcntl(t->unixfd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(t->unixfd, F_SETFL, fl | O_NONBLOCK);
    }
    t->daddr.sun_family = AF_UNIX;
    snprintf(t->daddr.sun_path, sizeof t->daddr.sun_path, "%s", sock);

    if (pthread_create(&t->poll_thread, NULL, acl_mon_thread, t) != 0) {
        acl_log(t, "readiness thread failed errno=%d (staying on hidraw)", errno);
        goto fail;
    }
    t->poll_started = 1;
    /* This session owns the pad's lightbar from here on: the host paints it, so
     * the daemon's idle painter must stay off. It cannot infer this on its own
     * -- a session that writes only sporadically looks idle to it within a
     * second, and it would then repaint over the host's colour. The claim is
     * counted per session (one acquire here, one release in stop, gated by
     * lb_owned_held), so with two pads bridged one session's stop cannot clear
     * the other's ownership. */
    t->lb_owned_held = 1;
    ds5_idle_lb_set_owned(true);
    acl_log(t, "raw-ACL forwarder ON: sock=%s tag=%s (root ds5_txd does the inject)",
            sock, t->tagged ? machex : "none(legacy)");
    return t;

    /* Both jumps here sit before the monitor thread exists — the socket failure
     * precedes pthread_create, and a failed pthread_create creates no thread — so
     * nothing else can be holding these fds and closing them here is unambiguous.
     * The lightbar claim is taken further down, so there is none to release.
     * Closing matters most in the failure that brings us here: the plausible cause
     * of socket() failing is EMFILE, and leaking the two pipe fds on every attempt
     * made the next plug-in strictly more likely to fail the same way. */
fail:
    if (t->unixfd >= 0) {
        close(t->unixfd);
    }
    if (t->wake_pipe[0] >= 0) {
        close(t->wake_pipe[0]);
    }
    if (t->wake_pipe[1] >= 0) {
        close(t->wake_pipe[1]);
    }
    free(t);
    return NULL;
}

/* [tag][report] as one atomic datagram (iovec: no report-body copy). */
static ssize_t send_tagged(ds5_acl_tx_t *t, uint8_t kind, const uint8_t *report, size_t len)
{
    t->tag[1] = kind;
    struct iovec iov[2] = {
        { .iov_base = t->tag, .iov_len = ACL_TAG_LEN },
        { .iov_base = (void *)report, .iov_len = len },
    };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &t->daddr;
    mh.msg_namelen = sizeof t->daddr;
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    return sendmsg(t->unixfd, &mh, 0);
}

void ds5_acl_tx_set_fifo_depth(ds5_acl_tx_t *t, int depth)
{
    /* Tell the daemon which elastic audio-FIFO depth this session may use.
     * Deep (10) only when the HOST advertised the rate-servo capability —
     * the servo is what bounds a deep FIFO's parked latency; against a
     * fixed-pace host it just parks ~107ms of permanent audio latency.
     * Best-effort datagram; the daemon's boot default stays shallow. */
    if (!t || t->unixfd < 0) {
        return;
    }
    uint8_t msg[4] = { ACL_TAG_M0, ACL_TAG_CTRL, ACL_CTRL_FIFO_DEPTH,
                       (depth < 0) ? 0xFF : (uint8_t)depth };
    (void)sendto(t->unixfd, msg, sizeof msg, 0,
                 (struct sockaddr *)&t->daddr, sizeof t->daddr);
    acl_log(t, "ctrl: audio-FIFO depth -> %d", depth);
}

int ds5_acl_tx_send(ds5_acl_tx_t *t, const uint8_t *report, size_t len)
{
    if (!t) {
        return DS5_ACL_TX_HIDRAW;
    }
    if (!report || len == 0 || len > ACL_MAX_REPORT) {
        return DS5_ACL_TX_HIDRAW;
    }
    if (!__atomic_load_n(&t->ready, __ATOMIC_ACQUIRE)) {
        /* Not ready -> the caller seeds this exact report via hidraw. A tagged
         * pad ALSO sends it as an identity ASSERT: after a daemon restart with
         * the pad still connected there is no HCI connect event (and webOS's
         * HCIGETCONNLIST is empty) to learn handle->address from, so the daemon
         * matches these asserted bytes against its on-air capture of the very
         * hidraw write we trigger, binds the link and flips readiness.
         * Best-effort, non-blocking; hidraw stays the sender of record. */
        if (t->tagged) {
            (void)send_tagged(t, ACL_TAG_ASSERT, report, len);
        }
        return DS5_ACL_TX_HIDRAW;
    }

    ssize_t wr;
    if (t->tagged) {
        wr = send_tagged(t, ACL_TAG_INJECT, report, len);
        if (wr == (ssize_t)(ACL_TAG_LEN + len)) {
            t->injected++;
            return DS5_ACL_TX_SENT;
        }
    } else {
        wr = sendto(t->unixfd, report, len, 0,
                    (struct sockaddr *)&t->daddr, sizeof t->daddr);
        if (wr == (ssize_t)len) {
            t->injected++;
            return DS5_ACL_TX_SENT;
        }
    }
    if (wr < 0 && (errno == EINTR || errno == EAGAIN ||
                   errno == EWOULDBLOCK || errno == ENOBUFS)) {
        t->dropped++;
        return DS5_ACL_TX_DROP;
    }
    return DS5_ACL_TX_HIDRAW;
}

int ds5_acl_tx_qstats(ds5_acl_tx_t *t, ds5_acl_qstats_t *out)
{
    if (!t || !out || !t->tmpl_path[0]) {
        return 0;
    }
    char path[280];
    snprintf(path, sizeof path, "%s.st", t->tmpl_path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    /* Version-TOLERANT parse. The daemon and this app live in separate trees and
     * are deployed separately, so they WILL run out of step across an upgrade.
     * The v2 record keeps its first 24 bytes byte-identical to v1 for exactly
     * that reason: accept any version we know, parse the common prefix, and take
     * the tail only when it is really there. Rejecting an unknown version here
     * would silently stop PACE_FEEDBACK and drop the host rate servo to its blind
     * fallback with no counter anywhere showing why. */
    uint8_t rec[36];
    memset(rec, 0, sizeof rec);
    ssize_t n = read(fd, rec, sizeof rec);
    close(fd);
    if (n < 24 || rec[0] != 'D' || rec[1] != 'S' || rec[2] != '5' || rec[3] != 'Q' ||
        rec[4] < 1 || rec[4] > 2) {
        return 0;
    }
    out->ver         = rec[4];
    out->valid       = rec[5];
    out->outstanding = rec[6];
    out->fifo_count  = rec[7];
    out->maxq        = (uint16_t)(rec[8] | (rec[9] << 8));
    out->fifo_cap    = (uint16_t)(rec[10] | (rec[11] << 8));
    out->inj_total   = (uint32_t)rec[12] | ((uint32_t)rec[13] << 8) |
                       ((uint32_t)rec[14] << 16) | ((uint32_t)rec[15] << 24);
    out->drop_total  = (uint32_t)rec[16] | ((uint32_t)rec[17] << 8) |
                       ((uint32_t)rec[18] << 16) | ((uint32_t)rec[19] << 24);
    out->seq         = (uint32_t)rec[20] | ((uint32_t)rec[21] << 8) |
                       ((uint32_t)rec[22] << 16) | ((uint32_t)rec[23] << 24);
    out->gap50_events = 0;
    out->gap80_events = 0;
    out->flush_events = 0;
    out->nocp_age_ms  = 0;
    out->drop_age     = 0;
    out->drop_ovf     = 0;
    if (out->ver >= 2 && n >= 36) {
        out->gap50_events = (uint16_t)(rec[24] | (rec[25] << 8));
        out->gap80_events = (uint16_t)(rec[26] | (rec[27] << 8));
        out->flush_events = (uint16_t)(rec[28] | (rec[29] << 8));
        out->nocp_age_ms  = (uint16_t)(rec[30] | (rec[31] << 8));
        out->drop_age     = (uint16_t)(rec[32] | (rec[33] << 8));
        out->drop_ovf     = (uint16_t)(rec[34] | (rec[35] << 8));
    }
    return out->valid ? 1 : 0;
}

void ds5_acl_tx_stats(ds5_acl_tx_t *t, long *injected, long *dropped, int *ready)
{
    if (!t) {
        if (injected) {
            *injected = 0;
        }
        if (dropped) {
            *dropped = 0;
        }
        if (ready) {
            *ready = 0;
        }
        return;
    }
    if (injected) {
        *injected = t->injected;
    }
    if (dropped) {
        *dropped = t->dropped;
    }
    if (ready) {
        *ready = __atomic_load_n(&t->ready, __ATOMIC_ACQUIRE);
    }
}

void ds5_acl_tx_stop(ds5_acl_tx_t *t)
{
    if (!t) {
        return;
    }
    t->running = 0;
    if (t->lb_owned_held) {
        /* Release exactly the one claim this session took in start; the flag
         * keeps a repeated stop from decrementing another session's claim. */
        t->lb_owned_held = 0;
        ds5_idle_lb_set_owned(false);   /* hand the bar back to the idle painter */
    }
    if (t->wake_pipe[1] >= 0) {
        ssize_t wr = write(t->wake_pipe[1], "x", 1);
        (void)wr;
    }
    if (t->poll_started) {
        pthread_join(t->poll_thread, NULL);
        t->poll_started = 0;
    }
    if (t->wake_pipe[0] >= 0) close(t->wake_pipe[0]);
    if (t->wake_pipe[1] >= 0) close(t->wake_pipe[1]);
    if (t->unixfd >= 0) {
        close(t->unixfd);
        t->unixfd = -1;
    }
    free(t);
}
