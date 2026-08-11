#ifndef CTM_XPAD_EVDEV_H
#define CTM_XPAD_EVDEV_H

/* Flydigi dongle in XInput mode: feed the kernel xpad driver's evdev node to
 * the host as Xbox-360-wired HID reports, and turn the host's OUT reports back
 * into evdev force feedback.
 *
 * This is NOT HID passthrough. The dongle's gamepad interface is claimed by
 * xpad (vendor class 0xff, not class 03), so there is no hidraw node to relay:
 * the only way to move that pad's input is to read evdev, rebuild the 20-byte
 * wired-360 report ourselves and tag it with the interface's IN endpoint so the
 * host's composite device sees it arrive on the right pipe.
 *
 * The module owns the evdev fd, the feeder thread, the FF effect slot and the
 * xpad interface's endpoints. It reaches the controller only through the owner
 * services in ctm_controller_priv.h; the feeder thread runs while
 * ctm_ctl_readers_run(owner) is true. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_composite.h"
#include "ctm_controller_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctm_xpad ctm_xpad_t;

ctm_xpad_t *ctm_xpad_create(ctm_controller_t *owner);
void ctm_xpad_destroy(ctm_xpad_t *x);

/* Locate the xpad evdev node (/dev/input/eventN) of the USB device on
 * `usb_busid`. 0 on success. Also used by the pump's evdev-only open tier,
 * which has no feeder yet but must know the node exists. */
int ctm_xpad_find_evdev(const char *usb_busid, char *path_out, size_t path_len);

/* Resolve the xpad interface endpoints, open + grab the evdev node and start
 * the feeder thread. `cp` supplies the USB sysfs walk. 0 on success; logs the
 * reason and leaves nothing running on failure. When: session start, for a
 * composite_evdev_gamepad type. */
int ctm_xpad_start(ctm_xpad_t *x, ctm_composite_t *cp, const char *usb_busid);

/* Join the feeder, stop any playing rumble, ungrab and close. Safe when the
 * feeder never started. When: session teardown. */
void ctm_xpad_stop(ctm_xpad_t *x);

/* True while the evdev node is open, i.e. while host OUT reports can be turned
 * into force feedback. */
bool ctm_xpad_active(const ctm_xpad_t *x);

/* OUT endpoint of the xpad-claimed interface: the address the host uses when it
 * sends rumble for this pad. 0 until ctm_xpad_start() succeeds. */
uint8_t ctm_xpad_out_ep(const ctm_xpad_t *x);

/* Apply one host OUT report as evdev force feedback. Reports that are not an
 * Xbox 360 wired rumble payload are ignored. */
void ctm_xpad_apply_rumble(ctm_xpad_t *x, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CTM_XPAD_EVDEV_H */
