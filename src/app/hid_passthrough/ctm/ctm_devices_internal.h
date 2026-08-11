#ifndef CTM_DEVICES_INTERNAL_H
#define CTM_DEVICES_INTERNAL_H

/* Declarations shared between the modules ui_devices.c was split into
 * (ctm_enumerate.c, ctm_classify.c, ctm_dev_composite.c, ctm_model.c).
 *
 * ctm_state.h is what the rest of the app compiles against. What lands here
 * instead is a detail of the device scan that two of those modules need and no
 * one outside it should call — keeping it out of ctm_state.h is the only thing
 * that says so; nothing prevents another translation unit from including this
 * header. */

#include "ctm_state.h"

/* How much this scanned interface looks like the gamepad interface of its
 * device: higher wins, negative means "not a gamepad interface at all".
 * Cross-module because two callers pick with it — the logical model choosing
 * the row that represents a device (best_scan_index_for_item) and the Flydigi
 * hidraw picker choosing which of a composite's nodes to bridge. */
int gamepad_iface_score_for_device(const device_info_t *dev);

#endif /* CTM_DEVICES_INTERNAL_H */
