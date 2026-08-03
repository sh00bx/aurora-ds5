#include "ds5_idle_lightbar.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "logging.h"

/* Control datagram understood by ds5_txd: [A5][5C][02][R][G][B]. */
#define DS5_ACL_TAG_M0       0xA5
#define DS5_ACL_TAG_CTRL     0x5C
#define DS5_ACL_CTRL_IDLE_LB 0x02

#define LB_UNUSED   0x000002u   /* connected to the TV, nobody using it */
#define LB_SDL      0x020000u   /* open as one of our SDL gamepads */
#define LB_OWNED    0x000000u   /* 0 tells the daemon not to paint at all */

static pthread_mutex_t lb_lock = PTHREAD_MUTEX_INITIALIZER;
static bool lb_owned = false;
static bool lb_sdl_open = false;
static uint32_t lb_sent = LB_UNUSED;   /* the daemon's own boot default */

static void lb_send_locked(void) {
    /* Ownership wins: while a passthrough session drives the pad the host paints
     * the bar, and the daemon must stay off it entirely. Inferring this from
     * daemon-side traffic is NOT good enough -- a session that only writes
     * sporadically looks idle within a second, and the painter then fights the
     * host's colour. Hence an explicit signal. */
    uint32_t want = lb_owned ? LB_OWNED : (lb_sdl_open ? LB_SDL : LB_UNUSED);
    if (want == lb_sent) {
        return;
    }

    const char *sp = getenv("DS5_ACL_SOCK");
    const char *sock = (sp && sp[0]) ? sp : "/tmp/ds5_acl.sock";
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_un dst;
    memset(&dst, 0, sizeof dst);
    dst.sun_family = AF_UNIX;
    snprintf(dst.sun_path, sizeof dst.sun_path, "%s", sock);
    uint8_t msg[6] = { DS5_ACL_TAG_M0, DS5_ACL_TAG_CTRL, DS5_ACL_CTRL_IDLE_LB,
                       (uint8_t) (want >> 16), (uint8_t) (want >> 8), (uint8_t) want };
    ssize_t w = sendto(fd, msg, sizeof msg, 0, (struct sockaddr *) &dst, sizeof dst);
    close(fd);
    /* Only remember it as sent if it actually left: otherwise a daemon that
     * starts later would never receive the current selection. */
    if (w == (ssize_t) sizeof msg) {
        lb_sent = want;
        commons_log_debug("Input", "DS5 idle lightbar -> %06x (owned=%d sdl=%d)", want, lb_owned, lb_sdl_open);
    }
}

void ds5_idle_lb_set_owned(bool owned) {
    pthread_mutex_lock(&lb_lock);
    lb_owned = owned;
    lb_send_locked();
    pthread_mutex_unlock(&lb_lock);
}

void ds5_idle_lb_set_sdl_open(bool open) {
    pthread_mutex_lock(&lb_lock);
    lb_sdl_open = open;
    lb_send_locked();
    pthread_mutex_unlock(&lb_lock);
}

void ds5_idle_lb_release(void) {
    pthread_mutex_lock(&lb_lock);
    lb_owned = false;
    lb_sdl_open = false;
    lb_send_locked();
    pthread_mutex_unlock(&lb_lock);
}
