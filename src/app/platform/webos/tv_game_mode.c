/*
 * Game mode for the duration of a stream.
 *
 * This used to be a root daemon of its own: a boot hook under
 * /var/lib/webosbrew started moonlight-guard.sh, which polled `pidof aurora`
 * every three seconds and drove gamemode.sh on the transitions it inferred from
 * that. It worked, but it was a second thing to install, it guessed at a state
 * the app already knows for certain, and it was engaged the whole time the app
 * was merely OPEN -- menus included -- because "aurora is running" was the only
 * signal it had.
 *
 * The script now travels inside the IPK and the app calls it directly. The
 * trigger stops being a guess (the session tells us exactly when a stream
 * starts and stops), game mode no longer leaks into menu browsing, and there is
 * nothing left to hand-install.
 *
 * Root comes from Homebrew Channel's exec, the same path the DS5 transport
 * already uses (see ds5_service.c). No Homebrew Channel means no root means no
 * game mode -- which is a supported configuration, not a failure: the stream
 * runs, it just runs without the TV's services and picture pipeline tuned for
 * it.
 */

#include "tv_game_mode.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "logging.h"
#include "lunasynccall.h"

#define HB_EXEC "luna://org.webosbrew.hbchannel.service/exec"

/* Where the bundled script ends up inside the installed app. */
#define SCRIPT_RELPATH "/tools/gamemode.sh"
#define APP_DIR_FALLBACK "/media/developer/apps/usr/palm/applications/com.aurora.ds5"

/*
 * Re-assert cadence. The old guard re-asserted every 3s around the clock; this
 * front-loads instead, because the two things that actually need a second look
 * both happen early: the panel switches to its HDR dimension a beat after the
 * stream starts (a different picture-mode bucket, so the mode has to be set
 * again there), and the stream's threads are not all spawned when the first
 * boost runs. After that it is only guarding against LG's power governor
 * re-enabling core hotplug, which is not a three-second problem.
 */
static const int ENFORCE_SCHEDULE_MS[] = {2000, 3000, 5000, 10000};
#define ENFORCE_STEADY_MS 15000

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;
    bool stop;
} state = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
};

static bool reply_ok(const char *reply) {
    return reply != NULL && strstr(reply, "\"returnValue\":true") != NULL;
}

/*
 * Run one gamemode.sh verb as root. Returns false when there is no root path at
 * all, so the caller can stop trying rather than log the same thing every tick.
 */
static bool run_verb(const char *verb) {
    const char *home = SDL_getenv("HOME");
    if (home == NULL || home[0] != '/') {
        home = APP_DIR_FALLBACK;
    }
    char payload[512];
    /* A truncated command would be a half-written shell line, which is worse
     * than not running at all. */
    int written = snprintf(payload, sizeof(payload), "{\"command\":\"sh '%s%s' %s\"}", home,
                           SCRIPT_RELPATH, verb);
    if (written < 0 || (size_t) written >= sizeof(payload)) {
        commons_log_error("GameMode", "command does not fit its buffer — skipping");
        return false;
    }

    char *reply = NULL;
    bool ok = HLunaServiceCallSync(HB_EXEC, payload, true, &reply) && reply_ok(reply);
    if (!ok) {
        commons_log_info("GameMode", "'%s' did not run (Homebrew Channel unavailable?): %s", verb,
                         reply ? reply : "no reply");
    } else if (strcmp(verb, "enforce") != 0) {
        /* on/off are worth seeing in full; the script logs what it changed and
         * what it put back, and that output is the only record of what happened
         * to the user's TV settings. */
        commons_log_info("GameMode", "%s: %s", verb, reply);
    }
    free(reply);
    return ok;
}

/* Wait for the stop signal, or for the timeout. True means "stop now". */
static bool wait_or_stop(int ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec += 1;
    }
    pthread_mutex_lock(&state.lock);
    while (!state.stop) {
        if (pthread_cond_timedwait(&state.cond, &state.lock, &until) != 0) {
            break;
        }
    }
    bool stop = state.stop;
    pthread_mutex_unlock(&state.lock);
    return stop;
}

static void *game_mode_thread(void *arg) {
    (void) arg;
    bool engaged = run_verb("on");

    if (engaged) {
        size_t step = 0;
        while (true) {
            int ms = step < sizeof(ENFORCE_SCHEDULE_MS) / sizeof(ENFORCE_SCHEDULE_MS[0])
                             ? ENFORCE_SCHEDULE_MS[step]
                             : ENFORCE_STEADY_MS;
            step++;
            if (wait_or_stop(ms)) {
                break;
            }
            if (!run_verb("enforce")) {
                /* Root went away mid-session. Nothing to re-assert and nothing
                 * to restore -- stop spending a luna round trip on it. */
                engaged = false;
                break;
            }
        }
    } else {
        /* Nothing was engaged, but still wait for the session to end so a
         * second stream does not start a second thread. */
        while (!wait_or_stop(ENFORCE_STEADY_MS)) {
        }
    }

    if (engaged) {
        run_verb("off");
    }

    pthread_mutex_lock(&state.lock);
    state.running = false;
    state.stop = false;
    pthread_mutex_unlock(&state.lock);
    return NULL;
}

void tv_game_mode_stream_begin(void) {
    pthread_mutex_lock(&state.lock);
    if (state.running) {
        pthread_mutex_unlock(&state.lock);
        return;
    }
    state.running = true;
    state.stop = false;
    pthread_mutex_unlock(&state.lock);

    pthread_attr_t attr;
    pthread_t tid;
    if (pthread_attr_init(&attr) != 0) {
        goto failed;
    }
    /* Detached: nothing joins this, and HLunaServiceCallSync has no timeout of
     * its own, so it must never sit on the UI thread. */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, game_mode_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        return;
    }
    commons_log_warn("GameMode", "could not start the game mode thread");
failed:
    pthread_mutex_lock(&state.lock);
    state.running = false;
    pthread_mutex_unlock(&state.lock);
}

void tv_game_mode_stream_end(void) {
    pthread_mutex_lock(&state.lock);
    bool running = state.running;
    state.stop = true;
    pthread_cond_broadcast(&state.cond);
    pthread_mutex_unlock(&state.lock);
    if (!running) {
        /* Nothing to stop, but clear the flag again so the next stream is not
         * greeted by a stale stop request. */
        pthread_mutex_lock(&state.lock);
        state.stop = false;
        pthread_mutex_unlock(&state.lock);
    }
}

static void *recover_thread(void *arg) {
    (void) arg;
    /* "recover" is the conditional form of "off": it looks for the traces a
     * killed session leaves behind (the picture state file, the parked power
     * governor) and only then puts things back, so a normal start costs one
     * round trip and changes nothing. */
    run_verb("recover");
    return NULL;
}

void tv_game_mode_recover_stale(void) {
    pthread_attr_t attr;
    pthread_t tid;
    if (pthread_attr_init(&attr) != 0) {
        return;
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, recover_thread, NULL) != 0) {
        commons_log_warn("GameMode", "could not start the recovery thread");
    }
    pthread_attr_destroy(&attr);
}
