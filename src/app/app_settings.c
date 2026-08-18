#include "app_settings.h"
#include "config.h"
#include "hid_passthrough/hid_pt_device_prefs.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ini.h>

#include "util/ini_ext.h"
#include "util/nullable.h"
#include "util/path.h"
#include "util/i18n.h"

#include "ini_writer.h"

static int find_ch_idx_by_config(int config);

static int find_ch_idx_by_value(const char *value);

static const char *serialize_audio_config(int config);

static int parse_audio_config(const char *value);

static int settings_parse(app_settings_t *config, const char *section, const char *name, const char *value);

static void set_string(char **field, const char *value);

static void set_int(int *field, const char *value);

/* Set while parsing when the ini actually carried a use_ntsc_refresh key, so
 * settings_read() can tell "explicitly off" apart from "written before the key
 * existed" (see the migration there). */
static bool settings_seen_ntsc_key = false;

void settings_sync_refresh_rate(app_settings_t *config) {
    settings_reconcile_refresh_rate(config);
}

void settings_reconcile_refresh_rate(app_settings_t *config) {
    if (!config) {
        return;
    }
#if defined(TARGET_WEBOS)
    int ntsc = settings_ntsc_refresh_rate_x100_for_fps(config->stream.fps);
    if (ntsc > 0) {
        if (config->use_ntsc_refresh) {
            /* Auto NTSC for presets; keep a user Custom FPS fractional rate if present. */
            if (config->client_refresh_rate_x100 == 0 ||
                config->client_refresh_rate_x100 == ntsc) {
                config->client_refresh_rate_x100 = ntsc;
                return;
            }
            int implied = (config->client_refresh_rate_x100 + 50) / 100;
            if (implied != config->stream.fps) {
                config->client_refresh_rate_x100 = ntsc;
            }
            return;
        }
        /* Integer presets: clear auto-NTSC; keep a distinct Custom FPS fractional rate. */
        if (config->client_refresh_rate_x100 == 0 ||
            config->client_refresh_rate_x100 == ntsc) {
            config->client_refresh_rate_x100 = 0;
            return;
        }
        int implied = (config->client_refresh_rate_x100 + 50) / 100;
        if (implied != config->stream.fps) {
            config->client_refresh_rate_x100 = 0;
        }
        return;
    }
#endif
    if (config->client_refresh_rate_x100 > 0) {
        int implied = (config->client_refresh_rate_x100 + 50) / 100;
        if (implied != config->stream.fps) {
            config->client_refresh_rate_x100 = 0;
#if !defined(TARGET_WEBOS)
        } else {
            config->stream.fps = implied;
        }
#else
        }
#endif
    }
}

int settings_ntsc_refresh_rate_x100_for_fps(int nominal_fps)
{
    /* 1000/1001 × nominal_fps, rounded to centihertz (Limelight clientRefreshRateX100). */
    switch (nominal_fps) {
    case 30:
        return 2997;
    case 60:
        return 5994;
    case 120:
        return 11988;
    case 240:
        return 23976;
    default:
        return 0;
    }
}

void settings_apply_ntsc_preset_refresh(app_settings_t *config, int nominal_fps)
{
#if defined(TARGET_WEBOS)
    if (!config) {
        return;
    }
    (void) nominal_fps;
    int ntsc = settings_ntsc_refresh_rate_x100_for_fps(config->stream.fps);
    if (ntsc > 0) {
        /* Preset selection / NTSC checkbox: force auto NTSC or integer (clears Custom). */
        config->client_refresh_rate_x100 = config->use_ntsc_refresh ? ntsc : 0;
        return;
    }
    settings_reconcile_refresh_rate(config);
#else
    (void) config;
    (void) nominal_fps;
#endif
}


const audio_config_entry_t audio_configs[] = {
        {AUDIO_CONFIGURATION_STEREO,      "stereo", translatable("Stereo")},
        {AUDIO_CONFIGURATION_51_SURROUND, "5.1ch",  translatable("5.1 Surround")},
        {AUDIO_CONFIGURATION_71_SURROUND, "7.1ch",  translatable("7.1 Surround")},
};
const size_t audio_config_len = sizeof(audio_configs) / sizeof(audio_config_entry_t);

void settings_initialize(app_settings_t *config, char *conf_dir) {
    memset(config, 0, sizeof(CONFIGURATION));
    LiInitializeStreamConfiguration(&config->stream);

    config->stream.width = 1280;
    config->stream.height = 720;
    config->stream.fps = 60;
    config->stream.bitrate = settings_optimal_bitrate(NULL, 1280, 720, 60);
    config->stream.packetSize = 1392;
    config->stream.streamingRemotely = STREAM_CFG_AUTO;
    config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

    config->debug_level = 0;
    set_string(&config->language, "auto");
    set_string(&config->audio_backend, "auto");
    set_string(&config->decoder, "auto");
    config->audio_device = NULL;
    config->sops = true;
#if FEATURE_INPUT_EVKBD
    /* On by default: without it webOS keeps the keys it has opinions about (F12
     * becomes TV volume) and they never reach the host, which is the opposite of
     * what someone plugging a keyboard into a streaming client wants. Costs
     * nothing when no keyboard is attached — the scan simply finds none. */
    config->keyboard_capture = true;
#endif
    config->localaudio = false;
    config->fullscreen = true;
    if (!config->fullscreen) {
        config->window_state.w = 1280;
        config->window_state.h = 720;
    }
    // TODO make this automatic
    config->unsupported = true;
    config->quitappafter = false;
    config->autoresume = false;
    config->viewonly = false;
    config->rotate = 0;
    config->auto_adjust_bitrate = false;
    config->abr_mode = 0;
    config->absmouse = true;
    config->virtual_mouse = false;
    config->hdr = false;
    config->force_full_color_range = false;
    config->vrr = false;
    config->hevc = true;
    config->av1 = false;
    config->idr_refresh_interval_ms = 0;
    config->show_stats_on_start = false;
    config->show_stats_compact = false;
    config->stick_deadzone = 7;
    config->client_refresh_rate_x100 = 0;
    config->use_ntsc_refresh = false;
    config->smooth_frame_pacing = false;
    config->hid_passthrough = false;
    config->hid_passthrough_port = 48054;
    config->hid_passthrough_autoplug = true;
    config->webos_game_mode = true;

#if defined(TARGET_WEBOS)
    config->soft_recovery = true;
    hid_pt_prefs_init();
#else
    config->soft_recovery = false;
#endif

#if defined(TARGET_WEBOS)
    settings_apply_ntsc_preset_refresh(config, config->stream.fps);
#endif

    config->conf_dir = conf_dir;
    config->ini_path = path_join(conf_dir, CONF_NAME_MOONLIGHT);
    config->condb_path = path_join(conf_dir, "gamecontrollerdb.txt");
    config->key_dir = path_join(conf_dir, "key");
}

bool settings_read(app_settings_t *config) {
    settings_seen_ntsc_key = false;
    int ret = ini_parse(config->ini_path, (ini_handler) settings_parse, config);
    if (ret == 0) {
#if defined(TARGET_WEBOS)
        /* Migration: use_ntsc_refresh is new (upstream v1.1.7). Before it existed we
         * always forced the fractional NTSC rate for a standard preset fps, so an ini
         * written by an older build carries e.g. 11988 but no use_ntsc_refresh key.
         * Without this, reconcile() would read the missing key as "integer wanted" and
         * silently reset 119.88 Hz to a flat 120 on first launch after the update. */
        if (!settings_seen_ntsc_key) {
            int ntsc = settings_ntsc_refresh_rate_x100_for_fps(config->stream.fps);
            if (ntsc > 0 && config->client_refresh_rate_x100 == ntsc) {
                config->use_ntsc_refresh = true;
            }
        }
#endif
        settings_reconcile_refresh_rate(config);
    }
    return ret == 0;
}

bool settings_save(app_settings_t *config) {
    if (config->ini_path == NULL) {
        return false;
    }
    settings_reconcile_refresh_rate(config);
    FILE *fp = fopen(config->ini_path, "w");
    if (!fp) {
        return false;
    }
    ini_write_string(fp, "language", config->language);
    ini_write_bool(fp, "fullscreen", config->fullscreen);
    ini_write_int(fp, "debug_level", config->debug_level);

    ini_write_section(fp, "streaming");
    ini_write_int(fp, "width", config->stream.width);
    ini_write_int(fp, "height", config->stream.height);
    ini_write_int(fp, "net_fps", config->stream.fps);
    ini_write_int(fp, "bitrate", config->stream.bitrate);
    ini_write_int(fp, "packetsize", config->stream.packetSize);
    ini_write_int(fp, "rotate", config->rotate);
    ini_write_bool(fp, "auto_adjust_bitrate", config->auto_adjust_bitrate);
    ini_write_int(fp, "abr_mode", config->abr_mode);

    ini_write_section(fp, "host");
    ini_write_bool(fp, "sops", config->sops);
    ini_write_bool(fp, "localaudio", config->localaudio);
    ini_write_bool(fp, "quitappafter", config->quitappafter);
    ini_write_bool(fp, "autoresume", config->autoresume);
    ini_write_bool(fp, "viewonly", config->viewonly);

    ini_write_section(fp, "input");
    ini_write_bool(fp, "absmouse", config->absmouse);
    ini_write_bool(fp, "virtual_mouse", config->virtual_mouse);
#if FEATURE_INPUT_EVMOUSE
    ini_write_bool(fp, "hardware_mouse", config->hardware_mouse);
#endif
#if FEATURE_INPUT_EVKBD
    ini_write_bool(fp, "keyboard_capture", config->keyboard_capture);
#endif
    ini_write_bool(fp, "swap_abxy", config->swap_abxy);
    ini_write_int(fp, "stick_deadzone", config->stick_deadzone);
    ini_write_bool(fp, "syskey_capture", config->syskey_capture);
    ini_write_bool(fp, "hid_passthrough", config->hid_passthrough);
    ini_write_int(fp, "hid_passthrough_port", config->hid_passthrough_port);
    ini_write_bool(fp, "hid_passthrough_autoplug", config->hid_passthrough_autoplug);
    ini_write_bool(fp, "webos_game_mode", config->webos_game_mode);

    ini_write_section(fp, "video");
    ini_write_string(fp, "decoder", config->decoder);
    ini_write_bool(fp, "hdr", config->hdr);
    ini_write_bool(fp, "force_full_color_range", config->force_full_color_range);
    ini_write_bool(fp, "vrr", config->vrr);
    ini_write_bool(fp, "hevc", config->hevc);
    ini_write_bool(fp, "av1", config->av1);
    ini_write_int(fp, "idr_refresh_interval_ms", config->idr_refresh_interval_ms);
    ini_write_bool(fp, "show_stats_on_start", config->show_stats_on_start);
    ini_write_bool(fp, "show_stats_compact", config->show_stats_compact);
    ini_write_int(fp, "client_refresh_rate_x100", config->client_refresh_rate_x100);
    ini_write_bool(fp, "use_ntsc_refresh", config->use_ntsc_refresh);
    ini_write_bool(fp, "smooth_frame_pacing", config->smooth_frame_pacing);
    ini_write_bool(fp, "soft_recovery", config->soft_recovery);

    ini_write_section(fp, "audio");
    ini_write_string(fp, "backend", config->audio_backend);
    if (config->audio_device && config->audio_device[0]) {
        ini_write_string(fp, "device", config->audio_device);
    }
    ini_write_string(fp, "surround", serialize_audio_config(config->stream.audioConfiguration));

    if (!config->fullscreen) {
        ini_write_section(fp, "window");
        ini_write_int(fp, "x", config->window_state.x);
        ini_write_int(fp, "y", config->window_state.y);
        ini_write_int(fp, "width", config->window_state.w);
        ini_write_int(fp, "height", config->window_state.h);
    }

#if defined(TARGET_WEBOS)
    /* settings_save() truncates and rewrites the whole ini from known fields, so
     * without this the per-device HID auto-plug prefs written by
     * hid_pt_prefs_flush() are silently dropped on the next full save (e.g. app
     * exit) — the DualSense would then fail to auto-bridge on the next launch. */
    hid_pt_prefs_write_section(fp);
#endif

    return fclose(fp) == 0;
}

void settings_clear(app_settings_t *config) {
    free_nullable(config->decoder);
    free_nullable(config->audio_backend);
    free_nullable(config->audio_device);
    free_nullable(config->language);
    free_nullable(config->ini_path);
    free_nullable(config->condb_path);
    free_nullable(config->key_dir);
}

#define BITRATE_300_MBPS 300000

int settings_optimal_bitrate(const SS4S_VideoCapabilities *capabilities, int w, int h, int fps) {
    (void) capabilities;
    (void) w;
    (void) h;
    (void) fps;
    return BITRATE_300_MBPS;
}


bool audio_config_valid(int config) {
    return find_ch_idx_by_config(config) >= 0;
}

const char *serialize_audio_config(int config) {
    return audio_configs[find_ch_idx_by_config(config)].value;
}

int parse_audio_config(const char *value) {
    int index = value ? find_ch_idx_by_value(value) : -1;
    if (index < 0) {
        index = 0;
    }
    return audio_configs[index].configuration;
}

int find_ch_idx_by_config(int config) {
    for (int i = 0; i < audio_config_len; i++) {
        if (audio_configs[i].configuration == config) {
            return i;
        }
    }
    return -1;
}

int find_ch_idx_by_value(const char *value) {
    for (int i = 0; i < audio_config_len; i++) {
        if (strcmp(audio_configs[i].value, value) == 0) {
            return i;
        }
    }
    return -1;
}

static int settings_parse(app_settings_t *config, const char *section, const char *name, const char *value) {
#if defined(TARGET_WEBOS)
    if (hid_pt_prefs_ini_handler(section, name, value)) {
        return 1;
    }
#endif
    if (INI_FULL_MATCH("streaming", "width")) {
        set_int(&config->stream.width, value);
    } else if (INI_FULL_MATCH("streaming", "height")) {
        set_int(&config->stream.height, value);
    } else if (INI_FULL_MATCH("streaming", "net_fps")) {
        set_int(&config->stream.fps, value);
    } else if (INI_FULL_MATCH("streaming", "bitrate")) {
        set_int(&config->stream.bitrate, value);
    } else if (INI_FULL_MATCH("streaming", "packetsize")) {
        set_int(&config->stream.packetSize, value);
    } else if (INI_FULL_MATCH("streaming", "rotate")) {
        set_int(&config->rotate, value);
    } else if (INI_FULL_MATCH("streaming", "auto_adjust_bitrate")) {
        config->auto_adjust_bitrate = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("streaming", "abr_mode")) {
        set_int(&config->abr_mode, value);
        if (config->abr_mode < 0 || config->abr_mode > 2) {
            config->abr_mode = 0;
        }
    } else if (INI_FULL_MATCH("video", "idr_refresh_interval_ms")) {
        set_int(&config->idr_refresh_interval_ms, value);
        if (config->idr_refresh_interval_ms < 0) {
            config->idr_refresh_interval_ms = 0;
        } else if (config->idr_refresh_interval_ms > 0 && config->idr_refresh_interval_ms < 500) {
            config->idr_refresh_interval_ms = 500;
        } else if (config->idr_refresh_interval_ms > 60000) {
            config->idr_refresh_interval_ms = 60000;
        } else if (config->idr_refresh_interval_ms > 0) {
            config->idr_refresh_interval_ms = ((config->idr_refresh_interval_ms + 250) / 500) * 500;
            if (config->idr_refresh_interval_ms < 500) {
                config->idr_refresh_interval_ms = 500;
            }
        }
    } else if (INI_FULL_MATCH("video", "idr_refresh_interval_sec")) {
        /* Legacy seconds key → milliseconds */
        int sec = 0;
        set_int(&sec, value);
        if (sec < 0) {
            config->idr_refresh_interval_ms = 0;
        } else if (sec == 0) {
            config->idr_refresh_interval_ms = 0;
        } else {
            if (sec > 60) {
                sec = 60;
            }
            config->idr_refresh_interval_ms = sec * 1000;
        }
    } else if (INI_NAME_MATCH("hevc")) {
        config->hevc = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "av1") || INI_NAME_MATCH("av1")) {
        config->av1 = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "video_simple_sdp")) {
        /* Legacy: ignored; client always negotiates RFI + slices when applicable. */
    } else if (INI_FULL_MATCH("video", "presentation_offset_ms")) {
        /* Legacy: ignored; tight sync uses a fixed presentation offset in the decoder. */
    } else if (INI_NAME_MATCH("show_stats_on_start")) {
        config->show_stats_on_start = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("show_stats_compact")) {
        config->show_stats_compact = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hdr")) {
        config->hdr = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("vrr")) {
        config->vrr = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "client_refresh_rate_x100")) {
        set_int(&config->client_refresh_rate_x100, value);
        if (config->client_refresh_rate_x100 < 0) {
            config->client_refresh_rate_x100 = 0;
        } else if (config->client_refresh_rate_x100 > 24000) {
            config->client_refresh_rate_x100 = 24000;
        }
    } else if (INI_FULL_MATCH("video", "use_ntsc_refresh")) {
        config->use_ntsc_refresh = INI_IS_TRUE(value);
        settings_seen_ntsc_key = true;
    } else if (INI_FULL_MATCH("video", "smooth_frame_pacing")) {
        config->smooth_frame_pacing = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "soft_recovery")) {
        config->soft_recovery = INI_IS_TRUE(value);
    } else if (INI_FULL_MATCH("video", "pause_at_decode_time")) {
        /* Upstream-Legacy: dort entfernt, bei uns nie eingefuehrt — still ignorieren. */
    } else if (INI_FULL_MATCH("video", "force_full_color_range")) {
        config->force_full_color_range = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("surround")) {
        config->stream.audioConfiguration = parse_audio_config(value);
    } else if (INI_NAME_MATCH("sops")) {
        config->sops = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("localaudio")) {
        config->localaudio = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("quitappafter")) {
        config->quitappafter = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("autoresume")) {
        config->autoresume = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("viewonly")) {
        config->viewonly = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("absmouse")) {
        config->absmouse = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("virtual_mouse")) {
        config->virtual_mouse = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hardware_mouse")) {
#if FEATURE_INPUT_EVMOUSE
        config->hardware_mouse = INI_IS_TRUE(value);
#endif
    } else if (INI_NAME_MATCH("keyboard_capture")) {
#if FEATURE_INPUT_EVKBD
        config->keyboard_capture = INI_IS_TRUE(value);
#endif
    } else if (INI_NAME_MATCH("stick_deadzone")) {
        set_int(&config->stick_deadzone, value);
        if (config->stick_deadzone < 0) {
            config->stick_deadzone = 0;
        } else if (config->stick_deadzone > 100) {
            config->stick_deadzone = 100;
        }
    } else if (INI_NAME_MATCH("swap_abxy")) {
        config->swap_abxy = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("syskey_capture")) {
        config->syskey_capture = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hid_passthrough")) {
        config->hid_passthrough = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hid_passthrough_autoplug")) {
        config->hid_passthrough_autoplug = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("webos_game_mode")) {
        config->webos_game_mode = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("hid_passthrough_port")) {
        set_int(&config->hid_passthrough_port, value);
        if (config->hid_passthrough_port <= 0 || config->hid_passthrough_port > 65535) {
            config->hid_passthrough_port = 48054;
        }
    } else if (INI_FULL_MATCH("video", "decoder")) {
        set_string(&config->decoder, value);
    } else if (INI_FULL_MATCH("audio", "backend")) {
        set_string(&config->audio_backend, value);
    } else if (INI_FULL_MATCH("audio", "device")) {
        set_string(&config->audio_device, value);
    } else if (INI_NAME_MATCH("language")) {
        set_string(&config->language, value);
    } else if (INI_NAME_MATCH("fullscreen")) {
#if FEATURE_FORCE_FULLSCREEN
        config->fullscreen = true;
#else
        config->fullscreen = INI_IS_TRUE(value);
#endif
    } else if (INI_NAME_MATCH("debug_level")) {
        set_int(&config->debug_level, value);
    } else if (INI_FULL_MATCH("window", "x")) {
        set_int(&config->window_state.x, value);
    } else if (INI_FULL_MATCH("window", "y")) {
        set_int(&config->window_state.y, value);
    } else if (INI_FULL_MATCH("window", "width")) {
        set_int(&config->window_state.w, value);
    } else if (INI_FULL_MATCH("window", "height")) {
        set_int(&config->window_state.h, value);
    }
    return 1;
}

static void set_string(char **field, const char *value) {
    free_nullable(*field);
    *field = value != NULL ? strdup(value) : NULL;
}

static void set_int(int *field, const char *value) {
    if (value == NULL) {
        *field = 0;
        return;
    }
    errno = 0;
    int val = (int) strtol(value, NULL, 10);
    if (errno != 0) {
        *field = 0;
        return;
    }
    *field = val;
}