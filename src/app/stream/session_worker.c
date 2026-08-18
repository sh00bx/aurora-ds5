#include "session_worker.h"

#include "session_priv.h"
#include "app.h"
#include "util/bus.h"
#include "logging.h"
#include "errors.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#if TARGET_WEBOS
#include "platform/webos/tv_game_mode.h"
#endif
#include "stream/connection/session_connection.h"
#include "stream/audio/session_audio.h"
#include "stream/video/session_video.h"
#include "stream/adaptive_bitrate.h"
#include "app_session.h"
#include "backend/pcmanager/worker/worker.h"
#include "app_settings.h"

#include <stdlib.h>
#include <stdio.h>

#if TARGET_WEBOS
#include <SDL.h>
#endif

/* Host-PTS smooth pacing (ss4s FeedWithPTS): the env must ALWAYS be set
 * explicitly — ss4s defaults to ON when the variable is absent, our default
 * is OFF (A/B baseline; NDL is believed to present ASAP ignoring PTS). Read
 * by the ss4s module in VideoOpen, so it takes effect per stream start.
 *
 * Upstream v1.1.7 hardcodes pacing to always-on and dropped the setting. We keep
 * the toggle (it is our A/B handle for the FeedWithPTS question) but adopt their
 * panel-refresh anchoring for the PTS grid. */
static void session_apply_smooth_pacing_env(const session_t *session) {
#if TARGET_WEBOS
    const app_settings_t *cfg = app_configuration;
    bool smooth = cfg != NULL && cfg->smooth_frame_pacing;
    const char *smooth_val = smooth ? "1" : "0";
    /* Shared names for SMP + NDL; NDL_* aliases for older module builds. */
    setenv("SS4S_SMOOTH_PACING", smooth_val, 1);
    setenv("SS4S_NDL_SMOOTH_PACING", smooth_val, 1);
    /* Historical Starfish default; not exposed in settings. */
    setenv("SS4S_PAUSE_AT_DECODE_TIME", "1", 1);
    /* Tight drift: 0.5 frame (SS4S reads as percent of interval via maxDrift factor). */
    setenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES", "0.5", 1);

    /* Prefer measured panel refresh for the PTS grid so display cadence matches the OLED,
     * not only the host's clientRefreshRateX100 / stream fps (common microstutter source). */
    int stream_x100 = session->config.stream.clientRefreshRateX100;
    if (stream_x100 <= 0 && session->config.stream.fps > 0) {
        stream_x100 = session->config.stream.fps * 100;
    }

    int x100 = stream_x100;
    int panel_hz = 0;
    const char *source = "stream";
    if (SDL_webOSGetRefreshRate(&panel_hz) && panel_hz >= 20 && panel_hz <= 240) {
        int panel_x100 = panel_hz * 100;
        if (stream_x100 <= 0) {
            x100 = panel_x100;
            source = "panel";
        } else {
            int stream_hz = (stream_x100 + 50) / 100;
            /* Same ballpark (±2 Hz): anchor PTS to the panel. Far apart: keep stream. */
            if (abs(stream_hz - panel_hz) <= 2) {
                x100 = panel_x100;
                source = "panel";
            }
        }
    }

    if (x100 > 0) {
        /* interval_us = 1e6 * 100 / x100  (e.g. 12000 → 8333 µs) */
        long interval_us = (100000000L + (x100 / 2)) / x100;
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", interval_us);
        setenv("SS4S_SMOOTH_PACING_INTERVAL_US", buf, 1);
        setenv("SS4S_NDL_PACING_INTERVAL_US", buf, 1);
        commons_log_info("Session",
                         "Smooth pacing %s, interval %ld µs from %s (x100=%d, stream_x100=%d, panel=%d Hz)",
                         smooth_val, interval_us, source, x100, stream_x100, panel_hz);
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
        // webOS NDL Opus passthrough only accepts the mapping {0,1,4,5,2,3}
        // (FL FR SL SR FC LFE). Asking the host for SDL order (012345) makes every
        // 5.1 frame take the SS4S opus_fix re-encode path, which adds latency and
        // drops out momentarily. The client-side Opus decoder emits channels in the
        // requested mapping order too, so this layout is what the NDL 6-channel PCM
        // sink expects either way.
        // 6 ch, 4 streams, 2 coupled, FL FR SL SR FC LFE:
        surround_params = "642014523";
        commons_log_info("Session", "5.1 surroundParams=%s (webOS channel layout; skips opus_fix re-encode)",
                         surround_params);
    }
#endif
    short gamepad_mask = app_input_gamepads_mask(&app->input);
#if defined(TARGET_WEBOS)
    if (session->config.hid_passthrough) {
        /* Computed on the main thread in session_create; read-only here. */
        gamepad_mask &= (short) ~session->input.moonlightExcludedMask;
    }
#endif
    int ret = gs_start_app(client, server, &session->config.stream, appId, server->isGfe, session->config.sops,
                           session->config.local_audio, gamepad_mask, surround_params,
                           app_configuration != NULL && app_configuration->vrr);
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
#if TARGET_WEBOS
    /* Hand the TV over to the stream: game picture/sound preset, discovery and
     * cast services out of the DS5's airtime, cores pinned, stream threads
     * boosted. Runs on its own thread and restores everything at thread_cleanup
     * below -- including when this session ends in an error. */
    if (app_configuration != NULL && app_configuration->webos_game_mode) {
        tv_game_mode_stream_begin();
    }
#endif
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
#if TARGET_WEBOS
    /* Unconditional: the begin call is gated on the preference, but the end
     * must not be -- a preference toggled mid-session would otherwise leave the
     * TV in game mode with nothing left to turn it off. It is a no-op when
     * nothing was engaged. */
    tv_game_mode_stream_end();
#endif
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