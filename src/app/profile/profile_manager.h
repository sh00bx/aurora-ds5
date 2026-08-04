#pragma once

#include "app_settings.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct streaming_profile_t {
    char id[37];
    char name[64];
    int width;
    int height;
    int fps;
    int bitrate;
    int rotate;
    bool hdr;
    bool hevc;
} streaming_profile_t;

void profile_manager_init(const char *conf_dir, const app_settings_t *current_settings);

void profile_manager_deinit(void);

int profile_manager_count(void);

const streaming_profile_t *profile_manager_get(int index);

const streaming_profile_t *profile_manager_get_by_id(const char *id);

const streaming_profile_t *profile_manager_get_active(void);

const char *profile_manager_active_id(void);

bool profile_manager_set_active(const char *id);

bool profile_manager_apply_to_settings(app_settings_t *settings);

bool profile_manager_save_from_settings(const app_settings_t *settings, const char *id);

bool profile_manager_create(const char *name, const app_settings_t *settings, char *out_id, size_t out_len);

bool profile_manager_rename(const char *id, const char *name);

bool profile_manager_delete(const char *id);

bool profile_manager_save_active(void);
