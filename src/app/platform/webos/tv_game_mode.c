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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "logging.h"
#include "lunasynccall.h"

#define HB_EXEC "luna://org.webosbrew.hbchannel.service/exec"

/*
 * Where the bundled script ends up inside the installed app. The app directory
 * is not one fixed place: everything up to the G4 installs under
 * /media/developer, a rooted C5 under /media/cryptofs. Probing only the first
 * one makes a rooted C5 look like a TV with no root at all.
 */
#define SCRIPT_RELPATH "/tools/gamemode.sh"
#define APP_DIR_DEVELOPER "/media/developer/apps/usr/palm/applications/com.aurora.ds5"
#define APP_DIR_CRYPTOFS "/media/cryptofs/apps/usr/palm/applications/com.aurora.ds5"

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

/*
 * How long to let the panel settle after the session reports an HDR change
 * before re-asserting. The dimension does not flip with the metadata -- it
 * arrives a beat later (measured: a second or two), and an enforce that beats
 * it there reads the bucket we are leaving and changes nothing.
 */
#define HDR_SETTLE_MS 1500

/*
 * What a mid-session HDR change may cost. A kick pulls an enforce forward, and
 * an enforce is a gamemode.sh round trip: it holds the script's lock and
 * re-boosts every stream thread. The source of these notifications cannot be
 * trusted to be sane -- hosts exist that flip the HDR flag repeatedly on a
 * stream whose picture never changes, and the session layer's dedupe only
 * catches transitions, which is exactly what a flapping host produces. So the
 * bound lives here, where the cost is known, rather than with the caller, which
 * cannot see that its source is flapping. Once the gap or the budget is spent,
 * an HDR change is picked up by the ordinary schedule again -- one steady tick
 * later at worst, which is the behaviour the kick is an optimisation over.
 */
#define HDR_KICK_MIN_GAP_MS 5000
#define HDR_KICK_MAX_PER_CYCLE 6

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;
    bool stop;
    /* A stream reached STREAMING while the worker was still busy (tearing the
     * previous cycle down, or running the start-up recovery). The worker picks
     * this up when its shell work is done and runs one more full cycle instead
     * of exiting -- dropping such a begin would leave the whole new session
     * without game mode. */
    bool restart;
    /* The stream's HDR state just changed. The panel is about to move to its
     * other picture dimension, which carries its own picture mode, so one
     * enforce is owed off-schedule. */
    bool hdr_kick;
    /* netprio raised the kernel's receive buffers for this session and nothing
     * has put them back yet. It runs BEFORE the connection, so it outlives the
     * question of whether game mode ever engaged -- or ever started, for a
     * connection that fails before it reaches STREAMING -- and the teardown has
     * to lower them again on every one of those paths. */
    bool net_armed;
    /* A netprio call is out on its own thread and the session it belongs to is
     * still alive. The teardown clears it, which is how that thread learns its
     * session ended while it was still in the luna call -- the one case where a
     * raise can land after its own undo has already run. */
    bool net_pending;
    /* Rate limit for the kick: when the last one was taken (monotonic
     * milliseconds, 0 = none yet this cycle), how many this cycle has spent,
     * and whether the "I am dropping these" line has been said once already --
     * a flapping host would otherwise produce one log line per flip. */
    int64_t hdr_kick_at;
    int hdr_kicks;
    bool hdr_drop_said;
    /* Some verb has come back with returnValue:true in this process, so root
     * exists. A failure after that is a failure of one call -- most often the
     * script walking away from a contended lock -- and not the "this TV has no
     * Homebrew Channel" configuration. */
    bool root_seen;
} state = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
};

/*
 * The condvar cannot stay on the static PTHREAD_COND_INITIALIZER: that one
 * measures against the wall clock, and this TV steps the wall clock whenever
 * NTP lands after a network reconnect. A backwards step postpones every
 * pending tick by the size of the step -- including the early one that has to
 * catch the panel's late switch into its HDR dimension. Only an attribute set
 * BEFORE creation can put a condvar on CLOCK_MONOTONIC, so it is built once,
 * lazily, from whichever entry point runs first. The static initializer stays
 * as it was so the condvar is never invalid, only ever less precise.
 */
static pthread_once_t cond_once = PTHREAD_ONCE_INIT;
static bool cond_monotonic = false;

/* Returns true when the condvar came out on CLOCK_MONOTONIC, which is what the
 * caller must then time its deadline against. On false the condvar is still
 * usable, just on the wall clock: waiting on the wall clock is far better than
 * timing a wall-clock wait with a monotonic deadline. */
static bool cond_init_monotonic(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    bool monotonic = false;
    if (pthread_condattr_init(&attr) == 0) {
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 && pthread_cond_init(cond, &attr) == 0) {
            monotonic = true;
        }
        pthread_condattr_destroy(&attr);
    }
    if (!monotonic) {
        pthread_cond_init(cond, NULL);
    }
    return monotonic;
}

static void init_cond(void) {
    cond_monotonic = cond_init_monotonic(&state.cond);
}

/* Deadline for a pthread_cond_timedwait, on whichever clock the condvar it
 * belongs to was built with. */
static void deadline_after(struct timespec *until, int ms, bool monotonic) {
    clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, until);
    until->tv_sec += ms / 1000;
    until->tv_nsec += (long) (ms % 1000) * 1000000L;
    if (until->tv_nsec >= 1000000000L) {
        until->tv_nsec -= 1000000000L;
        until->tv_sec += 1;
    }
}

/* Monotonic milliseconds for measuring gaps, independent of the condvar's
 * clock. Returns 0 when the clock cannot be read, which the one caller reads as
 * "no idea how long ago" and lets the kick through -- the per-cycle budget
 * still bounds it. */
static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t) ts.tv_sec * 1000 + (int64_t) (ts.tv_nsec / 1000000L);
}

/* Start a fresh kick budget. Called with the lock held, wherever a new cycle
 * begins. */
static void hdr_kick_reset_locked(void) {
    state.hdr_kick = false;
    state.hdr_kick_at = 0;
    state.hdr_kicks = 0;
    state.hdr_drop_said = false;
}

/* Every public entry point starts here: waiter and wakers must both find the
 * condvar ready, and any of them can be the first to run. */
static void ensure_init(void) {
    pthread_once(&cond_once, init_cond);
}

static bool reply_ok(const char *reply) {
    return reply != NULL && strstr(reply, "\"returnValue\":true") != NULL;
}

/*
 * Resolve the script once: the live HOME first, then both known install roots,
 * and keep whichever one actually carries the script. Two threads reach this --
 * the game-mode worker and the session thread that runs netprio before it opens
 * its sockets -- so the one-time resolution goes through pthread_once rather
 * than a plain "is the cache filled yet" test.
 */
static char script_buf[320];

static void resolve_script(void) {
    const char *roots[3];
    size_t count = 0;
    const char *home = SDL_getenv("HOME");
    if (home != NULL && home[0] == '/') {
        roots[count++] = home;
    }
    roots[count++] = APP_DIR_DEVELOPER;
    roots[count++] = APP_DIR_CRYPTOFS;
    for (size_t i = 0; i < count; i++) {
        char probe[320];
        int written = snprintf(probe, sizeof(probe), "%s%s", roots[i], SCRIPT_RELPATH);
        if (written < 0 || (size_t) written >= sizeof(probe) || access(probe, R_OK) != 0) {
            continue;
        }
        memcpy(script_buf, probe, (size_t) written + 1);
        return;
    }
    /* Nothing readable from in here. Ask for the first candidate anyway and let
     * the reply say so: the shell runs as root and may well see a path this
     * process cannot. */
    snprintf(script_buf, sizeof(script_buf), "%s%s", roots[0], SCRIPT_RELPATH);
}

static pthread_once_t script_once = PTHREAD_ONCE_INIT;

static const char *script_path(void) {
    pthread_once(&script_once, resolve_script);
    return script_buf;
}

/*
 * Run one gamemode.sh verb as root. Returns false when there is no root path at
 * all, so the caller can stop trying rather than log the same thing every tick.
 *
 * script_ran, when asked for, separates "this TV has no root" from "the script
 * ran and came back non-zero" -- which for netprio is the ordinary outcome of
 * losing the race for the script's lock. Homebrew Channel hands the command's
 * own output back in its reply, so a line the script printed proves the exec
 * got that far. A firmware that returns no output on a non-zero exit leaves
 * this false and the caller falls back to state.root_seen.
 */
static bool run_verb_ex(const char *verb, bool *script_ran) {
    char payload[512];
    /* A truncated command would be a half-written shell line, which is worse
     * than not running at all. */
    int written = snprintf(payload, sizeof(payload), "{\"command\":\"sh '%s' %s\"}",
                           script_path(), verb);
    if (written < 0 || (size_t) written >= sizeof(payload)) {
        commons_log_error("GameMode", "command does not fit its buffer — skipping");
        return false;
    }

    char *reply = NULL;
    bool ok = HLunaServiceCallSync(HB_EXEC, payload, true, &reply) && reply_ok(reply);
    /* Every line the script prints carries this prefix (log() in gamemode.sh),
     * so finding one proves the exec reached the script. */
    bool ran = reply != NULL && strstr(reply, "[gamemode]") != NULL;
    if (script_ran != NULL) {
        *script_ran = ran;
    }
    if (ok) {
        pthread_mutex_lock(&state.lock);
        state.root_seen = true;
        pthread_mutex_unlock(&state.lock);
    }
    if (!ok) {
        /* Say which of the two it was. "The script ran and came back non-zero"
         * is a different fact from "the exec never got there", and for a verb
         * that walks away from a contended lock the first one is routine. */
        commons_log_info("GameMode", "'%s' %s: %s", verb,
                         ran ? "came back non-zero" : "did not run (Homebrew Channel unavailable?)",
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

static bool run_verb(const char *verb) {
    return run_verb_ex(verb, NULL);
}

typedef enum {
    WAIT_TIMEOUT = 0, /* the wait ran out: the scheduled tick is due */
    WAIT_KICK,        /* HDR changed: one enforce is owed off-schedule */
    WAIT_STOP,        /* the session ended */
} wait_result;

/* Wait for the stop signal, an HDR kick, or the timeout. */
static wait_result wait_or_stop(int ms) {
    struct timespec until;
    deadline_after(&until, ms, cond_monotonic);
    pthread_mutex_lock(&state.lock);
    while (!state.stop && !state.hdr_kick) {
        if (pthread_cond_timedwait(&state.cond, &state.lock, &until) != 0) {
            break;
        }
    }
    wait_result result = WAIT_TIMEOUT;
    if (state.stop) {
        result = WAIT_STOP;
    } else if (state.hdr_kick) {
        state.hdr_kick = false;
        result = WAIT_KICK;
    }
    pthread_mutex_unlock(&state.lock);
    return result;
}

/* Claim the pending netprio restore, if there is one. Whoever takes it owes the
 * TV an "off" or a "recover" -- both lower the buffers again. */
static bool net_armed_take(void) {
    pthread_mutex_lock(&state.lock);
    bool armed = state.net_armed;
    state.net_armed = false;
    pthread_mutex_unlock(&state.lock);
    return armed;
}

/*
 * What the worker was started for. Everything shares this one thread -- and the
 * running flag -- on purpose: two gamemode.sh shells must never run at once, or
 * one restarts the services the other just stopped.
 */
typedef enum {
    WORKER_STREAM = 0,   /* the full cycle: on, the enforce schedule, off */
    WORKER_RECOVER,      /* app start-up: put back whatever a killed session left engaged */
    WORKER_NET_RESTORE,  /* the receive buffers and nothing else */
} worker_job;

static void *game_mode_thread(void *arg) {
    worker_job job = (worker_job) (intptr_t) arg;
    while (true) {
        if (job == WORKER_RECOVER) {
            job = WORKER_STREAM;
            /* "recover" is the conditional form of "off": it looks for the
             * traces a killed session leaves behind (the picture state file,
             * the parked power governor) and only then puts things back, so a
             * normal start costs one round trip and changes nothing. */
            run_verb("recover");
            /* It restores the netprio state file along with the rest, so a
             * raise that was outstanding when it started is undone by the time
             * it returns -- drop the latch with it, or the next unbalanced
             * stream_end starts a second restore for a session that no longer
             * exists. Taken AFTER the verb because that is the point at which
             * the restore has provably happened; a raise that lands in the
             * moment between the two is claimed here without having been
             * undone, and is then put back by the next stream's "off" or the
             * next start's "recover", both of which restore that file
             * unconditionally. */
            net_armed_take();
        } else if (job == WORKER_NET_RESTORE) {
            job = WORKER_STREAM;
            /* Only the buffers. This job exists for a connection that failed
             * before it ever reached STREAMING, so game mode was never turned
             * on: no services were stopped, no cores pinned, no picture mode
             * written. "recover" would walk all of that -- ten systemctl
             * starts, the governor, the legacy guard -- to put back the one
             * thing that is actually engaged. */
            run_verb("netprio-off");
        } else {
            bool engaged = run_verb("on");

            if (engaged) {
                size_t step = 0;
                while (true) {
                    int ms = step < sizeof(ENFORCE_SCHEDULE_MS) / sizeof(ENFORCE_SCHEDULE_MS[0])
                                     ? ENFORCE_SCHEDULE_MS[step]
                                     : ENFORCE_STEADY_MS;
                    wait_result waited = wait_or_stop(ms);
                    if (waited == WAIT_STOP) {
                        break;
                    }
                    if (waited == WAIT_KICK) {
                        /* Let the panel reach its new dimension first, then
                         * enforce there. A kick deliberately does NOT consume a
                         * schedule step: HDR changing does not make the tick it
                         * interrupted unnecessary. The settle is a wait like
                         * any other, so an ending session still wins it. */
                        if (wait_or_stop(HDR_SETTLE_MS) == WAIT_STOP) {
                            break;
                        }
                    } else {
                        step++;
                    }
                    if (!run_verb("enforce")) {
                        /* One failed tick proves nothing about what is still
                         * engaged: exec fails transiently too (memory pressure
                         * is a steady state on this TV, and the script can
                         * exit non-zero while services stay stopped). Stop
                         * spending a round trip per tick, but keep the final
                         * "off" -- it is a no-op if root is really gone, and
                         * it is the only restore path in every other case. */
                        while (wait_or_stop(ENFORCE_STEADY_MS) != WAIT_STOP) {
                        }
                        break;
                    }
                }
            } else {
                /* Nothing was engaged, but still wait for the session to end
                 * so a second stream does not start a second thread. */
                while (wait_or_stop(ENFORCE_STEADY_MS) != WAIT_STOP) {
                }
            }

            if (engaged) {
                net_armed_take();
                run_verb("off");
            } else if (net_armed_take()) {
                /* No game mode to end, but netprio raised the receive buffers
                 * before the connection and nothing else will lower them.
                 * "recover" is the conditional "off": it puts back exactly what
                 * is still engaged, which here is the netprio state file and
                 * nothing else. */
                run_verb("recover");
            }
        }

        pthread_mutex_lock(&state.lock);
        if (state.restart) {
            /* A stream began while the shell work above was still running;
             * give it the cycle it asked for instead of exiting. */
            state.restart = false;
            state.stop = false;
            hdr_kick_reset_locked();
            pthread_mutex_unlock(&state.lock);
            continue;
        }
        state.running = false;
        state.stop = false;
        pthread_mutex_unlock(&state.lock);
        return NULL;
    }
}

/* Spawn the detached worker. Detached: nothing joins it, and
 * HLunaServiceCallSync has no timeout of its own, so it must never sit on the
 * UI thread. The job travels in the argument pointer -- the thread never
 * dereferences it. */
static bool spawn_worker(worker_job job) {
    pthread_attr_t attr;
    pthread_t tid;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, game_mode_thread, (void *) (intptr_t) job);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void tv_game_mode_stream_begin(void) {
    ensure_init();
    pthread_mutex_lock(&state.lock);
    if (state.running) {
        /* The worker still exists: the previous cycle is tearing down (one
         * "off" is seconds of luna work) or the start-up recovery is running.
         * Leave it a note to run a fresh cycle for this session once its
         * shell work is done. */
        state.restart = true;
        pthread_mutex_unlock(&state.lock);
        return;
    }
    state.running = true;
    state.stop = false;
    state.restart = false;
    hdr_kick_reset_locked();
    pthread_mutex_unlock(&state.lock);

    if (!spawn_worker(WORKER_STREAM)) {
        commons_log_warn("GameMode", "could not start the game mode thread");
        pthread_mutex_lock(&state.lock);
        state.running = false;
        pthread_mutex_unlock(&state.lock);
    }
}

void tv_game_mode_stream_end(void) {
    ensure_init();
    pthread_mutex_lock(&state.lock);
    bool running = state.running;
    /* A begin deferred behind a still-running teardown belongs to the session
     * that is ending right here -- cancel the deferred cycle with it. */
    state.restart = false;
    state.stop = true;
    pthread_cond_broadcast(&state.cond);
    /* netprio runs before LiStartConnection, so it can have raised the kernel's
     * receive buffers for a connection that never reached STREAMING -- in which
     * case tv_game_mode_stream_begin was never called and there is no worker to
     * hand the restore to. Start one here instead. This path is the reason
     * netprio may not rely on the game-mode cycle for its undo. */
    /* Whatever netprio is still doing on its own thread, it belongs to the
     * session that ends here. Clearing this is how that thread learns it has to
     * undo its own raise if it lands after this point. */
    state.net_pending = false;
    bool orphan_net = !running && state.net_armed;
    if (orphan_net) {
        state.net_armed = false;
        state.running = true;
        state.stop = false;
    }
    pthread_mutex_unlock(&state.lock);
    if (orphan_net) {
        /* Just the buffers: this connection never reached STREAMING, so nothing
         * else was ever engaged for it. */
        if (!spawn_worker(WORKER_NET_RESTORE)) {
            commons_log_warn("GameMode", "could not start the thread that lowers the receive buffers again");
            pthread_mutex_lock(&state.lock);
            state.running = false;
            /* Put the record back rather than dropping it with the thread: the
             * next teardown can still hand it to a worker, and the state file
             * on disk is what the next app start's recover reads either way. */
            state.net_armed = true;
            pthread_mutex_unlock(&state.lock);
        }
        return;
    }
    if (!running) {
        /* Nothing to stop, but clear the flag again so the next stream is not
         * greeted by a stale stop request. */
        pthread_mutex_lock(&state.lock);
        state.stop = false;
        pthread_mutex_unlock(&state.lock);
    }
}

/*
 * netprio is the one verb a stream waits for, and the only one that may not run
 * on the detached worker: a bigger rmem_max reaches sockets created after it and
 * no others, and the caller creates them on the next line. But waiting on the
 * luna call ITSELF is what must not happen. HLunaServiceCallSync is a bare
 * condition wait on the reply with no timeout of its own, so a Homebrew Channel
 * that never answers would park the session thread here for good -- before
 * LiStartConnection, with the UI on "Connecting" and no way out: session_interrupt
 * signals the session's own condvar, and this thread is not in it, so the app
 * then hangs again in SDL_WaitThread when the session is destroyed.
 *
 * So the call goes on a thread of its own and the session thread waits on this
 * handshake instead, with a deadline. A stream that starts with the stock
 * receive buffer is a bad outcome; a frozen app is a much worse one.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool monotonic;
    bool done;
    bool ok;
    bool script_ran;
    /* Both sides let go of this independently -- the waiter may walk away long
     * before the call comes back -- so whichever is last frees it. */
    int refs;
} net_prepare_call;

/*
 * The whole budget the connection pays for the raise. The script's own patience
 * for a contended lock is five seconds by design (an "off" that is still
 * restoring holds it for tens of seconds, and losing that race is a normal
 * outcome). This deadline has to sit ABOVE that, or it always fires first and
 * the honest "the script was busy" answer -- the one the reply carries -- can
 * never be read; beyond it, waiting only serves the case where the reply never
 * comes at all, which is exactly the case that must not be waited for.
 */
#define NET_PREPARE_WAIT_MS 7000

static void net_prepare_release(net_prepare_call *call) {
    pthread_mutex_lock(&call->lock);
    bool last = --call->refs == 0;
    pthread_mutex_unlock(&call->lock);
    if (!last) {
        return;
    }
    pthread_cond_destroy(&call->cond);
    pthread_mutex_destroy(&call->lock);
    free(call);
}

static void *net_prepare_thread(void *arg) {
    net_prepare_call *call = (net_prepare_call *) arg;
    bool script_ran = false;
    bool ok = run_verb_ex("netprio", &script_ran);
    bool undo_it_ourselves = false;
    pthread_mutex_lock(&state.lock);
    if (ok) {
        /* Armed from HERE and not from the waiter: if the deadline has already
         * passed, the raise still happened, and a raise nobody recorded is a
         * raise nothing lowers again until the next app start's recover. */
        state.net_armed = true;
        /* And the session can be over already -- this is the one call that can
         * come back after the teardown has run, in which case there is no
         * worker left to hand the undo to and nothing would lower the buffers
         * until the next stream. Take it on ourselves then. */
        if (!state.net_pending && !state.running) {
            state.net_armed = false;
            state.running = true;
            state.stop = false;
            undo_it_ourselves = true;
        }
    }
    state.net_pending = false;
    pthread_mutex_unlock(&state.lock);
    if (undo_it_ourselves && !spawn_worker(WORKER_NET_RESTORE)) {
        commons_log_warn("GameMode", "could not start the thread that lowers the receive buffers again");
        pthread_mutex_lock(&state.lock);
        state.running = false;
        state.net_armed = true; /* leave the record: the next teardown or app start still owes the undo */
        pthread_mutex_unlock(&state.lock);
    }
    pthread_mutex_lock(&call->lock);
    call->ok = ok;
    call->script_ran = script_ran;
    call->done = true;
    pthread_cond_broadcast(&call->cond);
    pthread_mutex_unlock(&call->lock);
    net_prepare_release(call);
    return NULL;
}

void tv_game_mode_net_prepare(void) {
    ensure_init();
    net_prepare_call *call = calloc(1, sizeof(*call));
    if (call == NULL) {
        /* Worth a line rather than a silent skip: this TV is under memory
         * pressure as a steady state, so an allocation failing here is a fact
         * about the moment the stream started. */
        commons_log_warn("GameMode", "no memory for the netprio handshake — stock receive buffer for this stream");
        return;
    }
    if (pthread_mutex_init(&call->lock, NULL) != 0) {
        free(call);
        commons_log_warn("GameMode", "could not set up the netprio handshake — stock receive buffer for this stream");
        return;
    }
    call->monotonic = cond_init_monotonic(&call->cond);
    call->refs = 2; /* this thread and the one it is about to start */
    pthread_mutex_lock(&state.lock);
    state.net_pending = true;
    pthread_mutex_unlock(&state.lock);

    pthread_attr_t attr;
    pthread_t tid;
    int rc = -1;
    if (pthread_attr_init(&attr) == 0) {
        /* Detached: nothing joins it, and after the deadline nothing waits for
         * it either. */
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(&tid, &attr, net_prepare_thread, call);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        net_prepare_release(call);
        net_prepare_release(call);
        pthread_mutex_lock(&state.lock);
        state.net_pending = false;
        pthread_mutex_unlock(&state.lock);
        commons_log_warn("GameMode", "could not start the thread that raises the receive buffers");
        return;
    }

    struct timespec until;
    deadline_after(&until, NET_PREPARE_WAIT_MS, call->monotonic);
    pthread_mutex_lock(&call->lock);
    while (!call->done) {
        if (pthread_cond_timedwait(&call->cond, &call->lock, &until) != 0) {
            break;
        }
    }
    bool done = call->done;
    bool ok = call->ok;
    bool script_ran = call->script_ran;
    pthread_mutex_unlock(&call->lock);
    net_prepare_release(call);

    if (ok) {
        return;
    }
    if (!done) {
        /* The verb is still out there. It will finish, and if its raise lands it
         * arms its own undo -- but too late for the sockets this session is
         * about to create. */
        commons_log_warn("GameMode",
                         "netprio has not come back after %d ms — starting the stream with the stock receive buffer",
                         NET_PREPARE_WAIT_MS);
        return;
    }
    /*
     * Two different failures end up here and only one of them is permanent.
     * Losing the race for the script's lock is a normal outcome by that script's
     * own design, and says nothing at all about root. So the one-shot
     * explanation is spent only while nothing in this process has ever reached
     * the script; once root is proven, every further failure is reported for
     * what it is, once per stream.
     */
    pthread_mutex_lock(&state.lock);
    bool root_seen = state.root_seen;
    pthread_mutex_unlock(&state.lock);
    if (script_ran || root_seen) {
        commons_log_info("GameMode",
                         "netprio did not run this time (the script was busy) — stock receive buffer for this stream");
        return;
    }
    /* No root here, and that is a supported configuration: say what it means
     * once and then stay quiet, because every stream would repeat it. */
    static bool said_it = false;
    if (!said_it) {
        said_it = true;
        commons_log_info("GameMode",
                         "no netprio: this stream keeps the stock receive buffer (webOS clamps it to 512 KiB)");
    }
}

bool tv_game_mode_notify_hdr(bool hdr_active) {
    ensure_init();
    int64_t now = now_ms();
    pthread_mutex_lock(&state.lock);
    /* Only a cycle that is actually running has something to re-assert, and
     * arming the flag outside one would spend the kick on the NEXT session. */
    bool take = state.running && !state.stop;
    bool limited = false;
    if (take) {
        /* now == 0 means the monotonic clock could not be read; let it through
         * and rely on the budget. */
        limited = state.hdr_kicks >= HDR_KICK_MAX_PER_CYCLE ||
                  (state.hdr_kick_at != 0 && now != 0 && now - state.hdr_kick_at < HDR_KICK_MIN_GAP_MS);
        take = !limited;
    }
    bool say_limited = false;
    if (take) {
        state.hdr_kick = true;
        state.hdr_kicks++;
        state.hdr_kick_at = now;
        pthread_cond_broadcast(&state.cond);
    } else if (limited && !state.hdr_drop_said) {
        state.hdr_drop_said = true;
        say_limited = true;
    }
    pthread_mutex_unlock(&state.lock);
    if (take) {
        commons_log_info("GameMode", "HDR %s — re-asserting the picture mode",
                         hdr_active ? "engaged" : "gone");
    } else if (say_limited) {
        /* Said once per cycle: a host that flips the flag would otherwise write
         * this line per flip. */
        commons_log_info("GameMode",
                         "HDR is changing faster than it can be worth re-asserting — leaving it to the schedule");
    }
    return take;
}

void tv_game_mode_recover_stale(void) {
    ensure_init();
    pthread_mutex_lock(&state.lock);
    if (state.running) {
        pthread_mutex_unlock(&state.lock);
        return;
    }
    state.running = true;
    state.stop = false;
    state.restart = false;
    hdr_kick_reset_locked();
    pthread_mutex_unlock(&state.lock);
    if (!spawn_worker(WORKER_RECOVER)) {
        commons_log_warn("GameMode", "could not start the recovery thread");
        pthread_mutex_lock(&state.lock);
        state.running = false;
        state.restart = false;
        pthread_mutex_unlock(&state.lock);
    }
}
