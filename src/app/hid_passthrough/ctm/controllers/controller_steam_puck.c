/* Steam Controller ("puck", USB dongle bridged via hidraw — the no-/dev/bus/usb
 * workaround). STAGE 1: classification only. STAGE 2 adds select_node
 * (gamepad-interface selection, fixes §6a) + on_plug_init (lizard-mode exit). */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <string.h>

static bool steam_puck_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "28de") == 0 &&
           strcmp(dev->pid, "1304") == 0;
}

/* Pump policy: composite relay, no watchdog — the puck's forwarded interfaces
 * only report on user action, so silence is the normal idle state. */
static const ctm_pump_policy_t steam_puck_policy = {
    .input_idle_timeout_ms = 0,
};

const ctm_controller_ops_t ctm_controller_steam_puck_ops = {
    .kind = "steam_puck",
    .policy = &steam_puck_policy,
    .matches = steam_puck_matches,
    .select_node = NULL,    /* STAGE 2: pick the gamepad/vendor hidraw interface */
    .on_plug_init = NULL,   /* STAGE 2: SET_SETTINGS lizard-mode exit */
    .patch_output = NULL,
    .set_settings = NULL,
    .composite = true,   /* forward all 7 interfaces; host builds the composite */
};
