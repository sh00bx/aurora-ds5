/* Off-thread host feature requests. See ctm_feature_worker.h. */

#include "ctm_feature_worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif

#define FEAT_QUEUE_CAP 8

typedef struct {
    uint16_t type;                 /* CTMB_MSG_FEATURE_GET / _SET */
    uint32_t request_id;
    uint32_t len;
    int fd;                        /* dup'd; owned (closed) by the worker */
    uint8_t payload[CTM_MAX_REPORT];
} feat_req_t;

struct ctm_feature_worker {
    ctm_controller_t *owner;
    pthread_t thread;
    int started;
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    feat_req_t q[FEAT_QUEUE_CAP];
    int head, count;
    int run;
};

ctm_feature_worker_t *ctm_feature_worker_create(ctm_controller_t *owner)
{
    ctm_feature_worker_t *w = (ctm_feature_worker_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->owner = owner;
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cv, NULL);
    return w;
}

void ctm_feature_worker_destroy(ctm_feature_worker_t *w)
{
    if (!w) return;
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cv);
    free(w);
}

static void *feature_worker_main(void *arg)
{
    ctm_feature_worker_t *w = (ctm_feature_worker_t *)arg;
    prctl(PR_SET_NAME, (unsigned long)"ctm-feature", 0, 0, 0);
    for (;;) {
        pthread_mutex_lock(&w->mutex);
        while (w->run && w->count == 0)
            pthread_cond_wait(&w->cv, &w->mutex);
        if (!w->run && w->count == 0) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        feat_req_t req;
        memcpy(&req, &w->q[w->head], sizeof(req));
        w->head = (w->head + 1) % FEAT_QUEUE_CAP;
        w->count--;
        int running = w->run;
        pthread_mutex_unlock(&w->mutex);

        if (!running) {            /* shutting down: drop without BT traffic */
            if (req.fd >= 0) close(req.fd);
            continue;
        }
        int ok = 0;
        if (req.type == CTMB_MSG_FEATURE_GET) {
            if (req.fd >= 0 && ioctl(req.fd, HIDIOCGFEATURE(req.len), req.payload) >= 0)
                ok = ctm_ctl_send(w->owner, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK,
                                  req.request_id, req.payload, req.len) == 0;
            if (!ok) (void)ctm_ctl_send(w->owner, CTMB_MSG_FEATURE_REPORT, 0,
                                        req.request_id, NULL, 0);
        } else {
            if (req.fd >= 0)
                ok = ioctl(req.fd, HIDIOCSFEATURE((int)req.len), req.payload) >= 0;
            (void)ctm_ctl_send(w->owner, CTMB_MSG_FEATURE_REPORT, ok ? CTMB_FLAG_OK : 0,
                               req.request_id, NULL, 0);
        }
        if (req.fd >= 0) close(req.fd);
    }
    return NULL;
}

int ctm_feature_worker_enqueue(ctm_feature_worker_t *w, uint16_t type, uint32_t request_id,
                               const uint8_t *payload, uint32_t len, int fd)
{
    if (!w || len == 0 || len > CTM_MAX_REPORT) return -1;
    int dupfd = dup(fd);
    if (dupfd < 0) return -1;
    if (!w->started) {
        w->run = 1;
        w->head = w->count = 0;
        if (pthread_create(&w->thread, NULL, feature_worker_main, w) != 0) {
            w->run = 0;
            close(dupfd);
            return -1;
        }
        w->started = 1;
    }
    pthread_mutex_lock(&w->mutex);
    if (w->count == FEAT_QUEUE_CAP) {
        pthread_mutex_unlock(&w->mutex);
        close(dupfd);
        return -1;
    }
    feat_req_t *req = &w->q[(w->head + w->count) % FEAT_QUEUE_CAP];
    req->type = type;
    req->request_id = request_id;
    req->len = len;
    req->fd = dupfd;
    memcpy(req->payload, payload, len);
    w->count++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mutex);
    return 0;
}

void ctm_feature_worker_stop(ctm_feature_worker_t *w)
{
    if (!w || !w->started) return;
    pthread_mutex_lock(&w->mutex);
    w->run = 0;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);
    w->started = 0;
}
