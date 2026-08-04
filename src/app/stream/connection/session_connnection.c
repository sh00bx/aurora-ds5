#include "ui/streaming/streaming.controller.h"
#include "stream/input/session_input.h"

#include "util/i18n.h"
#include "logging.h"
#include "input/input_gamepad.h"
#include "app.h"
#include "stream/session_priv.h"

static session_t *current_session = NULL;

/* Per-stage timing into the same jail-visible file session_worker.c writes its
 * milestones to — breaks LiStartConnection's internal cost down by stage
 * (RTSP handshake, control/video/audio stream setup, input). */
static void connection_stage_mark(int stage, const char *what) {
    FILE *f = fopen("/tmp/aurora-connect.log", "a");
    if (!f) { return; }
    fprintf(f, "%lu stage:%s %s\n", (unsigned long) SDL_GetTicks(), LiGetStageName(stage), what);
    fclose(f);
}

static void connection_stage_starting(int stage) { connection_stage_mark(stage, "starting"); }

static void connection_stage_complete(int stage) { connection_stage_mark(stage, "complete"); }

static void connection_terminated(int errorCode) {
    if (!current_session) {
        return;
    }
    if (errorCode == ML_ERROR_GRACEFUL_TERMINATION) {
        session_interrupt(current_session, false, STREAMING_INTERRUPT_HOST);
    } else {
        commons_log_error("Session", "Connection terminated, errorCode = 0x%x", errorCode);
        streaming_error(current_session, 0, "Connection terminated, errorCode = 0x%x", errorCode);
        session_interrupt(current_session, false, STREAMING_INTERRUPT_NETWORK);
    }
}

static void connection_log_message(const char *format, ...) {
    va_list arglist;
    va_start(arglist, format);
    commons_log_vprintf(COMMONS_LOG_LEVEL_INFO, "Limelight", format, arglist);
    va_end(arglist);
}

static void connection_status_update(int status) {
    switch (status) {
        case CONN_STATUS_OKAY:
            commons_log_info("Session", "Connection is okay");
            streaming_notice_show(NULL);
            break;
        case CONN_STATUS_POOR:
            commons_log_warn("Session", "Connection is poor");
            streaming_notice_show(locstr("Unstable connection."));
            break;
        default:
            break;
    }
}

static void connection_stage_failed(int stage, int errorCode) {
    if (!current_session) {
        return;
    }
    const char *stageName = LiGetStageName(stage);
    commons_log_error("Session", "Connection failed at stage %d (%s), errorCode = %d (%s)", stage, stageName, errorCode,
                      strerror(errorCode));
    streaming_error(current_session, errorCode, "Connection failed at stage %d (%s), errorCode = %d (%s)", stage,
                    stageName, errorCode, strerror(errorCode));
}

static void connection_rumble(unsigned short controllerNumber, unsigned short lowFreqMotor,
                              unsigned short highFreqMotor) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_rumble(&current_session->app->input, controllerNumber, lowFreqMotor, highFreqMotor);
}

static void connection_rumble_triggers(unsigned short controllerNumber, unsigned short leftTrigger,
                                       unsigned short rightTrigger) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_rumble_triggers(&current_session->app->input, controllerNumber, leftTrigger, rightTrigger);
}

static void connection_set_motion_event_state(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_set_motion_event_state(&current_session->app->input, controllerNumber, motionType, reportRateHz);
}

static void connection_set_controller_led(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_set_controller_led(&current_session->app->input, controllerNumber, r, g, b);
}

static void connection_set_adaptive_triggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft,
                                             uint8_t typeRight, uint8_t *left, uint8_t *right) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_set_adaptive_triggers(&current_session->app->input, controllerNumber, eventFlags, typeLeft,
                                            typeRight, left, right);
}

static void connection_set_player_led(uint16_t controllerNumber, uint8_t ledValue) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_set_player_led(&current_session->app->input, controllerNumber, ledValue);
}

static void connection_set_mic_led(uint16_t controllerNumber, uint8_t ledState) {
    if (!current_session) {
        return;
    }
#if defined(TARGET_WEBOS)
    /* Pads, die per CTM als natives HID gebrueckt sind, bekommen Rumble/LED/Motion direkt
     * ueber die Bruecke — der Moonlight-Pfad wuerde sie doppelt ansteuern. */
    if (current_session->input.moonlightExcludedMask & (1u << controllerNumber)) {
        return;
    }
#endif
    app_input_gamepad_set_mic_led(&current_session->app->input, controllerNumber, ledState);
}

static void connection_set_hdr(bool hdrEnabled) {
    if (!current_session) {
        return;
    }
    commons_log_info("Session", "Host HDR signal: %s", hdrEnabled ? "enabled" : "disabled");
    streaming_set_hdr(current_session, hdrEnabled);
}

CONNECTION_LISTENER_CALLBACKS connection_callbacks = {
        .stageStarting = connection_stage_starting,
        .stageComplete = connection_stage_complete,
        .stageFailed = connection_stage_failed,
        .connectionStarted = NULL,
        .connectionTerminated = connection_terminated,
        .logMessage = connection_log_message,
        .rumble = connection_rumble,
        .rumbleTriggers = connection_rumble_triggers,
        .setMotionEventState = connection_set_motion_event_state,
        .setControllerLED = connection_set_controller_led,
        .setAdaptiveTriggers = connection_set_adaptive_triggers,
        .setPlayerLed = connection_set_player_led,
        .setMicLed = connection_set_mic_led,
        .connectionStatusUpdate = connection_status_update,
        .setHdrMode = connection_set_hdr
};


CONNECTION_LISTENER_CALLBACKS *session_connection_callbacks_prepare(session_t *session) {
    current_session = session;
    return &connection_callbacks;
}

void session_connection_callbacks_reset(session_t *session) {
    current_session = NULL;
}