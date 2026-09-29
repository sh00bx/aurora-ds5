#include "app.h"
#include "app_launch.h"
#include "util/path.h"
#include "logging.h"

#if defined(TARGET_WEBOS) && !defined(DEBUG)

#include <sys/resource.h>

#endif

static int settings_load(app_settings_t *settings);

int main(int argc, char *argv[]) {
#ifdef TARGET_WEBOS
    if (getenv("EGL_PLATFORM") == NULL) {
        setenv("EGL_PLATFORM", "wayland", 0);
    }
    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    }
#ifndef DEBUG
    // Don't generate core dumps in release builds
    struct rlimit rlim = {0, 0};
    setrlimit(RLIMIT_CORE, &rlim);

    /* No mlockall here any more (upstream aurora-tv 35c1cc0aa, issue #75).
     * MCL_FUTURE also pins every NDL/decoder mapping the process touches later;
     * upstream measured VmLck 288 MB and system Mlocked 421 MB right after the
     * NDL load, MemFree fell to 91 MB and the watchdog rebooted the set. On our
     * G4 the same lock showed VmLck 320 MB (09-27) and pushed Unevictable up,
     * which lowers the memchute emergency threshold that kills the home app.
     * The old fallback without MCL_ONFAULT was the exact upstream failure mode.
     * Latency-critical first touches are pre-faulted where they matter
     * (session_video.c), so nothing on the hot path depends on the lock. */
#endif
#endif
    app_t app;
    int ret = app_init(&app, settings_load, argc, argv);
    if (ret != 0) {
        return ret;
    }

    app_launch_params_t *params = app_handle_launch(&app, argc, argv);

    app_ui_open(&app.ui, true, params);

    while (app.running) {
        app_run_loop(&app);
    }

    app_launch_param_free(params);

    app_deinit(&app);
    return ret;
}


static int settings_load(app_settings_t *settings) {
    bool persistent = true;
    settings_initialize(settings, path_pref(&persistent));
    settings->conf_persistent = persistent;
    if (!settings_read(settings)) {
        commons_log_warn("Settings", "Failed to read settings %s", settings->ini_path);
    }
    return 0;
}