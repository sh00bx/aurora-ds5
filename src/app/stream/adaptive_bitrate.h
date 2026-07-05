#pragma once

#include "libgamestream/client.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ABR_MODE_BALANCED = 0,
    ABR_MODE_QUALITY = 1,
    ABR_MODE_LOW_LATENCY = 2,
} abr_mode_t;

typedef struct adaptive_bitrate_service adaptive_bitrate_service_t;

typedef struct {
    GS_CLIENT gs_client;
    const SERVER_DATA *server;
    int initial_bitrate;
    abr_mode_t mode;
} adaptive_bitrate_config_t;

adaptive_bitrate_service_t *adaptive_bitrate_start(const adaptive_bitrate_config_t *config);

void adaptive_bitrate_stop(adaptive_bitrate_service_t *service);

const char *abr_mode_to_string(abr_mode_t mode);
