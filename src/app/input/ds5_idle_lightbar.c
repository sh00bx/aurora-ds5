#include "ds5_idle_lightbar.h"

#include <errno.h>
#include <fcntl.h>
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

/* Bound on how many times one flush may re-send while another thread keeps
 * flipping the state under it. Whoever flips last runs its own flush, so the
 * selection still converges; this only stops a pathological spin. */
#define LB_FLUSH_MAX_ROUNDS 8

/* State lock: guards the inputs and lb_sent. NEVER held across a syscall --
 * this is set from the SDL main loop, and that thread pumps every keyboard
 * event (app_process_events -> SDL_PumpEvents). Anything that can stall while
 * holding it stalls the keyboard with it. */
static pthread_mutex_t lb_state_lock = PTHREAD_MUTEX_INITIALIZER;
/* Send lock: serializes the socket itself, so the datagrams cannot overtake
 * each other. Taken alone, never together with lb_state_lock held. */
static pthread_mutex_t lb_send_lock = PTHREAD_MUTEX_INITIALIZER;

static bool lb_owned = false;
static bool lb_sdl_open = false;
static uint32_t lb_sent = LB_UNUSED;   /* the daemon's own boot default */
static int lb_fd = -1;                 /* guarded by lb_send_lock */
static bool lb_disabled = false;       /* AURORA_DS5_IDLE_LB=0 kill switch */
static bool lb_disabled_read = false;

static uint32_t lb_want_locked(void) {
    /* Ownership wins: while a passthrough session drives the pad the host paints
     * the bar, and the daemon must stay off it entirely. Inferring this from
     * daemon-side traffic is NOT good enough -- a session that only writes
     * sporadically looks idle within a second, and the painter then fights the
     * host's colour. Hence an explicit signal. */
    return lb_owned ? LB_OWNED : (lb_sdl_open ? LB_SDL : LB_UNUSED);
}

/* Send one selection. Caller holds lb_send_lock and no other lock.
 * Returns true only if the datagram actually left. */
static bool lb_send_one(uint32_t want) {
    if (lb_fd < 0) {
        const char *sp = getenv("DS5_ACL_SOCK");
        const char *sock = (sp && sp[0]) ? sp : "/tmp/ds5_acl.sock";
#ifdef SOCK_NONBLOCK
        lb_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
        lb_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
#endif
        if (lb_fd < 0) {
            return false;
        }
        /* Belt and braces: O_NONBLOCK is what keeps a full daemon queue from
         * parking this thread forever (an AF_UNIX datagram send is flow
         * controlled and blocks when the receiver's queue is full). */
        int fl = fcntl(lb_fd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(lb_fd, F_SETFL, fl | O_NONBLOCK);
        }
        /* connect() once so later sends are a plain send() to a fixed peer and
         * the path is resolved a single time. */
        struct sockaddr_un dst;
        memset(&dst, 0, sizeof dst);
        dst.sun_family = AF_UNIX;
        snprintf(dst.sun_path, sizeof dst.sun_path, "%s", sock);
        if (connect(lb_fd, (struct sockaddr *) &dst, sizeof dst) != 0) {
            /* No daemon (or not up yet): drop the socket and retry on the next
             * change, exactly as before -- behaviour without a daemon is a
             * no-op. */
            close(lb_fd);
            lb_fd = -1;
            return false;
        }
    }

    uint8_t msg[6] = { DS5_ACL_TAG_M0, DS5_ACL_TAG_CTRL, DS5_ACL_CTRL_IDLE_LB,
                       (uint8_t) (want >> 16), (uint8_t) (want >> 8), (uint8_t) want };
    ssize_t w;
    do {
        w = send(lb_fd, msg, sizeof msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (w < 0 && errno == EINTR);

    if (w == (ssize_t) sizeof msg) {
        commons_log_debug("Input", "DS5 idle lightbar -> %06x", want);
        return true;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* Daemon queue full. The bar is cosmetic -- never wait on it. The
         * selection stays un-sent and the next state change retries. */
        commons_log_debug("Input", "DS5 idle lightbar %06x skipped: daemon queue full", want);
        return false;
    }
    /* Peer gone/replaced (daemon restart): drop the fd so the next attempt
     * reconnects to the new socket. */
    close(lb_fd);
    lb_fd = -1;
    return false;
}

/* Push the current selection out, converging on whatever the state says now.
 * Runs with no state lock held, so a stalled or slow daemon can never hold up
 * a caller's own thread beyond one non-blocking syscall. */
static void lb_flush(void) {
    if (lb_disabled) {
        return;
    }
    pthread_mutex_lock(&lb_send_lock);
    for (int round = 0; round < LB_FLUSH_MAX_ROUNDS; round++) {
        pthread_mutex_lock(&lb_state_lock);
        uint32_t want = lb_want_locked();
        bool need = (want != lb_sent);
        pthread_mutex_unlock(&lb_state_lock);

        if (!need || !lb_send_one(want)) {
            break;
        }

        pthread_mutex_lock(&lb_state_lock);
        lb_sent = want;
        pthread_mutex_unlock(&lb_state_lock);
    }
    pthread_mutex_unlock(&lb_send_lock);
}

/* AURORA_DS5_IDLE_LB=0 turns the whole signal off without a rebuild -- a
 * one-line bisect handle if this path is ever suspected again. */
static void lb_check_disabled(void) {
    pthread_mutex_lock(&lb_state_lock);
    if (!lb_disabled_read) {
        const char *v = getenv("AURORA_DS5_IDLE_LB");
        lb_disabled = (v != NULL && v[0] == '0' && v[1] == '\0');
        lb_disabled_read = true;
        if (lb_disabled) {
            commons_log_info("Input", "DS5 idle lightbar signalling disabled by AURORA_DS5_IDLE_LB=0");
        }
    }
    pthread_mutex_unlock(&lb_state_lock);
}

void ds5_idle_lb_set_owned(bool owned) {
    lb_check_disabled();
    pthread_mutex_lock(&lb_state_lock);
    lb_owned = owned;
    pthread_mutex_unlock(&lb_state_lock);
    lb_flush();
}

void ds5_idle_lb_set_sdl_open(bool open) {
    lb_check_disabled();
    pthread_mutex_lock(&lb_state_lock);
    lb_sdl_open = open;
    pthread_mutex_unlock(&lb_state_lock);
    lb_flush();
}

void ds5_idle_lb_release(void) {
    lb_check_disabled();
    pthread_mutex_lock(&lb_state_lock);
    lb_owned = false;
    lb_sdl_open = false;
    pthread_mutex_unlock(&lb_state_lock);
    lb_flush();
    /* Shutdown: give the fd back too. */
    pthread_mutex_lock(&lb_send_lock);
    if (lb_fd >= 0) {
        close(lb_fd);
        lb_fd = -1;
    }
    pthread_mutex_unlock(&lb_send_lock);
}
