/* enet_transport.h -- additive ENet/UDP client transport for the CTM bridge.
 *
 * This sits ALONGSIDE the existing TCP path (connect_tcp + send_msg/recv_msg in
 * main.c). It is only used when the continuous dual-probe loop selects ENet; the
 * TCP code path is untouched and byte-identical. One CtmBridgeProtocol message
 * is carried per reliable ENet packet on a single channel, matching the Windows
 * EnetBridgeBackend.
 *
 * ENet hosts are single-threaded: every enet_* call here must happen on the
 * thread that owns the host (the main session loop). The input thread therefore
 * does NOT call into this module directly -- it hands reports to a thread-safe
 * outbox (enet_client_queue_output) that the session loop drains via
 * enet_client_service().
 */
#ifndef CTM_ENET_TRANSPORT_H
#define CTM_ENET_TRANSPORT_H

#include "ctm_bridge_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* enet_initialize()/enet_deinitialize() are process-global; main() owns them. */
int enet_client_global_init(void);
void enet_client_global_deinit(void);

typedef struct ctm_enet_client ctm_enet_client_t;

/* Allocate an idle client. Returns NULL on allocation/host-create failure. */
ctm_enet_client_t *enet_client_create(void);
void enet_client_destroy(ctm_enet_client_t *client);

/* Try to connect to host:port over ENet/UDP, waiting at most timeout_ms for the
 * CONNECT to complete. Returns 0 on success, -1 on timeout/failure. Short
 * timeouts are expected -- the dual-probe loop calls this first, briefly, then
 * falls back to TCP. */
int enet_client_connect(ctm_enet_client_t *client, const char *host, int port, unsigned int timeout_ms);

/* Gracefully disconnect (if connected) and return to idle. */
void enet_client_disconnect(ctm_enet_client_t *client);

/* 1 while the peer is connected, 0 once it has dropped. */
int enet_client_connected(const ctm_enet_client_t *client);

/* Pump the host for up to timeout_ms: flushes queued outbound reports, drives
 * acks/resends, and decodes inbound packets into the message inbox. Returns 0
 * normally, -1 if the link dropped during the call (caller should treat this as
 * disconnect). */
int enet_client_service(ctm_enet_client_t *client, unsigned int timeout_ms);

/* Like enet_client_service, but instead of blocking enet_host_service for
 * timeout_ms it poll()s the host socket and an internal eventfd (written by
 * enet_client_send_msg) so a queued report wakes the pump immediately, then
 * services the host non-blocking. max_timeout_ms is capped internally (<=10ms)
 * so ENet's retransmit/keepalive timers still fire. Owning thread only. */
int enet_client_service_wait(ctm_enet_client_t *client, unsigned int max_timeout_ms);

/* Build and queue one framed message (header + payload) for reliable delivery.
 * Thread-safe: callable from the input thread. The bytes are actually put on the
 * wire by the next enet_client_service() on the owning thread. Returns 0 on
 * success, -1 if not connected or on allocation failure. */
int enet_client_send_msg(ctm_enet_client_t *client, uint16_t type, uint32_t flags,
                         uint32_t request_id, const void *payload, size_t len);

/* Pop one decoded message from the inbox. Returns 1 and fills *h (and, when the
 * payload is non-empty, mallocs *payload which the caller must free) if a
 * message was available; returns 0 if the inbox is empty. */
int enet_client_recv_msg(ctm_enet_client_t *client, ctmb_header_t *h, uint8_t **payload);

#ifdef __cplusplus
}
#endif

#endif /* CTM_ENET_TRANSPORT_H */
