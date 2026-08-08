#define _GNU_SOURCE

#include "ctm_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Budget for one TCP connect attempt. The caller keeps its own retry loop, so
 * this only has to be long enough for a LAN handshake — it is the ceiling on
 * how long a stop request can sit unnoticed on the connect path. */
#define CTM_CONNECT_TIMEOUT_MS 1500

static uint64_t ctm_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read exactly len bytes, or fail. Interruptible: a header that only half
 * arrives used to park the session thread here forever, out of reach of the
 * stop path. Try-first/wait-second keeps the common case at the same single
 * syscall the old blocking recv cost — the caller already poll()ed the socket,
 * so the bytes are normally sitting there and only a short read waits. */
static int recv_all(int fd, void *data, size_t len, int cancel_fd)
{
    uint8_t *p = (uint8_t *)data;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, cancel_fd >= 0 ? MSG_DONTWAIT : 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && cancel_fd >= 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfds[2];
            pfds[0].fd = fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
            pfds[1].fd = cancel_fd; pfds[1].events = POLLIN; pfds[1].revents = 0;
            int pr = poll(pfds, 2, -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (pfds[1].revents & POLLIN) return -1;   /* cancelled */
            continue;
        }
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static void tune_tcp(int fd)
{
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    /* Same AC_VO marking as the ENet socket (enet_transport.c): without it a
     * TCP-fallback session rides AC_BE against our own video downlink.
     * connect_tcp resolves AF_UNSPEC, so mark per family — IP_TOS fails
     * silently on an IPv6 socket (that needs IPV6_TCLASS). SO_PRIORITY is
     * what maps skb->priority to the WMM AC and is family-agnostic. */
    int tos = 0xB8;      /* DSCP EF */
    int prio = 6;        /* TC_PRIO_INTERACTIVE -> 802.11 AC_VO */
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 && ss.ss_family == AF_INET6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
    } else {
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    }
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
}

/* One address, non-blocking, bounded, cancellable. A plain connect(2) here
 * parks the session thread for the kernel's whole SYN-retry budget when the
 * host is powered down or its firewall drops rather than resets — and plug_out
 * joins that thread, so the freeze lands on the LVGL main thread. Sets
 * *cancelled when the stop request (not the budget) ended the attempt, so the
 * caller stops walking the address list instead of paying the budget again. */
static int connect_one(const struct addrinfo *rp, int cancel_fd, int *cancelled)
{
    int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        uint64_t deadline = ctm_now_us() + (uint64_t)CTM_CONNECT_TIMEOUT_MS * 1000ull;
        for (;;) {
            struct pollfd pfds[2];
            int nfds = 0;
            pfds[nfds].fd = fd; pfds[nfds].events = POLLOUT; pfds[nfds].revents = 0; nfds++;
            int cancel_idx = -1;
            if (cancel_fd >= 0) {
                cancel_idx = nfds;
                pfds[nfds].fd = cancel_fd; pfds[nfds].events = POLLIN; pfds[nfds].revents = 0; nfds++;
            }
            uint64_t now = ctm_now_us();
            int wait_ms = now >= deadline ? 0 : (int)((deadline - now) / 1000ull);
            int pr = poll(pfds, (nfds_t)nfds, wait_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            if (pr > 0 && cancel_idx >= 0 && (pfds[cancel_idx].revents & POLLIN)) {
                *cancelled = 1;
                close(fd);
                return -1;
            }
            /* Timeout, or POLLOUT: either way the fd is ours to dispose of —
             * bailing without the close leaks one descriptor per attempt, and
             * the caller retries this every 500ms for as long as the host is
             * down. SO_ERROR is what tells the two apart. */
            if (pr == 0) {
                close(fd);
                return -1;
            }
            break;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
            close(fd);
            return -1;
        }
    }
    /* Back to blocking: send_all wants it, and recv_all does its own
     * non-blocking probe rather than relying on the socket flag. */
    if (fcntl(fd, F_SETFL, flags) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_tcp(const char *host, int port, int cancel_fd)
{
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_text, &hints, &result) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        int cancelled = 0;
        fd = connect_one(rp, cancel_fd, &cancelled);
        if (fd >= 0 || cancelled) break;
    }
    freeaddrinfo(result);
    if (fd >= 0) tune_tcp(fd);
    return fd;
}

void ctm_transport_init(ctm_transport_t *t, ctm_enet_client_t *enet)
{
    if (!t) return;
    t->kind = CTM_TRANSPORT_NONE;
    t->fd = -1;
    /* Cancellation is sticky: written once by ctm_transport_cancel and never
     * drained, so a poll that starts after the cancel still returns at once and
     * a reconnect attempt cannot resurrect the transport. -1 (creation failed)
     * degrades to the bounded connect budget, same idiom as enet_transport's
     * wake_efd. */
    t->cancel_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    t->enet = enet;
    t->send_sequence = 0;
    pthread_mutex_init(&t->fd_mutex, NULL);
    pthread_mutex_init(&t->send_mutex, NULL);
}

void ctm_transport_destroy(ctm_transport_t *t)
{
    if (!t) return;
    if (t->cancel_efd >= 0) {
        close(t->cancel_efd);
        t->cancel_efd = -1;
    }
    pthread_mutex_destroy(&t->send_mutex);
    pthread_mutex_destroy(&t->fd_mutex);
}

void ctm_transport_cancel(ctm_transport_t *t)
{
    if (!t) return;
    if (t->cancel_efd >= 0) {
        uint64_t one = 1;
        ssize_t wr = write(t->cancel_efd, &one, sizeof(one));
        (void)wr; /* nothing drains it, so a full counter is still "cancelled" */
    }
    /* The eventfd reaches the connect poll and recv_all; a send parked on a
     * full socket buffer only comes back from a shutdown. fd_mutex is a leaf
     * held across this one syscall and nothing else — deliberately NOT
     * send_mutex, which the parked sender is holding and which would block the
     * caller (the LVGL thread, via plug_out) for exactly as long as the freeze
     * this is here to remove. Holding it also settles the fd-reuse race against
     * the close() in ctm_transport_disconnect: we either shut down a live fd or
     * see -1, never a number handed on to someone else. */
    pthread_mutex_lock(&t->fd_mutex);
    if (t->fd >= 0) shutdown(t->fd, SHUT_RDWR);
    pthread_mutex_unlock(&t->fd_mutex);
    /* ENet needs nothing: its pump caps each wait at 10ms, so the owning thread
     * sees the caller's stop flag on the next tick. */
}

void ctm_transport_attach_tcp(ctm_transport_t *t, int fd)
{
    if (!t) return;
    pthread_mutex_lock(&t->fd_mutex);
    t->kind = CTM_TRANSPORT_TCP;
    t->fd = fd;
    pthread_mutex_unlock(&t->fd_mutex);
    if (fd >= 0) tune_tcp(fd);
}

int ctm_transport_connect_once(ctm_transport_t *t, const char *host, int port,
                               unsigned int enet_timeout_ms)
{
    if (!t) return -1;

    /* 1) ENet first, brief timeout. */
    if (t->enet && enet_client_connect(t->enet, host, port, enet_timeout_ms) == 0) {
        pthread_mutex_lock(&t->fd_mutex);
        t->kind = CTM_TRANSPORT_ENET;
        t->fd = -1;
        pthread_mutex_unlock(&t->fd_mutex);
        return 0;
    }

    /* 2) Fall back to TCP. */
    int fd = connect_tcp(host, port, t->cancel_efd);
    if (fd >= 0) {
        pthread_mutex_lock(&t->fd_mutex);
        t->kind = CTM_TRANSPORT_TCP;
        t->fd = fd;
        pthread_mutex_unlock(&t->fd_mutex);
        return 0;
    }
    return -1;
}

int ctm_transport_send_msg(ctm_transport_t *t, uint16_t type, uint32_t flags,
                           uint32_t request_id, const void *payload, size_t len)
{
    if (!t || len > CTMB_MAX_PAYLOAD) return -1;

    /* ENet: enet_client_send_msg is thread-safe (queues to an outbox the
     * service pump drains on the owning thread) and stamps its own header. */
    if (t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_send_msg(t->enet, type, flags, request_id, payload, len);
    }

    if (t->fd < 0) return -1;
    ctmb_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = CTMB_MAGIC;
    h.version = CTMB_VERSION;
    h.type = type;
    h.flags = flags;
    h.timestamp_us = ctm_now_us();
    h.request_id = request_id;
    h.payload_len = (uint32_t)len;

    pthread_mutex_lock(&t->send_mutex);
    h.sequence = ++t->send_sequence;
    int rc = 0;
    if (send_all(t->fd, &h, sizeof(h)) != 0) rc = -1;
    else if (len && send_all(t->fd, payload, len) != 0) rc = -1;
    pthread_mutex_unlock(&t->send_mutex);
    return rc;
}

int ctm_transport_recv_msg(ctm_transport_t *t, ctmb_header_t *h, uint8_t **payload)
{
    *payload = NULL;
    if (!t) return -1;
    if (t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_recv_msg(t->enet, h, payload);
    }
    if (t->fd < 0) return -1;
    if (recv_all(t->fd, h, sizeof(*h), t->cancel_efd) != 0) return -1;
    if (h->magic != CTMB_MAGIC || h->version != CTMB_VERSION ||
        h->payload_len > CTMB_MAX_PAYLOAD) {
        return -1;
    }
    if (h->payload_len) {
        *payload = (uint8_t *)malloc(h->payload_len);
        if (!*payload) return -1;
        if (recv_all(t->fd, *payload, h->payload_len, t->cancel_efd) != 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }
    return 1;
}

int ctm_transport_service(ctm_transport_t *t, unsigned int timeout_ms)
{
    if (t && t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_service(t->enet, timeout_ms);
    }
    return 0;
}

int ctm_transport_service_wait(ctm_transport_t *t, unsigned int max_timeout_ms)
{
    if (t && t->kind == CTM_TRANSPORT_ENET) {
        return enet_client_service_wait(t->enet, max_timeout_ms);
    }
    return 0;
}

int ctm_transport_connected(const ctm_transport_t *t)
{
    if (!t) return 0;
    if (t->kind == CTM_TRANSPORT_ENET) return enet_client_connected(t->enet);
    return t->fd >= 0;
}

int ctm_transport_pollfd(const ctm_transport_t *t)
{
    if (!t || t->kind != CTM_TRANSPORT_TCP) return -1;
    return t->fd;
}

void ctm_transport_disconnect(ctm_transport_t *t)
{
    if (!t) return;
    if (t->kind == CTM_TRANSPORT_ENET) {
        if (t->enet) enet_client_disconnect(t->enet);
    }
    /* Retire the fd under fd_mutex before closing it, so a concurrent
     * ctm_transport_cancel either shuts it down while it is still ours or sees
     * -1 — it can never land on the number the kernel reissued after the close.
     * ENet mode holds no fd, so the branch below is simply a no-op there. */
    pthread_mutex_lock(&t->fd_mutex);
    int fd = t->fd;
    t->fd = -1;
    pthread_mutex_unlock(&t->fd_mutex);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    t->kind = CTM_TRANSPORT_NONE;
}
