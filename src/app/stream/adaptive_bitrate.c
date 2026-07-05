#include "adaptive_bitrate.h"

#include "stream/session.h"
#include "stream/video/session_video.h"

#include <Limelight.h>
#include <libgamestream/errors.h>
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

#define ABR_TICK_MS 1000
#define ABR_START_DELAY_TICKS 3

struct adaptive_bitrate_service {
    SDL_Thread *thread;
    SDL_atomic_t stop;
    GS_CLIENT gs_client;
    SERVER_DATA server_copy;
    int initial_bitrate;
    int current_bitrate;
    int min_bitrate;
    int max_bitrate;
    abr_mode_t mode;
    bool server_supported;
    int server_enable_retries;
    int tick_count;
    int stable_seconds;
    int loss_streak;
    Uint32 last_adjust_ticks;
};

const char *abr_mode_to_string(abr_mode_t mode) {
    switch (mode) {
        case ABR_MODE_QUALITY:
            return "quality";
        case ABR_MODE_LOW_LATENCY:
            return "lowLatency";
        case ABR_MODE_BALANCED:
        default:
            return "balanced";
    }
}

static void abr_apply_mode_preset(adaptive_bitrate_service_t *service) {
    switch (service->mode) {
        case ABR_MODE_QUALITY:
            service->min_bitrate = service->initial_bitrate / 2;
            if (service->min_bitrate < 5000) {
                service->min_bitrate = 5000;
            }
            service->max_bitrate = service->initial_bitrate * 3 / 2;
            if (service->max_bitrate > 150000) {
                service->max_bitrate = 150000;
            }
            break;
        case ABR_MODE_LOW_LATENCY:
            service->min_bitrate = 2000;
            service->max_bitrate = service->initial_bitrate * 6 / 5;
            break;
        case ABR_MODE_BALANCED:
        default:
            service->min_bitrate = service->initial_bitrate * 3 / 10;
            if (service->min_bitrate < 3000) {
                service->min_bitrate = 3000;
            }
            service->max_bitrate = service->initial_bitrate * 2;
            if (service->max_bitrate > 150000) {
                service->max_bitrate = 150000;
            }
            break;
    }
}

static bool abr_set_bitrate(adaptive_bitrate_service_t *service, int kbps, const char *source) {
    if (kbps == service->current_bitrate) {
        return false;
    }
    int from = service->current_bitrate;
    if (gs_set_bitrate(service->gs_client, &service->server_copy, kbps) != GS_OK) {
        return false;
    }
    service->current_bitrate = kbps;
    commons_log_info("ABR", "[%s] %d kbps -> %d kbps", source, from, kbps);
    return true;
}

static void abr_tick_local(adaptive_bitrate_service_t *service, float packet_loss) {
    Uint32 now = SDL_GetTicks();
    int cooldown = service->mode == ABR_MODE_LOW_LATENCY ? 1500 : 2000;
    if (now - service->last_adjust_ticks < (Uint32) cooldown) {
        return;
    }

    int target = service->current_bitrate;
    if (packet_loss > 5.0f) {
        target = (int) (service->current_bitrate * 0.7);
        service->stable_seconds = 0;
        service->loss_streak++;
    } else if (packet_loss > 2.0f) {
        service->loss_streak++;
        if (service->loss_streak >= 2) {
            target = (int) (service->current_bitrate * 0.9);
            service->stable_seconds = 0;
        }
    } else if (packet_loss > 0.5f) {
        service->loss_streak++;
        service->stable_seconds = 0;
        if (service->loss_streak >= 4) {
            target = (int) (service->current_bitrate * 0.95);
        }
    } else {
        service->loss_streak = 0;
        service->stable_seconds++;
        int probe_threshold = service->mode == ABR_MODE_QUALITY ? 3 : 5;
        if (service->stable_seconds >= probe_threshold && service->current_bitrate < service->max_bitrate) {
            target = (int) (service->current_bitrate * 1.05);
            service->stable_seconds = 0;
        }
    }

    if (target < service->min_bitrate) {
        target = service->min_bitrate;
    }
    if (target > service->max_bitrate) {
        target = service->max_bitrate;
    }
    if (abr_set_bitrate(service, target, "local")) {
        service->last_adjust_ticks = now;
    }
}

static int abr_thread(void *userdata) {
    adaptive_bitrate_service_t *service = userdata;
    while (!SDL_AtomicGet(&service->stop)) {
        SDL_Delay(ABR_TICK_MS);
        if (SDL_AtomicGet(&service->stop)) {
            break;
        }
        service->tick_count++;
        if (service->tick_count == ABR_START_DELAY_TICKS) {
            GS_ABR_CAPABILITIES caps;
            if (gs_get_abr_capabilities(service->gs_client, &service->server_copy, &caps) == GS_OK &&
                caps.supported) {
                service->server_supported = true;
                service->server_enable_retries = 1;
                commons_log_info("ABR", "Server ABR supported, version %d", caps.version);
            }
        }
        if (service->tick_count < ABR_START_DELAY_TICKS) {
            continue;
        }

        const struct VIDEO_STATS *stats = &vdec_summary_stats;
        float packet_loss = stats->totalFrames > 0
            ? (float) stats->networkDroppedFrames / (float) stats->totalFrames * 100.0f
            : 0.0f;
        unsigned int rtt = 0;
        LiGetEstimatedRttInfo(&rtt, NULL);

        if (service->server_supported && service->server_enable_retries > 0) {
            GS_ABR_CONFIG config = {
                .enabled = true,
                .min_bitrate = service->min_bitrate,
                .max_bitrate = service->max_bitrate,
                .mode = abr_mode_to_string(service->mode),
            };
            if (gs_set_abr_mode(service->gs_client, &service->server_copy, &config) == GS_OK) {
                service->server_enable_retries = 0;
            } else if (++service->server_enable_retries > 5) {
                service->server_supported = false;
            }
            continue;
        }

        if (service->server_supported && service->server_enable_retries == 0) {
            GS_ABR_FEEDBACK feedback = {
                .packet_loss = packet_loss,
                .rtt_ms = (int) rtt,
                .decode_fps = stats->decodedFps,
                .dropped_frames = (int) stats->networkDroppedFrames,
                .current_bitrate = service->current_bitrate,
            };
            GS_ABR_ACTION action;
            if (gs_report_abr_feedback(service->gs_client, &service->server_copy, &feedback, &action) == GS_OK &&
                action.has_new_bitrate) {
                int clamped = action.new_bitrate;
                if (clamped < service->min_bitrate) {
                    clamped = service->min_bitrate;
                }
                if (clamped > service->max_bitrate) {
                    clamped = service->max_bitrate;
                }
                abr_set_bitrate(service, clamped, "server");
                continue;
            }
        }

        abr_tick_local(service, packet_loss);
    }
    return 0;
}

adaptive_bitrate_service_t *adaptive_bitrate_start(const adaptive_bitrate_config_t *config) {
    if (!config || !config->gs_client || !config->server) {
        return NULL;
    }
    adaptive_bitrate_service_t *service = calloc(1, sizeof(*service));
    if (!service) {
        return NULL;
    }
    service->gs_client = config->gs_client;
    service->server_copy = *config->server;
    service->server_copy.uuid = config->server->uuid ? strdup(config->server->uuid) : NULL;
    service->server_copy.mac = config->server->mac ? strdup(config->server->mac) : NULL;
    service->server_copy.hostname = config->server->hostname ? strdup(config->server->hostname) : NULL;
    service->server_copy.serverInfo.address = config->server->serverInfo.address
        ? strdup(config->server->serverInfo.address) : NULL;
    service->initial_bitrate = config->initial_bitrate;
    service->current_bitrate = config->initial_bitrate;
    service->mode = config->mode;
    abr_apply_mode_preset(service);
    SDL_AtomicSet(&service->stop, 0);
    service->thread = SDL_CreateThread(abr_thread, "abr", service);
    if (!service->thread) {
        free((void *) service->server_copy.uuid);
        free((void *) service->server_copy.mac);
        free((void *) service->server_copy.hostname);
        free((void *) service->server_copy.serverInfo.address);
        free(service);
        return NULL;
    }
    commons_log_info("ABR", "Started at %d kbps, mode %s", service->initial_bitrate, abr_mode_to_string(service->mode));
    return service;
}

void adaptive_bitrate_stop(adaptive_bitrate_service_t *service) {
    if (!service) {
        return;
    }
    SDL_AtomicSet(&service->stop, 1);
    if (service->thread) {
        SDL_WaitThread(service->thread, NULL);
    }
    if (service->current_bitrate != service->initial_bitrate) {
        abr_set_bitrate(service, service->initial_bitrate, "restore");
    }
    if (service->server_supported) {
        GS_ABR_CONFIG config = {.enabled = false, .min_bitrate = 0, .max_bitrate = 0, .mode = "balanced"};
        gs_set_abr_mode(service->gs_client, &service->server_copy, &config);
    }
    free((void *) service->server_copy.uuid);
    free((void *) service->server_copy.mac);
    free((void *) service->server_copy.hostname);
    free((void *) service->server_copy.serverInfo.address);
    free(service);
}
