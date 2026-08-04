#include "profile_manager.h"

#include <ini.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/ini_ext.h"
#include "util/path.h"
#include "ini_writer.h"
#include "logging.h"

#define MAX_PROFILES 16
#define CONF_NAME_PROFILES "profiles.ini"

static char *profiles_path;
static streaming_profile_t profiles[MAX_PROFILES];
static int profile_count;
static char active_id[37];

static void profile_from_settings(streaming_profile_t *profile, const app_settings_t *settings) {
    profile->width = settings->stream.width;
    profile->height = settings->stream.height;
    profile->fps = settings->stream.fps;
    profile->bitrate = settings->stream.bitrate;
    profile->rotate = settings->rotate;
    profile->hdr = settings->hdr;
    profile->hevc = settings->hevc;
}

static void profile_to_settings(const streaming_profile_t *profile, app_settings_t *settings) {
    settings->stream.width = profile->width;
    settings->stream.height = profile->height;
    settings->stream.fps = profile->fps;
    settings->stream.bitrate = profile->bitrate;
    settings->rotate = profile->rotate;
    settings->hdr = profile->hdr;
    settings->hevc = profile->hevc;
}

static streaming_profile_t *find_profile(const char *id) {
    for (int i = 0; i < profile_count; i++) {
        if (strcmp(profiles[i].id, id) == 0) {
            return &profiles[i];
        }
    }
    return NULL;
}

static void generate_profile_id(char *out, size_t out_len) {
    snprintf(out, out_len, "%08x%08x", (unsigned) time(NULL), (unsigned) rand());
}

static bool profiles_save(void) {
    if (!profiles_path) {
        return false;
    }
    FILE *fp = fopen(profiles_path, "w");
    if (!fp) {
        return false;
    }
    ini_write_section(fp, "meta");
    ini_write_string(fp, "active", active_id[0] ? active_id : profiles[0].id);
    for (int i = 0; i < profile_count; i++) {
        const streaming_profile_t *p = &profiles[i];
        ini_write_section(fp, p->id);
        ini_write_string(fp, "name", p->name);
        ini_write_int(fp, "width", p->width);
        ini_write_int(fp, "height", p->height);
        ini_write_int(fp, "fps", p->fps);
        ini_write_int(fp, "bitrate", p->bitrate);
        ini_write_int(fp, "rotate", p->rotate);
        ini_write_bool(fp, "hdr", p->hdr);
        ini_write_bool(fp, "hevc", p->hevc);
    }
    return fclose(fp) == 0;
}

static int profiles_parse(void *userdata, const char *section, const char *name, const char *value) {
    (void) userdata;
    if (!section || !name) {
        return 1;
    }
    if (strcmp(section, "meta") == 0 && strcmp(name, "active") == 0 && value) {
        strncpy(active_id, value, sizeof(active_id) - 1);
        return 1;
    }
    streaming_profile_t *profile = find_profile(section);
    if (!profile) {
        if (profile_count >= MAX_PROFILES) {
            return 1;
        }
        profile = &profiles[profile_count++];
        memset(profile, 0, sizeof(*profile));
        strncpy(profile->id, section, sizeof(profile->id) - 1);
    }
    if (strcmp(name, "name") == 0 && value) {
        strncpy(profile->name, value, sizeof(profile->name) - 1);
    } else if (strcmp(name, "width") == 0) {
        profile->width = atoi(value);
    } else if (strcmp(name, "height") == 0) {
        profile->height = atoi(value);
    } else if (strcmp(name, "fps") == 0) {
        profile->fps = atoi(value);
    } else if (strcmp(name, "bitrate") == 0) {
        profile->bitrate = atoi(value);
    } else if (strcmp(name, "rotate") == 0) {
        profile->rotate = atoi(value);
    } else if (strcmp(name, "hdr") == 0) {
        profile->hdr = INI_IS_TRUE(value);
    } else if (strcmp(name, "hevc") == 0) {
        profile->hevc = INI_IS_TRUE(value);
    }
    return 1;
}

static void profiles_create_default(const app_settings_t *settings) {
    profile_count = 0;
    streaming_profile_t *profile = &profiles[profile_count++];
    strncpy(profile->id, "default", sizeof(profile->id) - 1);
    strncpy(profile->name, "Default", sizeof(profile->name) - 1);
    if (settings) {
        profile_from_settings(profile, settings);
    } else {
        profile->width = 1920;
        profile->height = 1080;
        profile->fps = 60;
        profile->bitrate = 10000;
    }
    strncpy(active_id, profile->id, sizeof(active_id) - 1);
    profiles_save();
}

void profile_manager_init(const char *conf_dir, const app_settings_t *current_settings) {
    srand((unsigned) time(NULL));
    free(profiles_path);
    profiles_path = path_join(conf_dir, CONF_NAME_PROFILES);
    profile_count = 0;
    active_id[0] = 0;
    if (ini_parse(profiles_path, profiles_parse, NULL) != 0 || profile_count == 0) {
        profiles_create_default(current_settings);
    } else if (!find_profile(active_id)) {
        strncpy(active_id, profiles[0].id, sizeof(active_id) - 1);
    }
}

void profile_manager_deinit(void) {
    free(profiles_path);
    profiles_path = NULL;
    profile_count = 0;
    active_id[0] = 0;
}

int profile_manager_count(void) {
    return profile_count;
}

const streaming_profile_t *profile_manager_get(int index) {
    if (index < 0 || index >= profile_count) {
        return NULL;
    }
    return &profiles[index];
}

const streaming_profile_t *profile_manager_get_by_id(const char *id) {
    return id ? find_profile(id) : NULL;
}

const streaming_profile_t *profile_manager_get_active(void) {
    return find_profile(active_id);
}

const char *profile_manager_active_id(void) {
    return active_id;
}

bool profile_manager_set_active(const char *id) {
    if (!id || !find_profile(id)) {
        return false;
    }
    strncpy(active_id, id, sizeof(active_id) - 1);
    return profiles_save();
}

bool profile_manager_apply_to_settings(app_settings_t *settings) {
    const streaming_profile_t *profile = profile_manager_get_active();
    if (!profile || !settings) {
        return false;
    }
    profile_to_settings(profile, settings);
    return true;
}

bool profile_manager_save_from_settings(const app_settings_t *settings, const char *id) {
    streaming_profile_t *profile = find_profile(id);
    if (!profile || !settings) {
        return false;
    }
    profile_from_settings(profile, settings);
    return profiles_save();
}

bool profile_manager_create(const char *name, const app_settings_t *settings, char *out_id, size_t out_len) {
    if (!name || !settings || profile_count >= MAX_PROFILES) {
        return false;
    }
    streaming_profile_t *profile = &profiles[profile_count++];
    generate_profile_id(profile->id, sizeof(profile->id));
    strncpy(profile->name, name, sizeof(profile->name) - 1);
    profile_from_settings(profile, settings);
    if (out_id && out_len > 0) {
        strncpy(out_id, profile->id, out_len - 1);
        out_id[out_len - 1] = 0;
    }
    return profiles_save();
}

bool profile_manager_rename(const char *id, const char *name) {
    streaming_profile_t *profile = find_profile(id);
    if (!profile || !name) {
        return false;
    }
    strncpy(profile->name, name, sizeof(profile->name) - 1);
    return profiles_save();
}

bool profile_manager_delete(const char *id) {
    if (!id || profile_count <= 1) {
        return false;
    }
    int index = -1;
    for (int i = 0; i < profile_count; i++) {
        if (strcmp(profiles[i].id, id) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return false;
    }
    for (int i = index; i < profile_count - 1; i++) {
        profiles[i] = profiles[i + 1];
    }
    profile_count--;
    if (strcmp(active_id, id) == 0) {
        strncpy(active_id, profiles[0].id, sizeof(active_id) - 1);
    }
    return profiles_save();
}

bool profile_manager_save_active(void) {
    const streaming_profile_t *active = profile_manager_get_active();
    return active != NULL;
}
