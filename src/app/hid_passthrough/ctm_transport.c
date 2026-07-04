#define _GNU_SOURCE

#include "ctm_transport.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

static int recv_all(int fd, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0 && errno == EINTR) continue;
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
     * TCP-fallback session rides AC_BE against our own video downlink. */
    int tos = 0xB8;      /* DSCP EF */
    int prio = 6;        /* TC_PRIO_INTERACTIVE -> 802.11 AC_VO */
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
}

static int connect_tcp(const char *host, int port)
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
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
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
    t->enet = enet;
    t->send_sequence = 0;
    pthread_mutex_init(&t->send_mutex, NULL);
}

void ctm_transport_destroy(ctm_transport_t *t)
{
    if (!t) return;
    pthread_mutex_destroy(&t->send_mutex);
}

void ctm_transport_attach_tcp(ctm_transport_t *t, int fd)
{
    if (!t) return;
    t->kind = CTM_TRANSPORT_TCP;
    t->fd = fd;
    if (fd >= 0) tune_tcp(fd);
}

int ctm_transport_connect_once(ctm_transport_t *t, const char *host, int port,
                               unsigned int enet_timeout_ms)
{
    if (!t) return -1;

    /* 1) ENet first, brief timeout. */
    if (t->enet && enet_client_connect(t->enet, host, port, enet_timeout_ms) == 0) {
        t->kind = CTM_TRANSPORT_ENET;
        t->fd = -1;
        return 0;
    }

    /* 2) Fall back to TCP. */
    int fd = connect_tcp(host, port);
    if (fd >= 0) {
        t->kind = CTM_TRANSPORT_TCP;
        t->fd = fd;
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
    if (recv_all(t->fd, h, sizeof(*h)) != 0) return -1;
    if (h->magic != CTMB_MAGIC || h->version != CTMB_VERSION ||
        h->payload_len > CTMB_MAX_PAYLOAD) {
        return -1;
    }
    if (h->payload_len) {
        *payload = (uint8_t *)malloc(h->payload_len);
        if (!*payload) return -1;
        if (recv_all(t->fd, *payload, h->payload_len) != 0) {
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

void ctm_transport_disconnect(ctm_transport_t *t)
{
    if (!t) return;
    if (t->kind == CTM_TRANSPORT_ENET) {
        if (t->enet) enet_client_disconnect(t->enet);
    } else if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
    }
    t->fd = -1;
    t->kind = CTM_TRANSPORT_NONE;
}
