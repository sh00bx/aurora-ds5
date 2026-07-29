#pragma once

#include <lvgl.h>

#include "client.h"
#include "stream/session.h"
#include "controller_info.h"

typedef struct app_t app_t;

/* Latency-chain palette: blue for the milliseconds spent away from the TV (the
 * network, then the host), pale grey for the ones the TV adds itself. The legend
 * tints each number with the same value, so the bar needs no separate key. */
#define STREAMING_CHAIN_COLOR_NET    0x64B5F6
#define STREAMING_CHAIN_COLOR_HOST   0x2196F3
#define STREAMING_CHAIN_COLOR_DECODE 0xC9CDD2
#define STREAMING_CHAIN_COLOR_SUBMIT 0x9E9E9E

typedef struct {
    lv_fragment_t base;
    app_t *global;
    lv_obj_t *detached_root;
    lv_obj_t *hint;
    lv_obj_t *overlay;
    lv_group_t *group;
    lv_obj_t *progress;
    lv_obj_t *video;
    lv_obj_t *actions;
    lv_obj_t *kbd_btn, *vmouse_btn;
#if defined(TARGET_WEBOS)
    lv_obj_t *hid_devices_btn;
    lv_obj_t *hid_panel;
#endif
    lv_obj_t *suspend_btn, *quit_btn;
    lv_obj_t *stats;
    /* Detailed stats panel. All NULL in compact mode. The latency chain segments
     * follow the order the milliseconds happen in: network, host, decode, submit. */
    struct {
        /* Density/contrast are switched at runtime between the two ways the panel is
         * used, so the pieces that carry either are kept at hand. `dim` holds every
         * label that renders below full opacity, with the value it uses unpinned. */
        lv_obj_t *title;
        lv_obj_t *sections[3];
        lv_obj_t *chain_row;
        lv_obj_t *throughput_row;
        lv_obj_t *throughput_cols[2];
        lv_obj_t *pads_block;
        struct {
            lv_obj_t *obj;
            lv_opa_t base_opa;
        } dim[16];
        uint8_t dim_count;
        lv_obj_t *stream;
        lv_obj_t *audio;
        lv_obj_t *latency_total;
        lv_obj_t *chain_bar;
        lv_obj_t *chain[4];
        lv_obj_t *chain_legend;
        lv_obj_t *net_fps;
        lv_obj_t *render_fps;
        lv_obj_t *drop_rate;
        lv_obj_t *bitrate;
        struct {
            lv_obj_t *row;
            lv_obj_t *name;
            lv_obj_t *badge;
            lv_obj_t *power;
        } pads[CONTROLLER_INFO_MAX];
        lv_obj_t *pads_empty;
    } stats_items;
    lv_obj_t *stats_compact_label;  /* Single-line stats when show_stats_compact */
    lv_obj_t *stats_quality_indicator;  /* Colored dot: green/yellow/red by latency */
    lv_obj_t *stats_pin;
    lv_obj_t *notice, *notice_label;
    lv_obj_t *soft_kbd;
    lv_style_t overlay_button_style;
    lv_style_t overlay_button_style_focused;
    lv_style_t overlay_button_label_style;
    lv_point_t button_points[6];
    /* Network speed test mode */
    bool network_test;
    uint8_t network_test_duration; /* seconds */
    lv_timer_t *network_test_timer;
} streaming_controller_t;

/* Usually references to SERVER_DATA and APP_LIST should not be kept, but in this struct, they will only be used once */
typedef struct {
    app_t *global;
    uuidstr_t uuid;
    APP_LIST app;
    bool network_test;
    uint8_t network_test_duration; /* seconds, 0 = default */
} streaming_scene_arg_t;

extern const lv_fragment_class_t streaming_controller_class;

lv_obj_t *streaming_scene_create(lv_fragment_t *self, lv_obj_t *parent);

void streaming_styles_init(streaming_controller_t *controller);

void streaming_styles_reset(streaming_controller_t *controller);

void streaming_overlay_resized(streaming_controller_t *controller);

/** Pinned, the panel rides along over the game: tighter, lighter on the background,
 * and with the muted text pulled up so it survives the thinner backdrop. */
void streaming_stats_set_pinned_look(streaming_controller_t *controller, bool pinned);

bool streaming_overlay_shown();

bool streaming_soft_keyboard_shown();

#if defined(TARGET_WEBOS)
bool streaming_hid_panel_shown();
#endif

bool streaming_stats_shown();

bool streaming_refresh_stats();

void streaming_toggle_stats_pin(void);

void streaming_notice_show(const char *message);