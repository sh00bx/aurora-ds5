#ifndef CTM_COMPOSITE_H
#define CTM_COMPOSITE_H

/* Composite USB controllers (Steam puck, Flydigi dongle): forward EVERY HID
 * interface of one USB device, not just the node the app classified.
 *
 * The module owns the sibling interfaces (fd + reader thread + endpoints) and
 * the primary interface's endpoint/interface numbers, which is all the state
 * the pump needs to tag input and route output. It reaches the controller only
 * through the owner services in ctm_controller_priv.h.
 *
 * Threads: one reader per sibling, started by ctm_composite_start_readers()
 * and joined by ctm_composite_shutdown(). They run while
 * ctm_ctl_readers_run(owner) is true.
 *
 * The sysfs walk here deliberately goes through /sys/class/input rather than
 * /sys/class/hidraw: the hidraw realpath is FLAKY in the dev-mode jail, so the
 * composite open must not depend on it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_controller_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctm_composite ctm_composite_t;

/* Build the (empty) composite state for a controller. `usb_busid` may be empty,
 * in which case the USB device is resolved by VID/PID scan; `primary_path` is
 * the /dev/hidrawN the pump itself reads, which is excluded from the siblings.
 * The strings are copied. Returns NULL only on allocation failure. */
ctm_composite_t *ctm_composite_create(ctm_controller_t *owner,
                                      unsigned int vid, unsigned int pid,
                                      const char *usb_busid, const char *primary_path);
void ctm_composite_destroy(ctm_composite_t *cp);

/* Open every class-03 HID interface of the device R/W except the primary, and
 * record the primary's endpoints + interface number. When: session start, for
 * composite types, BEFORE the input thread starts (it tags primary input with
 * the primary IN endpoint). Logs and leaves the state empty on failure. */
void ctm_composite_open(ctm_composite_t *cp);

/* Start one reader thread per opened sibling. When: after the primary input
 * thread is up. */
void ctm_composite_start_readers(ctm_composite_t *cp);

/* Join whatever readers were started, close every sibling fd and forget them.
 * Safe when nothing was started (the session-abort paths). When: session end. */
void ctm_composite_shutdown(ctm_composite_t *cp);

int ctm_composite_count(const ctm_composite_t *cp);
uint8_t ctm_composite_primary_in_ep(const ctm_composite_t *cp);
uint8_t ctm_composite_primary_out_ep(const ctm_composite_t *cp);

/* The sibling fd that owns OUT endpoint `ep`, or `fallback_fd` (the primary)
 * when `ep` is 0, is the primary's, or matches no sibling. */
int ctm_composite_out_fd_for_ep(const ctm_composite_t *cp, uint8_t ep, int fallback_fd);

/* The hidraw fd a feature request addresses: the host encodes the target USB
 * interface number in the high byte of request_id. Falls back to `fallback_fd`
 * (the primary) for a non-composite type or an unknown interface. */
int ctm_composite_feature_fd(const ctm_composite_t *cp, bool composite,
                             uint32_t request_id, int fallback_fd);

/* --- sysfs helpers, shared with the xpad feeder ---------------------------- */

/* Resolve this device's USB sysfs dir (the one holding idVendor). Prefers the
 * stable bus id over the VID/PID scan, which is ambiguous with duplicates.
 * 0 on success. */
int ctm_composite_usb_device_dir(const ctm_composite_t *cp, char *out, size_t out_len);

/* Read the IN (0x80 set) and OUT (0x80 clear) endpoint addresses of a USB
 * interface dir from its ep_* children. Leaves either at 0 if absent. */
void ctm_composite_iface_endpoints(const char *ifdir, uint8_t *in_ep, uint8_t *out_ep);

/* Find /dev/hidrawN under a USB interface dir (ifdir/<hid>/hidraw/hidrawN). */
int ctm_composite_find_hidraw_under(const char *ifdir, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif /* CTM_COMPOSITE_H */
