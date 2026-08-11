#ifndef CTM_CONTROLLER_PRIV_H
#define CTM_CONTROLLER_PRIV_H

/* Owner services for the controller subsystem modules (D2 stage 3).
 *
 * `struct ctm_controller` is defined in controller_common.c and stays there.
 * The modules split out of that file (ctm_composite, ctm_xpad_evdev,
 * ctm_hid_io, ctm_feature_worker) hold an opaque `ctm_controller_t *owner` and
 * reach the controller ONLY through the functions below, so the state each
 * module owns lives in its own struct and cannot be aliased from another.
 *
 * Internal to ctm/controllers/ — check with
 * `grep -rl ctm_controller_priv.h src/` before assuming otherwise. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "ctm_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest HID report this bridge moves in one piece, in either direction. */
#define CTM_MAX_REPORT 4096

/* Monotonic clock in microseconds. Used for every pacing schedule, deadline
 * and telemetry window in the subsystem. */
static inline uint64_t ctm_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Compare a hex-string sysfs attr (id/vendor, id/product) to a numeric value.
 * 0 when the text is empty or not a number, so a missing attr never matches. */
static inline int ctm_hex_equals(const char *text, unsigned int value)
{
    if (!text || !text[0]) return 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 16);
    return end != text && parsed == value;
}

/* ui/root.c — true while the streaming overlay / soft keyboard / HID panel owns
 * input. Declared here instead of including ui/root.h to keep the controller
 * layer free of LVGL headers.
 *
 * A relaxed load of a flag the LVGL thread publishes; it reads no LVGL object
 * and takes no lock, so it is safe to call from the controller pump and the
 * evdev feeder and costs nothing on the per-report path. The one imprecision is
 * timing: a report may be forwarded against a gate state one publish old. It
 * used to be worse than that — the gate was recomputed per read by chasing the
 * live streaming fragment, which LVGL frees underneath the worker threads. */
extern bool ui_should_block_input(void);

/* --- owner services --------------------------------------------------------
 * All of these are safe to call from any of the controller's threads. */

/* Append one line to this controller's log file, stderr and the UI sink. */
void ctm_ctl_log(ctm_controller_t *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Send one framed CTMB message over this controller's transport. 0 = sent. */
int ctm_ctl_send(ctm_controller_t *c, uint16_t type, uint32_t flags,
                 uint32_t request_id, const void *payload, size_t len);

/* Count one input report forwarded to the host, and refresh the input
 * watchdog's liveness ticket. For readers that are NOT the primary input
 * thread (the composite siblings, the xpad feeder): the primary thread owns
 * last_rx_us and updates it directly. */
void ctm_ctl_note_input_report(ctm_controller_t *c);

/* Count one output report accepted by the device (or its injector). */
void ctm_ctl_note_output_report(ctm_controller_t *c);

/* True while this session's non-primary readers (composite siblings, xpad
 * feeder) should keep looping: the session has started them and neither
 * plug-out nor a teardown has been requested yet. */
bool ctm_ctl_readers_run(ctm_controller_t *c);

#ifdef __cplusplus
}
#endif

#endif /* CTM_CONTROLLER_PRIV_H */
