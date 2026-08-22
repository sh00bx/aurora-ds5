#pragma once

#include <stdbool.h>
#include <SDL_joystick.h>
#include <SDL_gamecontroller.h>

#include "config.h"

typedef struct app_input_t app_input_t;
typedef struct app_gamepad_state_t app_gamepad_state_t;

bool app_input_init_gamepad(app_input_t *input, int device_index);

void app_input_close_gamepad(app_input_t *input, SDL_JoystickID sdl_id);

int app_input_get_gamepads_count(app_input_t *input);

short app_input_get_max_gamepads(app_input_t *input);

short app_input_gamepads_mask(app_input_t *input);

void app_input_gamepad_rumble(app_input_t *input, unsigned short controllerNumber, unsigned short lowFreqMotor,
                              unsigned short highFreqMotor);

void app_input_gamepad_rumble_triggers(app_input_t *input, unsigned short controllerNumber, unsigned short leftTrigger,
                                       unsigned short rightTrigger);

void app_input_gamepad_set_motion_event_state(app_input_t *input, unsigned short controllerNumber, uint8_t motionType,
                                              uint16_t reportRateHz);

void app_input_gamepad_set_controller_led(app_input_t *input, unsigned short controllerNumber, uint8_t r, uint8_t g,
                                          uint8_t b);

void app_input_gamepad_set_adaptive_triggers(app_input_t *input, unsigned short controllerNumber, uint8_t eventFlags,
                                             uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

void app_input_gamepad_set_player_led(app_input_t *input, unsigned short controllerNumber, uint8_t ledValue);

void app_input_gamepad_set_mic_led(app_input_t *input, unsigned short controllerNumber, uint8_t ledState);

app_gamepad_state_t * app_input_gamepad_state_init(app_input_t *input, SDL_GameController *controller);
void app_input_gamepad_state_deinit(app_gamepad_state_t *state);

app_gamepad_state_t *app_input_gamepad_state_by_index(app_input_t *input, int index);

app_gamepad_state_t *app_input_gamepad_state_by_instance_id(app_input_t *input, SDL_JoystickID instance_id);

/* Hand the DS5 idle lightbar back to its "connected, unused" colour. Called on
 * input shutdown so quitting the app does not leave the pad on the in-use
 * colour; the per-controller open/close path does this on its own. */
void app_input_ds5_idle_lightbar_release(void);

#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
/* Track the app's foreground state for the controller touchpad grab. The grab is
 * an exclusive EVIOCGRAB, so it must not outlive the foreground - it would leave
 * the pad's touchpad dead as a TV pointer in every other app. Call it from the
 * SDL_APP_WILLENTERBACKGROUND / SDL_APP_DIDENTERFOREGROUND handlers; it walks
 * the gamepad slots without locking and is main-thread only. */
void app_input_gamepad_set_foreground(app_input_t *input, bool foreground);
#else
#define app_input_gamepad_set_foreground(input, foreground) ((void) 0)
#endif
