#pragma once

#include <stdbool.h>
#include "ui/config.h"

#include "lvgl.h"
#include "lv_sdl_img.h"

#include "backend/pcmanager.h"

typedef struct app_t app_t;
typedef struct app_launch_params_t app_launch_params_t;

typedef struct launcher_fragment_args_t {
    app_t *app;
    const app_launch_params_t *params;
} launcher_fragment_args_t;

/** Vertical focus zones of the home screen, ordered top to bottom. */
typedef enum launcher_zone_t {
    LAUNCHER_ZONE_TOPBAR = 0,
    LAUNCHER_ZONE_FILTER = 1,
    LAUNCHER_ZONE_GRID = 2,
} launcher_zone_t;

/** Height of the home top bar (dp); settings overlay sits below this band. */
#define LAUNCHER_TOPBAR_DPX 60

/** Height of the platform filter row (dp); only occupied while the row is shown. */
#define LAUNCHER_FILTERBAR_DPX 34

/** Platform segments the filter bar can show, excluding the leading "All". */
#define LAUNCHER_MAX_PLATFORMS 8
/** Longest platform name/label handled by the filter bar, in bytes. */
#define LAUNCHER_PLATFORM_LABEL_MAX 40

typedef struct launcher_fragment_t {
    lv_fragment_t base;

    app_t *global;

    /* Top-bar (formerly the side nav). Contains logo + Aurora label on the left,
     * and the action buttons on the right. */
    lv_obj_t *nav;
    /* Detail area below the top bar; full width, hosts the apps fragment (game rail). */
    lv_obj_t *detail;
    /* Wrapper that grows to fill whatever the bars leave over. */
    lv_obj_t *detail_stack;
    /* Platform filter band between the top bar and the grid. */
    lv_obj_t *filter_row;
    lv_obj_t *profile_dropdown;
    /** Profile combobox list open; blocks arrow-key focus changes until closed. */
    lv_obj_t *active_dropdown;

    /* Server selector button shown in the top bar. Click opens the server popup
     * (full PC list with status icons, long-press for context menu). The label
     * inside the button reflects the currently selected PC. */
    lv_obj_t *server_btn;
    lv_obj_t *server_label;

    lv_obj_t *add_btn, *pref_btn, *help_btn, *quit_btn;

    /* Segmented platform filter ("All" + one segment per platform the host
     * reports). Hidden while the host offers nothing to group by. */
    lv_obj_t *platform_bar;
    /* Backing storage for the button-matrix map: lv_btnmatrix keeps the pointer
     * we hand it, so the strings have to outlive the call. Slot 0 is "All",
     * the last entry is the "" terminator. Labels are the shortened names shown
     * on screen; values are the platform names as the host reports them, which
     * is what the filter actually matches on. */
    char platform_labels[LAUNCHER_MAX_PLATFORMS + 1][LAUNCHER_PLATFORM_LABEL_MAX];
    char platform_values[LAUNCHER_MAX_PLATFORMS + 1][LAUNCHER_PLATFORM_LABEL_MAX];
    const char *platform_map[LAUNCHER_MAX_PLATFORMS + 2];
    /* Segment count including "All"; 0 while the bar is hidden. */
    int platform_segments;
    /* Selected segment; 0 = "All". */
    int platform_selected;

    /* Focus groups, one per vertical zone: top bar, platform filter, game rail. */
    lv_group_t *nav_group, *filter_group, *detail_group;

    /* Which zone currently owns keypad/gamepad input; see launcher_zone_t. */
    int focus_zone;
    /* Guards against re-entering the zone switch from the FOCUSED event it emits. */
    bool focus_switching;

    /* Style applied to top-bar action buttons (icon-only). */
    lv_style_t topbar_btn_style;

    bool pane_initialized;
    bool first_created;
    /* Used by apps.controller while it (re)builds the rail to suppress spurious focus
     * callbacks; preserved from the previous architecture. */
    bool detail_changing;

    const app_launch_params_t *launch_params;
    bool def_host_selected;
    bool def_app_requested;
    bool auto_resume_done;        // once-guard: only auto-resume once per app start
    int  pending_def_app;         // def_app id injected into the next select_pc()
    uuidstr_t auto_resume_uuid;   // host scheduled for deferred auto-resume

    /* Full-screen overlay over the game area (below the top bar). Hosts embedded Settings. */
    lv_obj_t *settings_layer;
    lv_fragment_t *settings_fragment;
} launcher_fragment_t;


lv_obj_t *launcher_win_create(lv_fragment_t *self, lv_obj_t *parent);

launcher_fragment_t *launcher_instance();

void launcher_select_server(launcher_fragment_t *controller, const uuidstr_t *uuid);

/* Refresh the visible server-button label from the currently selected PC (or fall
 * back to a generic "Select server" placeholder when nothing is selected). */
void launcher_refresh_server_label(launcher_fragment_t *controller);

/** After closing launcher-embedded UI, point keypad/gamepad focus back at the main bar. */
void launcher_restore_nav_focus(launcher_fragment_t *controller);

/** Leave the game grid upwards: onto the platform filter, or the top bar if there is none. */
void launcher_focus_above_grid(launcher_fragment_t *controller);

/** Wire gamepad/remote navigation for the top-bar profile combobox. */
void launcher_attach_profile_dropdown_nav(launcher_fragment_t *controller);

/** Wire gamepad/remote navigation for the top-bar platform filter. */
void launcher_attach_platform_bar_nav(launcher_fragment_t *controller);

/**
 * CHILD_CREATED handler for the top bar: puts focusable children into nav_group
 * and gives each one the shared arrow-key handler.
 */
void launcher_nav_child_added(lv_event_t *event);

/**
 * Step the app-group filter one segment, wrapping around at both ends.
 *
 * Home-screen shortcut for the controller shoulder buttons, so switching group
 * doesn't mean walking the cursor up out of the grid first. Works from any of
 * the three zones and takes no controller argument on purpose: the caller is the
 * input driver, which has no business knowing whether a launcher exists.
 *
 * @param dir negative to step left, positive to step right.
 * @return true if a group change was applied, false when the home screen isn't
 *         what's on screen or there is nothing to cycle through.
 */
bool launcher_cycle_platform(int dir);

/**
 * Rebuild the platform filter segments from the app list.
 * @param names    platform names, or NULL when there is nothing to group by
 * @param count    number of entries in @p names
 * @param selected currently applied platform name, or NULL for "All"
 */
void launcher_set_platform_segments(launcher_fragment_t *controller, const char *const *names, int count,
                                    const char *selected);

extern const lv_fragment_class_t launcher_controller_class;
