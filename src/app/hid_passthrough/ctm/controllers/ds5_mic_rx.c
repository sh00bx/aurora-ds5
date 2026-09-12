#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ds5_mic_rx.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ctm_bridge_protocol.h"
#include "ctm_controller_priv.h"

struct ds5_mic_rx {
    ctm_controller_t *c;
    pthread_t thread;
    int thread_started;
    int fd;                 /* bound AF_UNIX DGRAM, non-blocking */
    int wake[2];            /* stop pipe */
    char path[108];
    uint8_t addr[6];        /* LSB-first, as the daemon stamps it */
    volatile int stop;
    uint16_t seq;           /* CTMB frame sequence, per session, from 0 */
    unsigned long frames, bad, send_fail, foreign;
};

/* "aa:bb:cc:dd:ee:ff" -> LSB-first bytes + 12 lowercase hex digits (display
 * order), the spelling ds5_acl_tx.c gives the per-address template file. */
static int mic_parse_mac(const char *s, uint8_t out[6], char hex[13])
{
    if (!s || !s[0]) return 0;
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 0;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return 0;
        out[5 - i] = (uint8_t)v[i];
        snprintf(hex + i * 2, 3, "%02x", v[i]);
    }
    return 1;
}

static void *mic_rx_thread(void *arg)
{
    ds5_mic_rx_t *r = (ds5_mic_rx_t *)arg;
    ctm_controller_t *c = r->c;
    uint8_t buf[DS5_MIC_DGRAM_MAX + 64];
    /* CTMB payload: header + Opus. Sized for the largest datagram we accept. */
    uint8_t msg[sizeof(ctmb_ds5_mic_t) + 255];
    uint64_t last_log_us = ctm_now_us();
    unsigned long log_frames = 0, log_bad = 0, log_fail = 0;

    while (!r->stop && ctm_ctl_readers_run(c)) {
        struct pollfd pfds[2];
        pfds[0].fd = r->fd;      pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = r->wake[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
        int pr = poll(pfds, 2, 250);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfds[1].revents & POLLIN) break;
        if (!(pfds[0].revents & POLLIN)) continue;
        for (;;) {
            /* One datagram per recvmsg, credentials alongside: only root (the
             * daemon) may feed this socket. Anything else in the jail that
             * found the path is dropped without a look. */
            struct iovec iov = { buf, sizeof buf };
            union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(struct ucred))]; } cbuf;
            struct msghdr mh;
            memset(&mh, 0, sizeof mh);
            mh.msg_iov = &iov; mh.msg_iovlen = 1;
            mh.msg_control = cbuf.b; mh.msg_controllen = sizeof cbuf.b;
            ssize_t n = recvmsg(r->fd, &mh, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                break;
            }
            int from_root = 0;
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_CREDENTIALS) {
                    struct ucred cr;
                    memcpy(&cr, CMSG_DATA(cm), sizeof cr);
                    from_root = (cr.uid == 0);
                }
            }
            if (!from_root || (mh.msg_flags & MSG_TRUNC)) { r->bad++; log_bad++; continue; }
            if (n < DS5_MIC_DGRAM_HDR ||
                buf[0] != DS5_MIC_DGRAM_MAGIC0 || buf[1] != DS5_MIC_DGRAM_MAGIC1 ||
                buf[2] != DS5_MIC_DGRAM_MAGIC2 || buf[3] != DS5_MIC_DGRAM_MAGIC3 ||
                buf[4] != DS5_MIC_DGRAM_VERSION) {
                r->bad++; log_bad++; continue;
            }
            const size_t opus_len = buf[7];
            if (opus_len == 0 || (size_t)n != DS5_MIC_DGRAM_HDR + opus_len) { r->bad++; log_bad++; continue; }
            if (memcmp(buf + 8, r->addr, 6) != 0) {
                /* The daemon addresses the socket by pad; a mismatch means a
                 * stale node or a spoof. Not ours either way. */
                r->foreign++; continue;
            }
            ctmb_ds5_mic_t hdr;
            memset(&hdr, 0, sizeof hdr);
            hdr.seq = r->seq++;
            hdr.format = CTMB_DS5_MIC_FORMAT_OPUS_48K_10MS;
            hdr.frame_len = (uint8_t)opus_len;
            memcpy(msg, &hdr, sizeof hdr);
            memcpy(msg + sizeof hdr, buf + DS5_MIC_DGRAM_HDR, opus_len);
            if (ctm_ctl_send(c, CTMB_MSG_DS5_MIC, CTMB_FLAG_OK, 0, msg, sizeof hdr + opus_len) != 0) {
                r->send_fail++; log_fail++;
            } else {
                r->frames++; log_frames++;
            }
        }
        /* One line per 30 s, and only while something flowed: a session with
         * the mic disarmed logs nothing here. */
        uint64_t now = ctm_now_us();
        if (now - last_log_us >= 30000000ull) {
            if (log_frames || log_bad || log_fail) {
                ctm_ctl_log(c, "mic uplink: 30s frames=%lu bad=%lu send_fail=%lu",
                            log_frames, log_bad, log_fail);
            }
            log_frames = log_bad = log_fail = 0;
            last_log_us = now;
        }
    }
    return NULL;
}

ds5_mic_rx_t *ds5_mic_rx_start(ctm_controller_t *c, const char *bt_mac)
{
    uint8_t addr[6];
    char hex[13];
    if (!c || !mic_parse_mac(bt_mac, addr, hex)) return NULL;

    ds5_mic_rx_t *r = (ds5_mic_rx_t *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->c = c;
    r->fd = -1;
    r->wake[0] = r->wake[1] = -1;
    memcpy(r->addr, addr, 6);

    /* Next to the report socket, so a DS5_ACL_SOCK override relocates both. */
    char dir[96] = "/tmp";
    const char *sp = getenv("DS5_ACL_SOCK");
    if (sp && sp[0]) {
        const char *slash = strrchr(sp, '/');
        if (slash && slash > sp && (size_t)(slash - sp) < sizeof dir) {
            memcpy(dir, sp, (size_t)(slash - sp));
            dir[slash - sp] = '\0';
        }
    }
    snprintf(r->path, sizeof r->path, "%s/ds5_mic.%s.sock", dir, hex);

    if (pipe2(r->wake, O_NONBLOCK | O_CLOEXEC) != 0) {
        ctm_ctl_log(c, "mic uplink: wake pipe failed errno=%d", errno);
        goto fail;
    }
    r->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (r->fd < 0) {
        ctm_ctl_log(c, "mic uplink: socket failed errno=%d", errno);
        goto fail;
    }
    {
        int one = 1;
        (void)setsockopt(r->fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
        int rcv = 256 * 1024;   /* ~3 s of frames; the thread drains far faster */
        (void)setsockopt(r->fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    }
    struct sockaddr_un ua;
    memset(&ua, 0, sizeof ua);
    ua.sun_family = AF_UNIX;
    snprintf(ua.sun_path, sizeof ua.sun_path, "%s", r->path);
    unlink(r->path);   /* a previous session's node (crash, kill) must not block us */
    if (bind(r->fd, (struct sockaddr *)&ua, sizeof ua) != 0) {
        ctm_ctl_log(c, "mic uplink: bind %s failed errno=%d", r->path, errno);
        goto fail;
    }
    if (pthread_create(&r->thread, NULL, mic_rx_thread, r) != 0) {
        ctm_ctl_log(c, "mic uplink: thread failed errno=%d", errno);
        unlink(r->path);
        goto fail;
    }
    r->thread_started = 1;
    ctm_ctl_log(c, "mic uplink: listening on %s (daemon lever /tmp/ds5_mic decides whether frames come)",
                r->path);
    return r;

fail:
    if (r->fd >= 0) close(r->fd);
    if (r->wake[0] >= 0) close(r->wake[0]);
    if (r->wake[1] >= 0) close(r->wake[1]);
    free(r);
    return NULL;
}

void ds5_mic_rx_stop(ds5_mic_rx_t *r)
{
    if (!r) return;
    r->stop = 1;
    if (r->wake[1] >= 0) (void)write(r->wake[1], "x", 1);
    if (r->thread_started) pthread_join(r->thread, NULL);
    if (r->frames || r->bad || r->send_fail || r->foreign) {
        ctm_ctl_log(r->c, "mic uplink: stopped frames=%lu bad=%lu send_fail=%lu foreign=%lu",
                    r->frames, r->bad, r->send_fail, r->foreign);
    }
    if (r->fd >= 0) close(r->fd);
    unlink(r->path);
    if (r->wake[0] >= 0) close(r->wake[0]);
    if (r->wake[1] >= 0) close(r->wake[1]);
    free(r);
}
