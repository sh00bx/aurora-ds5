#include "worker.h"
#include "backend/pcmanager/priv.h"
#include "backend/pcmanager/pclist.h"

#include <assert.h>

#include "errors.h"
#include "util/bus.h"
#include "app.h"
#include "logging.h"
#include "ui/fatal_error.h"

int worker_host_update(worker_context_t *context) {
    const pclist_t *node = pcmanager_node(context->manager, &context->uuid);
    if (node == NULL) {
        return GS_FAILED;
    }
    return pcmanager_update_by_host(context, node->server->serverInfo.address, node->server->extPort, true);
}

int pcmanager_update_by_host(worker_context_t *context, const char *ip, uint16_t port, bool force) {
    assert(context != NULL);
    assert(context->manager != NULL);
    assert(ip != NULL);
    int ret = 0;

    pcmanager_t *manager = context->manager;

    // Fetch server info
    GS_CLIENT client = app_gs_client_new(manager->app);
    SERVER_DATA *server = serverdata_new();
    Uint32 t0 = SDL_GetTicks();
    ret = gs_get_status(client, server, strdup(ip), port, app_configuration->unsupported);
    Uint32 dt = SDL_GetTicks() - t0;
    if (dt > 150) {
        /* Connect-latency instrumentation: a serverinfo query slower than 150ms
         * is part of why the host tile takes long to become clickable — record
         * it in the same jail-visible file the session worker marks. */
        FILE *cf = fopen("/tmp/aurora-connect.log", "a");
        if (cf) {
            fprintf(cf, "%lu host_update %s took %lums ret=%d\n",
                    (unsigned long) SDL_GetTicks(), ip, (unsigned long) dt, ret);
            fclose(cf);
        }
    }
    if (ret == GS_OK) {
        SERVER_STATE state = {.code = server->paired ? SERVER_STATE_AVAILABLE : SERVER_STATE_NOT_PAIRED};
        pclist_upsert(manager, (const uuidstr_t *) server->uuid, &state, server);
    } else {
        const char *error = NULL;
        gs_get_error(&error);
        if (error) {
            context->error = strdup(error);
        }
        serverdata_free(server);
        if (!uuidstr_is_empty(&context->uuid)) {
            SERVER_STATE state = {.code = ret == GS_IO_ERROR ? SERVER_STATE_OFFLINE : SERVER_STATE_ERROR};
            pclist_upsert(manager, &context->uuid, &state, NULL);
        }
    }
    gs_destroy(client);

    return ret;
}

