/* Generic HID controller — verbatim relay, and the factory fallback. Its
 * matches() is always true so it claims any device no specific type took. */

#define _GNU_SOURCE

#include "ctm_controller.h"

static bool generic_matches(const ctm_controller_dev_t *dev)
{
    (void)dev;
    return true;
}

/* Pump policy: all off. generic_matches() claims anything no specific type
 * took, so this policy is what an UNKNOWN device gets — every feature here
 * keys on a report layout we would only be guessing at, and the idle timeout
 * in particular would tear down any device that reports on user action only. */
static const ctm_pump_policy_t generic_policy = {
    .input_idle_timeout_ms = 0,
};

const ctm_controller_ops_t ctm_controller_generic_ops = {
    .kind = "generic",
    .policy = &generic_policy,
    .matches = generic_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .patch_output = NULL,   /* verbatim */
    .set_settings = NULL,
};
