#include "app.h"

#include "ui/root.h"
#include "ui/streaming/streaming.controller.h"

#include "stream/input/session_input.h"
#include "app_settings.h"
#include "stream/input/vk.h"
#include "stream/session.h"
#include "stream/session_priv.h"

#include <Limelight.h>
#include <SDL.h>

#include "util/bus.h"
#include "util/user_event.h"
#include "logging.h"
#include "app_webos.h"

bool stream_input_webos_intercept_remote_keys(stream_input_t *input, const SDL_KeyboardEvent *event, short *keyCode) {
    session_t *session = input->session;
    switch ((unsigned int) event->keysym.scancode) {
        case SDL_SCANCODE_WEBOS_EXIT: {
            if (event->state == SDL_PRESSED) {
                bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            }
            return true;
        }
        case SDL_SCANCODE_WEBOS_HOME: {
            /* webOS folds two physically different keys onto this one scancode: the
             * remote's Home button and a USB keyboard's left Super. That is not a
             * guess — the TV's own xkb keycode table maps <IR_KEY_HOME> to
             * Qt::Key_Super_L, and the SDL hint that unlocks the key is documented
             * as "home key. (L_Super on keyboard)".
             *
             * So the two intents collide, and system-key capture is the switch
             * between them: with it on the user has asked for host system keys, and
             * a Windows key that arrives as VK_HOME opens nothing. With it off we
             * keep forwarding Home (Reshade et al) and the ribbon behaviour.
             *
             * The collision only exists while the keyboard has no route of its own.
             * Once the evdev grab holds it, EVIOCGRAB takes it away from the
             * compositor entirely -- SDL sees nothing from that device, which is why
             * the worker has to rebuild the modifier state by hand -- so its Super
             * arrives as KEY_LEFTMETA on the evdev path and can no longer reach this
             * scancode. Anything landing here in that state is the remote's Home
             * button, and forwarding it as a Windows key would cost webOS Home for
             * no gain, because the keyboard's Super is already getting through. */
            bool key_has_own_route = false;
#if FEATURE_INPUT_EVKBD
            key_has_own_route = session != NULL && session_evkbd_is_grabbing(&input->evkbd);
#endif
            if (session != NULL && !input->view_only && !key_has_own_route) {
                *keyCode = app_configuration->syskey_capture ? VK_LWIN : VK_HOME;
                return false;
            }
            if (event->state == SDL_RELEASED) {
                app_webos_open_ribbon();
            }
            return true;
        }
        case SDL_SCANCODE_WEBOS_BACK:
            if (streaming_soft_keyboard_shown()) {
                bus_pushevent(USER_CLOSE_SOFT_KEYBOARD, NULL, NULL);
                return true;
            }
            *keyCode = VK_ESCAPE /* SDL_SCANCODE_ESCAPE */;
            return false;
        case SDL_SCANCODE_WEBOS_CH_UP:
            if (session == NULL || ui_should_block_input()) {
                return true;
            }
            *keyCode = VK_PRIOR /* SDL_SCANCODE_PAGEUP */;
            return false;
        case SDL_SCANCODE_WEBOS_CH_DOWN:
            if (session == NULL || ui_should_block_input()) {
                return true;
            }
            *keyCode = VK_NEXT /* SDL_SCANCODE_PAGEDOWN */;
            return false;
        case SDL_SCANCODE_WEBOS_BLUE:
            /* BLUE opens the on-screen keyboard. Matches the keyboard button's blue styling
             * in the streaming overlay (see streaming.view.c). */
            if (input->view_only) {
                return true;
            }
            if (event->state == SDL_PRESSED) {
                bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
            }
            return true;
        case SDL_SCANCODE_WEBOS_RED:
            /* RED keeps opening the streaming overlay (options menu). */
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            return true;
        default:
            return false;
    }
}
