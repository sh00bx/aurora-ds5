#ifndef CTM_FEATURE_WORKER_H
#define CTM_FEATURE_WORKER_H

/* Host feature requests (HIDIOCGFEATURE / HIDIOCSFEATURE), executed off the
 * session thread.
 *
 * A feature ioctl over BT is a full TV->controller->TV transaction (10-100+ ms).
 * The session thread is the only ENet pump, so running one there stalled every
 * input report in the outbox for its duration — measured as the burst-signature
 * arrival gaps at the host. This module owns a queue and a single lazily-started
 * worker thread that does the blocking ioctl and sends the reply itself.
 *
 * The worker deliberately does NOT take the hid_io write mutex: holding it for a
 * blocked BT transaction would stall the session thread's output writes on that
 * same mutex, recreating the very stall this thread removes. The kernel
 * hidraw/hidp layer serializes raw-report transactions internally, and this
 * single worker already serializes feature ops among themselves. */

#include <stddef.h>
#include <stdint.h>

#include "ctm_controller_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctm_feature_worker ctm_feature_worker_t;

ctm_feature_worker_t *ctm_feature_worker_create(ctm_controller_t *owner);
void ctm_feature_worker_destroy(ctm_feature_worker_t *w);

/* Queue one request, lazily starting the worker. `type` is CTMB_MSG_FEATURE_GET
 * or _SET; `fd` is the hidraw descriptor the request addresses, which the caller
 * resolves on its own thread (the composite mapping is session-thread state) —
 * this function dup()s it, so the worker survives the original being closed on
 * unplug (the ioctl then fails with ENODEV instead of touching a reused
 * descriptor) and the caller keeps ownership of what it passed in.
 *
 * 0 = queued and the worker will send the reply. -1 = nothing was queued and
 * the CALLER must send the failure reply. */
int ctm_feature_worker_enqueue(ctm_feature_worker_t *w, uint16_t type, uint32_t request_id,
                               const uint8_t *payload, uint32_t len, int fd);

/* Stop + join the worker. Queued-but-unstarted requests are dropped without
 * emitting BT traffic. When: session teardown, BEFORE the transport is released
 * (the worker replies through it). */
void ctm_feature_worker_stop(ctm_feature_worker_t *w);

#ifdef __cplusplus
}
#endif

#endif /* CTM_FEATURE_WORKER_H */
