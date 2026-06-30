#include "app.h"
#include "app_launch.h"
#include "util/path.h"
#include "logging.h"

#if defined(TARGET_WEBOS) && !defined(DEBUG)

#include <sys/resource.h>
#include <sys/mman.h>

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

    /* Best-effort mlockall: pin current and future pages to avoid soft/major
     * page faults during streaming. Fails silently with EPERM on stock webOS
     * (no CAP_IPC_LOCK), which is fine — this just tightens worst-case jitter
     * when the capability is granted (e.g. under memory pressure).
     *
     * MCL_ONFAULT (Linux 4.4+) locks pages only as they are faulted in instead
     * of pre-committing all reserved-but-untouched memory. Without it, mlockall
     * pins every worker-thread stack (glibc default 8MB each; ~44 threads) fully
     * resident even though they are >95% empty => ~350MB of locked dead RAM, the
     * dominant memory-pressure / OOM driver on this 2GB TV. With ONFAULT the
     * latency intent is preserved (hot-path pages get touched early and stay
     * locked, no swap) but the empty stack tails are never committed -> aurora
     * RSS drops from ~500MB to its real working set (~170MB). Fall back to the
     * plain flags if the kernel/headers lack ONFAULT, so we never lose locking. */
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) != 0) {
        (void) mlockall(MCL_CURRENT | MCL_FUTURE);
    }
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