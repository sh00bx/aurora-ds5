#ifndef CTM_TRANSPORT_H
#define CTM_TRANSPORT_H

/* Unified CTMB transport: one framed-message channel that is either raw TCP or
 * ENet/UDP. Promotes the abstraction that lived inline in hidraw_bridge.c
 * (hb_transport_t) and tv_bridge_worker.c (send_msg/recv_msg/connect_tcp/
 * dual-probe) into a single module so the framing + connect logic exists once.
 *
 * Behaviour matches the previously-proven worker path on the wire:
 *   - TCP connect via getaddrinfo (hostname or IP), TCP_NODELAY + keepalive.
 *     The connect itself is no longer the worker's blocking connect(2): it is
 *     non-blocking with a bounded budget and a cancel fd, because the old one
 *     could park a caller for the kernel's SYN-retry budget with no way out
 *     (see ctm_transport_cancel). Same handshake, same socket options.
 *   - send stamps magic/version/timestamp and a monotonic sequence under a
 *     mutex (thread-safe: the input thread and session loop may both send)
 *   - recv validates magic + version + payload_len
 *   - ENet rides the existing ctm_enet_client_t (one message per reliable
 *     packet); the owning thread drives it via ctm_transport_service()
 *
 * The transport is framing + connect ONLY. Pacing, CRC, and per-controller
 * patching stay in the layer above. */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_bridge_protocol.h"
#include "enet_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTM_TRANSPORT_NONE = 0,
    CTM_TRANSPORT_TCP = 1,
    CTM_TRANSPORT_ENET = 2
} ctm_transport_kind_t;

typedef struct {
    ctm_transport_kind_t kind;
    /* PRIVATE. TCP socket; -1 otherwise. Do not touch from outside this module.
     * Closed only by ctm_transport_disconnect, which runs either on the owning
     * session thread or on the UI thread once that thread has been joined; any
     * other thread that reads this field races that close and can act on a
     * descriptor number the kernel has already recycled. Use
     * ctm_transport_cancel() to interrupt, and ctm_transport_pollfd() to wait. */
    int fd;
    int cancel_efd;             /* PRIVATE: sticky cancel, see ctm_transport_cancel */
    pthread_mutex_t fd_mutex;   /* PRIVATE: fd publish/retire vs. cancel's shutdown */
    ctm_enet_client_t *enet;    /* borrowed; caller owns its lifecycle */
    pthread_mutex_t send_mutex; /* serializes TCP header+payload writes */
    uint32_t send_sequence;
} ctm_transport_t;

/* Initialize to idle. enet may be NULL (TCP-only). */
void ctm_transport_init(ctm_transport_t *t, ctm_enet_client_t *enet);

/* Close the cancel eventfd and destroy the mutexes. Does not touch the
 * (borrowed) enet client, and does NOT close the TCP fd — call
 * ctm_transport_disconnect first or the socket leaks. */
void ctm_transport_destroy(ctm_transport_t *t);

/* Wrap an already-accepted TCP socket (listen/server mode). */
void ctm_transport_attach_tcp(ctm_transport_t *t, int fd);

/* One dual-probe attempt: try ENet (if t->enet) for enet_timeout_ms, else TCP.
 * Sets kind/fd on success. Returns 0 if connected, -1 if neither answered this
 * attempt. Callers keep their own retry/backoff loop. */
int ctm_transport_connect_once(ctm_transport_t *t, const char *host, int port,
                               unsigned int enet_timeout_ms);

/* Send one framed message (header + payload). Thread-safe. Returns 0/-1. */
int ctm_transport_send_msg(ctm_transport_t *t, uint16_t type, uint32_t flags,
                           uint32_t request_id, const void *payload, size_t len);

/* Pop one received message. Returns 1 (got — fills *h and, for non-empty
 * payloads, mallocs *payload which the caller frees), 0 (none right now, ENet
 * only), -1 (link dropped / bad header). */
int ctm_transport_recv_msg(ctm_transport_t *t, ctmb_header_t *h, uint8_t **payload);

/* Pump ENet for up to timeout_ms (flush outbox, acks, decode inbound). TCP is a
 * no-op returning 0. Returns -1 if the ENet link dropped. */
int ctm_transport_service(ctm_transport_t *t, unsigned int timeout_ms);

/* ENet only: poll-driven variant of ctm_transport_service. Waits (up to a
 * capped max_timeout_ms) on the ENet socket + an internal eventfd so a report
 * queued by the input thread wakes the pump immediately. TCP is a no-op
 * returning 0. Returns -1 if the ENet link dropped. Owning thread only. */
int ctm_transport_service_wait(ctm_transport_t *t, unsigned int max_timeout_ms);

/* 1 if connected (TCP fd open / ENet peer up), else 0. */
int ctm_transport_connected(const ctm_transport_t *t);

/* Interrupt the three things this module can block on, from any thread: the TCP
 * connect poll, a recv waiting on a header, and a send parked on a full socket
 * buffer. Sticky — once cancelled the transport stays cancelled (including
 * across a disconnect/reconnect) until ctm_transport_destroy, so a wait that
 * starts after the cancel still returns at once, and connect_once refuses to
 * start another probe. This is how a stop request reaches the owning thread;
 * callers never reach into t->fd themselves.
 *
 * Does NOT reach inside the borrowed ENet client: enet_client_connect and
 * enet_client_disconnect run their own closed deadline loops (1200 ms / 200 ms
 * as called from here) with no cancellation input, so a stop can still cost one
 * in-flight ENet probe plus a disconnect budget. That is the residual, and it
 * is bounded — do not read it as the cancel having failed. */
void ctm_transport_cancel(ctm_transport_t *t);

/* The TCP fd, for poll(POLLIN) only; -1 when not on TCP. Owning session thread
 * only, and only for the duration of the poll — see the note on t->fd. */
int ctm_transport_pollfd(const ctm_transport_t *t);

/* Close the TCP fd / disconnect ENet; return to idle (enet client preserved). */
void ctm_transport_disconnect(ctm_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* CTM_TRANSPORT_H */
