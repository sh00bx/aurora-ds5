//
// Created by Mariotaku on 2021/08/31.
//

#include "app.h"
#include "appitem.view.h"
#include "apps.controller.h"
#include "launcher.controller.h"
#include "ui/streaming/streaming.controller.h"

#include "coverloader.h"
#include "backend/apploader/apploader.h"
#include <errors.h>
#include <assert.h>

#include "lvgl/lv_ext_utils.h"
#include "lvgl/util/lv_app_utils.h"
#include "lv_gridview.h"

#include "util/user_event.h"
#include "util/i18n.h"
#include "pair.dialog.h"
#include "ui/common/progress_dialog.h"

#include "lvgl/theme/lv_theme_moonlight_colors.h"

#include <stdlib.h>
#include <string.h>

#define APPS_GRID_COLS 7
/* Tile aspect, width:height. The grid sizes tiles from the available *width* and
 * derives the height from this ratio, so a tile fills its column track exactly
 * -- see update_grid_config().
 *
 * 2:3 is what the covers actually are: Playnite/Steam library art is 600x900,
 * and the Switch batch was normalised to the same. (Measured on the live
 * library: 80 of 104 covers are 600x900, the rest 3:4.) The bundled fallback
 * asset is 3:4, which is why this used to say 600x800 -- but sizing tiles 3:4
 * letterboxes every real cover, and that letterboxing reads as yet more gap
 * between the columns, which is the thing we were trying to get rid of. */
#define APPS_COVER_ASPECT_W 2
#define APPS_COVER_ASPECT_H 3

typedef void (*action_cb_t)(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static lv_obj_t *apps_view(lv_fragment_t *self, lv_obj_t *container);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj);

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void host_info_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void send_wol_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void on_host_updated(const uuidstr_t *uuid, void *userdata);

static void on_host_removed(const uuidstr_t *uuid, void *userdata);

static void item_click_cb(lv_event_t *event);

static void item_longpress_cb(lv_event_t *event);

static void launcher_launch_game(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_toggle_fav(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_toggle_hidden(apps_fragment_t *controller, const apploader_item_t *app);

static void launcher_quit_game(apps_fragment_t *controller);

static void applist_focus_enter(lv_event_t *event);

static void applist_focus_leave(lv_event_t *event);

static void gridview_focus_with_key_state(lv_obj_t *grid, int idx);

static void update_view_state(apps_fragment_t *controller);

static void appitem_bind(apps_fragment_t *controller, lv_obj_t *item, apploader_item_t *app);

static int adapter_item_count(lv_obj_t *, void *data);

static lv_obj_t *adapter_create_view(lv_obj_t *parent);

static void adapter_bind_view(lv_obj_t *, lv_obj_t *, void *data, int position);

static void quitgame_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata);

static void apps_controller_ctor(lv_fragment_t *self, void *args);

static void apps_controller_dtor(lv_fragment_t *self);

static void appload_started(void *userdata);

static void appload_loaded(apploader_list_t *apps, void *userdata);

static void appload_errored(int code, const char *error, void *userdata);

static void quit_dialog_cb(lv_event_t *event);

static void actions_click_cb(lv_event_t *event);

static void action_cb_wol(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void action_cb_host_reload(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void action_cb_pair(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index);

static void update_grid_config(apps_fragment_t *controller);

static void open_context_menu(apps_fragment_t *fragment, appitem_viewholder_t *holder);

static void context_menu_cancel_cb(lv_event_t *event);

static void context_menu_click_cb(lv_event_t *event);

static void app_detail_dialog(apps_fragment_t *fragment, const apploader_item_t *app);

static void app_detail_click_cb(lv_event_t *event);

static void set_actions(apps_fragment_t *controller, const char **labels, const action_cb_t *callbacks);

/**
 *
 * @param old_list
 * @param new_list
 * @param num_changes number of changes. It will be assigned to -1 if the whole dataset has been changed.
 * @return Allocated array of changes. It should be freed by caller.
 */
static lv_gridview_data_change_t *apps_list_detect_change(const apploader_list_t *old_list, const int *old_map,
                                                          int old_count, const apploader_list_t *new_list,
                                                          const int *new_map, int new_count, int *num_changes);

static void applist_key_up_to_topbar(lv_event_t *event);

static void show_progress(apps_fragment_t *fragment);

static void show_ok(apps_fragment_t *fragment);

static void show_error(apps_fragment_t *fragment, const char *title, const char *hint, const char *detail);

static bool apps_item_passes_filter(apps_fragment_t *controller, const apploader_item_t *item);

static const char *apps_item_platform(const apploader_item_t *item);

static void apps_rebuild_platforms(apps_fragment_t *controller, const apploader_list_t *list);

static void apps_free_platforms(apps_fragment_t *controller);

static int *apps_build_visible_map(apps_fragment_t *controller, const apploader_list_t *list, int *out_count);

static void apps_apply_visible_map(apps_fragment_t *controller, int *map, int count);

static void apps_publish_platforms(apps_fragment_t *controller);

static apps_fragment_t *current_instance = NULL;

/** Bucket for apps the host reports without a platform (Desktop, Playnite itself, ...). */
static const char *apps_platform_other() {
    return locstr("Other");
}

const static lv_gridview_adapter_t apps_adapter = {
        .item_count = adapter_item_count,
        .create_view = adapter_create_view,
        .bind_view = adapter_bind_view,
};
const static pcmanager_listener_t pc_listeners = {
        .updated = on_host_updated,
        .removed = on_host_removed,
};

const lv_fragment_class_t apps_controller_class = {
        .constructor_cb = apps_controller_ctor,
        .destructor_cb = apps_controller_dtor,
        .create_obj_cb = apps_view,
        .obj_will_delete_cb = obj_will_delete,
        .obj_created_cb = on_view_created,
        .obj_deleted_cb = on_destroy_view,
        .event_cb = on_event,
        .instance_size = sizeof(apps_fragment_t),
};

static const char *action_labels_offline[] = {translatable("Wake"), translatable("Retry"), ""};
static const action_cb_t action_callbacks_offline[] = {action_cb_wol, action_cb_host_reload};
static const char *action_labels_error[] = {translatable("Retry"), ""};
static const action_cb_t action_callbacks_error[] = {action_cb_host_reload};
static const char *actions_unpaired[] = {translatable("Pair"), ""};
static const action_cb_t action_callbacks_unpaired[] = {action_cb_pair};
static const char *actions_apps_none[] = {""};

static void apps_controller_ctor(lv_fragment_t *self, void *args) {
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->apploader_cb.start = appload_started;
    controller->apploader_cb.data = appload_loaded;
    controller->apploader_cb.error = appload_errored;
    apps_fragment_arg_t *arg = args;
    controller->global = arg->global;
    controller->uuid = arg->host;
    controller->def_app = arg->def_app;
    controller->apploader = apploader_create(arg->global, &controller->uuid, &controller->apploader_cb, controller);

    controller->platforms = NULL;
    controller->platform_count = 0;
    controller->platform_filter = NULL;
    controller->visible_map = NULL;
    controller->visible_count = 0;

    appitem_style_init(&controller->appitem_style);
}

static void apps_controller_dtor(lv_fragment_t *self) {
    apps_fragment_t *fragment = (apps_fragment_t *) self;
    appitem_style_deinit(&fragment->appitem_style);
    apploader_destroy(fragment->apploader);
    if (fragment->apploader_apps != NULL) {
        apploader_list_free(fragment->apploader_apps);
    }
    apps_free_platforms(fragment);
    free(fragment->platform_filter);
    fragment->platform_filter = NULL;
    free(fragment->visible_map);
    fragment->visible_map = NULL;
    fragment->visible_count = 0;
}

static lv_obj_t *apps_view(lv_fragment_t *self, lv_obj_t *container) {
    apps_fragment_t *controller = (apps_fragment_t *) self;
    lv_obj_t *view = lv_obj_create(container);
    lv_obj_remove_style_all(view);
    lv_obj_add_flag(view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scroll_dir(view, LV_DIR_NONE);
    lv_obj_set_style_bg_color(view, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);

    lv_obj_t *applist = controller->applist = lv_gridview_create(view);
    lv_obj_add_flag(applist, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scroll_dir(applist, LV_DIR_VER);
    lv_obj_clear_flag(applist, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_set_scrollbar_mode(applist, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(applist, lv_dpx(24), 0);
    lv_obj_set_style_pad_ver(applist, lv_dpx(16), 0);
    lv_obj_set_style_pad_gap(applist, lv_dpx(12), 0);
    lv_obj_set_style_radius(applist, 0, 0);
    lv_obj_set_style_border_side(applist, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_bg_opa(applist, LV_OPA_TRANSP, 0);
    lv_obj_set_style_anim_time(applist, 220, 0);
    lv_obj_set_size(applist, LV_PCT(100), LV_PCT(100));
    lv_obj_align(applist, LV_ALIGN_TOP_MID, 0, 0);
    lv_gridview_set_key_focus_clamp(applist, true);
    lv_obj_update_layout(applist);

    lv_gridview_set_adapter(applist, &apps_adapter);
    lv_obj_t *appload = controller->appload = lv_spinner_create(view, 1000, 60);
    launcher_fragment_t *parent_controller = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    lv_group_add_obj(parent_controller->detail_group, appload);
    lv_obj_add_flag(appload, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(appload, lv_dpx(60), lv_dpx(60));
    lv_obj_center(appload);

    lv_obj_t *apperror = controller->apperror = lv_obj_create(view);
    lv_obj_add_flag(apperror, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(apperror, LV_PCT(80), LV_PCT(60));
    lv_obj_center(apperror);
    lv_obj_set_flex_flow(apperror, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(apperror, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *errortitle = controller->errortitle = lv_label_create(apperror);
    lv_obj_set_width(errortitle, LV_PCT(100));
    lv_obj_set_style_text_font(errortitle, lv_theme_get_font_large(apperror), 0);
    lv_obj_t *errorlabel = controller->errorhint = lv_label_create(apperror);
    lv_obj_set_width(errorlabel, LV_PCT(100));
    lv_obj_t *errordetail = controller->errordetail = lv_label_create(apperror);
    lv_obj_set_style_border_width(errordetail, LV_DPX(2), 0);
    lv_obj_set_style_border_opa(errordetail, LV_OPA_50, 0);
    lv_obj_set_style_border_color(errordetail, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
    lv_obj_set_style_pad_all(errordetail, LV_DPX(10), 0);
    lv_obj_set_style_radius(errordetail, LV_DPX(20), 0);
    lv_obj_set_width(errordetail, LV_PCT(100));
    lv_obj_set_flex_grow(errordetail, 1);

    lv_obj_t *actions = controller->actions = lv_btnmatrix_create(apperror);
    lv_obj_set_style_outline_width(actions, 0, 0);
    lv_obj_set_style_outline_width(actions, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_all(actions, LV_DPX(5), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_side(actions, LV_BORDER_SIDE_NONE, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(actions, LV_PART_ITEMS);
    lv_obj_set_height(actions, lv_font_get_line_height(font) + LV_DPI_DEF / 10 + LV_DPX(5) * 2);
    lv_obj_set_style_max_width(actions, LV_PCT(100), 0);
    lv_obj_add_flag(actions, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_btnmatrix_set_btn_ctrl_all(actions, LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT);

    set_actions(controller, actions_apps_none, NULL);

    return view;
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    apps_fragment_t *controller = (apps_fragment_t *) self;
    controller->coverloader = coverloader_new(controller->global);
    pcmanager_register_listener(pcmanager, &pc_listeners, controller);
    lv_obj_t *applist = controller->applist;
    lv_obj_add_event_cb(applist, item_click_cb, LV_EVENT_SHORT_CLICKED, controller);
    lv_obj_add_event_cb(applist, item_longpress_cb, LV_EVENT_LONG_PRESSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_enter, LV_EVENT_FOCUSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_leave, LV_EVENT_DEFOCUSED, controller);
    lv_obj_add_event_cb(applist, applist_focus_leave, LV_EVENT_LEAVE, controller);
    lv_obj_add_event_cb(applist, applist_key_up_to_topbar, LV_EVENT_KEY | LV_EVENT_PREPROCESS, controller);
    lv_obj_add_event_cb(controller->actions, actions_click_cb, LV_EVENT_VALUE_CHANGED, controller);

    update_grid_config(controller);
    lv_obj_set_user_data(controller->applist, controller);

    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state != NULL && state->code != SERVER_STATE_QUERYING) {
        pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
        if (state->code == SERVER_STATE_AVAILABLE) {
            apploader_load(controller->apploader);
        }
    }
    current_instance = controller;
    update_view_state(controller);
}

static void obj_will_delete(lv_fragment_t *self, lv_obj_t *obj) {
    LV_UNUSED(obj);
    apps_fragment_t *fragment = (apps_fragment_t *) self;
    apploader_cancel(fragment->apploader);
    fragment->def_app = 0;
}

static void update_grid_config(apps_fragment_t *controller) {
    lv_obj_t *applist = controller->applist;
    if (!applist) {
        return;
    }
    lv_obj_t *view = lv_obj_get_parent(applist);
    lv_obj_update_layout(view);
    lv_obj_update_layout(applist);
    lv_coord_t view_w = lv_obj_get_width(view);
    lv_coord_t view_h = lv_obj_get_height(view);
    if (view_w <= 0) {
        view_w = lv_disp_get_hor_res(NULL);
    }
    if (view_h <= 0) {
        view_h = lv_disp_get_ver_res(NULL);
    }
    const int col_count = APPS_GRID_COLS;
    lv_coord_t pad_l = lv_obj_get_style_pad_left(applist, 0);
    lv_coord_t pad_r = lv_obj_get_style_pad_right(applist, 0);
    lv_coord_t pad_t = lv_obj_get_style_pad_top(applist, 0);
    lv_coord_t pad_b = lv_obj_get_style_pad_bottom(applist, 0);
    lv_coord_t gap_col = lv_obj_get_style_pad_column(applist, 0);
    lv_coord_t gap_row = lv_obj_get_style_pad_row(applist, 0);

    /* Size the tile from the available *width*, then derive its height from the
     * cover aspect. lv_gridview lays columns out as equal fr tracks (see
     * update_col_dsc in lv_gridview.c) and centers the item inside its track, so
     * a tile narrower than its track leaves dead space on both sides. Deriving
     * the width from the row height instead -- as this did before -- produced
     * 211px covers floating in 346px tracks at 1080p: 135px of gap per column,
     * which is what made the grid look sparse and unfinished. Making the tile
     * exactly as wide as its track means the covers butt up against the column
     * gap and nothing else. */
    lv_coord_t col_width = (view_w - pad_l - pad_r - gap_col * (col_count - 1)) / col_count;
    if (col_width < LV_DPX(40)) {
        col_width = LV_MAX(LV_DPX(40), view_w / col_count);
    }
    lv_coord_t row_height = col_width * APPS_COVER_ASPECT_H / APPS_COVER_ASPECT_W;
    /* Never let a single row exceed the viewport: on an unexpectedly short/wide
     * surface the aspect-derived height could push the first row out of view,
     * leaving nothing focusable on screen. */
    lv_coord_t max_row_height = view_h - pad_t - pad_b;
    if (max_row_height > LV_DPX(40) && row_height > max_row_height) {
        row_height = max_row_height;
    }
    if (row_height < LV_DPX(40)) {
        row_height = LV_DPX(40);
    }
    controller->col_count = col_count;
    controller->col_width = col_width;
    controller->col_height = row_height;
    lv_obj_set_size(applist, LV_PCT(100), LV_PCT(100));
    lv_gridview_set_config(applist, col_count, row_height, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_CENTER);

    controller->appitem_style.defcover_src.header.w = col_width;
    controller->appitem_style.defcover_src.header.h = row_height;
}

static void on_destroy_view(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    current_instance = NULL;
    apps_fragment_t *controller = (apps_fragment_t *) self;
    /* lv_fragment_del_obj() deletes the view before it calls us, so these three
     * already point at freed heap. Retracting the segments below re-enters the
     * launcher's zone router, which would read the grid's flags. */
    controller->applist = NULL;
    controller->appload = NULL;
    controller->apperror = NULL;
    controller->show_hidden_apps = false;
    /* The bar outlives this fragment (it belongs to the top bar), so retract the
     * segments -- otherwise switching hosts leaves the previous host's platforms
     * on screen, clickable, pointing at a fragment that no longer exists. */
    apps_free_platforms(controller);
    apps_publish_platforms(controller);
    pcmanager_unregister_listener(pcmanager, &pc_listeners);
    coverloader_unref(controller->coverloader);
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = (apps_fragment_t *) self;
    switch (code) {
        case USER_SIZE_CHANGED: {
            update_grid_config(controller);
            lv_gridview_rebind(controller->applist);
            break;
        }
        case USER_SHOW_HIDDEN_APPS: {
            ui_userevent_t *event = userdata;
            if (uuidstr_t_equals_t(&controller->uuid, event->data1)) {
                controller->show_hidden_apps = true;
                apploader_load(controller->apploader);
            }
            free(event->data1);
            return true;
        }
        default:
            break;
    }
    return false;
}

static void on_host_updated(const uuidstr_t *uuid, void *userdata) {
    apps_fragment_t *controller = (apps_fragment_t *) userdata;
    if (controller != current_instance) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    const SERVER_STATE *state = pcmanager_state(pcmanager, uuid);
    if (state == NULL) { return; }
    if (state->code == SERVER_STATE_AVAILABLE) {
        apploader_load(controller->apploader);
    }
    update_view_state(controller);
}

static void on_host_removed(const uuidstr_t *uuid, void *userdata) {
    apps_fragment_t *controller = (apps_fragment_t *) userdata;
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    lv_fragment_del((lv_fragment_t *) controller);
}

static void host_info_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (!controller->base.managed->obj_created) { return; }
    lv_btnmatrix_clear_btn_ctrl_all(controller->actions, LV_BTNMATRIX_CTRL_DISABLED);
}

static void send_wol_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (!controller->base.managed->obj_created) { return; }
    lv_btnmatrix_clear_btn_ctrl_all(controller->actions, LV_BTNMATRIX_CTRL_DISABLED);
    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state == NULL) { return; }
    if (state->code & SERVER_STATE_ONLINE || result != GS_OK) { return; }
    pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
}

static void update_view_state(apps_fragment_t *controller) {
    if (controller != current_instance) { return; }
    if (!controller->base.managed->obj_created || controller->base.managed->destroying_obj) { return; }
    launcher_fragment_t *parent_controller = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    parent_controller->detail_changing = true;
    const SERVER_STATE *state = pcmanager_state(pcmanager, &controller->uuid);
    if (state == NULL) {
        parent_controller->detail_changing = false;
        return;
    }
    switch (state->code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING: {
            // waiting to load server info
            show_progress(controller);
            break;
        }
        case SERVER_STATE_AVAILABLE: {
            switch (apploader_state(controller->apploader)) {
                case APPLOADER_STATE_LOADING: {
                    // is loading apps
                    if (controller->apploader_apps) {
                        break;
                    }
                    show_progress(controller);
                    break;
                }
                case APPLOADER_STATE_ERROR: {
                    // apploader has error
                    show_error(controller, locstr("Failed to load apps"),
                               locstr("Press \"Retry\" to load again.\n\nRestart your computer if error persists."),
                               controller->apploader_error);

                    set_actions(controller, action_labels_error, action_callbacks_error);
                    lv_group_focus_obj(controller->actions);
                    lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
                    break;
                }
                case APPLOADER_STATE_IDLE: {
                    // has apps
                    show_ok(controller);
                    break;
                }
            }
            break;
        }
        case SERVER_STATE_NOT_PAIRED: {
            show_error(controller, locstr("Not paired"),
                       locstr("Press \"Pair\" to pair this host with your account."),
                       NULL);

            set_actions(controller, actions_unpaired, action_callbacks_unpaired);
            lv_group_focus_obj(controller->actions);
            lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
            break;
        }
        case SERVER_STATE_ERROR: {
            // server has error
            show_error(controller, locstr("Host error"),
                       locstr("Press \"Retry\" to load again.\n\nRestart your computer if error persists."),
                       state->error.errmsg);

            set_actions(controller, action_labels_error, action_callbacks_error);
            lv_group_focus_obj(controller->actions);
            lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
            break;
        }
        case SERVER_STATE_OFFLINE: {
            // server has error
            show_error(controller, locstr("Offline"),
                       locstr("Press \"Wake\" to send Wake-on-LAN packet to turn the computer on if it supports this feature, "
                              "press \"Retry\" to connect again.\n\n"
                              "Try restart your computer if these doesn't work."),
                       NULL);

            set_actions(controller, action_labels_offline, action_callbacks_offline);
            lv_btnmatrix_clear_btn_ctrl(controller->actions, 0, LV_BTNMATRIX_CTRL_DISABLED);
            lv_group_focus_obj(controller->actions);
            lv_obj_add_state(controller->actions, LV_STATE_FOCUS_KEY);
            break;
        }
        default: {
            break;
        }
    }
    parent_controller->detail_changing = false;
}

static void show_progress(apps_fragment_t *fragment) {
    lv_obj_add_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(fragment->actions, LV_STATE_DISABLED);

    lv_group_focus_obj(fragment->appload);
    lv_obj_add_state(fragment->appload, LV_STATE_FOCUS_KEY);
}

static void show_ok(apps_fragment_t *fragment) {
    lv_obj_clear_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(fragment->actions, LV_STATE_DISABLED);

    if (lv_group_get_focused(lv_obj_get_group(fragment->applist)) != fragment->applist) {
        lv_group_focus_obj(fragment->applist);
    }
    /* show_ok() runs on every successful poll via update_view_state(), not
     * just on a genuine transition into this state. Only apply an explicit
     * item-level focus if the grid doesn't already have one -- unconditionally
     * re-focusing using focus_backup (which is only updated on blur, never
     * while the user is actively navigating inside the grid) was snapping
     * focus back to a stale position every ~10s regardless of where the user
     * had actually navigated to. This was the actual cause of the reported
     * "loses focus every ~10s" bug: a regression introduced in commit
     * 683e2404 ("Add fractional refresh support and input improvements"),
     * which added this unconditional refocus call -- upstream's show_ok()
     * has no such call at all. */
    if (lv_gridview_get_focused_index(fragment->applist) < 0) {
        int idx = fragment->focus_backup >= 0 ? fragment->focus_backup : 0;
        gridview_focus_with_key_state(fragment->applist, idx);
    }
}

static void show_error(apps_fragment_t *fragment, const char *title, const char *hint, const char *detail) {
    lv_obj_add_flag(fragment->appload, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->apperror, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fragment->applist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fragment->actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(fragment->actions, LV_STATE_DISABLED);

    lv_obj_set_style_opa(fragment->errordetail, detail ? LV_OPA_100 : LV_OPA_0, 0);
    lv_label_set_text_static(fragment->errortitle, title);
    lv_label_set_text_static(fragment->errorhint, hint);
}

static void appload_started(void *userdata) {
    apps_fragment_t *fragment = userdata;
    update_view_state(fragment);
}

static void appload_loaded(apploader_list_t *apps, void *userdata) {
    apps_fragment_t *fragment = userdata;
    if (!fragment->base.managed->obj_created || fragment->base.managed->destroying_obj) {
        return;
    }
    /* Refresh the platform segments first: that may retire a filter whose
     * platform vanished, which in turn decides what the new mapping contains. */
    apps_rebuild_platforms(fragment, apps);
    int new_visible_count = 0;
    int *new_visible_map = apps_build_visible_map(fragment, apps, &new_visible_count);

    int num_changes = -1;
    lv_gridview_data_change_t *changes = apps_list_detect_change(fragment->apploader_apps,
                                                                fragment->visible_map, fragment->visible_count,
                                                                apps, new_visible_map, new_visible_count,
                                                                &num_changes);

    /* Every poll that isn't a byte-for-byte identical list (e.g. the backend
     * returning apps in a new order) forces a full grid rebuild via
     * lv_gridview_set_data_advanced() below. lv_gridview itself already
     * refuses to recycle the currently-focused item's view during that
     * rebuild (see grid_recycle_item() in lv_gridview.c), so if the grid
     * currently owns input focus, that focus already survives on its own --
     * no action needed here.
     *
     * What still breaks is fragment->focus_backup, which is only updated when
     * focus *leaves* the grid (applist_focus_leave). If the grid doesn't
     * currently own focus (e.g. the user is on the top bar), that backup is a
     * grid position in the *old* mapping, which a reorder (or a platform
     * appearing/disappearing) may have just invalidated -- so re-resolve it by
     * app id before it's used to restore focus later (show_ok /
     * applist_focus_enter / apps_focus_rail).
     * We deliberately do NOT call lv_gridview_focus() here: doing so while
     * the grid already owns focus would fight the preservation above using
     * this same possibly-stale backup value, which is what caused focus to
     * visibly jump during normal browsing after the first pass at this fix. */
    if (num_changes != 0) {
        int live_focus = lv_gridview_get_focused_index(fragment->applist);
        if (live_focus < 0) {
            int prev_position = fragment->focus_backup;
            int focused_app_id = -1;
            if (fragment->apploader_apps != NULL && fragment->visible_map != NULL
                && prev_position >= 0 && prev_position < fragment->visible_count) {
                int raw = fragment->visible_map[prev_position];
                if (raw >= 0 && (size_t) raw < fragment->apploader_apps->count) {
                    focused_app_id = fragment->apploader_apps->items[raw].base.id;
                }
            }
            int new_position = 0;
            if (focused_app_id >= 0 && new_visible_map != NULL) {
                for (int i = 0; i < new_visible_count; i++) {
                    if (apps->items[new_visible_map[i]].base.id == focused_app_id) {
                        new_position = i;
                        break;
                    }
                }
            }
            fragment->focus_backup = new_position;
        }
    }

    /* Rebind the gridview to the NEW list before releasing the old one: the grid keeps
     * using grid->data until set_data_advanced() has swapped it, so freeing the previous
     * list up front would hand it a dangling pointer. The mapping has to be swapped in
     * lockstep with the list -- the adapter reads both. */
    apploader_list_t *old_apps = fragment->apploader_apps;
    fragment->apploader_apps = apps;
    apps_apply_visible_map(fragment, new_visible_map, new_visible_count);

    lv_gridview_set_data_advanced(fragment->applist, apps, changes, num_changes);
    if (changes != NULL) {
        free(changes);
    }
    apploader_list_free(old_apps);
    apps_publish_platforms(fragment);
    update_view_state(fragment);

    if (fragment->def_app > 0 && !fragment->def_app_launched) {
        fragment->def_app_launched = true;
        const apploader_item_t *app = NULL;
        for (int i = 0; i < apps->count; ++i) {
            if (apps->items[i].base.id == fragment->def_app) {
                app = &apps->items[i];
                break;
            }
        }
        if (app != NULL) {
            launcher_launch_game(fragment, app);
        }
    }
}

static void appload_errored(int code, const char *error, void *userdata) {
    LV_UNUSED(code);
    apps_fragment_t *fragment = userdata;
    fragment->apploader_error = error;
    update_view_state(fragment);
}

static void appitem_bind(apps_fragment_t *controller, lv_obj_t *item, apploader_item_t *app) {
    appitem_viewholder_t *holder = lv_obj_get_user_data(item);

    /* Tiles are sized in appitem_view() at creation time, but lv_gridview keeps
     * recycling those same objects, so a tile created under one geometry keeps
     * that size forever. That only became visible once the grid could be
     * resized while populated (showing/hiding the platform filter row changes
     * the height available to it), which left old and new tiles side by side at
     * different sizes in the same row. Re-assert the size on every bind: it is a
     * no-op when nothing changed. */
    if (lv_obj_get_width(item) != controller->col_width || lv_obj_get_height(item) != controller->col_height) {
        lv_obj_set_size(item, controller->col_width, controller->col_height);
        /* The cached cover was decoded for the old tile size. */
        holder->app_id = 0;
    }

    /* lv_gridview's fill_rows() rebinds every visible tile on every single
     * lv_gridview_set_data_advanced() call, including when the view wasn't
     * recycled and is already showing this exact app (e.g. every ~10s poll
     * that finds nothing changed). coverloader_display() unconditionally
     * cancels any in-flight load and kicks off a new one, which redraws the
     * cover art even when it's already correct -- that's what caused the
     * cover art to visibly flicker on every poll. Skip it when this view is
     * already displaying the right app's cover. */
    if (holder->app_id != app->base.id) {
        coverloader_display(controller->coverloader, &controller->uuid, app->base.id, item,
                            controller->col_width, controller->col_height);
    }
    lv_label_set_text(holder->title, app->base.name);

    int current_id = pcmanager_server_current_app(pcmanager, &controller->uuid);
    if (current_id == app->base.id) {
        lv_obj_clear_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_opa(item, app->hidden ? LV_OPA_50 : LV_OPA_COVER, 0);
    holder->app_id = app->base.id;
}

static void item_click_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *target_parent = lv_obj_get_parent(target);
    if (target_parent != controller->applist) {
        return;
    }
    appitem_viewholder_t *holder = (appitem_viewholder_t *) lv_obj_get_user_data(target);
    int current_app = pcmanager_server_current_app(pcmanager, &controller->uuid);
    if (current_app != 0) {
        if (holder->app_id == current_app) {
            open_context_menu(controller, holder);
        }
        return;
    }
    const apploader_item_t *item = apploader_list_item_by_id(controller->apploader_apps, holder->app_id);
    assert(item != NULL);
    launcher_launch_game(holder->controller, item);
}

static void item_longpress_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *target_parent = lv_obj_get_parent(target);
    if (target_parent != controller->applist) {
        return;
    }
    lv_event_send(target, LV_EVENT_RELEASED, lv_event_get_indev(event));
    open_context_menu(controller, (appitem_viewholder_t *) lv_obj_get_user_data(target));
}

static void launcher_launch_game(apps_fragment_t *controller, const apploader_item_t *app) {
    LV_ASSERT(app->base.id != 0);
    streaming_scene_arg_t args = {
            .global = controller->global,
            .uuid = controller->uuid,
            .app = app->base,
    };
    app_ui_t *ui = &controller->global->ui;
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &args);
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(ui->fm));
    lv_fragment_manager_push(ui->fm, fragment, container);
}

static void launcher_toggle_fav(apps_fragment_t *controller, const apploader_item_t *app) {
    pcmanager_favorite_app(pcmanager, &controller->uuid, app->base.id, !app->fav);
    apploader_load(controller->apploader);
}

static void launcher_toggle_hidden(apps_fragment_t *controller, const apploader_item_t *app) {
    pcmanager_set_app_hidden(pcmanager, &controller->uuid, app->base.id, !app->hidden);
    controller->show_hidden_apps = true;
    apploader_load(controller->apploader);
}

static void launcher_quit_game(apps_fragment_t *controller) {
    controller->quit_progress = progress_dialog_create(locstr("Quitting game..."));
    pcmanager_quitapp(pcmanager, &controller->uuid, quitgame_cb, NULL);
}

static int adapter_item_count(lv_obj_t *grid, void *data) {
    if (data == NULL) { return 0; }
    apps_fragment_t *controller = lv_obj_get_user_data(grid);
    /* visible_map is built against apploader_apps; if the grid is still holding
     * an older list the mapping doesn't describe it and must not be used. */
    if (data != controller->apploader_apps) { return 0; }
    return controller->visible_count;
}

static lv_obj_t *adapter_create_view(lv_obj_t *parent) {
    apps_fragment_t *controller = lv_obj_get_user_data(parent);
    return appitem_view(controller, parent);
}

static void adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position) {
    apps_fragment_t *controller = lv_obj_get_user_data(grid);
    apploader_list_t *list = data;
    if (list == NULL || list != controller->apploader_apps || controller->visible_map == NULL) {
        return;
    }
    if (position < 0 || position >= controller->visible_count) {
        return;
    }
    int index = controller->visible_map[position];
    if (index < 0 || (size_t) index >= list->count) {
        return;
    }
    appitem_bind(controller, item_view, &list->items[index]);
}


/* lv_gridview_focus() reports its FOCUSED event using lv_indev_get_act(),
 * which is NULL whenever focus is routed into the grid programmatically
 * (group-focus mechanics, fragment lifecycle, etc.) rather than from a live
 * keypress. LVGL's default LV_EVENT_FOCUSED handler only applies
 * LV_STATE_FOCUS_KEY for a genuine keypad/encoder indev, so in the
 * programmatic case the tile ends up focused internally
 * (grid->focused_index is correct) but shows no selection outline at all.
 * Force the state directly after focusing, same as upstream's
 * set_detail_opened() already does for non-gridview objects. */
static void gridview_focus_with_key_state(lv_obj_t *grid, int idx) {
    lv_gridview_focus(grid, idx);
    lv_obj_t *focused_item = lv_gridview_get_item_view(grid, idx);
    if (focused_item) {
        lv_obj_add_state(focused_item, LV_STATE_FOCUS_KEY);
    }
}

static void applist_focus_enter(lv_event_t *event) {
    if (event->target != event->current_target) { return; }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    int idx = controller->focus_backup >= 0 ? controller->focus_backup : 0;
    gridview_focus_with_key_state(controller->applist, idx);
}

static void applist_focus_leave(lv_event_t *event) {
    if (event->target != event->current_target) { return; }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    controller->focus_backup = lv_gridview_get_focused_index(controller->applist);
    lv_gridview_focus(controller->applist, -1);
}

static void quitgame_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    LV_UNUSED(userdata);
    apps_fragment_t *controller = current_instance;
    if (controller == NULL) { return; }
    if (!uuidstr_t_equals_t(&controller->uuid, uuid)) { return; }
    if (controller->quit_progress) {
        lv_msgbox_close(controller->quit_progress);
        controller->quit_progress = NULL;
    }
    lv_gridview_rebind(controller->applist);
    if (result == GS_OK) {
        return;
    }
    static const char *btn_texts[] = {translatable("OK"), ""};
    lv_obj_t *dialog = lv_msgbox_create_i18n(NULL, locstr("Unable to quit game"),
                                             locstr("Please make sure you are quitting with the same client."),
                                             btn_texts, false);
    lv_obj_add_event_cb(dialog, quit_dialog_cb, LV_EVENT_VALUE_CHANGED, controller);
    lv_obj_center(dialog);
}

static void quit_dialog_cb(lv_event_t *event) {
    lv_obj_t *dialog = lv_event_get_current_target(event);
    lv_msgbox_close_async(dialog);
}

static void actions_click_cb(lv_event_t *event) {
    apps_fragment_t *controller = lv_event_get_user_data(event);
    uint16_t index = lv_btnmatrix_get_selected_btn(controller->actions);
    const action_cb_t *actions = lv_obj_get_user_data(controller->actions);
    actions[index](controller, controller->actions, index);
}

static void action_cb_wol(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(index);
    lv_btnmatrix_set_btn_ctrl_all(buttons, LV_BTNMATRIX_CTRL_DISABLED);
    pcmanager_send_wol(pcmanager, &controller->uuid, send_wol_cb, NULL);
}

static void action_cb_host_reload(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(index);
    lv_btnmatrix_set_btn_ctrl_all(buttons, LV_BTNMATRIX_CTRL_DISABLED);
    pcmanager_request_update(pcmanager, &controller->uuid, host_info_cb, NULL);
}

static void action_cb_pair(apps_fragment_t *controller, lv_obj_t *buttons, uint16_t index) {
    LV_UNUSED(buttons);
    LV_UNUSED(index);
    pair_dialog_open(&controller->uuid);
}

static void open_context_menu(apps_fragment_t *fragment, appitem_viewholder_t *holder) {
    const apploader_item_t *app = apploader_list_item_by_id(fragment->apploader_apps, holder->app_id);
    assert(app != NULL);
    lv_obj_t *msgbox = lv_msgbox_create(NULL, app->base.name, NULL, NULL, false);
    lv_obj_set_user_data(msgbox, (void *) holder);
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(content, context_menu_cancel_cb, LV_EVENT_CANCEL, fragment);
    lv_obj_add_event_cb(content, context_menu_click_cb, LV_EVENT_SHORT_CLICKED, fragment);

    int currentId = pcmanager_server_current_app(pcmanager, &fragment->uuid);
    if (!currentId || currentId == app->base.id) {
        lv_obj_t *start_btn = lv_list_add_btn(content, NULL,
                                              currentId == app->base.id ? locstr("Resume streaming")
                                                                        : locstr("Start streaming"));
        lv_obj_add_flag(start_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(start_btn, launcher_launch_game);
    }

    if (currentId) {
        lv_obj_t *quit_btn = lv_list_add_btn(content, NULL, locstr("Stop streaming"));
        lv_obj_add_flag(quit_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(quit_btn, launcher_quit_game);
    }
    lv_obj_t *fav_btn = lv_list_add_btn(content, NULL, app->fav ? locstr("Unstar") : locstr("Star"));
    lv_obj_add_flag(fav_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(fav_btn, launcher_toggle_fav);

    lv_obj_t *hide_btn = lv_list_add_btn(content, NULL, app->hidden ? locstr("Unhide") : locstr("Hide"));
    lv_obj_add_flag(hide_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(hide_btn, launcher_toggle_hidden);

    lv_obj_t *info_btn = lv_list_add_btn(content, NULL, locstr("Info"));
    lv_obj_add_flag(info_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(info_btn, app_detail_dialog);

    lv_obj_t *cancel_btn = lv_list_add_btn(content, NULL, locstr("Cancel"));
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(msgbox);
}

static void context_menu_cancel_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target->parent != lv_event_get_current_target(e)) { return; }
    lv_msgbox_close(lv_event_get_current_target(e)->parent);
}

static void context_menu_click_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *current_target = lv_event_get_current_target(e);
    if (target->parent != current_target) { return; }
    lv_obj_t *mbox = lv_event_get_current_target(e)->parent;
    apps_fragment_t *self = lv_event_get_user_data(e);
    appitem_viewholder_t *holder = lv_obj_get_user_data(mbox);
    const apploader_item_t *app = apploader_list_item_by_id(self->apploader_apps, holder->app_id);
    assert(app != NULL);
    if (lv_obj_get_user_data(target) == launcher_quit_game) {
        launcher_quit_game(self);
    } else if (lv_obj_get_user_data(target) == launcher_launch_game) {
        launcher_launch_game(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_toggle_fav) {
        launcher_toggle_fav(self, app);
    } else if (lv_obj_get_user_data(target) == launcher_toggle_hidden) {
        launcher_toggle_hidden(self, app);
    } else if (lv_obj_get_user_data(target) == app_detail_dialog) {
        app_detail_dialog(self, app);
    }
    lv_msgbox_close_async(mbox);
}

static void app_detail_dialog(apps_fragment_t *fragment, const apploader_item_t *app) {
    LV_UNUSED(fragment);
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, app->base.name, "text", btn_txts, false);
    lv_obj_t *msgobj = lv_msgbox_get_text(msgbox);
    lv_label_set_text_fmt(msgobj,
                          "ID: %d\n"
                          "Support HDR: %s",
                          app->base.id, app->base.hdr ? "Yes" : "No");
    lv_obj_center(msgbox);
    lv_obj_add_event_cb(msgbox, app_detail_click_cb, LV_EVENT_CLICKED, NULL);
}

static void app_detail_click_cb(lv_event_t *event) {
    lv_msgbox_close(lv_event_get_current_target(event));
}

static void set_actions(apps_fragment_t *controller, const char **labels, const action_cb_t *callbacks) {
    lv_btnmatrix_set_map(controller->actions, labels);
    int num_actions = 0;
    for (int i = 0; labels[i][0] != '\0'; i++) {
        num_actions++;
    }
    lv_obj_set_style_min_width(controller->actions, LV_PCT(20 * num_actions), 0);
    lv_obj_set_user_data(controller->actions, (void *) callbacks);
}


/**
 * Decide whether the grid can keep its current views. Compares the *visible*
 * sequences (post-filter), because that -- not the raw list -- is what the grid
 * actually renders.
 */
static lv_gridview_data_change_t *apps_list_detect_change(const apploader_list_t *old_list, const int *old_map,
                                                          int old_count, const apploader_list_t *new_list,
                                                          const int *new_map, int new_count, int *num_changes) {
    if (old_list == NULL && new_list == NULL) {
        *num_changes = 0;
        /* Must be non-NULL: lv_gridview_set_data_advanced() treats a NULL
         * changes pointer as "invalidate everything" regardless of
         * num_changes (see its `changes == NULL || num_changes < 0` check),
         * so a genuinely-empty change set still needs a real (if unused)
         * pointer to avoid a full, unnecessary grid rebuild. Freed by the
         * caller. */
        return calloc(1, sizeof(lv_gridview_data_change_t));
    } else if ((old_list != NULL) != (new_list != NULL)) {
        *num_changes = -1;
        return NULL;
    } else if (old_count != new_count) {
        *num_changes = -1;
        return NULL;
    }
    for (int i = 0; i < old_count; i++) {
        if (old_map == NULL || new_map == NULL) {
            *num_changes = -1;
            return NULL;
        }
        int old_index = old_map[i], new_index = new_map[i];
        if (old_index < 0 || (size_t) old_index >= old_list->count ||
            new_index < 0 || (size_t) new_index >= new_list->count) {
            *num_changes = -1;
            return NULL;
        }
        if (old_list->items[old_index].base.id != new_list->items[new_index].base.id) {
            *num_changes = -1;
            return NULL;
        }
    }
    *num_changes = 0;
    /* Non-NULL sentinel -- see comment above. num_changes=0 means the widget
     * never iterates this array, but it must not be NULL or the widget will
     * still fully recycle and rebuild every visible tile on every poll (the
     * cause of the cover art flickering on every ~10s refresh), even when
     * nothing actually changed. */
    return calloc(1, sizeof(lv_gridview_data_change_t));
}

/**
 * Group an item belongs to; never NULL, so grouping has no special cases.
 *
 * The store library wins over the platform when the host reports one: every
 * Steam title is also "PC (Windows)", so grouping purely by platform buries the
 * store libraries in one big PC bucket. This matches how Playnite itself can
 * group a library by source.
 */
static const char *apps_item_platform(const apploader_item_t *item) {
    const char *library = item->base.library;
    if (library != NULL && library[0] != '\0') {
        return library;
    }
    const char *platform = item->base.platform;
    if (platform == NULL || platform[0] == '\0') {
        return apps_platform_other();
    }
    return platform;
}

static bool apps_item_passes_filter(apps_fragment_t *controller, const apploader_item_t *item) {
    if (item->hidden && !controller->show_hidden_apps) {
        return false;
    }
    if (controller->platform_filter == NULL) {
        return true;
    }
    return strcmp(apps_item_platform(item), controller->platform_filter) == 0;
}

static void apps_free_platforms(apps_fragment_t *controller) {
    for (int i = 0; i < controller->platform_count; i++) {
        free(controller->platforms[i]);
    }
    free(controller->platforms);
    controller->platforms = NULL;
    controller->platform_count = 0;
}

/**
 * Collect the distinct platforms present in @p list, preserving first-seen order
 * (the list is already name-sorted, so this is stable across polls). Hidden apps
 * only contribute a platform when hidden apps are being shown, otherwise the bar
 * would offer a segment that filters down to nothing.
 */
static void apps_rebuild_platforms(apps_fragment_t *controller, const apploader_list_t *list) {
    apps_free_platforms(controller);
    if (list == NULL || list->count == 0) {
        return;
    }
    char **names = calloc(list->count, sizeof(char *));
    if (names == NULL) {
        return;
    }
    int count = 0;
    for (size_t i = 0; i < list->count; i++) {
        const apploader_item_t *item = &list->items[i];
        if (item->hidden && !controller->show_hidden_apps) {
            continue;
        }
        const char *platform = apps_item_platform(item);
        bool seen = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(names[j], platform) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            names[count] = strdup(platform);
            if (names[count] == NULL) {
                break;
            }
            count++;
        }
    }
    /* Keep the catch-all bucket last. It is first-seen in a name-sorted list
     * more often than not (Desktop, Playnite, ...), and a bar reading
     * "All | Other | PC | Switch" puts the least interesting segment where the
     * eye lands first. */
    const char *other = apps_platform_other();
    for (int i = 0; i < count - 1; i++) {
        if (strcmp(names[i], other) == 0) {
            char *tmp = names[i];
            memmove(&names[i], &names[i + 1], (size_t) (count - 1 - i) * sizeof(char *));
            names[count - 1] = tmp;
            break;
        }
    }
    controller->platforms = names;
    controller->platform_count = count;

    /* A filter naming a platform that no longer exists would leave an empty grid
     * with no way back, so drop it. */
    if (controller->platform_filter != NULL) {
        bool still_there = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], controller->platform_filter) == 0) {
                still_there = true;
                break;
            }
        }
        if (!still_there) {
            free(controller->platform_filter);
            controller->platform_filter = NULL;
        }
    }
}

/**
 * Build the grid position -> item index mapping for the active filter.
 * Returns a newly allocated array (caller owns it) and writes its length to
 * @p out_count. Returns NULL for an empty result, which is a valid mapping.
 */
static int *apps_build_visible_map(apps_fragment_t *controller, const apploader_list_t *list, int *out_count) {
    *out_count = 0;
    if (list == NULL || list->count == 0) {
        return NULL;
    }
    int *map = calloc(list->count, sizeof(int));
    if (map == NULL) {
        return NULL;
    }
    int count = 0;
    /* lv_gridview addresses rows with a uint8_t, so it can't show more than
     * 255 rows worth of items -- same cap the pre-filter code applied. */
    const int cap = 255 * LV_MAX(1, controller->col_count);
    for (size_t i = 0; i < list->count && count < cap; i++) {
        if (!apps_item_passes_filter(controller, &list->items[i])) {
            continue;
        }
        map[count++] = (int) i;
    }
    *out_count = count;
    return map;
}

static void apps_apply_visible_map(apps_fragment_t *controller, int *map, int count) {
    free(controller->visible_map);
    controller->visible_map = map;
    controller->visible_count = count;
}

/** Hand the segment labels to the top bar so it can render the filter control. */
static void apps_publish_platforms(apps_fragment_t *controller) {
    launcher_fragment_t *launcher = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    if (launcher == NULL) {
        return;
    }
    launcher_set_platform_segments(launcher, (const char *const *) controller->platforms,
                                   controller->platform_count, controller->platform_filter);
}

void apps_set_platform_filter(apps_fragment_t *controller, const char *platform) {
    if (controller == NULL) {
        return;
    }
    const char *current = controller->platform_filter;
    if ((current == NULL && platform == NULL) ||
        (current != NULL && platform != NULL && strcmp(current, platform) == 0)) {
        return;
    }
    free(controller->platform_filter);
    controller->platform_filter = platform != NULL ? strdup(platform) : NULL;

    int count = 0;
    int *map = apps_build_visible_map(controller, controller->apploader_apps, &count);
    apps_apply_visible_map(controller, map, count);

    /* Switching platforms changes which item sits at every grid position, so the
     * old focus index is meaningless -- start at the top of the new selection. */
    controller->focus_backup = 0;
    if (controller->applist != NULL) {
        lv_gridview_set_data_advanced(controller->applist, controller->apploader_apps, NULL, -1);
        lv_obj_scroll_to_y(controller->applist, 0, LV_ANIM_OFF);
        if (lv_gridview_get_focused_index(controller->applist) >= 0 || count > 0) {
            gridview_focus_with_key_state(controller->applist, count > 0 ? 0 : -1);
        }
    }
}

void apps_relayout(apps_fragment_t *controller) {
    if (controller == NULL) {
        return;
    }
    lv_coord_t prev_w = controller->col_width, prev_h = controller->col_height;
    int prev_cols = controller->col_count;
    update_grid_config(controller);
    if (prev_w == controller->col_width && prev_h == controller->col_height && prev_cols == controller->col_count) {
        return;
    }
    /* A rebind alone re-runs bind_view on the views that already exist; it does
     * not rebuild the row layout. lv_gridview_set_config() only reflows
     * everything when the *column count* changed, so a pure row-height change
     * (which is what showing the filter row causes) would leave the grid
     * half-laid-out. Force the full reset instead. */
    lv_gridview_set_data_advanced(controller->applist, controller->apploader_apps, NULL, -1);
    int focused = lv_gridview_get_focused_index(controller->applist);
    if (focused >= 0 && focused < controller->visible_count) {
        gridview_focus_with_key_state(controller->applist, focused);
    }
}

void apps_focus_rail(apps_fragment_t *controller) {
    if (!controller) {
        return;
    }
    if (lv_obj_has_flag(controller->applist, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_group_focus_obj(controller->applist);
    if (controller->visible_count <= 0) {
        return;
    }
    /* Prefer where the grid already is; only fall back to the backup taken when
     * focus last left it. Either can point past the end after a filter change or
     * a shrinking app list, so clamp before using it. */
    int idx = lv_gridview_get_focused_index(controller->applist);
    if (idx < 0) {
        idx = controller->focus_backup;
    }
    if (idx < 0 || idx >= controller->visible_count) {
        idx = 0;
    }
    gridview_focus_with_key_state(controller->applist, idx);
}

static void applist_key_up_to_topbar(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_KEY || lv_event_get_key(event) != LV_KEY_UP) {
        return;
    }
    apps_fragment_t *controller = lv_event_get_user_data(event);
    int idx = lv_gridview_get_focused_index(controller->applist);
    if (idx < 0 || idx >= controller->col_count) {
        return;
    }
    launcher_fragment_t *launcher = (launcher_fragment_t *) lv_fragment_get_parent(&controller->base);
    launcher_focus_above_grid(launcher);
    lv_event_stop_processing(event);
}