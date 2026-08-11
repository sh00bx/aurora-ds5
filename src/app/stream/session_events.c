#include "session_events.h"
#include "session_priv.h"

#if defined(TARGET_WEBOS)
#include "input/input_gamepad.h"
#include "hid_passthrough/hid_pt_gamepad_match.h"
#endif

bool session_handle_input_event(session_t *session, const SDL_Event *event) {
#if defined(TARGET_WEBOS)
    /* HID-passthrough exclusion must NOT sit behind the accepting_input gate.
     * A pad that SDL-enumerates while input is still blocked (connect/loading
     * overlay, display-topology settle — or a DS5 that BT-connects seconds
     * after stream start because it slept between runs) never reaches the
     * gated JOYDEVICEADDED handler, so it is never reconciled/excluded; its
     * first button press then leaks a MultiController event and the host
     * spawns a parallel default Xbox pad next to the bridged DS5 — the
     * Xbox<->DS5 input flap (RCA 2026-07-17: session-start reconcile at +0s
     * logged "No Moonlight gamepad match", SDL connect at +2.3s was swallowed
     * by this gate, Xbox pad appeared at first input +10s). */
    if (event->type == SDL_JOYDEVICEADDED) {
        SDL_JoystickID iid = SDL_JoystickGetDeviceInstanceID(event->jdevice.which);
        stream_input_t *in = &session->input;
        app_gamepad_state_t *gp =
            iid >= 0 ? app_input_gamepad_state_by_instance_id(in->input, iid) : NULL;
        if (gp != NULL && hid_pt_gamepad_is_autoplug(in->input, gp)) {
            hid_passthrough_manager_t *mgr = session_get_hid_passthrough(session);
            if (mgr != NULL && hid_passthrough_manager_active(mgr)) {
                hid_passthrough_manager_request_rescan(mgr, in);
                return true;  /* passthrough owns this pad; never announce it */
            }
        }
    }
#endif
    if (!session_accepting_input(session)) {
        return false;
    }
    stream_input_t *input = &session->input;
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            stream_input_handle_key(input, &event->key);
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            stream_input_handle_caxis(input, &event->caxis);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            stream_input_handle_cbutton(input, &event->cbutton);
            break;
        }
        case SDL_CONTROLLERSENSORUPDATE: {
            stream_input_handle_csensor(input, &event->csensor);
            break;
        }
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERTOUCHPADUP: {
            stream_input_handle_ctouchpad(input, &event->ctouchpad);
            break;
        }
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED: {
            stream_input_handle_jdevice(input, &event->jdevice);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED: {
            stream_input_handle_cdevice(input, &event->cdevice);
            break;
        }
        case SDL_MOUSEMOTION: {
            stream_input_handle_mmotion(input, &event->motion, false);
            break;
        }
        case SDL_MOUSEWHEEL: {
            if (!input->view_only && !input->no_sdl_mouse) {
                stream_input_handle_mwheel(input, &event->wheel);
            }
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (!input->view_only && !input->no_sdl_mouse) {
                stream_input_handle_mbutton(input, &event->button);
            }
            break;
        }
        case SDL_TEXTINPUT: {
            stream_input_handle_text(input, &event->text);
            break;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
            stream_input_handle_touch(input, &event->tfinger);
            break;
        }
        default:
            return false;
    }
    return true;
}
