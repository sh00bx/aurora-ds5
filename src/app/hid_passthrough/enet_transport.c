#define _GNU_SOURCE

#include "enet_transport.h"

#include <enet/enet.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define CTM_ENET_CHANNEL 0
/* 2 channels: 0 = control + input (both directions), 1 = haptics ISO audio
 * host->TV, so bulk audio can't head-of-line-block reliable control traffic.
 * ENet negotiates min(client, agent) channels, so an old 1-channel agent
 * keeps everything on 0; the receive path dispatches by message type and
 * ignores the channel either way. */
#define CTM_ENET_CHANNEL_COUNT 2
#define CTM_ENET_INBOX_CAP 256
#define CTM_ENET_OUTBOX_CAP 256

typedef struct {
    ctmb_header_t header;
    uint8_t *payload;
} ctm_enet_msg_t;

struct ctm_enet_client {
    ENetHost *host;
    ENetPeer *peer;
    int connected;
    uint32_t send_sequence;

    /* Written (8 bytes) by enet_client_send_msg from the reader thread to wake
     * the session thread that owns the host; polled by enet_client_service_wait.
     * -1 if eventfd creation failed (falls back to a short poll tick). */
    int wake_efd;

    pthread_mutex_t out_mutex;
    ctm_enet_msg_t outbox[CTM_ENET_OUTBOX_CAP];
    int out_head;
    int out_count;

    pthread_mutex_t in_mutex;
    ctm_enet_msg_t inbox[CTM_ENET_INBOX_CAP];
    int in_head;
    int in_count;
};

static uint64_t enet_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

int enet_client_global_init(void) {
    return enet_initialize();
}

void enet_client_global_deinit(void) {
    enet_deinitialize();
}

ctm_enet_client_t *enet_client_create(void) {
    ctm_enet_client_t *client = (ctm_enet_client_t *) calloc(1, sizeof(*client));
    if (!client) {
        return NULL;
    }
    client->host = enet_host_create(AF_INET, NULL, 1, CTM_ENET_CHANNEL_COUNT, 0, 0);
    if (!client->host) {
        free(client);
        return NULL;
    }
    /* Mark input traffic as WMM voice class. Measured 2026-07-03 (pcap at the
     * host NIC): TV->host input loses ~1%/burst (122 loss-gaps >=50ms per 5min)
     * while host->TV is clean - the tiny input uplink loses the airtime race
     * against our own 60Mbps video downlink on the same channel. DSCP EF +
     * SO_PRIORITY 6 map to AC_VO (shorter AIFS/contention window), letting
     * these ~120B packets win medium access against the video bursts. */
    {
        int tos = 0xB8;      /* DSCP EF */
        int prio = 6;        /* TC_PRIO_INTERACTIVE -> 802.11 AC_VO */
        setsockopt(client->host->socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        setsockopt(client->host->socket, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
    }
    client->wake_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_mutex_init(&client->out_mutex, NULL);
    pthread_mutex_init(&client->in_mutex, NULL);
    return client;
}

static void free_msg_ring(ctm_enet_msg_t *ring, int cap, int head, int count) {
    for (int i = 0; i < count; i++) {
        int idx = (head + i) % cap;
        free(ring[idx].payload);
        ring[idx].payload = NULL;
    }
}

static void enet_client_reset_queues(ctm_enet_client_t *client) {
    pthread_mutex_lock(&client->out_mutex);
    free_msg_ring(client->outbox, CTM_ENET_OUTBOX_CAP, client->out_head, client->out_count);
    client->out_head = 0;
    client->out_count = 0;
    pthread_mutex_unlock(&client->out_mutex);

    pthread_mutex_lock(&client->in_mutex);
    free_msg_ring(client->inbox, CTM_ENET_INBOX_CAP, client->in_head, client->in_count);
    client->in_head = 0;
    client->in_count = 0;
    pthread_mutex_unlock(&client->in_mutex);
}

void enet_client_destroy(ctm_enet_client_t *client) {
    if (!client) {
        return;
    }
    if (client->peer) {
        enet_peer_reset(client->peer);
        client->peer = NULL;
    }
    enet_client_reset_queues(client);
    if (client->host) {
        enet_host_destroy(client->host);
        client->host = NULL;
    }
    if (client->wake_efd >= 0) {
        close(client->wake_efd);
        client->wake_efd = -1;
    }
    pthread_mutex_destroy(&client->out_mutex);
    pthread_mutex_destroy(&client->in_mutex);
    free(client);
}

static void enet_client_ingest(ctm_enet_client_t *client, const enet_uint8 *data, size_t length) {
    if (!data || length < sizeof(ctmb_header_t)) {
        return;
    }
    ctmb_header_t header;
    memcpy(&header, data, sizeof(header));
    if (header.magic != CTMB_MAGIC || header.version != CTMB_VERSION || header.payload_len > CTMB_MAX_PAYLOAD) {
        return;
    }
    if (length < sizeof(ctmb_header_t) + header.payload_len) {
        return;
    }

    uint8_t *payload = NULL;
    if (header.payload_len) {
        payload = (uint8_t *) malloc(header.payload_len);
        if (!payload) {
            return;
        }
        memcpy(payload, data + sizeof(ctmb_header_t), header.payload_len);
    }

    pthread_mutex_lock(&client->in_mutex);
    if (client->in_count >= CTM_ENET_INBOX_CAP) {
        free(client->inbox[client->in_head].payload);
        client->inbox[client->in_head].payload = NULL;
        client->in_head = (client->in_head + 1) % CTM_ENET_INBOX_CAP;
        client->in_count--;
    }
    int idx = (client->in_head + client->in_count) % CTM_ENET_INBOX_CAP;
    client->inbox[idx].header = header;
    client->inbox[idx].payload = payload;
    client->in_count++;
    pthread_mutex_unlock(&client->in_mutex);
}

static void enet_client_dispatch(ctm_enet_client_t *client, const ctm_enet_msg_t *msg) {
    if (!client->peer) {
        return;
    }
    size_t total = sizeof(ctmb_header_t) + msg->header.payload_len;
    enet_uint32 packet_flags =
            (msg->header.type == CTMB_MSG_INPUT_REPORT) ? 0u : ENET_PACKET_FLAG_RELIABLE;
    ENetPacket *packet = enet_packet_create(NULL, total, packet_flags);
    if (!packet) {
        return;
    }
    memcpy(packet->data, &msg->header, sizeof(ctmb_header_t));
    if (msg->header.payload_len && msg->payload) {
        memcpy(packet->data + sizeof(ctmb_header_t), msg->payload, msg->header.payload_len);
    }
    if (enet_peer_send(client->peer, CTM_ENET_CHANNEL, packet) < 0) {
        enet_packet_destroy(packet);
    }
}

static void enet_client_flush_outbox(ctm_enet_client_t *client) {
    for (;;) {
        ctm_enet_msg_t msg;
        pthread_mutex_lock(&client->out_mutex);
        if (client->out_count <= 0) {
            pthread_mutex_unlock(&client->out_mutex);
            break;
        }
        msg = client->outbox[client->out_head];
        client->outbox[client->out_head].payload = NULL;
        client->out_head = (client->out_head + 1) % CTM_ENET_OUTBOX_CAP;
        client->out_count--;
        pthread_mutex_unlock(&client->out_mutex);

        enet_client_dispatch(client, &msg);
        free(msg.payload);
    }
}

static int enet_client_handle_event(ctm_enet_client_t *client, ENetEvent *event) {
    switch (event->type) {
        case ENET_EVENT_TYPE_CONNECT:
            client->peer = event->peer;
            client->connected = 1;
            return 0;
        case ENET_EVENT_TYPE_RECEIVE:
            enet_client_ingest(client, event->packet->data, event->packet->dataLength);
            enet_packet_destroy(event->packet);
            return 0;
        case ENET_EVENT_TYPE_DISCONNECT:
            client->connected = 0;
            client->peer = NULL;
            return -1;
        default:
            return 0;
    }
}

int enet_client_connect(ctm_enet_client_t *client, const char *host, int port, unsigned int timeout_ms) {
    if (!client || !client->host || !host) {
        return -1;
    }

    ENetAddress address;
    memset(&address, 0, sizeof(address));
    if (enet_address_set_host(&address, host) != 0) {
        return -1;
    }
    if (enet_address_set_port(&address, (enet_uint16) port) != 0) {
        return -1;
    }

    client->peer = enet_host_connect(client->host, &address, CTM_ENET_CHANNEL_COUNT, 0);
    if (!client->peer) {
        return -1;
    }
    client->connected = 0;

    uint64_t start = enet_now_us();
    for (;;) {
        ENetEvent event;
        int rc = enet_host_service(client->host, &event, 10);
        if (rc < 0) {
            break;
        }
        if (rc > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
            client->peer = event.peer;
            client->connected = 1;
            /* Deceleration 0 pins the packet throttle at full: ENet's default
             * throttle (decel 2 per bad RTT sample, ~2 samples/s from pings)
             * walks down under WiFi RTT jitter and then SILENTLY DESTROYS
             * unreliable-sequenced packets before they hit the wire — our
             * input reports are the only unreliable traffic, so every
             * throttle step is uncounted input loss (no drop counter fires
             * on either end). Streaming already saturates the link; ENet's
             * politeness throttle only hurts here. */
            enet_peer_throttle_configure(client->peer,
                                         ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                         ENET_PEER_PACKET_THROTTLE_ACCELERATION,
                                         0);
            return 0;
        }
        if (rc > 0) {
            (void) enet_client_handle_event(client, &event);
            if (client->connected) {
                enet_peer_throttle_configure(client->peer,
                                             ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                             ENET_PEER_PACKET_THROTTLE_ACCELERATION,
                                             0);
                return 0;
            }
        }
        if (enet_now_us() - start >= (uint64_t) timeout_ms * 1000ull) {
            break;
        }
    }

    if (client->peer) {
        enet_peer_reset(client->peer);
        client->peer = NULL;
    }
    client->connected = 0;
    enet_client_reset_queues(client);
    return -1;
}

void enet_client_disconnect(ctm_enet_client_t *client) {
    if (!client) {
        return;
    }
    if (client->peer && client->connected) {
        enet_peer_disconnect(client->peer, 0);
        uint64_t start = enet_now_us();
        for (;;) {
            ENetEvent event;
            int rc = enet_host_service(client->host, &event, 10);
            if (rc > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    client->peer = NULL;
                    break;
                }
            }
            if (rc < 0 || enet_now_us() - start >= 200000ull) {
                break;
            }
        }
    }
    if (client->peer) {
        enet_peer_reset(client->peer);
        client->peer = NULL;
    }
    client->connected = 0;
    enet_client_reset_queues(client);
}

int enet_client_connected(const ctm_enet_client_t *client) {
    return client && client->connected;
}

int enet_client_service(ctm_enet_client_t *client, unsigned int timeout_ms) {
    if (!client || !client->host) {
        return -1;
    }
    int dropped = 0;

    enet_client_flush_outbox(client);

    ENetEvent event;
    int rc = enet_host_service(client->host, &event, timeout_ms);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        if (enet_client_handle_event(client, &event) < 0) {
            dropped = 1;
        }
        while (enet_host_check_events(client->host, &event) > 0) {
            if (enet_client_handle_event(client, &event) < 0) {
                dropped = 1;
            }
        }
    }

    enet_client_flush_outbox(client);
    return dropped ? -1 : 0;
}

int enet_client_service_wait(ctm_enet_client_t *client, unsigned int max_timeout_ms) {
    if (!client || !client->host) {
        return -1;
    }
    int dropped = 0;

    /* Queue any pending outbound reports as peer commands first. */
    enet_client_flush_outbox(client);

    /* Sleep until the host socket is readable (inbound), the reader thread
     * enqueued a report (eventfd), or the cap elapses. The cap keeps ENet's
     * retransmit/keepalive timers firing; without the eventfd (creation
     * failed) fall back to the previous short tick so sends stay responsive. */
    unsigned int cap = (client->wake_efd >= 0) ? 10u : 1u;
    if (max_timeout_ms > cap) {
        max_timeout_ms = cap;
    }
    struct pollfd pfds[2];
    int nfds = 0;
    pfds[nfds].fd = client->host->socket;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    nfds++;
    int efd_idx = -1;
    if (client->wake_efd >= 0) {
        efd_idx = nfds;
        pfds[nfds].fd = client->wake_efd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    int pr = poll(pfds, (nfds_t) nfds, (int) max_timeout_ms);
    if (pr < 0 && errno != EINTR) {
        return -1;
    }
    if (pr > 0 && efd_idx >= 0 && (pfds[efd_idx].revents & POLLIN)) {
        uint64_t drain;
        while (read(client->wake_efd, &drain, sizeof(drain)) == (ssize_t) sizeof(drain)) {
            /* drain the accumulated wake count */
        }
    }

    /* Non-blocking pump: sends the queued commands, receives inbound, drives
     * acks/resends. All ENet host access stays on this (the session) thread. */
    ENetEvent event;
    int rc = enet_host_service(client->host, &event, 0);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        if (enet_client_handle_event(client, &event) < 0) {
            dropped = 1;
        }
        while (enet_host_check_events(client->host, &event) > 0) {
            if (enet_client_handle_event(client, &event) < 0) {
                dropped = 1;
            }
        }
    }

    /* Flush anything the reader thread enqueued during the pump, and push it on
     * the wire now rather than waiting for the next service. */
    enet_client_flush_outbox(client);
    enet_host_flush(client->host);
    return dropped ? -1 : 0;
}

int enet_client_send_msg(ctm_enet_client_t *client, uint16_t type, uint32_t flags, uint32_t request_id,
                         const void *payload, size_t len) {
    if (!client || !client->connected) {
        return -1;
    }
    if (len > CTMB_MAX_PAYLOAD) {
        return -1;
    }

    uint8_t *copy = NULL;
    if (len) {
        copy = (uint8_t *) malloc(len);
        if (!copy) {
            return -1;
        }
        memcpy(copy, payload, len);
    }

    pthread_mutex_lock(&client->out_mutex);
    if (client->out_count >= CTM_ENET_OUTBOX_CAP) {
        /* Prefer dropping the oldest (stale) input report: the ring also holds
         * reliable control messages (FEATURE_REPORT replies) that must not be
         * silently destroyed. Only drop the head if no input report is queued. */
        int rel = 0;
        int found_input = 0;
        for (int i = 0; i < client->out_count; i++) {
            int probe = (client->out_head + i) % CTM_ENET_OUTBOX_CAP;
            if (client->outbox[probe].header.type == CTMB_MSG_INPUT_REPORT) {
                rel = i;
                found_input = 1;
                break;
            }
        }
        if (!found_input) {
            /* Ring holds only reliable control traffic — there is no stale input
             * report to sacrifice, and a reliable message must never be silently
             * destroyed. */
            pthread_mutex_unlock(&client->out_mutex);
            free(copy);
            if (type == CTMB_MSG_INPUT_REPORT) {
                /* Incoming is a (stale-in-milliseconds) input report: drop it and
                 * report success — the caller treats a send failure as link-down. */
                return 0;
            }
            /* Incoming is itself a reliable control message and the outbox is full
             * of reliable messages: the uplink is stalled. Fail the send so the
             * caller surfaces link-down instead of us evicting queued reliable data. */
            return -1;
        }
        int drop = (client->out_head + rel) % CTM_ENET_OUTBOX_CAP;
        free(client->outbox[drop].payload);
        client->outbox[drop].payload = NULL;
        /* Close the gap by shifting the entries between head and the victim up
         * one slot, then advance the head past the vacated slot. */
        for (int i = rel; i > 0; i--) {
            int dst = (client->out_head + i) % CTM_ENET_OUTBOX_CAP;
            int src = (client->out_head + i - 1) % CTM_ENET_OUTBOX_CAP;
            client->outbox[dst] = client->outbox[src];
        }
        client->outbox[client->out_head].payload = NULL;
        client->out_head = (client->out_head + 1) % CTM_ENET_OUTBOX_CAP;
        client->out_count--;
    }
    int idx = (client->out_head + client->out_count) % CTM_ENET_OUTBOX_CAP;
    ctmb_header_t *h = &client->outbox[idx].header;
    memset(h, 0, sizeof(*h));
    h->magic = CTMB_MAGIC;
    h->version = CTMB_VERSION;
    h->type = type;
    h->flags = flags;
    h->sequence = ++client->send_sequence;
    h->timestamp_us = enet_now_us();
    h->request_id = request_id;
    h->payload_len = (uint32_t) len;
    client->outbox[idx].payload = copy;
    client->out_count++;
    pthread_mutex_unlock(&client->out_mutex);

    /* Wake the session thread (which owns the host) so this message is put on
     * the wire on the next service instead of waiting for the poll tick. The
     * reader thread only ever touches wake_efd here; it never calls into ENet. */
    if (client->wake_efd >= 0) {
        uint64_t one = 1;
        ssize_t wr = write(client->wake_efd, &one, sizeof(one));
        (void) wr; /* EAGAIN just means the counter is already pending a drain */
    }
    return 0;
}

int enet_client_recv_msg(ctm_enet_client_t *client, ctmb_header_t *h, uint8_t **payload) {
    if (!client || !h || !payload) {
        return 0;
    }
    *payload = NULL;
    pthread_mutex_lock(&client->in_mutex);
    if (client->in_count <= 0) {
        pthread_mutex_unlock(&client->in_mutex);
        return 0;
    }
    *h = client->inbox[client->in_head].header;
    *payload = client->inbox[client->in_head].payload;
    client->inbox[client->in_head].payload = NULL;
    client->in_head = (client->in_head + 1) % CTM_ENET_INBOX_CAP;
    client->in_count--;
    pthread_mutex_unlock(&client->in_mutex);
    return 1;
}
