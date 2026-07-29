#include "streaming.controller.h"
#include "app.h"

#include "util/i18n.h"
#include "util/font.h"
#include "hints.h"

#include <ctype.h>
#include <string.h>

#include "lvgl/ext/lv_child_group.h"
#include "lvgl/theme/lv_theme_moonlight.h"

static lv_obj_t *stat_label(streaming_controller_t *controller, lv_obj_t *parent, const char *title,
                            lv_coord_t pad_hor);

static lv_obj_t *overlay_title(lv_obj_t *parent, const char *title, streaming_controller_t *controller);

static lv_obj_t *panel_row(lv_obj_t *parent);

static lv_obj_t *panel_text(streaming_controller_t *controller, lv_obj_t *parent, lv_opa_t opa);

static lv_obj_t *section_header(streaming_controller_t *controller, lv_obj_t *parent, const char *title,
                                lv_obj_t **value_out);

static void latency_chain(lv_obj_t *parent, streaming_controller_t *controller);

static void pad_rows(lv_obj_t *parent, streaming_controller_t *controller);

static void throughput_columns(streaming_controller_t *controller, lv_obj_t *parent);

lv_obj_t *streaming_scene_create(lv_fragment_t *self, lv_obj_t *parent) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    controller->detached_root = obj;

    lv_obj_t *hint = lv_label_create(obj);
    lv_obj_set_style_pad_all(hint, LV_DPX(20), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text_fmt(hint, locstr("Hint: %s"), hints_obtain());
    controller->hint = hint;

    lv_obj_t *overlay = lv_obj_create(obj);
    lv_obj_remove_style_all(overlay);
    controller->overlay = overlay;

    controller->group = lv_group_create();
    lv_obj_add_event_cb(overlay, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->group);

    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));

    int stats_w = app_configuration->show_stats_compact ? 0 : LV_DPX(384);
    lv_disp_t *disp = lv_disp_get_default();
    int video_w = lv_disp_get_hor_res(disp) - LV_DPX(20) * 2 - (stats_w ? LV_DPX(30) + stats_w : 0);
    int video_h_pct = video_w * 100 / lv_disp_get_hor_res(disp);

    lv_obj_t *video = lv_obj_create(overlay);
    lv_obj_remove_style_all(video);
    lv_obj_set_size(video, video_w, LV_PCT(video_h_pct));
    int video_top = app_configuration->show_stats_compact ? LV_DPX(36) : LV_DPX(20);
    lv_obj_align(video, LV_ALIGN_TOP_LEFT, LV_DPX(20), video_top);
    lv_obj_clear_flag(video, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bottom_stack = lv_obj_create(overlay);
    lv_obj_remove_style_all(bottom_stack);
    lv_obj_set_width(bottom_stack, LV_PCT(100));
    lv_obj_set_height(bottom_stack, LV_DPX(260));
    lv_obj_align(bottom_stack, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_layout(bottom_stack, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bottom_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bottom_stack, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bottom_stack, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bottom_stack, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *actions = lv_obj_create(bottom_stack);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_DPX(200));
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(actions, LV_DPX(15), 0);
    lv_obj_set_style_pad_all(actions, LV_DPX(20), 0);
    static const lv_grad_dsc_t actions_grad = {
            .dir = LV_GRAD_DIR_VER,
            .stops = {
                    {.color = {.ch ={0, 0, 0, 0}}, .frac = 0},
                    {.color = {.ch ={0, 0, 0, 255}}, .frac = 255},
            },
            .stops_count = 2
    };
    lv_obj_set_style_bg_grad(actions, &actions_grad, 0);
    // We need a non-opaque opacity to properly render the elements
    lv_obj_set_style_bg_opa(actions, LV_OPA_COVER, 0);
    lv_obj_add_flag(actions, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(actions, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->group);

    lv_obj_t *kbd_btn = lv_btn_create(actions);
    lv_obj_add_flag(kbd_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(kbd_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(kbd_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(kbd_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t *kbd_label = lv_label_create(kbd_btn);
    lv_obj_add_style(kbd_label, &controller->overlay_button_label_style, 0);
    lv_label_set_text(kbd_label, locstr("Full keyboard"));

    lv_obj_t *vmouse_btn = lv_btn_create(actions);
    lv_obj_add_flag(vmouse_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(vmouse_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(vmouse_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(vmouse_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_t *vmouse_label = lv_label_create(vmouse_btn);
    lv_obj_add_style(vmouse_label, &controller->overlay_button_label_style, 0);
    lv_label_set_text(vmouse_label, locstr("Virtual Mouse"));

#if defined(TARGET_WEBOS)
    if (app_configuration->hid_passthrough) {
        lv_obj_t *hid_btn = lv_btn_create(actions);
        lv_obj_add_flag(hid_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_style(hid_btn, &controller->overlay_button_style, 0);
        lv_obj_add_style(hid_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_color(hid_btn, lv_palette_main(LV_PALETTE_PURPLE), 0);
        lv_obj_t *hid_label = lv_label_create(hid_btn);
        lv_obj_add_style(hid_label, &controller->overlay_button_label_style, 0);
        lv_label_set_text(hid_label, locstr("HID Devices"));
        controller->hid_devices_btn = hid_btn;
    }
#endif

    lv_obj_t *actions_spacing = lv_obj_create(actions);
    lv_obj_remove_style_all(actions_spacing);
    lv_obj_set_flex_grow(actions_spacing, 1);

    lv_obj_t *suspend_btn = lv_btn_create(actions);
    lv_obj_add_flag(suspend_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(suspend_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(suspend_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(suspend_btn, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_t *suspend_lbl = lv_label_create(suspend_btn);
    lv_obj_add_style(suspend_lbl, &controller->overlay_button_label_style, 0);
    lv_label_set_text(suspend_lbl, locstr("Disconnect"));

    lv_obj_t *exit_btn = lv_btn_create(actions);
    lv_obj_add_flag(exit_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(exit_btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(exit_btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(exit_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_obj_add_style(exit_lbl, &controller->overlay_button_label_style, 0);
    lv_label_set_text(exit_lbl, locstr("Quit game"));

    lv_obj_t *stats = lv_obj_create(overlay);
    lv_obj_remove_style_all(stats);
    lv_obj_set_style_text_color(stats, lv_color_white(), 0);
    lv_obj_set_style_pad_gap(stats, LV_DPX(5), 0);
    lv_obj_set_style_bg_color(stats, lv_color_black(), 0);
    /* Denser than the compact bar: this panel is read, not glanced at, and a bright
     * desktop or HDR scene behind it swallows anything lighter. Pinned (USER_1) it
     * stays up during play, so it gives some of that weight back. */
    lv_obj_set_style_bg_opa(stats, app_configuration->show_stats_compact ? LV_OPA_40 : LV_OPA_80, 0);
    lv_obj_set_style_bg_opa(stats, app_configuration->show_stats_compact ? LV_OPA_30 : LV_OPA_40,
                            LV_STATE_USER_1);
    lv_obj_set_user_data(stats, controller);

    if (app_configuration->show_stats_compact) {
        /* Artemis lite mode: slim horizontal bar at top, full width, single line */
        lv_obj_set_size(stats, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(stats, LV_DPX(12), 0);
        lv_obj_set_style_pad_ver(stats, LV_DPX(6), 0);
        lv_obj_set_style_pad_bottom(stats, LV_DPX(6), 0);
        lv_obj_align(stats, LV_ALIGN_TOP_LEFT, 0, 0);

        /* Quality indicator as colored label (●): green/yellow/red by latency */
        lv_obj_t *quality_dot = lv_label_create(stats);
        lv_label_set_text(quality_dot, "\u25CF");  /* ● U+25CF BLACK CIRCLE */
        lv_obj_set_style_text_color(quality_dot, lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_obj_set_style_text_font(quality_dot, lv_theme_get_font_small(stats), 0);
        controller->stats_quality_indicator = quality_dot;

        controller->stats_compact_label = lv_label_create(stats);
        lv_label_set_text(controller->stats_compact_label, "-");
        lv_obj_set_style_text_font(controller->stats_compact_label, lv_theme_get_font_small(stats), 0);
        lv_obj_set_flex_grow(controller->stats_compact_label, 1);

        /* Pin button inline, minimal */
        lv_obj_t *stats_pin = lv_btn_create(stats);
        lv_group_remove_obj(stats_pin);
        lv_obj_add_flag(stats_pin, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_opa(stats_pin, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(stats_pin, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(stats_pin, LV_DPX(4), 0);
        lv_obj_set_style_radius(stats_pin, LV_DPX(4), 0);
        lv_obj_set_style_text_opa(stats_pin, LV_OPA_50, 0);
        lv_obj_set_style_text_opa(stats_pin, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_ext_click_area(stats_pin, LV_DPX(5));
        lv_obj_t *stat_pin_content = lv_img_create(stats_pin);
        lv_obj_set_style_text_font(stat_pin_content, lv_theme_moonlight_get_iconfont_small(stats_pin), 0);
        lv_img_set_src(stat_pin_content, MAT_SYMBOL_PUSH_PIN);
        controller->stats_pin = stats_pin;

        memset(&controller->stats_items, 0, sizeof(controller->stats_items));
    } else {
        lv_obj_set_size(stats, LV_DPX(384), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(stats, LV_DPX(14), 0);
        lv_obj_set_style_radius(stats, LV_DPX(8), 0);
        lv_obj_set_style_clip_corner(stats, true, 0);
        lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
        controller->stats_compact_label = NULL;
        controller->stats_quality_indicator = NULL;
        memset(&controller->stats_items, 0, sizeof(controller->stats_items));
        controller->stats_items.title = overlay_title(stats, locstr("Performance"), controller);

        /* Stream identity. Resolution, codec and decoder name say what they are on
         * their own, so they get no label column — unlike the rows further down,
         * which are numbers that need naming. */
        lv_obj_t *stream_row = panel_row(stats);
        controller->stats_items.stream = panel_text(controller, stream_row, LV_OPA_COVER);
        lv_obj_t *audio_row = panel_row(stats);
        controller->stats_items.audio = panel_text(controller, audio_row, LV_OPA_70);

        controller->stats_items.sections[0] =
                section_header(controller, stats, "Latency", &controller->stats_items.latency_total);
        latency_chain(stats, controller);

        controller->stats_items.sections[1] = section_header(controller, stats, "Throughput", NULL);
        throughput_columns(controller, stats);

        controller->stats_items.sections[2] = section_header(controller, stats, "Controllers", NULL);
        pad_rows(stats, controller);
    }


    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    controller->video = video;
    controller->actions = actions;
    controller->kbd_btn = kbd_btn;
    controller->vmouse_btn = vmouse_btn;
    controller->quit_btn = exit_btn;
    controller->suspend_btn = suspend_btn;
    controller->stats = stats;

    streaming_overlay_resized(controller);

    // We return overlay instead of obj, and will delete the obj manually
    return overlay;
}

void streaming_styles_init(streaming_controller_t *controller) {
    lv_theme_t *theme = lv_disp_get_default()->theme;

    lv_style_init(&controller->overlay_button_style);
    lv_style_set_shadow_ofs_y(&controller->overlay_button_style, LV_DPX(3));
    lv_style_set_shadow_width(&controller->overlay_button_style, LV_DPX(4));
    lv_style_set_shadow_color(&controller->overlay_button_style, lv_color_black());
    lv_style_set_shadow_opa(&controller->overlay_button_style, LV_OPA_30);
    lv_style_set_radius(&controller->overlay_button_style, LV_DPX(8));
    lv_style_set_pad_hor(&controller->overlay_button_style, LV_DPX(15));
    lv_style_set_pad_ver(&controller->overlay_button_style, LV_DPX(10));
    lv_style_init(&controller->overlay_button_style_focused);
    lv_style_set_outline_color(&controller->overlay_button_style_focused, lv_palette_lighten(LV_PALETTE_BLUE, 3));

    lv_style_init(&controller->overlay_button_label_style);
    lv_style_set_text_font(&controller->overlay_button_label_style, theme->font_small);
}

void streaming_styles_reset(streaming_controller_t *controller) {
    lv_style_reset(&controller->overlay_button_style);
    lv_style_reset(&controller->overlay_button_style_focused);
    lv_style_reset(&controller->overlay_button_label_style);
}

void streaming_stats_set_pinned_look(streaming_controller_t *controller, bool pinned) {
    /* Compact mode builds none of this, and the panel is already a one-liner there. */
    if (!controller->stats || controller->stats_items.title == NULL) {
        return;
    }

    /* Pinned it rides along over the game: less air between the rows, and less of
     * the background — which the muted text pays for, so it is pulled up to match. */
    lv_coord_t row_gap = pinned ? LV_DPX(2) : LV_DPX(5);
    lv_coord_t bottom = pinned ? LV_DPX(8) : LV_DPX(14);
    lv_coord_t title_pad = pinned ? LV_DPX(5) : LV_DPX(10);
    lv_coord_t section_pad = pinned ? LV_DPX(6) : LV_DPX(12);
    lv_coord_t chain_pad = pinned ? LV_DPX(3) : LV_DPX(6);
    lv_opa_t lift = pinned ? 45 : 0;

    lv_obj_set_style_pad_gap(controller->stats, row_gap, 0);
    lv_obj_set_style_pad_bottom(controller->stats, bottom, 0);
    lv_obj_set_style_pad_ver(controller->stats_items.title, title_pad, 0);
    /* Pinned, the panel needs no nameplate — you put it there. The title carries the
     * pin button, though, so it comes back with the overlay, where it can be clicked. */
    if (pinned) {
        lv_obj_add_flag(controller->stats_items.title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(controller->stats_items.title, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; i++) {
        if (controller->stats_items.throughput_cols[i]) {
            lv_obj_set_style_pad_gap(controller->stats_items.throughput_cols[i], row_gap, 0);
        }
    }
    if (controller->stats_items.pads_block) {
        lv_obj_set_style_pad_gap(controller->stats_items.pads_block, row_gap, 0);
    }

    /* Without the title the first line would otherwise sit on the panel edge. */
    lv_obj_set_style_pad_top(controller->stats, pinned ? LV_DPX(10) : 0, 0);

    /* "Throughput" and "Controllers" name a grouping the eye already sees, and the
     * rows under them carry their own labels. Pinned they are the least legible
     * thing on screen over game content, so they go and hand their spacing to the
     * group below. "Latency" stays — it carries the total on its right. */
    lv_obj_t *labelled[2] = {controller->stats_items.throughput_row, controller->stats_items.pads_block};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *heading = controller->stats_items.sections[i + 1];
        if (heading) {
            if (pinned) {
                lv_obj_add_flag(heading, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(heading, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (labelled[i]) {
            lv_obj_set_style_pad_top(labelled[i], pinned ? section_pad : 0, 0);
        }
    }
    for (int i = 0; i < 3; i++) {
        if (controller->stats_items.sections[i]) {
            lv_obj_set_style_pad_top(controller->stats_items.sections[i], section_pad, 0);
        }
    }
    if (controller->stats_items.chain_row) {
        lv_obj_set_style_pad_top(controller->stats_items.chain_row, chain_pad, 0);
    }
    for (uint8_t i = 0; i < controller->stats_items.dim_count; i++) {
        lv_opa_t base = controller->stats_items.dim[i].base_opa;
        lv_opa_t opa = (lv_opa_t) (base + lift > LV_OPA_COVER ? LV_OPA_COVER : base + lift);
        lv_obj_set_style_text_opa(controller->stats_items.dim[i].obj, opa, 0);
    }
}

void streaming_overlay_resized(streaming_controller_t *controller) {
    lv_obj_update_layout(controller->actions);
    lv_obj_update_layout(controller->overlay);
}

/* Remember a label that renders muted, so the pinned look can pull it back up. */
static void register_dim(streaming_controller_t *controller, lv_obj_t *label, lv_opa_t opa) {
    if (controller->stats_items.dim_count >= (uint8_t) (sizeof(controller->stats_items.dim) /
                                                        sizeof(controller->stats_items.dim[0]))) {
        return;
    }
    controller->stats_items.dim[controller->stats_items.dim_count].obj = label;
    controller->stats_items.dim[controller->stats_items.dim_count].base_opa = opa;
    controller->stats_items.dim_count++;
}

static lv_obj_t *stat_label(streaming_controller_t *controller, lv_obj_t *parent, const char *title,
                            lv_coord_t pad_hor) {
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(container, pad_hor, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(container, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_t *label = lv_label_create(container);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(container), 0);
    /* The name recedes, the number carries — otherwise ten equal-weight rows read
     * as one grey block from the couch. Not fainter than this: whatever the game
     * is drawing shows through the panel. */
    lv_obj_set_style_text_opa(label, LV_OPA_70, 0);
    register_dim(controller, label, LV_OPA_70);
    lv_obj_t *value = lv_label_create(container);
    lv_obj_set_style_text_font(value, lv_theme_get_font_small(container), 0);
    return value;
}

/* Every row shares one side padding, so labels and values line up down the panel
 * no matter what a given row is built from. */
static lv_obj_t *panel_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, LV_DPX(15), 0);
    lv_obj_set_style_pad_gap(row, LV_DPX(8), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *panel_text(streaming_controller_t *controller, lv_obj_t *parent, lv_opa_t opa) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(parent), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_label_set_text(label, "-");
    if (opa < LV_OPA_COVER) {
        register_dim(controller, label, opa);
    }
    return label;
}

/* Section eyebrow: caps and wide tracking make it read as a heading without a
 * second type size, which the theme does not carry at this scale anyway. The
 * optional value sits on the right and belongs to the section, not to a row. */
static lv_obj_t *section_header(streaming_controller_t *controller, lv_obj_t *parent, const char *title,
                                lv_obj_t **value_out) {
    lv_obj_t *row = panel_row(parent);
    lv_obj_set_style_pad_top(row, LV_DPX(12), 0);

    lv_obj_t *label = panel_text(controller, row, LV_OPA_60);
    lv_obj_set_style_text_letter_space(label, LV_DPX(2), 0);
    char caps[24];
    size_t i = 0;
    for (; title[i] != '\0' && i + 1 < sizeof(caps); i++) {
        caps[i] = (char) toupper((unsigned char) title[i]);
    }
    caps[i] = '\0';
    lv_label_set_text(label, caps);
    lv_obj_set_flex_grow(label, 1);

    if (value_out != NULL) {
        *value_out = panel_text(controller, row, LV_OPA_COVER);
    }
    return row;
}

/* The latency chain: one bar split into the four stages a frame passes through, in
 * the order they happen. Blue is time spent away from the TV — the wire, then the
 * host — pale is what the TV itself adds. */
static void latency_chain(lv_obj_t *parent, streaming_controller_t *controller) {
    static const uint32_t segment_colors[4] = {
            STREAMING_CHAIN_COLOR_NET,
            STREAMING_CHAIN_COLOR_HOST,
            STREAMING_CHAIN_COLOR_DECODE,
            STREAMING_CHAIN_COLOR_SUBMIT,
    };

    lv_obj_t *row = panel_row(parent);
    lv_obj_set_style_pad_top(row, LV_DPX(6), 0);
    controller->stats_items.chain_row = row;

    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_remove_style_all(bar);
    lv_obj_set_height(bar, LV_DPX(8));
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_style_radius(bar, LV_DPX(4), 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    controller->stats_items.chain_bar = bar;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *segment = lv_obj_create(bar);
        lv_obj_remove_style_all(segment);
        /* Widths are computed from the measured milliseconds on every refresh, not
         * left to flex_grow: LVGL divides the free space by an integer grow unit,
         * which flattens small shares and hands the leftover to the last item. */
        lv_obj_set_width(segment, 0);
        lv_obj_set_height(segment, LV_PCT(100));
        lv_obj_set_style_bg_color(segment, lv_color_hex(segment_colors[i]), 0);
        lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
        lv_obj_clear_flag(segment, LV_OBJ_FLAG_SCROLLABLE);
        controller->stats_items.chain[i] = segment;
    }

    lv_obj_t *legend_row = panel_row(parent);
    lv_obj_t *legend = panel_text(controller, legend_row, LV_OPA_90);
    /* Each stage's number is tinted like its segment, so the bar needs no key. */
    lv_label_set_recolor(legend, true);
    controller->stats_items.chain_legend = legend;
}

/* Four numbers in two columns rather than four lines: the frame counters on the
 * left, what the link is doing on the right. Same values, half the height. */
static void throughput_columns(streaming_controller_t *controller, lv_obj_t *parent) {
    lv_obj_t *row = panel_row(parent);
    lv_obj_set_style_pad_gap(row, LV_DPX(16), 0);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_START, 0);
    controller->stats_items.throughput_row = row;

    for (int i = 0; i < 2; i++) {
        lv_obj_t *column = lv_obj_create(row);
        lv_obj_remove_style_all(column);
        lv_obj_set_height(column, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(column, 1);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(column, LV_DPX(5), 0);
        lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);
        controller->stats_items.throughput_cols[i] = column;
    }

    lv_obj_t *left = controller->stats_items.throughput_cols[0];
    lv_obj_t *right = controller->stats_items.throughput_cols[1];
    controller->stats_items.net_fps = stat_label(controller, left, "Received", 0);
    controller->stats_items.render_fps = stat_label(controller, left, "Rendered", 0);
    controller->stats_items.bitrate = stat_label(controller, right, "Bitrate", 0);
    controller->stats_items.drop_rate = stat_label(controller, right, "Frame drop", 0);
}

/* One row per controller: what it is, how it reaches the host, how much charge is
 * left. Rows are built once and hidden until a pad fills them. */
static void pad_rows(lv_obj_t *parent, streaming_controller_t *controller) {
    /* Wrapped in a block of their own: pinned, the section heading is gone and the
     * spacing that separated the group has to live somewhere. */
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(block, LV_DPX(5), 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    controller->stats_items.pads_block = block;

    for (int i = 0; i < CONTROLLER_INFO_MAX; i++) {
        lv_obj_t *row = panel_row(block);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *name = panel_text(controller, row, LV_OPA_COVER);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

        lv_obj_t *badge = panel_text(controller, row, LV_OPA_COVER);
        lv_obj_set_style_text_letter_space(badge, LV_DPX(1), 0);
        lv_obj_set_style_pad_hor(badge, LV_DPX(6), 0);
        lv_obj_set_style_pad_ver(badge, LV_DPX(2), 0);
        lv_obj_set_style_radius(badge, LV_DPX(4), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_40, 0);

        lv_obj_t *power = panel_text(controller, row, LV_OPA_COVER);
        lv_obj_set_width(power, LV_DPX(54));
        lv_obj_set_style_text_align(power, LV_TEXT_ALIGN_RIGHT, 0);

        controller->stats_items.pads[i].row = row;
        controller->stats_items.pads[i].name = name;
        controller->stats_items.pads[i].badge = badge;
        controller->stats_items.pads[i].power = power;
    }

    lv_obj_t *empty_row = panel_row(block);
    lv_obj_t *empty = panel_text(controller, empty_row, LV_OPA_60);
    lv_label_set_text(empty, "Nothing connected");
    controller->stats_items.pads_empty = empty_row;
}

static lv_obj_t *overlay_title(lv_obj_t *parent, const char *title, streaming_controller_t *controller) {
    lv_obj_t *stats_title = lv_label_create(parent);
    lv_label_set_text_static(stats_title, title);
    lv_obj_set_width(stats_title, LV_PCT(100));
    /* No flex_grow: the panel stacks in a column now, so growing would stretch the
     * title along the main axis and a size-content parent collapses it to nothing. */
    lv_obj_set_style_bg_opa(stats_title, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(stats_title, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(stats_title, LV_DPX(15), 0);
    lv_obj_set_style_pad_ver(stats_title, LV_DPX(10), 0);
    lv_obj_t *stats_pin = lv_btn_create(stats_title);
    lv_group_remove_obj(stats_pin);
    lv_obj_add_flag(stats_pin, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(stats_pin, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(stats_pin, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(stats_pin, 0, 0);
    lv_obj_set_style_radius(stats_pin, LV_DPX(4), 0);
    lv_obj_set_style_transform_width(stats_pin, LV_DPX(5), 0);
    lv_obj_set_style_transform_height(stats_pin, LV_DPX(5), 0);
    lv_obj_set_style_text_opa(stats_pin, LV_OPA_50, 0);

    lv_obj_set_style_transform_width(stats_pin, LV_DPX(5), LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(stats_pin, LV_DPX(5), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(stats_pin, LV_OPA_40, LV_STATE_PRESSED);

    lv_obj_set_style_bg_color(stats_pin, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_set_style_text_opa(stats_pin, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(stats_pin, LV_OPA_10, LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(stats_pin, LV_DPX(5));

    lv_obj_t *stat_pin_content = lv_img_create(stats_pin);
    lv_obj_set_style_text_font(stat_pin_content, lv_theme_moonlight_get_iconfont_small(stat_pin_content), 0);
    lv_img_set_src(stat_pin_content, MAT_SYMBOL_PUSH_PIN);

    lv_obj_align(stats_pin, LV_ALIGN_RIGHT_MID, 0, 0);
    controller->stats_pin = stats_pin;
    return stats_title;
}
