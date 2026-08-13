#pragma once

/**
 * The overlay's palette, shared by the command bar and the HID passthrough sheet.
 *
 * Two rules hold everywhere both surfaces draw:
 *
 *   - Colour says WHAT a thing is. A rail on the leading edge of a slab carries
 *     it: the colour of the remote key that triggers a command, or the link
 *     state of a device. A rail that is OVERLAY_SEAM means "no colour applies".
 *   - White says WHERE YOU ARE. The focused slab, and only the focused slab,
 *     lifts to OVERLAY_SLAB_HI behind an OVERLAY_CHALK border with a soft bloom.
 *
 * Keeping the two apart is what makes the overlay readable from the couch over
 * an arbitrary game frame: a blue focus ring disappears into a blue scene, and a
 * tinted one collides with whatever the tint already means here.
 */

/* Surfaces, dark to light. INK is the sheet, GRAPHITE its header and footer,
 * SLAB every actionable row, SLAB_HI that row once the cursor is on it. */
#define OVERLAY_INK        0x04070A
#define OVERLAY_GRAPHITE   0x10161C
#define OVERLAY_SLAB       0x0B1117
/** The device the settings column is showing, while the cursor is elsewhere. */
#define OVERLAY_SLAB_SEL   0x131C24
#define OVERLAY_SLAB_HI    0x1A242D
#define OVERLAY_SEAM       0x22303A
#define OVERLAY_CHALK      0xE9F1F7

/** A device that is bridged, and the filled part of every slider. */
#define OVERLAY_LIVE       0x17D9B4
/** Errors, and the one command that ends the game. */
#define OVERLAY_ALERT      0xFF4A5B

/* The LG remote's four colour keys, in their own colours. The command bar wears
 * them as rails, so the shortcut needs no second label to explain it. */
#define OVERLAY_KEY_RED    0xFF4A5B
#define OVERLAY_KEY_GREEN  0x38D96A
#define OVERLAY_KEY_YELLOW 0xFFC02E
#define OVERLAY_KEY_BLUE   0x3A9CFF

/* Text weights. The family is whatever the platform has (Museo Sans on webOS),
 * so hierarchy is carried by size, case, tracking and these opacities. */
#define OVERLAY_OPA_TEXT   LV_OPA_COVER
#define OVERLAY_OPA_MUTED  ((lv_opa_t) 140)
#define OVERLAY_OPA_FAINT  ((lv_opa_t) 105)

/* Surface weights. Nothing over the game is fully opaque: the stream stays
 * faintly readable through the sheet, which is what keeps a panel this size from
 * feeling like the app has been replaced by a menu. They stack, so a slab on the
 * sheet still ends up at about 96 % — enough for text over a bright desktop.
 * The focused slab is the most solid thing on screen, which is half of why it
 * reads as focused. */
#define OVERLAY_OPA_VEIL   ((lv_opa_t) 140)
#define OVERLAY_OPA_SHEET  ((lv_opa_t) 218)
#define OVERLAY_OPA_SLAB   ((lv_opa_t) 190)
#define OVERLAY_OPA_SLAB_FOCUS ((lv_opa_t) 238)
/* Header and footer are a wash of light over the sheet rather than a lighter
 * solid, so the whole sheet keeps one transparency. */
#define OVERLAY_OPA_BAR    ((lv_opa_t) 14)

/* Geometry, in DPX so it scales with the panel size the display asks for. */
#define OVERLAY_RAIL_W     LV_DPX(4)
#define OVERLAY_RADIUS     LV_DPX(6)
#define OVERLAY_SLAB_PAD   LV_DPX(17)
