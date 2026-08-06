/*
 * Bring up the bundled DS5 raw-ACL transport service.
 *
 * ds5_txd has to run as root, and it now ships inside this IPK instead of being
 * hand-installed under /var/lib/webosbrew. webOS installs our service jailed, so
 * something has to strip the jailer off its unit: that is Homebrew Channel's
 * elevate-service, reachable over luna. Every app update regenerates the unit in
 * its jailed form, so this is not a one-time setup step — it has to run, and be a
 * no-op, on every single app start.
 *
 * Two things make this safe to just fire and forget:
 *   - The whole sequence runs on a detached thread. HLunaServiceCallSync waits on
 *     a condvar with no timeout, so it must never sit on the UI thread.
 *   - Nothing downstream depends on it succeeding. ds5_acl_tx.c watches the
 *     daemon's readiness template with inotify and flips between raw-ACL and
 *     hidraw live, so a TV without Homebrew Channel simply stays on the
 *     daemon-free hidraw path and a daemon that comes up ten seconds late is
 *     picked up when it does.
 */

#include "ds5_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "lunasynccall.h"

#define DS5_SERVICE_ID "com.aurora.ds5.txd"

#define HB_SERVICE "luna://org.webosbrew.hbchannel.service"

/*
 * Prepare the luna config, then report whether the unit still needs elevating.
 *
 * The api-permissions write is the whole reason this runs before elevate-service
 * rather than after: elevate-service creates
 * api-permissions.d/<svc>.api.public.json as {"public":["<svc>/*"]}, which would
 * expose a ROOT service's API to every app on the TV. It only creates that file
 * when it does not already exist, so writing a restricted one first makes it skip
 * the step entirely — there is never a window in which the API is public. The
 * restricted form grants the service's own group, which is the group appinstalld
 * already hands to com.aurora.ds5 and to nobody else.
 *
 * The same branch repairs a public file left behind by an earlier build or by a
 * manual elevate-service run, and only then pays for an ls-control rescan.
 */
static const char *const PREP_COMMAND =
        "S=" DS5_SERVICE_ID "; N=0; F=0; "
        "for R in /var/luna-service2-dev /var/luna-service2; do "
        "U=$R/services.d/$S.service; [ -f $U ] || continue; "
        "grep -q '^Exec=/usr/bin/run-js-service' $U && N=1; "
        "[ -d $R/api-permissions.d ] || continue; "
        "P=$R/api-permissions.d/$S.api.public.json; "
        "if [ ! -f $P ] || grep -q public $P; then "
        "printf '{\\\"%s.group\\\":[\\\"%s/*\\\"]}' $S $S > $P.new && mv -f $P.new $P && F=1; "
        "fi; done; "
        /* hbchannel's exec has no timeout of its own and neither does our side of
         * the luna call, so bound the one step that talks to another daemon. */
        "[ $F = 1 ] && timeout 10 /usr/sbin/ls-control scan-services >/dev/null 2>&1; "
        "echo need_elevate=$N perms_fixed=$F";

/* The response payloads are small and we only ever look for fixed markers in
 * them, so a substring test beats dragging pbnjson in for three fields. */
static bool reply_ok(const char *reply) {
    return reply != NULL && strstr(reply, "\"returnValue\":true") != NULL;
}

static void *ds5_service_thread(void *arg) {
    (void) arg;

    char *reply = NULL;
    char payload[2048];
    /* A silently truncated command would be a half-written shell line, which is
     * a far worse failure than not running at all. */
    int written = snprintf(payload, sizeof(payload), "{\"command\":\"%s\"}", PREP_COMMAND);
    if (written < 0 || (size_t) written >= sizeof(payload)) {
        commons_log_error("DS5TXD", "elevation command does not fit its buffer — skipping");
        return NULL;
    }

    if (!HLunaServiceCallSync(HB_SERVICE "/exec", payload, true, &reply) || !reply_ok(reply)) {
        /* By far the most common reason is that Homebrew Channel is not
         * installed. That is a supported configuration, not a failure — it just
         * means no root transport, so say it once and leave the app on the
         * daemon-free path. */
        commons_log_info("DS5TXD", "no elevation path (Homebrew Channel unavailable?) — "
                                   "staying on the daemon-free hidraw path: %s",
                         reply ? reply : "no reply");
        free(reply);
        return NULL;
    }

    bool need_elevate = strstr(reply, "need_elevate=1") != NULL;
    bool perms_fixed = strstr(reply, "perms_fixed=1") != NULL;
    if (perms_fixed) {
        commons_log_info("DS5TXD", "restricted the service API to " DS5_SERVICE_ID ".group "
                                   "(elevate-service would have made it public)");
    }
    free(reply);
    reply = NULL;

    if (need_elevate) {
        /* elevateService runs the very same elevate-service script the manual
         * path uses, with our id as its only argument — the app name it derives
         * from that is com.aurora.ds5, which is what we want.
         *
         * That derivation is literally serviceName.split('.').slice(0, -1), so
         * the service id MUST stay "<app id>.<one segment>". Naming it
         * com.aurora.ds5txd instead would make elevate-service look for an app
         * called "com.aurora" and quietly elevate nothing. */
        commons_log_info("DS5TXD", "service is jailed — elevating");
        if (!HLunaServiceCallSync(HB_SERVICE "/elevateService", "{\"id\":\"" DS5_SERVICE_ID "\"}",
                                  true, &reply) || !reply_ok(reply)) {
            commons_log_warn("DS5TXD", "elevation failed, staying on the daemon-free path: %s",
                             reply ? reply : "no reply");
            free(reply);
            return NULL;
        }
        free(reply);
        reply = NULL;
    }

    /* Two attempts, because of one specific race: elevate-service rewrites the
     * unit but leaves a service process that is already holding the bus name
     * alone. If a jailed instance was still up, the first start lands on it; it
     * answers "not elevated" and exits, and ls-hubd brings up a fresh one off the
     * rewritten unit for the second try. Nothing else retries — a genuine failure
     * should show up in the log once, not loop. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (HLunaServiceCallSync("luna://" DS5_SERVICE_ID "/start", "{}", true, &reply) &&
            reply_ok(reply)) {
            commons_log_info("DS5TXD", "transport up: %s", reply);
            free(reply);
            return NULL;
        }
        bool jailed = reply != NULL && strstr(reply, "not elevated") != NULL;
        if (attempt == 0 && jailed) {
            commons_log_info("DS5TXD", "a jailed instance still held the bus — retrying");
            free(reply);
            reply = NULL;
            /* Give the old instance time to finish exiting, otherwise the retry
             * just lands on it again. */
            usleep(750 * 1000);
            continue;
        }
        commons_log_warn("DS5TXD", "transport did not start, staying on the daemon-free path: %s",
                         reply ? reply : "no reply");
        break;
    }
    free(reply);
    return NULL;
}

void ds5_service_bootstrap(void) {
    pthread_t tid;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return;
    }
    /* Detached: nothing joins this and nothing waits on its result. */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, ds5_service_thread, NULL) != 0) {
        commons_log_warn("DS5TXD", "could not start the elevation thread");
    }
    pthread_attr_destroy(&attr);
}
