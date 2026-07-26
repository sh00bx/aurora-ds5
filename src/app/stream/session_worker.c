#include "session_worker.h"
#include "session_priv.h"
#include "app.h"
#include "util/bus.h"
#include "logging.h"
#include "errors.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/connection/session_connection.h"
#include "stream/audio/session_audio.h"
#include "stream/video/session_video.h"
#include "stream/adaptive_bitrate.h"
#include "app_session.h"
#include "backend/pcmanager/worker/worker.h"

#if defined(TARGET_WEBOS)
#include "hid_passthrough/hid_pt_gamepad_match.h"
#endif

#include "app_settings.h"

#include <stdlib.h>
#include <stdio.h>

/* Host-PTS smooth pacing (ss4s FeedWithPTS): the env must ALWAYS be set
 * explicitly — ss4s defaults to ON when the variable is absent, our default
 * is OFF (A/B baseline; NDL is believed to present ASAP ignoring PTS). Read
 * by the ss4s module in VideoOpen, so it takes effect per stream start. */
static void session_apply_smooth_pacing_env(const session_t *session) {
#if TARGET_WEBOS
    const app_settings_t *cfg = app_configuration;
    bool smooth = cfg != NULL && cfg->smooth_frame_pacing;
    const char *smooth_val = smooth ? "1" : "0";
    /* Shared names for SMP + NDL; NDL_* aliases for older module builds. */
    setenv("SS4S_SMOOTH_PACING", smooth_val, 1);
    setenv("SS4S_NDL_SMOOTH_PACING", smooth_val, 1);

    int x100 = session->config.stream.clientRefreshRateX100;
    if (x100 > 0) {
        /* interval_us = 1e6 * 100 / x100  (e.g. 11988 → ~8341 µs) */
        long interval_us = (100000000L + (x100 / 2)) / x100;
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", interval_us);
        setenv("SS4S_SMOOTH_PACING_INTERVAL_US", buf, 1);
        setenv("SS4S_NDL_PACING_INTERVAL_US", buf, 1);
    } else {
        unsetenv("SS4S_SMOOTH_PACING_INTERVAL_US");
        unsetenv("SS4S_NDL_PACING_INTERVAL_US");
    }
#else
    (void) session;
#endif
}

/* Connect-phase instrumentation: append "<ms-since-app-start> <phase>" to a
 * jail-visible file so connect latency can be broken down per phase without a
 * console (app stdout goes to /dev/null under the system launcher). Cheap
 * (one open/write per milestone, only during connects), always on. */
static void connect_mark(const char *phase) {
    /* Timestamps are ms since THIS app run's SDL init, but jail /tmp survives
     * app restarts and instant-on standby — truncate on the first mark of each
     * run so the file never mixes runs with backwards-jumping timestamps. */
    static int fresh_run = 1;
    FILE *f = fopen("/tmp/aurora-connect.log", fresh_run ? "w" : "a");
    if (!f) { return; }
    if (fresh_run) {
        fprintf(f, "# run %s\n", APP_VERSION);
        fresh_run = 0;
    }
    fprintf(f, "%lu %s\n", (unsigned long) SDL_GetTicks(), phase);
    fclose(f);
}

int session_worker(session_t *session) {
    app_t *app = session->app;
    session_set_state(session, STREAMING_CONNECTING);
    bus_pushevent(USER_STREAM_CONNECTING, NULL, NULL);
    streaming_error(session, GS_OK, "");
    PSERVER_DATA server = session->server;
    int appId = session->app_id;
    session->player = NULL;

#if FEATURE_INPUT_EVMOUSE
    if (!session->config.view_only && session->config.hardware_mouse) {
        session_evmouse_wait_ready(&session->input.evmouse);
    }
#endif

    connect_mark("worker_start");
    commons_log_info("Session", "Launch app %d (host currentGame=%d)...", appId, server->currentGame);
    if (session->config.stream.clientRefreshRateX100 > 0) {
        commons_log_info("Session",
                         "Stream mode %dx%dx%d, clientRefreshRateX100=%d (%.2f Hz), bitrate %d kbps",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.clientRefreshRateX100,
                         session->config.stream.clientRefreshRateX100 / 100.0,
                         session->config.stream.bitrate);
    } else {
        commons_log_info("Session", "Stream mode %dx%dx%d, bitrate %d kbps (no fractional refresh rate)",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.bitrate);
    }
    GS_CLIENT client = app_gs_client_new(app);
    const char *surround_params = NULL;
#if TARGET_WEBOS
    if (session->config.stream.audioConfiguration == AUDIO_CONFIGURATION_51_SURROUND) {
        // 6 channels, 4 streams, 2 coupled streams, FL, FR, SL, SR, FC, LFE
        surround_params = "642014523";
    }
#endif
    short gamepad_mask = app_input_gamepads_mask(&app->input);
#if defined(TARGET_WEBOS)
    if (session->config.hid_passthrough) {
        session->input.moonlightExcludedMask = hid_pt_moonlight_excluded_mask_at_start(&app->input);
        gamepad_mask &= (short) ~session->input.moonlightExcludedMask;
    }
#endif
    int ret = gs_start_app(client, server, &session->config.stream, appId, server->isGfe, session->config.sops,
                           session->config.local_audio, gamepad_mask, surround_params);
    connect_mark(ret == GS_OK ? "gs_start_app ok" : "gs_start_app FAILED");
    if (ret != GS_OK) {
        session_set_state(session, STREAMING_ERROR);
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        if (gs_error) {
            streaming_error(session, ret, "Failed to launch session: %s (code %d)", gs_error, ret);
        } else {
            streaming_error(session, ret, "Failed to launch session: gamestream returned %d", ret);
        }
        commons_log_error("Session", "Failed to launch session: gamestream returned %d, gs_error=%s", ret, gs_error);
        goto thread_cleanup;
    }

    commons_log_info("Session", "Audio %d channels",
                     CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(session->config.stream.audioConfiguration));

    connect_mark("player_open");
    session->player = SS4S_PlayerOpen();
    SS4S_PlayerSetWaitAudioVideoReady(session->player, true);
    SS4S_PlayerSetViewportSize(session->player, app->ui.width, app->ui.height);
    SS4S_PlayerSetUserdata(session->player, app);

    session_apply_smooth_pacing_env(session);
    session_video_prepare_stream();

    connect_mark("li_start");
    int startResult = LiStartConnection(&server->serverInfo, &session->config.stream,
                                        session_connection_callbacks_prepare(session),
                                        &ss4s_dec_callbacks, &ss4s_aud_callbacks, session, 0, session, 0);
    connect_mark(startResult == 0 ? "li_connected (STREAMING)" : "li_start FAILED");
    if (startResult != 0) {
        session_set_state(session, STREAMING_ERROR);
        switch (startResult) {
            case CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported video codec.");
                break;
            case CALLBACKS_SESSION_ERROR_VDEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open video decoder.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported audio codec.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open audio backend.");
                break;
            default: {
                if (!streaming_errno) {
                    streaming_error(session, GS_WRONG_STATE, "Failed to start connection: Limelight returned %d (%s)",
                                    startResult, strerror(startResult));
                }
                break;
            }
        }
        commons_log_error("Session", "Failed to start connection: Limelight returned %d", startResult);
        goto thread_cleanup;
    }
    session_set_state(session, STREAMING_STREAMING);
    bus_pushevent(USER_STREAM_OPEN, NULL, NULL);
    if (session->config.auto_adjust_bitrate || session->config.soft_recovery) {
        adaptive_bitrate_config_t abr_config = {
                .gs_client = client,
                .server = server,
                .initial_bitrate = session->config.stream.bitrate,
                .mode = (abr_mode_t) session->config.abr_mode,
                .recovery_only = session->config.soft_recovery && !session->config.auto_adjust_bitrate,
        };
        session->abr = adaptive_bitrate_start(&abr_config);
    }
    SDL_LockMutex(session->mutex);
    while (!session->interrupted) {
        // Wait until interrupted
        SDL_CondWait(session->cond, session->mutex);
    }
    SDL_UnlockMutex(session->mutex);
    bus_pushevent(USER_STREAM_CLOSE, NULL, NULL);

    session_set_state(session, STREAMING_DISCONNECTING);
    LiStopConnection();

    if (session->quitapp) {
        commons_log_info("Session", "Sending app quit request ...");
        gs_quit_app(client, server);
    }
    worker_context_t update_ctx = {
            .app = app,
            .manager = pcmanager,
    };
    uuidstr_fromstr(&update_ctx.uuid, server->uuid);
    pcmanager_update_by_host(&update_ctx, server->serverInfo.address, server->extPort, true);

    // Don't always reset status as error state should be kept
    session_set_state(session, STREAMING_NONE);
    thread_cleanup:
    /* Restore only on a clean exit: streaming_errno != GS_OK means the session
     * ended in error/disconnect and the host is likely unreachable -- the
     * restore round-trips would just block teardown on timeouts. */
    adaptive_bitrate_stop(session->abr, streaming_errno == GS_OK);
    session->abr = NULL;
    session_connection_callbacks_reset(session);
    if (session->player != NULL) {
        SS4S_PlayerClose(session->player);
    }
    gs_destroy(client);
    bus_pushevent(USER_STREAM_FINISHED, NULL, NULL);
    app_bus_post(app, (bus_actionfunc) app_session_destroy, app);
    return 0;
}