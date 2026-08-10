#ifndef CTM_PAD_TABLE_H
#define CTM_PAD_TABLE_H

/* The controllers this bridge knows, as data.
 *
 * Everything the app decides purely from a device's VID:PID -- what kind of pad
 * it is, what to call it in the UI, whether the xpad driver claims it, whether
 * it is a composite dongle, whether its rumble path works, whether its Bluetooth
 * audio sink has to be blocked, and whether a HID interface from that vendor may
 * be trusted when it declares no usage -- is one row here. It used to be six
 * predicates and an allow-list spread over ui_devices.c and ui_bridge.c, each
 * free to disagree with the others about the same pad.
 *
 * What is NOT here, on purpose: every rule that is not a VID:PID fact. The
 * name-sniffing branches (a Microsoft device whose product string says "xbox",
 * Gulikit, the Flydigi identity heuristics over sysfs manufacturer/product
 * strings and USB bus ids) stay where they are, because a table row cannot state
 * them and pretending otherwise would hide them.
 *
 * Header-only and self-contained so a host build can include it and check the
 * rows without dragging in webOS-only enumeration code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
    CTM_PAD_KIND_UNKNOWN = 0,
    CTM_PAD_KIND_DS5,
    CTM_PAD_KIND_DS4,
    CTM_PAD_KIND_XBOX,
    CTM_PAD_KIND_PUCK,
    CTM_PAD_KIND_FLYDIGI,
    /* Claimed by the xpad driver, but not one of the named families above. */
    CTM_PAD_KIND_XINPUT,
} ctm_pad_kind_t;

/* The kernel's xpad driver binds this product, so it appears as an evdev/js
 * node and may have no hidraw node at all. */
#define CTM_PAD_XPAD_COMPATIBLE     (1u << 0)
/* A composite USB device whose gamepad is one interface among several; the
 * bridge forwards the whole enumeration rather than a single hidraw node. */
#define CTM_PAD_COMPOSITE           (1u << 1)
/* Bridging this pad must stop the TV from grabbing its Bluetooth audio sink,
 * or the headset jack goes to the TV instead of through the bridge. */
#define CTM_PAD_BLOCK_BT_AUDIO_SINK (1u << 2)
/* This pad's rumble path is not usable through the bridge, so its haptics gain
 * starts at 0 instead of the 100% every other pad gets. Absence of the flag
 * means "the normal default"; it can only ever turn haptics OFF, never on, so a
 * device with no row here keeps the default. */
#define CTM_PAD_NO_HAPTICS          (1u << 3)

/* A row with this pid matches every product of its vendor. Such rows exist only
 * to answer ctm_pad_vendor_makes_gamepads(); they carry no kind and no name. A
 * real product id of 0x0000 would collide with the wildcard, and no pad in this
 * table uses one. */
#define CTM_PAD_ANY_PID 0x0000u

/* The one vendor id that also appears outside a table lookup: two predicates
 * recognise an unlisted Microsoft pad by its product string. */
#define CTM_PAD_VENDOR_MICROSOFT 0x045eu

typedef struct {
    uint16_t vid;
    uint16_t pid;
    ctm_pad_kind_t kind;
    const char *display_name; /* NULL for wildcard rows */
    unsigned flags;
} ctm_pad_desc_t;

/* Ordered: exact rows before their vendor's wildcard row, because
 * ctm_pad_desc_find() returns the first hit. */
static const ctm_pad_desc_t ctm_pad_table[] = {
    /* Sony */
    {0x054c, 0x0ce6, CTM_PAD_KIND_DS5, "Sony DS5 Controller", CTM_PAD_BLOCK_BT_AUDIO_SINK},
    {0x054c, 0x09cc, CTM_PAD_KIND_DS4, "Sony DS4 Controller", CTM_PAD_NO_HAPTICS},
    {0x054c, 0x05c4, CTM_PAD_KIND_DS4, "Sony DS4 Controller", CTM_PAD_NO_HAPTICS},

    /* Microsoft, the products the app calls an Xbox controller by id alone. */
    {0x045e, 0x02d1, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x02dd, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x02e0, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x02e3, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x02ea, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x02fd, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0b00, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0b05, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0b0a, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0b12, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0b13, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0b20, CTM_PAD_KIND_XBOX, "Microsoft Xbox Controller", CTM_PAD_XPAD_COMPATIBLE},
    /* Microsoft, xpad-compatible but NOT named an Xbox controller by id: the
     * wired 360 pad and its receivers. is_xbox_device() only calls these Xbox
     * when the product string says so. */
    {0x045e, 0x028e, CTM_PAD_KIND_XINPUT, "XInput-compatible Gamepad", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x028f, CTM_PAD_KIND_XINPUT, "XInput-compatible Gamepad", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0291, CTM_PAD_KIND_XINPUT, "XInput-compatible Gamepad", CTM_PAD_XPAD_COMPATIBLE},
    {0x045e, 0x0719, CTM_PAD_KIND_XINPUT, "XInput-compatible Gamepad", CTM_PAD_XPAD_COMPATIBLE},
    /* Any other Microsoft product still counts as a gamepad vendor. */
    {0x045e, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0},

    /* Valve */
    {0x28de, 0x1304, CTM_PAD_KIND_PUCK, "Valve Software Steam Controller Puck", 0},

    /* Cypress: the USB bridge chip inside the Flydigi dongles. */
    {0x04b4, 0x2412, CTM_PAD_KIND_FLYDIGI, "Flydigi Controller", CTM_PAD_COMPOSITE},

    /* Vendors whose pads are not modelled individually, listed only so a HID
     * interface of theirs that declares no usage page is still treated as a
     * gamepad candidate. */
    {0x057e, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* Nintendo */
    {0x3537, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0},
    {0x2dc8, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* 8BitDo */
    {0x0e6f, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* PDP */
    {0x1532, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* Razer */
    {0x0738, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* Mad Catz */
    {0x046d, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* Logitech */
    {0x20d6, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* PowerA */
    {0x24c6, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* PowerA/ThrustMaster */
    {0x2563, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* ShanWan */
    {0x0079, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* DragonRise */
    {0x0810, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0},
    {0x1038, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* SteelSeries */
    {0x146b, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* BigBen */
    {0x1949, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* Amazon */
    {0x1bad, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0}, /* Harmonix */
    {0x2378, CTM_PAD_ANY_PID, CTM_PAD_KIND_UNKNOWN, NULL, 0},
};

#define CTM_PAD_TABLE_COUNT ((int) (sizeof(ctm_pad_table) / sizeof(ctm_pad_table[0])))

/* Parse one of the "%04x" id strings the enumeration stores. Returns 0 for
 * anything that is not a number, and 0 matches no row. */
static inline uint16_t ctm_pad_hex16(const char *s)
{
    if (!s || !s[0]) {
        return 0;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || v > 0xffffu) {
        return 0;
    }
    return (uint16_t) v;
}

/* The row for this product: the exact one if there is one, else its vendor's
 * wildcard row, else NULL. */
static inline const ctm_pad_desc_t *ctm_pad_desc_find(uint16_t vid, uint16_t pid)
{
    if (vid == 0) {
        return NULL;
    }
    const ctm_pad_desc_t *wildcard = NULL;
    for (int i = 0; i < CTM_PAD_TABLE_COUNT; ++i) {
        const ctm_pad_desc_t *row = &ctm_pad_table[i];
        if (row->vid != vid) {
            continue;
        }
        if (row->pid == pid) {
            return row;
        }
        if (row->pid == CTM_PAD_ANY_PID && !wildcard) {
            wildcard = row;
        }
    }
    return wildcard;
}

static inline const ctm_pad_desc_t *ctm_pad_desc_find_str(const char *vid, const char *pid)
{
    return ctm_pad_desc_find(ctm_pad_hex16(vid), ctm_pad_hex16(pid));
}

static inline ctm_pad_kind_t ctm_pad_kind_str(const char *vid, const char *pid)
{
    const ctm_pad_desc_t *row = ctm_pad_desc_find_str(vid, pid);
    return row ? row->kind : CTM_PAD_KIND_UNKNOWN;
}

/* The table's display name for a row, or `fallback` when the row has none (a
 * wildcard row) or there is no row. Naming branches go through this so a future
 * row without a name degrades to a generic label instead of a NULL pointer. */
static inline const char *ctm_pad_display_name(const ctm_pad_desc_t *pad, const char *fallback)
{
    return (pad && pad->display_name) ? pad->display_name : fallback;
}

static inline bool ctm_pad_has_flag_str(const char *vid, const char *pid, unsigned flag)
{
    const ctm_pad_desc_t *row = ctm_pad_desc_find_str(vid, pid);
    return row && (row->flags & flag) != 0;
}

/* Does this vendor ship gamepads? Used only where a HID interface declares no
 * usage page at all and the vendor is the last evidence available. */
static inline bool ctm_pad_vendor_makes_gamepads(uint16_t vid)
{
    if (vid == 0) {
        return false;
    }
    for (int i = 0; i < CTM_PAD_TABLE_COUNT; ++i) {
        if (ctm_pad_table[i].vid == vid) {
            return true;
        }
    }
    return false;
}

/* True for a product id this table calls an Xbox controller. Vendor-blind on
 * purpose: the one caller that needs it already knows the vendor is Microsoft,
 * and every CTM_PAD_KIND_XBOX row is a Microsoft row. */
static inline bool ctm_pad_pid_is_xbox(const char *pid)
{
    uint16_t p = ctm_pad_hex16(pid);
    if (p == CTM_PAD_ANY_PID) {
        return false;
    }
    for (int i = 0; i < CTM_PAD_TABLE_COUNT; ++i) {
        if (ctm_pad_table[i].pid == p && ctm_pad_table[i].kind == CTM_PAD_KIND_XBOX) {
            return true;
        }
    }
    return false;
}

#endif /* CTM_PAD_TABLE_H */
