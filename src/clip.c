/* ========================================================================= */
/**
 * @file clip.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "clip.h"

#include <cairo.h>
#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "backend/backend.h"
#include "action.h"
#include "clip_grid.h"
#include "backend/output_config.h"
#include "context_menu.h"
#include "dock_fx.h"
#include "dock_launchers.h"
#include "dockapps.h"
#include "dock_position.h"
#include "layer_panel.h"
#include "i18n.h"
#include "input/manager.h"
#include "toolkit/toolkit.h"
#include "util/files.h"

/* == Declarations ========================================================= */

/**
 * A lane of the Clip's grid beyond its column: A dock along the edge, at a
 * distance from it, holding the dock-apps of that lane and empty tiles
 * between them. Without an exclusive zone: Windows may go below, as on
 * Window Maker.
 */
typedef struct {
    /** The dock. */
    wlmtk_dock_t              *dock_ptr;
    /** Empty, transparent tiles: Holes, and room for launchers. */
    wlmtk_tile_t              *spacers_ptr;
    /** Number of tiles in @ref wlmaker_clip_lane_t::spacers_ptr. */
    size_t                    spacers;
} wlmaker_clip_lane_t;

/** Clip handle. */
struct _wlmaker_clip_t {
    /** The clip happens to be derived from a tile. */
    wlmtk_tile_t              super_tile;
    /** Original virtual method table fo the superclass' element. */
    wlmtk_element_vmt_t       orig_super_element_vmt;

    /** Backlink to the server. */
    wlmaker_server_t          *server_ptr;

    /** The toolkit dock, holding the clip tile. */
    wlmtk_dock_t              *wlmtk_dock_ptr;
    /** Placement of the dock-apps in the clip's dock. */
    wlmaker_dockapps_t        *dockapps_ptr;
    /**
     * Empty tiles after the clip, one per launcher of `Clip.Launchers`
     * shown: Room for the launchers shown there by `wlmdock --clip`, among
     * the dock-apps.
     */
    wlmtk_tile_t              *spacer_tiles_ptr;
    /** Number of tiles in @ref wlmaker_clip_t::spacer_tiles_ptr. */
    size_t                    spacer_tiles;

    /** The tile's texture buffer without any buttons pressed */
    struct wlr_buffer         *tile_buffer_ptr;
    /** The tile's texture buffer with the 'Next' buttons pressed. */
    struct wlr_buffer         *next_pressed_tile_buffer_ptr;
    /** The tile's texture buffer with the 'Previous' buttons pressed. */
    struct wlr_buffer         *prev_pressed_tile_buffer_ptr;

    /** Overlay buffer element: Contains the workspace's title and number. */
    wlmtk_buffer_t            overlay_buffer;
    /** Path to the image file. */
    char                      *image_path_ptr;
    /** Clip image. */
    wlmtk_image_t             *image_ptr;

    /** Description of the desired output, if any. */
    wlmbe_output_description_t output_description;

    /** Whether the pointer is currently inside the 'prev' button. */
    bool                      pointer_inside_prev_button;
    /** Whether the pointer is currently inside the 'next' button. */
    bool                      pointer_inside_next_button;
    /** Whether the 'prev' button had been pressed. */
    bool                      prev_button_pressed;
    /** Whether the 'next' button had been pressed. */
    bool                      next_button_pressed;

    /** Whether the left button is held on the Clip: It may become a drag. */
    bool                      drag_pending;
    /** Whether the Clip is being dragged to another corner. */
    bool                      dragging;
    /** Pointer position where the button was pressed, in layout coords. */
    double                    drag_start_x;
    /** Pointer position where the button was pressed, in layout coords. */
    double                    drag_start_y;
    /** Corner for the live preview while dragging, applied when idle. */
    wlmaker_dock_place_t      preview_place;
    /** Applies @ref wlmaker_clip_t::preview_place, from the event loop. */
    struct wl_event_source    *preview_event_source_ptr;
    /** Monitors SIGUSR1: `wlmdock --clip` changed the Clip's launchers. */
    struct wl_event_source    *sigusr1_event_source_ptr;

    /** Listener for @ref wlmtk_desktop_events_t::workspace_changed. */
    struct wl_listener        workspace_changed_listener;
    /** Listener for wlr_output_layout::events.change. */
    struct wl_listener        output_layout_change_listener;
    /** Listener for @ref wlmtk_element_events_t::pointer_motion. */
    struct wl_listener        pointer_motion_listener;
    /** Listener for @ref wlmtk_element_events_t::pointer_leave. */
    struct wl_listener        pointer_leave_listener;
    /** Listener for @ref wlmaker_server_t::theme_changed_event. */
    struct wl_listener        theme_changed_listener;

    /** The clip's style. */
    wlmaker_config_clip_style_t style;

    /** Path of the dock-apps state file. May be NULL. */
    char                      *dockapps_fname_ptr;
    /**
     * Dock-apps attached to the Dock: A dock after the Dock's launchers,
     * positioned over the Dock's column. NULL without a Dock.
     */
    wlmtk_dock_t              *dock_side_ptr;
    /** Placement of the dock-apps attached to the Dock. */
    wlmaker_dockapps_t        *dock_dockapps_ptr;
    /**
     * The state's `Dock.Dockapps`: Dock-apps that go on the Dock when not
     * placed anywhere yet (eg. the system tray, right after its launchers).
     */
    bspl_array_t              *dock_default_dockapps_ptr;
    /** Where the Dock is. */
    wlmaker_dock_place_t      dock_place;
    /** Number of the Dock's launchers, after its main tile. */
    size_t                    dock_launchers;
    /** Number of dock-apps on the Dock last stored as `DockappTiles`. */
    long                      published_dock_tiles;
    /**
     * Empty tiles of the Dock's dock-apps, one per launcher of the Dock:
     * Room for the launchers that `wlmdock` shows there. Launchers and
     * dock-apps share the column, in any order.
     */
    wlmtk_tile_t              *dock_spacer_tiles_ptr;
    /** Number of tiles in @ref wlmaker_clip_t::dock_spacer_tiles_ptr. */
    size_t                    dock_spacer_tiles;
    /** Last published layout (see @ref wlmaker_dock_layout_fname). */
    char                      *published_layout_ptr;

    /** Levels of the Clip [0] and the Dock [1]. */
    wlmaker_dock_level_t      level[2];
    /** Whether the Clip [0] or the Dock [1] is raised, by auto-raising. */
    bool                      raised[2];
    /** Timers for auto-raising and lowering the Clip [0] and Dock [1]. */
    struct wl_event_source    *level_timer_ptr[2];

    /** Drawers of the Dock, in their order. */
    wlmaker_drawer_t          **drawers_ptr;
    /** Number of drawers. */
    size_t                    drawers;

    /** Number of the Clip's launchers that `wlmdock --clip` shows. */
    size_t                    clip_launchers;
    /**
     * The Clip's grid, as Window Maker's: The cells of its launchers (the
     * first @ref wlmaker_clip_t::clip_launchers) and of its dock-apps (in
     * their placement's order). As of the last arrangement.
     */
    wlmaker_clip_grid_t       grid;
    /** For each icon of the grid: Whether its cell was not stored. */
    bool                      *grid_derived_ptr;
    /** Lanes 1 .. of the grid. Lane 0 is the Clip's own dock. */
    wlmaker_clip_lane_t       *lanes_ptr;
    /** Number of lanes in @ref wlmaker_clip_t::lanes_ptr. */
    size_t                    lanes;
    /** Whether arranging: Changes on the way don't arrange again. */
    bool                      arranging;
};

static bool _wlmaker_clip_pointer_axis(
    wlmtk_element_t *element_ptr,
    struct wlr_pointer_axis_event *wlr_pointer_axis_event_ptr);
static bool _wlmaker_clip_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);

static bool _wlmaker_clip_update_tiles(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr);
static void _wlmaker_clip_apply_button_state(wlmaker_clip_t *clip_ptr);
static bool _wlmaker_clip_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr);
static void _wlmaker_clip_pointer_grab_cancel(wlmtk_element_t *element_ptr);
static void _wlmaker_clip_end_drag(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_apply_preview(void *data_ptr);
static int _wlmaker_clip_handle_sigusr1(int signal_number, void *data_ptr);
static bool _wlmaker_clip_spacer_init(
    wlmtk_tile_t *tile_ptr,
    const struct wlmtk_tile_style *tile_style_ptr);
static bool _wlmaker_clip_spacer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr);
static void _wlmaker_clip_arrange(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_arrange_grid(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_update_grid(wlmaker_clip_t *clip_ptr);
static bool _wlmaker_clip_grid_frame(
    wlmaker_clip_t *clip_ptr,
    int *x_ptr,
    int *y_ptr,
    struct wlr_box *output_ptr);
static void _wlmaker_clip_host_attach(void *ud_ptr, wlmtk_tile_t *tile_ptr);
static void _wlmaker_clip_host_detach(void *ud_ptr, wlmtk_tile_t *tile_ptr);
static bool _wlmaker_clip_pool_spacers(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t **tiles_ptr_ptr,
    size_t *n_ptr,
    size_t spacers,
    const struct wlmtk_tile_style *tile_style_ptr);
static void _wlmaker_clip_destroy_lane(
    wlmaker_clip_t *clip_ptr,
    wlmaker_clip_lane_t *lane_ptr);
static void _wlmaker_clip_replace_lanes(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_handle_dockapps_changed(void *ud_ptr);
static void _wlmaker_clip_publish_layout(wlmaker_clip_t *clip_ptr);
static bool _wlmaker_clip_set_spacers(
    wlmaker_clip_t *clip_ptr,
    bool dock,
    size_t spacers,
    const struct wlmtk_tile_style *tile_style_ptr);
static void _wlmaker_clip_atfork_child(void);
static bool _wlmaker_clip_create_dock_side(
    wlmaker_clip_t *clip_ptr,
    bspl_dict_t *state_dict_ptr,
    const wlmaker_config_style_t *style_ptr,
    struct wlr_output *wlr_output_ptr);
static void _wlmaker_clip_reload_launchers(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_update_dock_side(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_publish_dock_tiles(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_sync_drawers(
    wlmaker_clip_t *clip_ptr,
    bspl_array_t *launchers_ptr);
static wlmaker_dock_level_t _wlmaker_clip_decode_level(bspl_dict_t *dict_ptr);
static void _wlmaker_clip_balloon(wlmaker_clip_t *clip_ptr);
static void _wlmaker_clip_apply_level(wlmaker_clip_t *clip_ptr, bool dock);
static void _wlmaker_clip_move_panel(
    wlmaker_clip_t *clip_ptr,
    wlmtk_panel_t *panel_ptr,
    wlmtk_desktop_layer_t layer);
static bool _wlmaker_clip_pointer_on(wlmaker_clip_t *clip_ptr, bool dock);
static int _wlmaker_clip_handle_level_timer_clip(void *data_ptr);
static int _wlmaker_clip_handle_level_timer_dock(void *data_ptr);
static bool _wlmaker_clip_place_at_cursor(
    wlmaker_clip_t *clip_ptr,
    wlmaker_dock_place_t *place_ptr);
static void _wlmaker_clip_set_place(
    wlmtk_dock_t *dock_ptr,
    wlmaker_dock_place_t place);
static void _wlmaker_clip_open_menu(wlmaker_clip_t *clip_ptr);
static bool _wlmaker_clip_update_spacers(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr);
static bool _wlmaker_clip_update_overlay(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr,
    const wlmaker_config_clip_style_t *clip_style_ptr);
static bool _wlmaker_clip_update_image(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr);
static char *_wlmaker_clip_resolve_image(
    wlm_util_files_t *files_ptr,
    const char *icon_ptr);
static struct wlr_buffer *_wlmaker_clip_create_tile(
    const struct wlmtk_tile_style *tile_style_ptr,
    bool prev_pressed,
    bool next_pressed);

static void _wlmaker_clip_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_clip_handle_output_layout_change(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_clip_handle_pointer_motion(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_clip_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_clip_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_clip_signal_wlmdock_with(
    wlmaker_server_t *server_ptr,
    int signal_number);
static bool _wlmaker_clip_client_is_wlmdock(struct wl_client *wl_client_ptr);

/* == Data ================================================================= */

/** The clip's extension to @ref wlmtk_element_t virtual method table. */
static const wlmtk_element_vmt_t _wlmaker_clip_element_vmt = {
    .pointer_axis = _wlmaker_clip_pointer_axis,
    .pointer_button = _wlmaker_clip_pointer_button,
    .pointer_accepts_motion = _wlmaker_clip_pointer_accepts_motion,
    .pointer_grab_cancel = _wlmaker_clip_pointer_grab_cancel,
};

#ifndef WLMAKER_CLIP_DEFAULT_ICON
/** Clip icon configured at build time. */
#define WLMAKER_CLIP_DEFAULT_ICON ""
#endif

/** Distance the pointer must move with the button held, to start a drag. */
static const double _wlmaker_clip_drag_threshold = 8;

/** TODO: Replace this. */
typedef struct {
    /** Positioning data. */
    wlmtk_dock_positioning_t  positioning;
} parse_args;

/** Enum descriptor for `enum wlr_edges`. */
static const bspl_enum_desc_t _wlmaker_clip_edges[] = {
    BSPL_ENUM("TOP", WLR_EDGE_TOP),
    BSPL_ENUM("BOTTOM", WLR_EDGE_BOTTOM),
    BSPL_ENUM("LEFT", WLR_EDGE_LEFT),
    BSPL_ENUM("RIGHT", WLR_EDGE_RIGHT),
    BSPL_ENUM_SENTINEL(),
};

/** Descriptor for the clip's plist. */
const bspl_desc_t _wlmaker_clip_desc[] = {
    BSPL_DESC_ENUM("Edge", true, parse_args,
                   positioning.edge, positioning.edge,
                   WLR_EDGE_NONE, _wlmaker_clip_edges),
    BSPL_DESC_ENUM("Anchor", true, parse_args,
                   positioning.anchor, positioning.anchor,
                   WLR_EDGE_NONE, _wlmaker_clip_edges),
    BSPL_DESC_SENTINEL(),
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_clip_t *wlmaker_clip_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *state_dict_ptr,
    const wlmaker_config_style_t *style_ptr)
{
    wlmaker_clip_t *clip_ptr = logged_calloc(1, sizeof(wlmaker_clip_t));
    if (NULL == clip_ptr) return NULL;
    clip_ptr->server_ptr = server_ptr;
    clip_ptr->style = style_ptr->clip;

    parse_args args = {};
    bspl_dict_t *dict_ptr = bspl_dict_get_dict(state_dict_ptr, "Clip");
    if (NULL == dict_ptr) {
        bs_log(BS_ERROR, "No 'Clip' dict found in state.");
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }
    bspl_decode_dict(dict_ptr, _wlmaker_clip_desc, &args);
    bspl_dict_t *output_dict_ptr = bspl_dict_get_dict(dict_ptr, "Output");
    if (NULL != output_dict_ptr) {
        if (!wlmbe_output_description_init_from_plist(
                &clip_ptr->output_description, output_dict_ptr)) {
            wlmaker_clip_destroy(clip_ptr);
            return NULL;
        }
    }

    clip_ptr->wlmtk_dock_ptr = wlmtk_dock_create(
        &args.positioning, &style_ptr->dock);
    wlmtk_element_set_visible(
        wlmtk_dock_element(clip_ptr->wlmtk_dock_ptr),
        true);

    if (!wlmtk_tile_init(
            &clip_ptr->super_tile,
            &style_ptr->tile)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }
    clip_ptr->orig_super_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&clip_ptr->super_tile),
        &_wlmaker_clip_element_vmt);
    wlmtk_util_connect_listener_signal(
        &wlmtk_tile_element(&clip_ptr->super_tile)->events.pointer_motion,
        &clip_ptr->pointer_motion_listener,
        _wlmaker_clip_handle_pointer_motion);
    wlmtk_util_connect_listener_signal(
        &wlmtk_tile_element(&clip_ptr->super_tile)->events.pointer_leave,
        &clip_ptr->pointer_leave_listener,
        _wlmaker_clip_handle_pointer_leave);

    if (!_wlmaker_clip_update_tiles(clip_ptr, &style_ptr->tile)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }

    wlmtk_element_set_visible(
        wlmtk_tile_element(&clip_ptr->super_tile), true);
    wlmtk_tile_set_background_buffer(
        &clip_ptr->super_tile, clip_ptr->tile_buffer_ptr);
    wlmtk_dock_add_tile(clip_ptr->wlmtk_dock_ptr, &clip_ptr->super_tile);

    // The launchers `wlmdock --clip` shows (those with an icon). The room
    // for them comes with the grid, once the dock-apps are there too.
    bspl_array_t *launchers_ptr = wlmaker_dock_launchers_load(
        server_ptr, "Clip");
    clip_ptr->clip_launchers = (NULL != launchers_ptr) ?
        bspl_array_size(launchers_ptr) : 0;
    if (NULL != launchers_ptr) bspl_array_unref(launchers_ptr);

    if (!wlmtk_buffer_init(&clip_ptr->overlay_buffer)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&clip_ptr->overlay_buffer), true);

    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &clip_ptr->output_description, server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(
            server_ptr->wlr_output_layout_ptr);
    }

    wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
        server_ptr->desktop_ptr, WLMTK_DESKTOP_LAYER_TOP);
    if (!wlmtk_layer_add_panel(
            layer_ptr,
            wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr),
            wlr_output_ptr)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }

    // Resolves to a full path, and verifies the icon file exists.
    clip_ptr->image_path_ptr = _wlmaker_clip_resolve_image(
        server_ptr->files_ptr, bspl_dict_get_string_value(dict_ptr, "Icon"));
    if (NULL == clip_ptr->image_path_ptr) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }
    if (!_wlmaker_clip_update_image(clip_ptr, &style_ptr->tile)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }

    if (!_wlmaker_clip_update_overlay(
            clip_ptr, &clip_ptr->super_tile.style, &clip_ptr->style)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    };

    wlmtk_util_connect_listener_signal(
        &wlmtk_desktop_events(server_ptr->desktop_ptr)->workspace_changed,
        &clip_ptr->workspace_changed_listener,
        _wlmaker_clip_handle_workspace_changed);
    wlmtk_util_connect_listener_signal(
        &server_ptr->theme_changed_event,
        &clip_ptr->theme_changed_listener,
        _wlmaker_clip_handle_theme_changed);


    // TODO(kaeser@gubbe.ch): This is a very hacky way of updating the output
    // before the layer's handler removes all associated panels. Should be
    // a native method of wlmtk_dock_t or wlmtk_panel_t.
    //
    // Unfortunately, this also *ADDS* it back *BEFORE* the layer's handler
    // adds the associated output.
    clip_ptr->output_layout_change_listener.notify =
        _wlmaker_clip_handle_output_layout_change;
    wl_list_insert(
        server_ptr->wlr_output_layout_ptr->events.change.listener_list.next,
        &clip_ptr->output_layout_change_listener.link);

    // Dock-apps are placed in the order of the `Dockapps` array, or as
    // re-arranged by the user (stored in `Dockapps.plist`).
    clip_ptr->dockapps_fname_ptr = wlm_util_files_xdg_config_fname(
        server_ptr->files_ptr, "Dockapps.plist");
    clip_ptr->dockapps_ptr = wlmaker_dockapps_create(
        clip_ptr->wlmtk_dock_ptr,
        bspl_dict_get_array(dict_ptr, "Dockapps"),
        clip_ptr->dockapps_fname_ptr);
    if (NULL == clip_ptr->dockapps_ptr) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }
    server_ptr->dockapps_ptr = clip_ptr->dockapps_ptr;
    wlmaker_dockapps_set_changed_callback(
        clip_ptr->dockapps_ptr, _wlmaker_clip_handle_dockapps_changed,
        clip_ptr);
    // Its dock-apps are spread over the lanes of the grid.
    wlmaker_dockapps_host_t host = {
        .attach = _wlmaker_clip_host_attach,
        .detach = _wlmaker_clip_host_detach,
        .ud_ptr = clip_ptr };
    wlmaker_dockapps_set_host(clip_ptr->dockapps_ptr, &host);

    // Dock-apps can also be attached to the Dock, as on Window Maker.
    clip_ptr->published_dock_tiles = -1;
    clip_ptr->level[0] = _wlmaker_clip_decode_level(dict_ptr);
    clip_ptr->level[1] = _wlmaker_clip_decode_level(
        bspl_dict_get_dict(state_dict_ptr, "Dock"));
    if (!_wlmaker_clip_create_dock_side(
            clip_ptr, state_dict_ptr, style_ptr, wlr_output_ptr)) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }

    // `wlmdock --clip` signals when the Clip's launchers were changed. The
    // signal is blocked for regular delivery: Not for launched programs.
    clip_ptr->sigusr1_event_source_ptr = wl_event_loop_add_signal(
        wl_display_get_event_loop(server_ptr->wl_display_ptr),
        SIGUSR1,
        _wlmaker_clip_handle_sigusr1,
        clip_ptr);
    static bool atfork_registered = false;
    if (!atfork_registered &&
        0 == pthread_atfork(NULL, NULL, _wlmaker_clip_atfork_child)) {
        atfork_registered = true;
    }

    server_ptr->clip_dock_ptr = clip_ptr->wlmtk_dock_ptr;
    server_ptr->clip_ptr = clip_ptr;
    // The Dock's drawers.
    _wlmaker_clip_reload_launchers(clip_ptr);
    _wlmaker_clip_apply_level(clip_ptr, false);
    _wlmaker_clip_apply_level(clip_ptr, true);
    bs_log(BS_INFO, "Created clip %p", clip_ptr);
    return clip_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_destroy(wlmaker_clip_t *clip_ptr)
{
    if (NULL != clip_ptr->server_ptr) {
        wlmaker_context_menu_cancel(clip_ptr->server_ptr, clip_ptr);
    }
    // Going away: No more arranging.
    if (NULL != clip_ptr->dockapps_ptr) {
        wlmaker_dockapps_set_changed_callback(clip_ptr->dockapps_ptr, NULL, NULL);
    }
    if (NULL != clip_ptr->dock_dockapps_ptr) {
        wlmaker_dockapps_set_changed_callback(
            clip_ptr->dock_dockapps_ptr, NULL, NULL);
    }
    if (NULL != clip_ptr->published_layout_ptr) {
        free(clip_ptr->published_layout_ptr);
        clip_ptr->published_layout_ptr = NULL;
    }
    if (NULL != clip_ptr->preview_event_source_ptr) {
        wl_event_source_remove(clip_ptr->preview_event_source_ptr);
        clip_ptr->preview_event_source_ptr = NULL;
    }
    for (int i = 0; i < 2; ++i) {
        if (NULL != clip_ptr->level_timer_ptr[i]) {
            wl_event_source_remove(clip_ptr->level_timer_ptr[i]);
            clip_ptr->level_timer_ptr[i] = NULL;
        }
    }
    if (NULL != clip_ptr->sigusr1_event_source_ptr) {
        wl_event_source_remove(clip_ptr->sigusr1_event_source_ptr);
        clip_ptr->sigusr1_event_source_ptr = NULL;
    }
    if (NULL != clip_ptr->server_ptr) {
        clip_ptr->server_ptr->clip_dock_ptr = NULL;
        clip_ptr->server_ptr->dockapps_ptr = NULL;
        if (clip_ptr->server_ptr->clip_ptr == clip_ptr) {
            clip_ptr->server_ptr->clip_ptr = NULL;
        }
    }
    // The lanes of the grid: Their dock-apps back to the Clip's dock.
    for (size_t i = 0; i < clip_ptr->lanes; ++i) {
        _wlmaker_clip_destroy_lane(clip_ptr, &clip_ptr->lanes_ptr[i]);
    }
    if (NULL != clip_ptr->lanes_ptr) free(clip_ptr->lanes_ptr);
    clip_ptr->lanes_ptr = NULL;
    clip_ptr->lanes = 0;
    if (NULL != clip_ptr->grid.cells_ptr) free(clip_ptr->grid.cells_ptr);
    clip_ptr->grid = (wlmaker_clip_grid_t){};
    if (NULL != clip_ptr->grid_derived_ptr) free(clip_ptr->grid_derived_ptr);
    clip_ptr->grid_derived_ptr = NULL;
    // Shutting down: The drawers don't move their dock-apps to the Clip.
    for (size_t i = 0; i < clip_ptr->drawers; ++i) {
        wlmaker_drawer_destroy(clip_ptr->drawers_ptr[i]);
    }
    if (NULL != clip_ptr->drawers_ptr) free(clip_ptr->drawers_ptr);
    clip_ptr->drawers_ptr = NULL;
    clip_ptr->drawers = 0;
    if (NULL != clip_ptr->dockapps_ptr) {
        wlmaker_dockapps_destroy(clip_ptr->dockapps_ptr);
        clip_ptr->dockapps_ptr = NULL;
    }
    if (NULL != clip_ptr->dock_dockapps_ptr) {
        wlmaker_dockapps_destroy(clip_ptr->dock_dockapps_ptr);
        clip_ptr->dock_dockapps_ptr = NULL;
    }
    if (NULL != clip_ptr->dock_default_dockapps_ptr) {
        bspl_array_unref(clip_ptr->dock_default_dockapps_ptr);
        clip_ptr->dock_default_dockapps_ptr = NULL;
    }
    for (size_t i = 0; i < clip_ptr->dock_spacer_tiles; ++i) {
        wlmtk_tile_t *t_ptr = &clip_ptr->dock_spacer_tiles_ptr[i];
        if (NULL != wlmtk_tile_element(t_ptr)->parent_container_ptr) {
            wlmtk_dock_remove_tile(clip_ptr->dock_side_ptr, t_ptr);
        }
        wlmtk_tile_fini(t_ptr);
    }
    if (NULL != clip_ptr->dock_spacer_tiles_ptr) {
        free(clip_ptr->dock_spacer_tiles_ptr);
        clip_ptr->dock_spacer_tiles_ptr = NULL;
    }
    clip_ptr->dock_spacer_tiles = 0;
    if (NULL != clip_ptr->dock_side_ptr) {
        wlmtk_layer_t *l_ptr = wlmtk_panel_get_layer(
            wlmtk_dock_panel(clip_ptr->dock_side_ptr));
        if (NULL != l_ptr) {
            wlmtk_layer_remove_panel(
                l_ptr, wlmtk_dock_panel(clip_ptr->dock_side_ptr));
        }
        wlmtk_dock_destroy(clip_ptr->dock_side_ptr);
        clip_ptr->dock_side_ptr = NULL;
    }
    if (NULL != clip_ptr->dockapps_fname_ptr) {
        free(clip_ptr->dockapps_fname_ptr);
        clip_ptr->dockapps_fname_ptr = NULL;
    }
    for (size_t i = 0; i < clip_ptr->spacer_tiles; ++i) {
        wlmtk_tile_t *t_ptr = &clip_ptr->spacer_tiles_ptr[i];
        if (NULL != wlmtk_tile_element(t_ptr)->parent_container_ptr) {
            wlmtk_dock_remove_tile(clip_ptr->wlmtk_dock_ptr, t_ptr);
        }
        wlmtk_tile_fini(t_ptr);
    }
    if (NULL != clip_ptr->spacer_tiles_ptr) {
        free(clip_ptr->spacer_tiles_ptr);
        clip_ptr->spacer_tiles_ptr = NULL;
    }
    clip_ptr->spacer_tiles = 0;

    wlmtk_util_disconnect_listener(&clip_ptr->output_layout_change_listener);
    wlmtk_util_disconnect_listener(&clip_ptr->workspace_changed_listener);
    wlmtk_util_disconnect_listener(&clip_ptr->theme_changed_listener);

    if (wlmtk_tile_element(&clip_ptr->super_tile)->parent_container_ptr) {
        wlmtk_tile_set_content(&clip_ptr->super_tile, NULL);
        wlmtk_tile_set_overlay(&clip_ptr->super_tile, NULL);
        wlmtk_dock_remove_tile(
            clip_ptr->wlmtk_dock_ptr,
            &clip_ptr->super_tile);
    }
    wlmtk_util_disconnect_listener(&clip_ptr->pointer_leave_listener);
    wlmtk_util_disconnect_listener(&clip_ptr->pointer_motion_listener);
    wlmtk_tile_fini(&clip_ptr->super_tile);
    wlmtk_buffer_fini(&clip_ptr->overlay_buffer);

    if (NULL != clip_ptr->image_ptr) {
        wlmtk_image_destroy(clip_ptr->image_ptr);
        clip_ptr->image_ptr = NULL;
    }
    if (NULL != clip_ptr->image_path_ptr) {
        free(clip_ptr->image_path_ptr);
        clip_ptr->image_path_ptr = NULL;
    }

    if (NULL != clip_ptr->wlmtk_dock_ptr) {
        if (NULL != wlmtk_panel_get_layer(
                wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr))) {
            wlmtk_layer_remove_panel(
                wlmtk_panel_get_layer(wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr)),
                wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr));
        }
        wlmtk_dock_destroy(clip_ptr->wlmtk_dock_ptr);
        clip_ptr->wlmtk_dock_ptr = NULL;
    }

    if (NULL != clip_ptr->tile_buffer_ptr) {
        wlr_buffer_drop(clip_ptr->tile_buffer_ptr);
        clip_ptr->tile_buffer_ptr = NULL;
    }
    if (NULL != clip_ptr->prev_pressed_tile_buffer_ptr) {
        wlr_buffer_drop(clip_ptr->prev_pressed_tile_buffer_ptr);
        clip_ptr->prev_pressed_tile_buffer_ptr = NULL;
    }
    if (NULL != clip_ptr->next_pressed_tile_buffer_ptr) {
        wlr_buffer_drop(clip_ptr->next_pressed_tile_buffer_ptr);
        clip_ptr->next_pressed_tile_buffer_ptr = NULL;
    }

    wlmbe_output_description_fini(&clip_ptr->output_description);
    free(clip_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tells `wlmdock` processes (the Dock, the Clip's launchers) to re-read
 * the state file: Sends SIGUSR1. */
static void _wlmaker_clip_signal_wlmdock(wlmaker_server_t *server_ptr)
{
    _wlmaker_clip_signal_wlmdock_with(server_ptr, SIGUSR1);
}

/* ------------------------------------------------------------------------- */
/** Sends `signal_number` to the `wlmdock` processes. SIGHUP: New theme. */
static void _wlmaker_clip_signal_wlmdock_with(
    wlmaker_server_t *server_ptr,
    int signal_number)
{
    // wlmdock (the Dock, and the Clip's launchers) re-reads the state file
    // on SIGUSR1.
    struct wl_list *clients_ptr = wl_display_get_client_list(
        server_ptr->wl_display_ptr);
    struct wl_client *wl_client_ptr;
    wl_client_for_each(wl_client_ptr, clients_ptr) {
        if (!_wlmaker_clip_client_is_wlmdock(wl_client_ptr)) continue;
        pid_t pid = 0;
        wl_client_get_credentials(wl_client_ptr, &pid, NULL, NULL);
        if (1 < pid && 0 != kill(pid, signal_number)) {
            bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%"PRIdMAX
                   ", %d)", (intmax_t)pid, signal_number);
        } else if (1 < pid) {
            bs_log(BS_INFO, "Sent signal %d to wlmdock %"PRIdMAX,
                   signal_number, (intmax_t)pid);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Whether the client is a `wlmdock`: By the process name, or by the
 * executable. A wlmdock that started over for a new theme with
 * execv("/proc/self/exe") is named "exe" (older wlmdock); one whose
 * binary was replaced by an update has " (deleted)" after the path.
 */
static bool _wlmaker_clip_client_is_wlmdock(struct wl_client *wl_client_ptr)
{
    char *name_ptr = wlmaker_dockapps_client_name(wl_client_ptr);
    bool is_wlmdock = (NULL != name_ptr && 0 == strcmp(name_ptr, "wlmdock"));
    free(name_ptr);
    if (is_wlmdock) return true;

    pid_t pid = 0;
    wl_client_get_credentials(wl_client_ptr, &pid, NULL, NULL);
    if (1 >= pid) return false;
    char path[64], exe[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%"PRIdMAX"/exe", (intmax_t)pid);
    ssize_t len = readlink(path, exe, sizeof(exe) - 1);
    if (0 >= len) return false;
    exe[len] = '\0';
    static const char deleted[] = " (deleted)";
    size_t dlen = sizeof(deleted) - 1;
    if ((size_t)len > dlen && 0 == strcmp(exe + len - dlen, deleted)) {
        exe[len - dlen] = '\0';
    }
    const char *base_ptr = strrchr(exe, '/');
    base_ptr = (NULL != base_ptr) ? base_ptr + 1 : exe;
    return 0 == strcmp(base_ptr, "wlmdock");
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_move_dock(
    wlmaker_server_t *server_ptr,
    const char *section_ptr,
    wlmaker_dock_place_t place)
{
    bool is_clip = (0 == strcmp(section_ptr, "Clip"));
    const char *other_section_ptr = is_clip ? "Dock" : "Clip";

    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        server_ptr->files_ptr, server_ptr->state_fname_ptr);
    wlmaker_dock_place_t from = {}, other = {};
    bool have_from = wlmaker_dock_place_decode(
        bspl_dict_get_dict(state_dict_ptr, section_ptr), &from);
    bool have_other = wlmaker_dock_place_decode(
        bspl_dict_get_dict(state_dict_ptr, other_section_ptr), &other);
    bspl_dict_unref(state_dict_ptr);

    // The Clip is live: That's where it comes from, if it was dragged.
    if (is_clip && NULL != server_ptr->clip_dock_ptr) {
        const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
            server_ptr->clip_dock_ptr);
        if (!have_from) from = (wlmaker_dock_place_t){
                pos_ptr->edge, pos_ptr->anchor };
        have_from = true;
    }
    bool other_moved = have_from && have_other &&
        wlmaker_dock_place_resolve(place, from, &other);

    bs_log(BS_INFO, "Moving %s to corner 0x%"PRIx32"%s", section_ptr,
           wlmaker_dock_place_corner(place),
           other_moved ? ", swapping with the other dock" : "");
    bspl_dict_t *d = wlmaker_dock_place_encode(place);
    if (NULL != d) {
        wlmaker_state_store_section(server_ptr->files_ptr,
                                    server_ptr->state_fname_ptr,
                                    section_ptr, d);
        bspl_dict_unref(d);
    }
    if (other_moved) {
        d = wlmaker_dock_place_encode(other);
        if (NULL != d) {
            wlmaker_state_store_section(server_ptr->files_ptr,
                                        server_ptr->state_fname_ptr,
                                        other_section_ptr, d);
            bspl_dict_unref(d);
        }
    }

    if (NULL != server_ptr->clip_dock_ptr) {
        if (is_clip) _wlmaker_clip_set_place(server_ptr->clip_dock_ptr, place);
        if (!is_clip && other_moved) {
            _wlmaker_clip_set_place(server_ptr->clip_dock_ptr, other);
        }
    }
    // The Dock's dock-apps go along with the Dock.
    if (NULL != server_ptr->clip_ptr) {
        _wlmaker_clip_reload_launchers(server_ptr->clip_ptr);
    }

    _wlmaker_clip_signal_wlmdock(server_ptr);
}

/* ------------------------------------------------------------------------- */
struct wlr_output *wlmaker_clip_output(wlmaker_server_t *server_ptr)
{
    struct wlr_output *wlr_output_ptr = NULL;
    if (NULL != server_ptr->clip_ptr) {
        wlr_output_ptr = wlmbe_output_description_first_fnmatch(
            &server_ptr->clip_ptr->output_description,
            server_ptr->wlr_output_layout_ptr);
    }
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(server_ptr->wlr_output_layout_ptr);
    }
    return wlr_output_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_launchers_changed(wlmaker_server_t *server_ptr)
{
    if (NULL != server_ptr->clip_ptr) {
        _wlmaker_clip_reload_launchers(server_ptr->clip_ptr);
    }
    _wlmaker_clip_signal_wlmdock(server_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_dock_surface_changed(wlmaker_server_t *server_ptr)
{
    if (NULL != server_ptr->clip_ptr) {
        _wlmaker_clip_update_dock_side(server_ptr->clip_ptr);
        for (size_t i = 0; i < server_ptr->clip_ptr->drawers; ++i) {
            wlmaker_drawer_update_position(server_ptr->clip_ptr->drawers_ptr[i]);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** @return Whether `name_ptr` is in `names_ptr`, after index `i`. */
static bool _wlmaker_clip_name_after(
    bspl_array_t *names_ptr, size_t i, const char *name_ptr)
{
    if (NULL == name_ptr) return false;
    for (size_t j = i + 1; j < bspl_array_size(names_ptr); ++j) {
        const char *n = bspl_array_string_value_at(names_ptr, j);
        if (NULL != n && 0 == strcmp(n, name_ptr)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_add_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    const char *name_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return false;
    // Started (again) by the user: No longer removed.
    if (NULL != name_ptr &&
        wlmaker_dockapps_is_removed(clip_ptr->dockapps_fname_ptr, name_ptr)) {
        wlmaker_dockapps_set_removed(
            clip_ptr->dockapps_fname_ptr, name_ptr, false);
    }
    if (NULL != clip_ptr->dock_dockapps_ptr &&
        wlmaker_dockapps_has_name(clip_ptr->dock_dockapps_ptr, name_ptr)) {
        bool rv = wlmaker_dockapps_add(
            clip_ptr->dock_dockapps_ptr, tile_ptr, name_ptr);
        _wlmaker_clip_publish_dock_tiles(clip_ptr);
        return rv;
    }
    // In a drawer, where it was before.
    for (size_t i = 0; i < clip_ptr->drawers; ++i) {
        wlmaker_dockapps_t *d_ptr = wlmaker_drawer_dockapps(
            clip_ptr->drawers_ptr[i]);
        if (NULL != d_ptr && wlmaker_dockapps_has_name(d_ptr, name_ptr)) {
            return wlmaker_dockapps_add(d_ptr, tile_ptr, name_ptr);
        }
    }
    // Not placed yet, and meant for the Dock: There.
    for (size_t i = 0;
         NULL != name_ptr && NULL != clip_ptr->dock_dockapps_ptr &&
             NULL != clip_ptr->dock_default_dockapps_ptr &&
             !wlmaker_dockapps_has_name(clip_ptr->dockapps_ptr, name_ptr) &&
             i < bspl_array_size(clip_ptr->dock_default_dockapps_ptr);
         ++i) {
        const char *n = bspl_array_string_value_at(
            clip_ptr->dock_default_dockapps_ptr, i);
        if (NULL == n || 0 != strcmp(n, name_ptr)) continue;
        // In the order of `Dock.Dockapps`, whichever starts first: Before
        // the first placed one that comes later there.
        size_t index = wlmaker_dockapps_size(clip_ptr->dock_dockapps_ptr);
        for (size_t k = 0; k < index; ++k) {
            const char *placed_ptr = wlmaker_dockapps_name(
                clip_ptr->dock_dockapps_ptr,
                wlmaker_dockapps_tile_at(clip_ptr->dock_dockapps_ptr, k));
            if (_wlmaker_clip_name_after(
                    clip_ptr->dock_default_dockapps_ptr, i, placed_ptr)) {
                index = k;
                break;
            }
        }
        bool rv = wlmaker_dockapps_insert(
            clip_ptr->dock_dockapps_ptr, tile_ptr, name_ptr, index);
        _wlmaker_clip_publish_dock_tiles(clip_ptr);
        return rv;
    }
    return wlmaker_dockapps_add(clip_ptr->dockapps_ptr, tile_ptr, name_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_remove_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    if (NULL == d_ptr) return;
    wlmaker_dockapps_remove(d_ptr, tile_ptr);
    if (d_ptr == server_ptr->clip_ptr->dock_dockapps_ptr) {
        _wlmaker_clip_publish_dock_tiles(server_ptr->clip_ptr);
    }
}

/* ------------------------------------------------------------------------- */
wlmaker_dockapps_t *wlmaker_clip_dockapps_of(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return NULL;
    if (0 <= wlmaker_dockapps_index(clip_ptr->dockapps_ptr, tile_ptr)) {
        return clip_ptr->dockapps_ptr;
    }
    if (NULL != clip_ptr->dock_dockapps_ptr &&
        0 <= wlmaker_dockapps_index(clip_ptr->dock_dockapps_ptr, tile_ptr)) {
        return clip_ptr->dock_dockapps_ptr;
    }
    for (size_t i = 0; i < clip_ptr->drawers; ++i) {
        wlmaker_dockapps_t *d_ptr = wlmaker_drawer_dockapps(
            clip_ptr->drawers_ptr[i]);
        if (NULL != d_ptr && 0 <= wlmaker_dockapps_index(d_ptr, tile_ptr)) {
            return d_ptr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_dockapps_column_of(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    if (NULL == d_ptr) return -1;
    for (int id = 0; id < wlmaker_clip_columns(server_ptr); ++id) {
        if (wlmaker_clip_dockapps_id(server_ptr, id) == d_ptr) return id;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
wlmaker_dockapps_t *wlmaker_clip_dockapps_id(
    wlmaker_server_t *server_ptr,
    int id)
{
    if (NULL == server_ptr->clip_ptr) return NULL;
    if (WLMAKER_COLUMN_CLIP == id) return server_ptr->clip_ptr->dockapps_ptr;
    if (WLMAKER_COLUMN_DOCK == id) return server_ptr->clip_ptr->dock_dockapps_ptr;
    wlmaker_drawer_t *drawer_ptr = wlmaker_clip_drawer(server_ptr, id);
    return NULL != drawer_ptr ? wlmaker_drawer_dockapps(drawer_ptr) : NULL;
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_columns(wlmaker_server_t *server_ptr)
{
    if (NULL == server_ptr->clip_ptr) return 0;
    return 2 + (int)server_ptr->clip_ptr->drawers;
}

/* ------------------------------------------------------------------------- */
wlmaker_drawer_t *wlmaker_clip_drawer(wlmaker_server_t *server_ptr, int id)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr || 2 > id || (size_t)(id - 2) >= clip_ptr->drawers) {
        return NULL;
    }
    return clip_ptr->drawers_ptr[id - 2];
}

/* ------------------------------------------------------------------------- */
wlmaker_drawer_t *wlmaker_clip_drawer_at(
    wlmaker_server_t *server_ptr,
    size_t index)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    for (size_t i = 0; NULL != clip_ptr && i < clip_ptr->drawers; ++i) {
        if (wlmaker_drawer_launcher_index(clip_ptr->drawers_ptr[i]) == index) {
            return clip_ptr->drawers_ptr[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_drawer_column(
    wlmaker_server_t *server_ptr,
    wlmaker_drawer_t *drawer_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    for (size_t i = 0; NULL != clip_ptr && i < clip_ptr->drawers; ++i) {
        if (clip_ptr->drawers_ptr[i] == drawer_ptr) return 2 + i;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
wlmaker_dockapps_t *wlmaker_clip_dockapps(
    wlmaker_server_t *server_ptr,
    bool dock)
{
    return wlmaker_clip_dockapps_id(
        server_ptr, dock ? WLMAKER_COLUMN_DOCK : WLMAKER_COLUMN_CLIP);
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_move_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    int column,
    size_t index)
{
    wlmaker_dockapps_t *from_ptr = wlmaker_clip_dockapps_of(
        server_ptr, tile_ptr);
    wlmaker_dockapps_t *to_ptr = wlmaker_clip_dockapps_id(server_ptr, column);
    if (NULL == from_ptr || NULL == to_ptr) return;
    if (from_ptr == to_ptr) {
        wlmaker_dockapps_move_to(to_ptr, tile_ptr, index);
        return;
    }
    const char *n_ptr = wlmaker_dockapps_name(from_ptr, tile_ptr);
    char *name_ptr = NULL != n_ptr ? logged_strdup(n_ptr) : NULL;
    wlmaker_dockapps_detach(from_ptr, tile_ptr);
    wlmaker_dockapps_insert(to_ptr, tile_ptr, name_ptr, index);
    if (NULL != name_ptr) free(name_ptr);
    _wlmaker_clip_publish_dock_tiles(server_ptr->clip_ptr);
}

/* ------------------------------------------------------------------------- */
const wlmaker_clip_grid_t *wlmaker_clip_grid(
    wlmaker_server_t *server_ptr,
    size_t *launchers_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return NULL;
    if (NULL != launchers_ptr) *launchers_ptr = clip_ptr->clip_launchers;
    return &clip_ptr->grid;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_clip_grid_icon_at_point(
    wlmaker_server_t *server_ptr,
    double x,
    double y,
    int *tile_x_ptr,
    int *tile_y_ptr)
{
    wlmaker_dock_column_t column;
    if (!wlmaker_clip_column_id(server_ptr, WLMAKER_COLUMN_CLIP, &column) ||
        0 >= column.pitch) return SIZE_MAX;
    const wlmaker_clip_grid_t *g_ptr = &server_ptr->clip_ptr->grid;
    for (size_t i = 0; i < g_ptr->n; ++i) {
        int cx, cy;
        wlmaker_clip_cell_position(column.place, column.x, column.y,
                                   column.pitch, &g_ptr->cells_ptr[i],
                                   &cx, &cy);
        struct wlr_box b = { .x = cx, .y = cy,
                             .width = column.size, .height = column.size };
        if (!wlr_box_contains_point(&b, x, y)) continue;
        if (NULL != tile_x_ptr) *tile_x_ptr = x - cx;
        if (NULL != tile_y_ptr) *tile_y_ptr = y - cy;
        return i;
    }
    return SIZE_MAX;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_grid_avoid(
    wlmaker_server_t *server_ptr,
    struct wlr_box *box_ptr,
    const struct wlr_box *area_ptr)
{
    wlmaker_dock_column_t column;
    if (!wlmaker_clip_column_id(server_ptr, WLMAKER_COLUMN_CLIP, &column) ||
        0 >= column.pitch) return false;
    const wlmaker_clip_grid_t *g_ptr = &server_ptr->clip_ptr->grid;
    bool vertical = wlmaker_dock_place_vertical(column.place);
    bool far = vertical ? WLR_EDGE_RIGHT == column.place.edge :
        WLR_EDGE_BOTTOM == column.place.edge;
    struct wlr_box b = *box_ptr;
    bool moved = false;
    // Past each icon it covers, away from the Clip's edge.
    for (size_t round = 0; round <= g_ptr->n; ++round) {
        bool hit = false;
        for (size_t i = 0; i < g_ptr->n; ++i) {
            int cx, cy;
            wlmaker_clip_cell_position(column.place, column.x, column.y,
                                       column.pitch, &g_ptr->cells_ptr[i],
                                       &cx, &cy);
            struct wlr_box c = { .x = cx, .y = cy,
                                 .width = column.size, .height = column.size };
            struct wlr_box isect;
            if (!wlr_box_intersection(&isect, &c, &b)) continue;
            hit = true;
            int gap = column.pitch - column.size;
            if (vertical) {
                b.x = far ? c.x - gap - b.width : c.x + c.width + gap;
            } else {
                b.y = far ? c.y - gap - b.height : c.y + c.height + gap;
            }
        }
        if (!hit) break;
        moved = true;
    }
    // Off the area: Better over the icons than off the screen.
    if (!moved || (NULL != area_ptr &&
                   (b.x < area_ptr->x || b.y < area_ptr->y ||
                    b.x + b.width > area_ptr->x + area_ptr->width ||
                    b.y + b.height > area_ptr->y + area_ptr->height))) {
        return false;
    }
    *box_ptr = b;
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_freeze_grid(wlmaker_server_t *server_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr || NULL == clip_ptr->dockapps_ptr) return;
    _wlmaker_clip_update_grid(clip_ptr);
    const wlmaker_clip_grid_t *g_ptr = &clip_ptr->grid;
    size_t nl = clip_ptr->clip_launchers;
    if (0 == g_ptr->n || NULL == clip_ptr->grid_derived_ptr) return;
    // Copies: Storing re-arranges, and replaces the grid.
    size_t n = g_ptr->n;
    wlmaker_clip_cell_t *cells_ptr = logged_calloc(
        n, sizeof(wlmaker_clip_cell_t));
    bool *derived_ptr = logged_calloc(n, sizeof(bool));
    wlmaker_dockapps_cell_update_t *u_ptr = logged_calloc(
        n, sizeof(wlmaker_dockapps_cell_update_t));
    if (NULL != cells_ptr && NULL != derived_ptr && NULL != u_ptr) {
        memcpy(cells_ptr, g_ptr->cells_ptr, n * sizeof(wlmaker_clip_cell_t));
        memcpy(derived_ptr, clip_ptr->grid_derived_ptr, n * sizeof(bool));
        bool any = false;
        for (size_t i = 0; i < nl; ++i) any |= derived_ptr[i];
        // Only those without a cell: A cell stored but not shown now (eg.
        // off a smaller screen) stays, for when it fits again.
        if (any) {
            wlmaker_dock_launchers_set_cells(
                server_ptr, cells_ptr, derived_ptr, nl);
        }
        size_t k = 0;
        for (size_t j = 0; nl + j < n; ++j) {
            if (!derived_ptr[nl + j]) continue;
            const char *name_ptr = wlmaker_dockapps_name(
                clip_ptr->dockapps_ptr,
                wlmaker_dockapps_tile_at(clip_ptr->dockapps_ptr, j));
            if (NULL == name_ptr) continue;
            u_ptr[k++] = (wlmaker_dockapps_cell_update_t){
                .name_ptr = name_ptr, .has_cell = true,
                .cell = cells_ptr[nl + j] };
        }
        if (0 < k) wlmaker_dockapps_set_cells(clip_ptr->dockapps_ptr, u_ptr, k);
    }
    if (NULL != u_ptr) free(u_ptr);
    if (NULL != derived_ptr) free(derived_ptr);
    if (NULL != cells_ptr) free(cells_ptr);
    _wlmaker_clip_arrange(clip_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_push_icons(
    wlmaker_server_t *server_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr || NULL == clip_ptr->dockapps_ptr) return false;
    _wlmaker_clip_update_grid(clip_ptr);
    const wlmaker_clip_grid_t *g_ptr = &clip_ptr->grid;
    size_t n = g_ptr->n, nl = clip_ptr->clip_launchers;
    if (0 == n || nl > n) return false;
    wlmaker_clip_cell_t *cells_ptr = logged_calloc(
        n, sizeof(wlmaker_clip_cell_t));
    bool *set_ptr = logged_calloc(n, sizeof(bool));
    wlmaker_dockapps_cell_update_t *u_ptr = logged_calloc(
        n, sizeof(wlmaker_dockapps_cell_update_t));
    bool pushed = NULL != cells_ptr && NULL != set_ptr && NULL != u_ptr &&
        wlmaker_clip_grid_push(g_ptr, cell_ptr, own, cells_ptr);
    if (pushed) {
        // All moved at once (the one moved there too): Storing re-arranges
        // and replaces the grid, so the dock-apps' names come first.
        bool any = false;
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            set_ptr[i] = cells_ptr[i].lane != g_ptr->cells_ptr[i].lane ||
                cells_ptr[i].slot != g_ptr->cells_ptr[i].slot;
            if (!set_ptr[i]) continue;
            if (i < nl) {
                any = true;
                continue;
            }
            const char *name_ptr = wlmaker_dockapps_name(
                clip_ptr->dockapps_ptr,
                wlmaker_dockapps_tile_at(clip_ptr->dockapps_ptr, i - nl));
            if (NULL == name_ptr) continue;
            u_ptr[k++] = (wlmaker_dockapps_cell_update_t){
                .name_ptr = name_ptr, .has_cell = true,
                .cell = cells_ptr[i] };
        }
        if (0 < k) wlmaker_dockapps_set_cells(clip_ptr->dockapps_ptr, u_ptr, k);
        if (any) {
            wlmaker_dock_launchers_set_cells(server_ptr, cells_ptr, set_ptr, nl);
        }
    }
    if (NULL != u_ptr) free(u_ptr);
    if (NULL != set_ptr) free(set_ptr);
    if (NULL != cells_ptr) free(cells_ptr);
    _wlmaker_clip_arrange(clip_ptr);
    return pushed;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_place_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    const wlmaker_clip_cell_t *cell_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return;
    wlmaker_dockapps_t *from_ptr = wlmaker_clip_dockapps_of(
        server_ptr, tile_ptr);
    if (NULL == from_ptr) return;
    const char *n_ptr = wlmaker_dockapps_name(from_ptr, tile_ptr);
    char *name_ptr = NULL != n_ptr ? logged_strdup(n_ptr) : NULL;
    // Its cell first: Attached, it goes right there.
    wlmaker_dockapps_cell_update_t u = {
        .name_ptr = name_ptr, .has_cell = true, .cell = *cell_ptr };
    if (NULL != name_ptr) wlmaker_dockapps_set_cells(clip_ptr->dockapps_ptr,
                                                     &u, 1);
    if (from_ptr != clip_ptr->dockapps_ptr) {
        wlmaker_dockapps_detach(from_ptr, tile_ptr);
        wlmaker_dockapps_insert(
            clip_ptr->dockapps_ptr, tile_ptr, name_ptr,
            wlmaker_dockapps_size(clip_ptr->dockapps_ptr));
        _wlmaker_clip_publish_dock_tiles(clip_ptr);
    }
    if (NULL != name_ptr) free(name_ptr);
    _wlmaker_clip_arrange(clip_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_column_dockapp_at(
    const wlmaker_dock_column_t *column_ptr,
    size_t position,
    size_t *index_ptr)
{
    size_t launchers = 0, dockapps = 0;
    for (size_t p = 0; p <= position; ++p) {
        // As many of each as there are: Beyond, the other kind.
        bool dockapp = (64 > p) ?
            0 != (column_ptr->dockapp_positions & (UINT64_C(1) << p)) :
            launchers >= column_ptr->launchers;
        if (dockapp && dockapps >= column_ptr->dockapps) dockapp = false;
        if (!dockapp && launchers >= column_ptr->launchers &&
            dockapps < column_ptr->dockapps) dockapp = true;
        if (p == position) {
            if (NULL != index_ptr) *index_ptr = dockapp ? dockapps : launchers;
            return dockapp;
        }
        if (dockapp) {
            ++dockapps;
        } else {
            ++launchers;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_dock_column_launcher_position(
    const wlmaker_dock_column_t *column_ptr,
    size_t index)
{
    size_t n = column_ptr->launchers + column_ptr->dockapps;
    for (size_t p = 0; p < n; ++p) {
        size_t i;
        if (!wlmaker_dock_column_dockapp_at(column_ptr, p, &i) && i == index) {
            return p;
        }
    }
    return index + column_ptr->dockapps;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_dock_column_dockapp_position(
    const wlmaker_dock_column_t *column_ptr,
    size_t index)
{
    size_t n = column_ptr->launchers + column_ptr->dockapps;
    for (size_t p = 0; p < n; ++p) {
        size_t i;
        if (wlmaker_dock_column_dockapp_at(column_ptr, p, &i) && i == index) {
            return p;
        }
    }
    return column_ptr->launchers + index;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_column_set_sections(wlmaker_dock_column_t *column_ptr)
{
    column_ptr->dockapp_positions = 0;
    for (size_t p = column_ptr->launchers;
         p < column_ptr->launchers + column_ptr->dockapps && 64 > p;
         ++p) {
        column_ptr->dockapp_positions |= UINT64_C(1) << p;
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_launcher_inserted(
    wlmaker_server_t *server_ptr,
    int column,
    size_t index,
    bool removed)
{
    // The Clip's icons keep their cells: Nothing moves.
    if (WLMAKER_COLUMN_DOCK != column) return;
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_id(server_ptr, column);
    if (NULL == d_ptr) return;
    if (removed) {
        wlmaker_dockapps_launcher_removed(d_ptr, index);
    } else {
        wlmaker_dockapps_launcher_inserted(d_ptr, index);
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_freeze_dockapps(wlmaker_server_t *server_ptr, int column)
{
    if (WLMAKER_COLUMN_CLIP == column) {
        wlmaker_clip_freeze_grid(server_ptr);
        return;
    }
    if (WLMAKER_COLUMN_DOCK != column) return;
    wlmaker_dock_column_t c;
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_id(server_ptr, column);
    if (NULL == d_ptr || !wlmaker_clip_column_id(server_ptr, column, &c)) return;
    wlmaker_dockapps_freeze_after(d_ptr, c.launchers);
}

/* ------------------------------------------------------------------------- */
/**
 * @return Whether the dock-app of `tile_ptr` is in the Dock or the Clip,
 *     with its column in `column_ptr` and its position in `position_ptr`.
 */
static bool _wlmaker_clip_dockapp_place(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    wlmaker_dock_column_t *column_ptr,
    size_t *position_ptr)
{
    int id = wlmaker_clip_dockapps_column_of(server_ptr, tile_ptr);
    if ((WLMAKER_COLUMN_CLIP != id && WLMAKER_COLUMN_DOCK != id) ||
        !wlmaker_clip_column_id(server_ptr, id, column_ptr)) return false;
    int index = wlmaker_dockapps_index(
        wlmaker_clip_dockapps_id(server_ptr, id), tile_ptr);
    if (0 > index) return false;
    *position_ptr = wlmaker_dock_column_dockapp_position(column_ptr, index);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * For a dock-app of the Clip: Its icon on the grid, and the cell next to it
 * towards `direction`, with the icon there (or SIZE_MAX).
 * @return Whether it can move there: The cell is on the output, and free
 *     next to another icon (than itself), or holds another icon (they swap).
 */
static bool _wlmaker_clip_grid_move_target(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    uint32_t direction,
    size_t *icon_ptr,
    wlmaker_clip_cell_t *cell_ptr,
    size_t *other_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr ||
        wlmaker_clip_dockapps_of(server_ptr, tile_ptr) != clip_ptr->dockapps_ptr) {
        return false;
    }
    int index = wlmaker_dockapps_index(clip_ptr->dockapps_ptr, tile_ptr);
    size_t icon = clip_ptr->clip_launchers + (size_t)index;
    if (0 > index || icon >= clip_ptr->grid.n) return false;
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        clip_ptr->wlmtk_dock_ptr);
    wlmaker_clip_cell_t c = wlmaker_clip_cell_towards(
        (wlmaker_dock_place_t){ pos_ptr->edge, pos_ptr->anchor },
        &clip_ptr->grid.cells_ptr[icon], direction);
    *icon_ptr = icon;
    *cell_ptr = c;
    *other_ptr = wlmaker_clip_grid_icon_at(&clip_ptr->grid, &c, icon);
    if (!wlmaker_clip_grid_contains(&clip_ptr->grid, &c) ||
        (0 == c.lane && 0 == c.slot)) return false;
    if (SIZE_MAX != *other_ptr) return true;
    return WLMAKER_CLIP_GRID_ATTACH ==
        wlmaker_clip_grid_snap(&clip_ptr->grid, &c, icon);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_move_dockapp_towards(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    uint32_t direction)
{
    size_t icon, other;
    wlmaker_clip_cell_t cell;
    if (!_wlmaker_clip_grid_move_target(server_ptr, tile_ptr, direction,
                                        &icon, &cell, &other)) {
        return false;
    }
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    wlmaker_clip_freeze_grid(server_ptr);
    wlmaker_clip_cell_t from = clip_ptr->grid.cells_ptr[icon];
    size_t nl = clip_ptr->clip_launchers;
    if (SIZE_MAX != other && other < nl) {
        // A launcher: It takes the dock-app's cell.
        wlmaker_clip_cell_t *cells_ptr = logged_calloc(
            nl, sizeof(wlmaker_clip_cell_t));
        bool *set_ptr = logged_calloc(nl, sizeof(bool));
        if (NULL != cells_ptr && NULL != set_ptr) {
            memcpy(cells_ptr, clip_ptr->grid.cells_ptr,
                   nl * sizeof(wlmaker_clip_cell_t));
            cells_ptr[other] = from;
            set_ptr[other] = true;
            wlmaker_dock_launchers_set_cells(server_ptr, cells_ptr, set_ptr, nl);
        }
        if (NULL != set_ptr) free(set_ptr);
        if (NULL != cells_ptr) free(cells_ptr);
    }
    wlmaker_dockapps_cell_update_t u[2] = {};
    size_t n = 0;
    u[n++] = (wlmaker_dockapps_cell_update_t){
        .name_ptr = wlmaker_dockapps_name(clip_ptr->dockapps_ptr, tile_ptr),
        .has_cell = true, .cell = cell };
    if (SIZE_MAX != other && other >= nl) {
        // Another dock-app: They swap.
        u[n++] = (wlmaker_dockapps_cell_update_t){
            .name_ptr = wlmaker_dockapps_name(
                clip_ptr->dockapps_ptr,
                wlmaker_dockapps_tile_at(clip_ptr->dockapps_ptr, other - nl)),
            .has_cell = true, .cell = from };
    }
    wlmaker_dockapps_set_cells(clip_ptr->dockapps_ptr, u, n);
    _wlmaker_clip_arrange(clip_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_dockapp_delta_towards(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    uint32_t direction)
{
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    if (NULL == d_ptr) return 0;
    // On the Clip's grid: In all four directions, cell by cell.
    if (NULL != server_ptr->clip_ptr &&
        d_ptr == server_ptr->clip_ptr->dockapps_ptr) {
        size_t icon, other;
        wlmaker_clip_cell_t cell;
        return _wlmaker_clip_grid_move_target(
            server_ptr, tile_ptr, direction, &icon, &cell, &other) ? 1 : 0;
    }
    wlmaker_dock_column_t column;
    size_t position;
    if (!_wlmaker_clip_dockapp_place(server_ptr, tile_ptr, &column,
                                     &position)) {
        return wlmaker_dockapps_delta_towards(d_ptr, tile_ptr, direction);
    }
    bool vertical = wlmaker_dock_place_vertical(column.place);
    int delta = 0;
    if (vertical && WLR_EDGE_TOP == direction) delta = -1;
    if (vertical && WLR_EDGE_BOTTOM == direction) delta = 1;
    if (!vertical && WLR_EDGE_LEFT == direction) delta = -1;
    if (!vertical && WLR_EDGE_RIGHT == direction) delta = 1;
    // Positions count away from the anchor.
    if (WLR_EDGE_BOTTOM == column.place.anchor ||
        WLR_EDGE_RIGHT == column.place.anchor) delta = -delta;
    int to = (int)position + delta;
    if (0 > to || (int)(column.launchers + column.dockapps) <= to) return 0;
    return delta;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_move_dockapp_by(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    int delta)
{
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    if (NULL == d_ptr || 0 == delta) return;
    wlmaker_dock_column_t column;
    size_t position;
    if (!_wlmaker_clip_dockapp_place(server_ptr, tile_ptr, &column,
                                     &position)) {
        wlmaker_dockapps_move(d_ptr, tile_ptr, delta);
        return;
    }
    int step = 0 < delta ? 1 : -1;
    wlmaker_dockapps_freeze_after(d_ptr, column.launchers);
    for (int i = 0; i != delta; i += step) {
        if (!wlmaker_clip_column_id(server_ptr, column.id, &column)) return;
        int index = wlmaker_dockapps_index(d_ptr, tile_ptr);
        if (0 > index) return;
        int to = (int)wlmaker_dock_column_dockapp_position(&column, index) +
            step;
        if (0 > to || (int)(column.launchers + column.dockapps) <= to) return;
        if (wlmaker_dock_column_dockapp_at(&column, to, NULL)) {
            // Past a dock-app: They swap, after the same launchers.
            wlmaker_dockapps_move(d_ptr, tile_ptr, step);
        } else {
            // Past a launcher.
            size_t after = wlmaker_dockapps_after_launchers(d_ptr, tile_ptr);
            wlmaker_dockapps_set_after_launchers(
                d_ptr, tile_ptr, 0 < step ? after + 1 : after - 1);
        }
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_set_dockapp_after(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t launchers)
{
    if (NULL == server_ptr->clip_ptr) return;
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    if (d_ptr != server_ptr->clip_ptr->dockapps_ptr &&
        d_ptr != server_ptr->clip_ptr->dock_dockapps_ptr) return;
    wlmaker_dockapps_set_after_launchers(d_ptr, tile_ptr, launchers);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_column(
    wlmaker_server_t *server_ptr,
    bool dock,
    wlmaker_dock_column_t *column_ptr)
{
    return wlmaker_clip_column_id(
        server_ptr, dock ? WLMAKER_COLUMN_DOCK : WLMAKER_COLUMN_CLIP,
        column_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_column_id(
    wlmaker_server_t *server_ptr,
    int id,
    wlmaker_dock_column_t *column_ptr)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return false;
    if (2 <= id) {
        wlmaker_drawer_t *drawer_ptr = wlmaker_clip_drawer(server_ptr, id);
        if (NULL == drawer_ptr ||
            !wlmaker_drawer_column(drawer_ptr, column_ptr)) return false;
        column_ptr->id = id;
        return true;
    }
    bool dock = WLMAKER_COLUMN_DOCK == id;
    const struct wlmtk_tile_style *ts_ptr = &clip_ptr->super_tile.style;
    *column_ptr = (wlmaker_dock_column_t){
        .id = id,
        .size = ts_ptr->size,
        .pitch = ts_ptr->size + server_ptr->style_ptr->dock.margin.width };

    struct wlr_output *wlr_output_ptr = wlmbe_primary_output(
        server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) return false;
    wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr,
                              wlr_output_ptr, &column_ptr->output);

    if (!dock) {
        const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
            clip_ptr->wlmtk_dock_ptr);
        column_ptr->place = (wlmaker_dock_place_t){
            pos_ptr->edge, pos_ptr->anchor };
        wlmtk_element_get_absolute_position(
            wlmtk_tile_element(&clip_ptr->super_tile),
            &column_ptr->x, &column_ptr->y);
        column_ptr->launchers = clip_ptr->clip_launchers;
        column_ptr->dockapps = wlmaker_dockapps_size(clip_ptr->dockapps_ptr);
        column_ptr->grid = true;
        column_ptr->dockapp_positions = wlmaker_dockapps_positions(
            clip_ptr->dockapps_ptr, column_ptr->launchers);
        column_ptr->mixed = true;
        return true;
    }

    if (NULL == clip_ptr->dock_side_ptr) return false;
    column_ptr->place = clip_ptr->dock_place;
    column_ptr->launchers = clip_ptr->dock_launchers;
    for (size_t i = 0; i < clip_ptr->drawers; ++i) {
        size_t index = wlmaker_drawer_launcher_index(clip_ptr->drawers_ptr[i]);
        if (64 > index) column_ptr->drawers |= UINT64_C(1) << index;
    }
    column_ptr->dockapps = wlmaker_dockapps_size(clip_ptr->dock_dockapps_ptr);
    column_ptr->dockapp_positions = wlmaker_dockapps_positions(
        clip_ptr->dock_dockapps_ptr, column_ptr->launchers);
    column_ptr->mixed = true;
    struct wlr_box b;
    if (NULL != server_ptr->dock_layer_panel_ptr &&
        wlmaker_layer_panel_get_box(server_ptr->dock_layer_panel_ptr, &b)) {
        column_ptr->x = b.x;
        column_ptr->y = b.y;
        if (WLR_EDGE_BOTTOM == column_ptr->place.anchor) {
            column_ptr->y = b.y + b.height - column_ptr->size;
        }
        if (WLR_EDGE_RIGHT == column_ptr->place.anchor) {
            column_ptr->x = b.x + b.width - column_ptr->size;
        }
        return true;
    }
    // No Dock surface (yet): Its corner of the output.
    const struct wlr_box *o_ptr = &column_ptr->output;
    uint32_t corner = wlmaker_dock_place_corner(column_ptr->place);
    column_ptr->x = (corner & WLR_EDGE_RIGHT) ?
        o_ptr->x + o_ptr->width - column_ptr->size : o_ptr->x;
    column_ptr->y = (corner & WLR_EDGE_BOTTOM) ?
        o_ptr->y + o_ptr->height - column_ptr->size : o_ptr->y;
    return true;
}

/* ------------------------------------------------------------------------- */
wlmaker_dock_level_t wlmaker_clip_get_level(
    wlmaker_server_t *server_ptr,
    bool dock)
{
    if (NULL == server_ptr->clip_ptr) return WLMAKER_DOCK_LEVEL_KEEP_ON_TOP;
    return server_ptr->clip_ptr->level[dock ? 1 : 0];
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_set_level(
    wlmaker_server_t *server_ptr,
    bool dock,
    wlmaker_dock_level_t level)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return;
    int i = dock ? 1 : 0;
    clip_ptr->level[i] = level;
    clip_ptr->raised[i] = false;

    // As Window Maker's WMState: `Lowered` and `AutoRaiseLower`.
    bspl_dict_t *d = bspl_dict_create();
    bspl_string_t *lowered_ptr = bspl_string_create(
        WLMAKER_DOCK_LEVEL_KEEP_ON_TOP == level ? "No" : "Yes");
    bspl_string_t *auto_ptr = bspl_string_create(
        WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER == level ? "Yes" : "No");
    if (NULL != d && NULL != lowered_ptr && NULL != auto_ptr &&
        bspl_dict_add(d, "Lowered", bspl_object_from_string(lowered_ptr)) &&
        bspl_dict_add(d, "AutoRaiseLower", bspl_object_from_string(auto_ptr))) {
        wlmaker_state_store_section(server_ptr->files_ptr,
                                    server_ptr->state_fname_ptr,
                                    dock ? "Dock" : "Clip", d);
    }
    if (NULL != auto_ptr) bspl_string_unref(auto_ptr);
    if (NULL != lowered_ptr) bspl_string_unref(lowered_ptr);
    if (NULL != d) bspl_dict_unref(d);
    _wlmaker_clip_apply_level(clip_ptr, dock);
}

/* ------------------------------------------------------------------------- */
wlmtk_desktop_layer_t wlmaker_clip_layer(
    wlmaker_server_t *server_ptr,
    bool dock)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    if (NULL == clip_ptr) return WLMTK_DESKTOP_LAYER_TOP;
    int i = dock ? 1 : 0;
    if (WLMAKER_DOCK_LEVEL_KEEP_ON_TOP == clip_ptr->level[i] ||
        clip_ptr->raised[i]) return WLMTK_DESKTOP_LAYER_TOP;
    return WLMTK_DESKTOP_LAYER_BOTTOM;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_dock_hovered(
    wlmaker_server_t *server_ptr,
    bool dock)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    int i = dock ? 1 : 0;
    if (NULL == clip_ptr ||
        WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER != clip_ptr->level[i]) return;
    if (clip_ptr->raised[i]) {
        // Stays raised.
        if (NULL != clip_ptr->level_timer_ptr[i]) {
            wl_event_source_timer_update(clip_ptr->level_timer_ptr[i], 0);
        }
        return;
    }
    if (NULL == clip_ptr->level_timer_ptr[i]) {
        clip_ptr->level_timer_ptr[i] = wl_event_loop_add_timer(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            dock ? _wlmaker_clip_handle_level_timer_dock :
            _wlmaker_clip_handle_level_timer_clip,
            clip_ptr);
        if (NULL == clip_ptr->level_timer_ptr[i]) return;
        // Window Maker's ClipAutoraiseDelay.
        wl_event_source_timer_update(clip_ptr->level_timer_ptr[i], 600);
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_dock_left(
    wlmaker_server_t *server_ptr,
    bool dock)
{
    wlmaker_clip_t *clip_ptr = server_ptr->clip_ptr;
    int i = dock ? 1 : 0;
    if (NULL == clip_ptr ||
        WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER != clip_ptr->level[i]) return;
    if (NULL == clip_ptr->level_timer_ptr[i]) {
        clip_ptr->level_timer_ptr[i] = wl_event_loop_add_timer(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            dock ? _wlmaker_clip_handle_level_timer_dock :
            _wlmaker_clip_handle_level_timer_clip,
            clip_ptr);
        if (NULL == clip_ptr->level_timer_ptr[i]) return;
    }
    // Not yet raised: Nothing to lower. Else after ClipAutolowerDelay.
    wl_event_source_timer_update(clip_ptr->level_timer_ptr[i],
                                 clip_ptr->raised[i] ? 1000 : 0);
    if (!clip_ptr->raised[i]) {
        wl_event_source_remove(clip_ptr->level_timer_ptr[i]);
        clip_ptr->level_timer_ptr[i] = NULL;
    }
}

/* ------------------------------------------------------------------------- */
const char *wlmaker_clip_dockapps_fname(wlmaker_server_t *server_ptr)
{
    if (NULL == server_ptr->clip_ptr) return NULL;
    return server_ptr->clip_ptr->dockapps_fname_ptr;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_axis.
 *
 * Moves to the next or previous workspace, depending on the axis (scroll-
 * wheel) direction.
 *
 * @param element_ptr
 * @param wlr_pointer_axis_event_ptr
 *
 * @return true
 */
bool _wlmaker_clip_pointer_axis(
    wlmtk_element_t *element_ptr,
    struct wlr_pointer_axis_event *wlr_pointer_axis_event_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_clip_t,
        super_tile.super_container.super_element);

    if (0 > wlr_pointer_axis_event_ptr->delta) {
        // Scroll wheel "up" -> next.
        wlmtk_desktop_switch_to_next_workspace(clip_ptr->server_ptr->desktop_ptr);
    } else if (0 < wlr_pointer_axis_event_ptr->delta) {
        // Scroll wheel "down" -> next.
        wlmtk_desktop_switch_to_previous_workspace(clip_ptr->server_ptr->desktop_ptr);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_button.
 *
 * Checks if the button press is on either 'next' or 'prev' button area,
 * updates visualization if pressed, and switches workspace if needed.
 *
 * @param element_ptr
 * @param button_event_ptr
 *
 * @return true.
 */
bool _wlmaker_clip_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_clip_t,
        super_tile.super_container.super_element);

    // Right button: Opens the Clip's menu, like on Window Maker.
    if (BTN_RIGHT == button_event_ptr->button &&
        WLMTK_BUTTON_DOWN == button_event_ptr->type) {
        _wlmaker_clip_open_menu(clip_ptr);
        return true;
    }

    if (BTN_LEFT != button_event_ptr->button) return true;

    // Releasing the button that was held on the Clip (not on its buttons):
    // Ends a drag, and places the Clip in that corner.
    if (clip_ptr->drag_pending &&
        WLMTK_BUTTON_UP == button_event_ptr->type) {
        bool dragging = clip_ptr->dragging;
        wlmaker_dock_place_t place;
        bool placed = dragging &&
            _wlmaker_clip_place_at_cursor(clip_ptr, &place);
        _wlmaker_clip_end_drag(clip_ptr);
        if (placed) {
            wlmaker_clip_move_dock(clip_ptr->server_ptr, "Clip", place);
        }
        return true;
    }

    switch (button_event_ptr->type) {
    case WLMTK_BUTTON_DOWN:
        // Pointer button tressed. Translate to button press if in area.
        if (clip_ptr->pointer_inside_next_button ||
            clip_ptr->pointer_inside_next_button) {
            clip_ptr->next_button_pressed = true;
            clip_ptr->prev_button_pressed = false;
        } else if (clip_ptr->pointer_inside_prev_button ||
                   clip_ptr->pointer_inside_prev_button) {
            clip_ptr->next_button_pressed = false;
            clip_ptr->prev_button_pressed = true;
        } else if (NULL != element_ptr->parent_container_ptr &&
                   NULL != clip_ptr->server_ptr->input_manager_ptr) {
            // Elsewhere on the Clip: Moving it with the button held drags
            // the Clip to another corner, like on Window Maker.
            struct wlr_cursor *c = wlmim_wlr_cursor(
                clip_ptr->server_ptr->input_manager_ptr);
            clip_ptr->drag_pending = true;
            clip_ptr->dragging = false;
            clip_ptr->drag_start_x = c->x;
            clip_ptr->drag_start_y = c->y;
            wlmtk_container_pointer_grab(
                element_ptr->parent_container_ptr, element_ptr);
        }
        break;

    case WLMTK_BUTTON_UP:
        // Button is released (closed the click). If we're within the area of
        // the pressed button: Trigger the action.
        if ((clip_ptr->pointer_inside_next_button ||
             clip_ptr->pointer_inside_next_button) &&
            clip_ptr->next_button_pressed) {
            clip_ptr->next_button_pressed = false;
            wlmtk_desktop_switch_to_next_workspace(
                clip_ptr->server_ptr->desktop_ptr);
        } else if ((clip_ptr->pointer_inside_prev_button ||
                    clip_ptr->pointer_inside_prev_button) &&
                   clip_ptr->prev_button_pressed) {
            clip_ptr->prev_button_pressed = false;
            wlmtk_desktop_switch_to_previous_workspace(
                clip_ptr->server_ptr->desktop_ptr);
        }
        break;

    case WLMTK_BUTTON_CLICK:
    default:
        break;
    }

    _wlmaker_clip_apply_button_state(clip_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Counts workspaces, for @ref wlmtk_desktop_for_each_workspace. */
static void _wlmaker_clip_count_workspace(
    __UNUSED__ bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr)
{
    ++*(size_t*)ud_ptr;
}

/** Context menu callback: Switches to the next workspace. */
static void _wlmaker_clip_menu_next(void *ud_ptr)
{
    wlmaker_server_t *server_ptr = ud_ptr;
    wlmtk_desktop_switch_to_next_workspace(server_ptr->desktop_ptr);
}

/** Context menu callback: Switches to the previous workspace. */
static void _wlmaker_clip_menu_previous(void *ud_ptr)
{
    wlmaker_server_t *server_ptr = ud_ptr;
    wlmtk_desktop_switch_to_previous_workspace(server_ptr->desktop_ptr);
}

/** Context menu callback: Adds a workspace. */
static void _wlmaker_clip_menu_add(void *ud_ptr)
{
    wlmaker_action_execute(ud_ptr, WLMAKER_ACTION_WORKSPACE_ADD, NULL);
}

/** Context menu callback: Destroys the last workspace. */
static void _wlmaker_clip_menu_destroy_last(void *ud_ptr)
{
    wlmaker_action_execute(
        ud_ptr, WLMAKER_ACTION_WORKSPACE_DESTROY_LAST, NULL);
}

/** @return The default `Launchers` of the Clip, or NULL if there are none. */
static bspl_array_t *_wlmaker_clip_default_launchers(
    wlmaker_server_t *server_ptr)
{
    bspl_dict_t *d = wlmaker_state_load_defaults(server_ptr->files_ptr, NULL);
    bspl_array_t *a = bspl_dict_get_array(
        bspl_dict_get_dict(d, "Clip"), "Launchers");
    if (NULL != a) a = bspl_array_ref(a);
    if (NULL != d) bspl_dict_unref(d);
    if (NULL != a && 0 == bspl_array_size(a)) {
        bspl_array_unref(a);
        a = NULL;
    }
    return a;
}

/** Context menu callback: Restores the Clip's default launchers. */
static void _wlmaker_clip_menu_restore(void *ud_ptr)
{
    wlmaker_clip_t *clip_ptr = ud_ptr;
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    bspl_array_t *a = _wlmaker_clip_default_launchers(server_ptr);
    if (NULL == a) return;
    bspl_dict_t *d = bspl_dict_create();
    if (NULL != d &&
        bspl_dict_add(d, "Launchers", bspl_object_from_array(a)) &&
        wlmaker_state_store_section(server_ptr->files_ptr,
                                    server_ptr->state_fname_ptr, "Clip", d)) {
        _wlmaker_clip_reload_launchers(clip_ptr);
        // `wlmdock --clip` shows them.
        _wlmaker_clip_signal_wlmdock(server_ptr);
    }
    if (NULL != d) bspl_dict_unref(d);
    bspl_array_unref(a);
}

/** Moves the Clip to the corner `corner` XOR `flip`. */
static void _wlmaker_clip_flip(wlmaker_server_t *server_ptr, uint32_t flip)
{
    if (NULL == server_ptr->clip_dock_ptr) return;
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        server_ptr->clip_dock_ptr);
    wlmaker_dock_place_t place = { pos_ptr->edge, pos_ptr->anchor };
    place = wlmaker_dock_place_from_corner(
        wlmaker_dock_place_corner(place) ^ flip,
        wlmaker_dock_place_vertical(place));
    wlmaker_clip_move_dock(server_ptr, "Clip", place);
}

/** Context menu callback: Moves the Clip to the other side. */
static void _wlmaker_clip_menu_flip_side(void *ud_ptr)
{
    _wlmaker_clip_flip(ud_ptr, WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
}

/** Context menu callback: Moves the Clip to the other end (top/bottom). */
static void _wlmaker_clip_menu_flip_end(void *ud_ptr)
{
    _wlmaker_clip_flip(ud_ptr, WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_add_move_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr)
{
    if (NULL == server_ptr->clip_dock_ptr) return true;
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        server_ptr->clip_dock_ptr);
    uint32_t corner = pos_ptr->edge | pos_ptr->anchor;
    return wlmaker_context_menu_add_item(
        context_menu_ptr,
        (corner & WLR_EDGE_LEFT) ?
        wlmaker_i18n("Move to Right Side") : wlmaker_i18n("Move to Left Side"),
        _wlmaker_clip_menu_flip_side, server_ptr) &&
        wlmaker_context_menu_add_item(
            context_menu_ptr,
            (corner & WLR_EDGE_TOP) ?
            wlmaker_i18n("Move to Bottom") : wlmaker_i18n("Move to Top"),
            _wlmaker_clip_menu_flip_end, server_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Opens the Clip's right-click menu: Workspace navigation and management.
 * Items that would do nothing (eg. switching with just one workspace) are
 * left out.
 *
 * @param clip_ptr
 */
void _wlmaker_clip_open_menu(wlmaker_clip_t *clip_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    if (NULL == server_ptr->style_ptr ||
        NULL == server_ptr->style_ptr->menu_style_ptr ||
        NULL == server_ptr->input_manager_ptr) return;

    size_t workspaces = 0;
    wlmtk_desktop_for_each_workspace(
        server_ptr->desktop_ptr, _wlmaker_clip_count_workspace, &workspaces);

    wlmaker_context_menu_t *cm_ptr = wlmaker_context_menu_create(
        server_ptr, clip_ptr, "Clip");  // i18n: not translated (a name)
    if (NULL == cm_ptr) return;
    bool rv = true;
    if (1 < workspaces) {
        rv = rv && wlmaker_context_menu_add_item(
            cm_ptr, wlmaker_i18n("Next Workspace"), _wlmaker_clip_menu_next,
            server_ptr);
        rv = rv && wlmaker_context_menu_add_item(
            cm_ptr, wlmaker_i18n("Previous Workspace"), _wlmaker_clip_menu_previous,
            server_ptr);
    }
    rv = rv && wlmaker_context_menu_add_item(
        cm_ptr, wlmaker_i18n("New Workspace"), _wlmaker_clip_menu_add, server_ptr);
    if (1 < workspaces) {
        rv = rv && wlmaker_context_menu_add_item(
            cm_ptr, wlmaker_i18n("Destroy Last Workspace"), _wlmaker_clip_menu_destroy_last,
            server_ptr);
    }
    // Moving the Clip to another corner. Like dragging it.
    rv = rv && wlmaker_clip_add_move_items(server_ptr, cm_ptr);
    bspl_array_t *defaults_ptr = _wlmaker_clip_default_launchers(server_ptr);
    if (NULL != defaults_ptr) {
        rv = rv && wlmaker_context_menu_add_item(
            cm_ptr, wlmaker_i18n("Restore Default Launchers"),
            _wlmaker_clip_menu_restore, clip_ptr);
        bspl_array_unref(defaults_ptr);
    }
    if (!rv) {
        wlmaker_context_menu_destroy(cm_ptr);
        return;
    }
    wlmaker_context_menu_open(cm_ptr);
}

/* ------------------------------------------------------------------------- */
/** Updates the button textures, based on current state what's pressed. */
static void _wlmaker_clip_apply_button_state(wlmaker_clip_t *clip_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = clip_ptr->tile_buffer_ptr;
    if ((clip_ptr->pointer_inside_next_button ||
         clip_ptr->pointer_inside_next_button)&&
        clip_ptr->next_button_pressed) {
        wlr_buffer_ptr = clip_ptr->next_pressed_tile_buffer_ptr;
    } else if ((clip_ptr->pointer_inside_prev_button ||
                clip_ptr->pointer_inside_prev_button) &&
               clip_ptr->prev_button_pressed) {
        wlr_buffer_ptr = clip_ptr->prev_pressed_tile_buffer_ptr;
    }
    wlmtk_tile_set_background_buffer(&clip_ptr->super_tile, wlr_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Updates the overlay buffer's content with workspace name and index. */
bool _wlmaker_clip_update_overlay(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr,
    const wlmaker_config_clip_style_t *clip_style_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        tile_style_ptr->size, tile_style_ptr->size);
    if (NULL == wlr_buffer_ptr) return false;

    int index = 0;
    const char *name_ptr = NULL;
    wlmtk_workspace_get_details(
        wlmtk_desktop_get_current_workspace(clip_ptr->server_ptr->desktop_ptr),
        &name_ptr, &index);

    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return false;
    }

    cairo_select_font_face(
        cairo_ptr,
        clip_style_ptr->font.face,
        CAIRO_FONT_SLANT_NORMAL,
        wlmtk_style_font_weight_cairo_from_wlmtk(clip_style_ptr->font.weight));
    cairo_set_font_size(cairo_ptr, clip_style_ptr->font.size);
    cairo_set_source_argb8888(cairo_ptr, clip_style_ptr->text_color);
    cairo_move_to(
        cairo_ptr,
        clip_style_ptr->font.size * 4 / 12,
        clip_style_ptr->font.size * 2 / 12 + clip_style_ptr->font.size);
    cairo_show_text(cairo_ptr, name_ptr);

    cairo_move_to(
        cairo_ptr,
        tile_style_ptr->size - clip_style_ptr->font.size * 14 / 12,
        tile_style_ptr->size - clip_style_ptr->font.size * 8 / 12);
    char buf[10];
    snprintf(buf, sizeof(buf), "%d", index);
    cairo_show_text(cairo_ptr, buf);

    cairo_destroy(cairo_ptr);

    wlmtk_buffer_set(&clip_ptr->overlay_buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);

    wlmtk_tile_set_overlay(
        &clip_ptr->super_tile,
        wlmtk_buffer_element(&clip_ptr->overlay_buffer));
    return true;
}

/* ------------------------------------------------------------------------- */
/** Updates (reloads) the content image. */
bool _wlmaker_clip_update_image(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    wlmtk_image_t *i = wlmtk_image_create_scaled(
        clip_ptr->image_path_ptr,
        tile_style_ptr->content_size,
        tile_style_ptr->content_size);
    if (NULL == i) return false;

    wlmtk_element_set_visible(wlmtk_image_element(i), true);
    wlmtk_tile_set_content(&clip_ptr->super_tile, wlmtk_image_element(i));

    if (NULL != clip_ptr->image_ptr) wlmtk_image_destroy(clip_ptr->image_ptr);
    clip_ptr->image_ptr = i;
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Finds the Clip's image: The `Icon` of the Clip's state, else the icon
 * configured at build time (CMake `WLMAKER_CLIP_DEFAULT_ICON`), else
 * `clip-56x56.png`. Each is a path (starting with '/' or '~'), or a name in
 * `${XDG_DATA_DIRS}/wlmaker/icons`. Candidates that aren't found are logged.
 *
 * @param files_ptr
 * @param icon_ptr            The `Icon` value from the state, or NULL.
 *
 * @return Path to the image file, to be released with free(), or NULL.
 */
char *_wlmaker_clip_resolve_image(
    wlm_util_files_t *files_ptr,
    const char *icon_ptr)
{
    const char *candidates[] = {
        icon_ptr, WLMAKER_CLIP_DEFAULT_ICON, "clip-56x56.png" };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *c = candidates[i];
        if (NULL == c || '\0' == *c) continue;

        char *path_ptr = NULL;
        if ('/' == c[0] || '~' == c[0]) {
            path_ptr = bs_file_resolve_path(c, NULL);
        } else {
            char *p = bs_strdupf("icons/%s", c);
            if (NULL != p && NULL != files_ptr) {
                path_ptr = wlm_util_files_xdg_data_find(files_ptr, p, S_IFREG);
            }
            free(p);
// The source tree is a last resort for running from the build directory;
// an installed package (WLMAKER_INSTALLED_ONLY) must not look there: the
// path is the builder's, e.g. a /tmp directory anyone could create.
#if defined(WLMAKER_SOURCE_DIR) && !defined(WLMAKER_INSTALLED_ONLY)
            if (NULL == path_ptr) {
                path_ptr = bs_strdupf(
                    WLMAKER_SOURCE_DIR "/share/wlmaker/icons/%s", c);
            }
#endif
        }
        if (NULL != path_ptr && bs_file_realpath_is(path_ptr, S_IFREG)) {
            return path_ptr;
        }
        free(path_ptr);
        bs_log(BS_WARNING, "Clip icon \"%s\" not found.", c);
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/**
 * Creates (or updates) the tile buffers for the button states.
 *
 * @param clip_ptr
 * @param tile_style_ptr
 *
 * @return true on success
 */
bool _wlmaker_clip_update_tiles(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    struct wlr_buffer *tile, *tile_prev, *tile_next;
    tile = _wlmaker_clip_create_tile(tile_style_ptr, false, false);
    tile_prev = _wlmaker_clip_create_tile(tile_style_ptr, true, false);
    tile_next = _wlmaker_clip_create_tile(tile_style_ptr, false, true);

    if (NULL == tile || NULL == tile_prev || NULL == tile_next) {
        if (tile) wlr_buffer_drop(tile);
        if (tile_prev) wlr_buffer_drop(tile_prev);
        if (tile_next) wlr_buffer_drop(tile_next);
        return false;
    }

    if (NULL != clip_ptr->tile_buffer_ptr) {
        wlr_buffer_drop(clip_ptr->tile_buffer_ptr);
    }
    if (NULL != clip_ptr->prev_pressed_tile_buffer_ptr) {
        wlr_buffer_drop(clip_ptr->prev_pressed_tile_buffer_ptr);
    }
    if (NULL != clip_ptr->next_pressed_tile_buffer_ptr) {
        wlr_buffer_drop(clip_ptr->next_pressed_tile_buffer_ptr);
    }
    clip_ptr->tile_buffer_ptr = tile;
    clip_ptr->prev_pressed_tile_buffer_ptr = tile_prev;
    clip_ptr->next_pressed_tile_buffer_ptr = tile_next;
    _wlmaker_clip_apply_button_state(clip_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Creates a wlr_buffer with texture suitable to show the 'next' and 'prev'
 * buttons in each raised or pressed state.
 *
 * @param tile_style_ptr
 * @param prev_pressed
 * @param next_pressed
 *
 * @return A wlr buffer.
 */
struct wlr_buffer *_wlmaker_clip_create_tile(
    const struct wlmtk_tile_style *tile_style_ptr,
    bool prev_pressed,
    bool next_pressed)
{
    struct wlr_buffer* wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        tile_style_ptr->size, tile_style_ptr->size);
    if (NULL == wlr_buffer_ptr) return NULL;

    double tsize = tile_style_ptr->size;
    double bsize = 22.0 / 64.0 * tile_style_ptr->size;
    double margin = tile_style_ptr->bezel_width;

    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }

    wlmaker_primitives_cairo_fill(cairo_ptr, &tile_style_ptr->fill);

    // Northern + Western sides. Drawn clock-wise.
    wlmaker_primitives_set_bezel_color(cairo_ptr, true);
    cairo_move_to(cairo_ptr, 0, 0);
    cairo_line_to(cairo_ptr, tsize - bsize, 0);
    cairo_line_to(cairo_ptr, tsize - bsize, margin);
    cairo_line_to(cairo_ptr, margin, margin);
    cairo_line_to(cairo_ptr, margin, tsize - bsize);
    cairo_line_to(cairo_ptr, 0, tsize - bsize);
    cairo_line_to(cairo_ptr, 0, 0);
    cairo_fill(cairo_ptr);

    // Southern + Eastern sides. Also drawn Also clock-wise.
    wlmaker_primitives_set_bezel_color(cairo_ptr, false);
    cairo_move_to(cairo_ptr, tsize, tsize);
    cairo_line_to(cairo_ptr, bsize, tsize);
    cairo_line_to(cairo_ptr, bsize, tsize - margin);
    cairo_line_to(cairo_ptr, tsize - margin, tsize - margin);
    cairo_line_to(cairo_ptr, tsize - margin, bsize);
    cairo_line_to(cairo_ptr, tsize, bsize);
    cairo_line_to(cairo_ptr, tsize, tsize);
    cairo_fill(cairo_ptr);

    // Diagonal at the north-eastern corner. Drawn clockwise.
    wlmaker_primitives_set_bezel_color(cairo_ptr, true);
    cairo_move_to(cairo_ptr, tsize - bsize, 0);
    cairo_line_to(cairo_ptr, tsize, bsize);
    cairo_line_to(cairo_ptr, tsize - margin, bsize);
    cairo_line_to(cairo_ptr, tsize - bsize, margin);
    cairo_line_to(cairo_ptr, tsize - bsize, 0);
    cairo_fill(cairo_ptr);

    // Diagonal at south-western corner. Drawn clockwise.
    wlmaker_primitives_set_bezel_color(cairo_ptr, false);
    cairo_move_to(cairo_ptr, 0, tsize - bsize);
    cairo_line_to(cairo_ptr, margin, tsize - bsize);
    cairo_line_to(cairo_ptr, bsize, tsize - margin);
    cairo_line_to(cairo_ptr, bsize, tsize);
    cairo_line_to(cairo_ptr, 0, tsize - bsize);
    cairo_fill(cairo_ptr);

    // The "Next" button, north-eastern corner.
    // Northern edge, illuminated when raised
    wlmaker_primitives_set_bezel_color(cairo_ptr, !next_pressed);
    cairo_move_to(cairo_ptr, tsize - bsize, 0);
    cairo_line_to(cairo_ptr, tsize, 0);
    cairo_line_to(cairo_ptr, tsize - margin, margin);
    cairo_line_to(cairo_ptr, tsize - bsize + 2 * margin, margin);
    cairo_line_to(cairo_ptr, tsize - bsize , 0);
    cairo_fill(cairo_ptr);

    // Eastern edge, illuminated when pressed
    wlmaker_primitives_set_bezel_color(cairo_ptr, next_pressed);
    cairo_move_to(cairo_ptr, tsize, 0);
    cairo_line_to(cairo_ptr, tsize, bsize);
    cairo_line_to(cairo_ptr, tsize - margin, bsize - 2 * margin);
    cairo_line_to(cairo_ptr, tsize - margin,  margin);
    cairo_line_to(cairo_ptr, tsize, 0);
    cairo_fill(cairo_ptr);

    // Diagonal, illuminated when pressed.
    wlmaker_primitives_set_bezel_color(cairo_ptr, next_pressed);
    cairo_move_to(cairo_ptr, tsize - bsize, 0);
    cairo_line_to(cairo_ptr, tsize - bsize + 2 * margin, margin);
    cairo_line_to(cairo_ptr, tsize - margin, bsize - 2 *margin);
    cairo_line_to(cairo_ptr, tsize, bsize);
    cairo_line_to(cairo_ptr, tsize - bsize, 0);
    cairo_fill(cairo_ptr);

    // The black triangle. Use relative sizes.
    double tpad = bsize * 5.0 / 22.0;
    double trsize = bsize * 7.0 / 22.0;
    double tmargin = bsize * 1.0 / 22.0;
    cairo_set_source_rgba(cairo_ptr, 0, 0, 0, 1.0);
    cairo_move_to(cairo_ptr, tsize - tpad, tpad);
    cairo_line_to(cairo_ptr, tsize - tpad, trsize + tpad);
    cairo_line_to(cairo_ptr, tsize - tpad - trsize, tpad);
    cairo_line_to(cairo_ptr, tsize - tpad, tpad);
    cairo_fill(cairo_ptr);

    // Northern edge of triangle, not illuminated.
    wlmaker_primitives_set_bezel_color(cairo_ptr, false);
    cairo_move_to(cairo_ptr, tsize - tpad, tpad);
    cairo_line_to(cairo_ptr, tsize - tpad - trsize, tpad);
    cairo_line_to(cairo_ptr, tsize - tpad - trsize - tmargin, tpad - tmargin);
    cairo_line_to(cairo_ptr, tsize - tpad + tmargin, tpad - tmargin);
    cairo_line_to(cairo_ptr, tsize - tpad, tpad);
    cairo_fill(cairo_ptr);

    // Eastern side of triangle, illuminated.
    wlmaker_primitives_set_bezel_color(cairo_ptr, true);
    cairo_move_to(cairo_ptr, tsize - tpad, tpad);
    cairo_line_to(cairo_ptr, tsize - tpad + tmargin, tpad - tmargin);
    cairo_line_to(cairo_ptr, tsize - tpad + tmargin, tpad + trsize + tmargin);
    cairo_line_to(cairo_ptr, tsize - tpad, tpad + trsize);
    cairo_line_to(cairo_ptr, tsize - tpad, tpad);
    cairo_fill(cairo_ptr);

    // The "Prev" button, south-western corner.
    // Southern edge, illuminated when pressed.
    wlmaker_primitives_set_bezel_color(cairo_ptr, prev_pressed);
    cairo_move_to(cairo_ptr, 0, tsize);
    cairo_line_to(cairo_ptr, margin, tsize - margin);
    cairo_line_to(cairo_ptr, bsize - 2 * margin, tsize - margin);
    cairo_line_to(cairo_ptr, bsize, tsize);
    cairo_line_to(cairo_ptr, 0, tsize);
    cairo_fill(cairo_ptr);

    // Western edge, illuminated when raised.
    wlmaker_primitives_set_bezel_color(cairo_ptr, !prev_pressed);
    cairo_move_to(cairo_ptr, 0, tsize);
    cairo_line_to(cairo_ptr, 0, tsize - bsize + 0);
    cairo_line_to(cairo_ptr, margin, tsize - bsize + 2 * margin);
    cairo_line_to(cairo_ptr, margin, tsize - margin);
    cairo_line_to(cairo_ptr, 0, tsize);
    cairo_fill(cairo_ptr);

    // Diagonal, illuminated when raised.
    wlmaker_primitives_set_bezel_color(cairo_ptr, !prev_pressed);
    cairo_move_to(cairo_ptr, 0, tsize - bsize + 0);
    cairo_line_to(cairo_ptr, bsize, tsize);
    cairo_line_to(cairo_ptr, bsize - 2 * margin, tsize - margin);
    cairo_line_to(cairo_ptr, margin, tsize - bsize + 2 * margin);
    cairo_line_to(cairo_ptr, 0, tsize - bsize + 0);
    cairo_fill(cairo_ptr);

    // The black triangle. Use relative sizes.
    cairo_set_source_rgba(cairo_ptr, 0, 0, 0, 1.0);
    cairo_move_to(cairo_ptr, tpad, tsize - tpad);
    cairo_line_to(cairo_ptr, tpad, tsize - trsize - tpad);
    cairo_line_to(cairo_ptr, tpad + trsize, tsize - tpad);
    cairo_line_to(cairo_ptr, tpad, tsize - tpad);
    cairo_fill(cairo_ptr);

    // Southern edge of triangle, illuminated.
    wlmaker_primitives_set_bezel_color(cairo_ptr, true);
    cairo_move_to(cairo_ptr, tpad, tsize - tpad);
    cairo_line_to(cairo_ptr, tpad + trsize, tsize - tpad);
    cairo_line_to(cairo_ptr, tpad + trsize + tmargin, tsize - tpad + tmargin);
    cairo_line_to(cairo_ptr, tpad - tmargin, tsize - tpad + tmargin);
    cairo_line_to(cairo_ptr, tpad, tsize - tpad);
    cairo_fill(cairo_ptr);

    // Eastern side of triangle, not illuminated.
    wlmaker_primitives_set_bezel_color(cairo_ptr, false);
    cairo_move_to(cairo_ptr, tpad, tsize - tpad);
    cairo_line_to(cairo_ptr, tpad - tmargin, tsize - tpad + tmargin);
    cairo_line_to(cairo_ptr, tpad - tmargin, tsize - tpad - trsize - tmargin);
    cairo_line_to(cairo_ptr, tpad, tsize - tpad - trsize);
    cairo_line_to(cairo_ptr, tpad, tsize - tpad);
    cairo_fill(cairo_ptr);

    cairo_destroy(cairo_ptr);
    return wlr_buffer_ptr;
}

/* -------------------------------------------------------------------------- */
/**
 * Handler for the `workspace_changed` signal of `wlmaker_server_t`.
 *
 * Will redraw the clip contents with the current workspace.
 *
 * @param listener_ptr
 * @param data_ptr            Points to the new `wlmtk_workspace_t`.
 */
void _wlmaker_clip_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_clip_t, workspace_changed_listener);

    _wlmaker_clip_update_overlay(
        clip_ptr, &clip_ptr->super_tile.style, &clip_ptr->style);
}

/* ------------------------------------------------------------------------- */
/** Handles when output layout changes; Re-computes the output to attach. */
void _wlmaker_clip_handle_output_layout_change(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_clip_t, output_layout_change_listener);

    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &clip_ptr->output_description,
        clip_ptr->server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(
            clip_ptr->server_ptr->wlr_output_layout_ptr);
    }
    wlmtk_layer_t *layer_ptr = wlmtk_panel_get_layer(
        wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr));
    if (NULL != layer_ptr) {
        wlmtk_layer_remove_panel(
            layer_ptr,
            wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr));
    } else {
        layer_ptr = wlmtk_desktop_get_layer(
            clip_ptr->server_ptr->desktop_ptr, WLMTK_DESKTOP_LAYER_TOP);
    }
    if (NULL != wlr_output_ptr) {
        wlmtk_layer_add_panel(
            layer_ptr,
            wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr),
            wlr_output_ptr);
    }
    // The lanes of its grid go along.
    _wlmaker_clip_replace_lanes(clip_ptr);

    // The Dock is on the primary output, its dock-apps go along. When the
    // output they were on went away, they were left on another output; and
    // when the primary output comes back, they return to it.
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    struct wlr_output *primary_ptr = wlmbe_primary_output(
        server_ptr->wlr_output_layout_ptr);
    if (NULL != clip_ptr->dock_side_ptr && NULL != primary_ptr) {
        wlmtk_panel_t *panel_ptr = wlmtk_dock_panel(clip_ptr->dock_side_ptr);
        layer_ptr = wlmtk_panel_get_layer(panel_ptr);
        if (NULL != layer_ptr) {
            wlmtk_layer_remove_panel(layer_ptr, panel_ptr);
        } else {
            layer_ptr = wlmtk_desktop_get_layer(
                server_ptr->desktop_ptr, WLMTK_DESKTOP_LAYER_TOP);
        }
        wlmtk_layer_add_panel(layer_ptr, panel_ptr, primary_ptr);
        _wlmaker_clip_update_dock_side(clip_ptr);
    }

    // The Dock's and the Clip's launchers (wlmdock) are layer surfaces, put
    // on an output when created: They go along, too.
    if (NULL != server_ptr->dock_layer_panel_ptr) {
        wlmaker_layer_panel_set_output(
            server_ptr->dock_layer_panel_ptr, primary_ptr);
    }
    if (NULL != server_ptr->clip_layer_panel_ptr) {
        wlmaker_layer_panel_set_output(
            server_ptr->clip_layer_panel_ptr, wlr_output_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_element_events_t::pointer_leave. Resets buttons. */
void _wlmaker_clip_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_clip_t, pointer_leave_listener);

    clip_ptr->pointer_inside_prev_button = false;
    clip_ptr->pointer_inside_next_button = false;
    _wlmaker_clip_apply_button_state(clip_ptr);
    wlmaker_clip_dock_left(clip_ptr->server_ptr, false);
    if (NULL != clip_ptr->server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(clip_ptr->server_ptr->dock_fx_ptr,
                                      clip_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_element_events_t::pointer_motion. */
void _wlmaker_clip_handle_pointer_motion(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_clip_t, pointer_motion_listener);
    wlmtk_pointer_motion_event_t *motion_event_ptr = data_ptr;

    clip_ptr->pointer_inside_prev_button = false;
    clip_ptr->pointer_inside_next_button = false;

    double tile_size = clip_ptr->super_tile.style.size;
    double button_size = (22.0 / 64.0) * tile_size;
    if (motion_event_ptr->x >= tile_size - button_size &&
        motion_event_ptr->x < tile_size &&
        motion_event_ptr->y >= 0 &&
        motion_event_ptr->y < button_size) {
        // Next button.
        clip_ptr->pointer_inside_next_button = true;
    } else if (motion_event_ptr->x >= 0 &&
               motion_event_ptr->x < button_size &&
               motion_event_ptr->y >= tile_size - button_size &&
               motion_event_ptr->y < tile_size) {
        // Prev button.
        clip_ptr->pointer_inside_prev_button = true;
    }

    _wlmaker_clip_apply_button_state(clip_ptr);
    wlmaker_clip_dock_hovered(clip_ptr->server_ptr, false);
    _wlmaker_clip_balloon(clip_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Shows the Clip's balloon, as Window Maker: The workspace's name, when it
 * is longer than 5 characters (and so may not fit on the Clip).
 */
void _wlmaker_clip_balloon(wlmaker_clip_t *clip_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    if (NULL == server_ptr->dock_fx_ptr) return;
    const char *name_ptr = NULL;
    int index = 0;
    wlmtk_workspace_get_details(
        wlmtk_desktop_get_current_workspace(server_ptr->desktop_ptr),
        &name_ptr, &index);
    if (NULL == name_ptr || 5 >= strlen(name_ptr) ||
        clip_ptr->drag_pending) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, clip_ptr);
        return;
    }
    wlmtk_element_t *e = wlmtk_tile_element(&clip_ptr->super_tile);
    struct wlr_box box = wlmtk_element_get_dimensions_box(e);
    int x, y;
    wlmtk_element_get_absolute_position(e, &x, &y);
    box.x += x;
    box.y += y;
    box.height -= 2;
    struct wlr_box area = {};
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, box.x + 1, box.y + 1);
    if (NULL != wlr_output_ptr) {
        wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr,
                                  wlr_output_ptr, &area);
    }
    wlmaker_dock_fx_balloon_enter(server_ptr->dock_fx_ptr, clip_ptr,
                                  name_ptr, &box, &area);
}

/* ------------------------------------------------------------------------- */
/** Event listener: The theme changed. Applies the new style to the clip. */
void _wlmaker_clip_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_clip_t, theme_changed_listener);
    wlmaker_config_style_t *style_ptr = data_ptr;

    if (!_wlmaker_clip_update_tiles(clip_ptr, &style_ptr->tile)) return;
    if (!_wlmaker_clip_update_overlay(
            clip_ptr, &style_ptr->tile, &style_ptr->clip)) return;
    if (!_wlmaker_clip_update_image(clip_ptr, &style_ptr->tile)) return;
    clip_ptr->style = style_ptr->clip;
    // The grid's lanes: Their dock-apps back to the Clip's dock, which takes
    // the style; then arranged again, in new docks.
    for (size_t i = 0; i < clip_ptr->lanes; ++i) {
        _wlmaker_clip_destroy_lane(clip_ptr, &clip_ptr->lanes_ptr[i]);
    }

    wlmtk_dock_set_style(
        clip_ptr->wlmtk_dock_ptr,
        &style_ptr->dock,
        &style_ptr->tile);

    // Need to apply the button state (again) after @ref wlmtk_dock_set_style,
    // since the latter will apply the generic tile backend. Which the tile
    // overwrites.
    _wlmaker_clip_apply_button_state(clip_ptr);
    _wlmaker_clip_update_spacers(clip_ptr, &style_ptr->tile);
    _wlmaker_clip_arrange(clip_ptr);

    // The Dock and the Clip's launchers (wlmdock) take the new theme too,
    // as Window Maker's Dock does at once.
    _wlmaker_clip_signal_wlmdock_with(clip_ptr->server_ptr, SIGHUP);
}

/* ------------------------------------------------------------------------- */
/** Extension of a spacer tile's element: lets the pointer through. */
static const wlmtk_element_vmt_t _wlmaker_clip_spacer_element_vmt = {
    .pointer_accepts_motion = _wlmaker_clip_spacer_accepts_motion,
};

/* ------------------------------------------------------------------------- */
/**
 * Initializes a spacer tile: The room kept for a launcher that
 * `wlmdock --clip` shows there. The tile is transparent and must not take the
 * pointer either: It is stacked above the launchers, and took their clicks
 * (Firefox and Thunar in the Clip did not open).
 */
bool _wlmaker_clip_spacer_init(
    wlmtk_tile_t *tile_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    if (!wlmtk_tile_init(tile_ptr, tile_style_ptr)) return false;
    wlmtk_element_extend(wlmtk_tile_element(tile_ptr),
                         &_wlmaker_clip_spacer_element_vmt);
    // Holes keep no margins between them.
    wlmtk_tile_element(tile_ptr)->placeholder = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/** A spacer tile never accepts the pointer. */
bool _wlmaker_clip_spacer_accepts_motion(
    __UNUSED__ wlmtk_element_t *element_ptr,
    __UNUSED__ wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    return false;
}

/* ------------------------------------------------------------------------- */
/** Gives the spacer tiles (of the Clip and the Dock) a transparent background. */
bool _wlmaker_clip_update_spacers(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    if (0 == clip_ptr->spacer_tiles && 0 == clip_ptr->dock_spacer_tiles) {
        return true;
    }
    // A freshly created buffer is all zero, ie. fully transparent.
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        tile_style_ptr->size, tile_style_ptr->size);
    if (NULL == wlr_buffer_ptr) return false;
    bool rv = true;
    for (size_t i = 0; i < clip_ptr->spacer_tiles; ++i) {
        rv &= wlmtk_tile_set_background_buffer(
            &clip_ptr->spacer_tiles_ptr[i], wlr_buffer_ptr);
    }
    for (size_t i = 0; i < clip_ptr->dock_spacer_tiles; ++i) {
        rv &= wlmtk_tile_set_background_buffer(
            &clip_ptr->dock_spacer_tiles_ptr[i], wlr_buffer_ptr);
    }
    wlr_buffer_drop(wlr_buffer_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Applies `place` to the dock, keeping its other positioning parameters. */
void _wlmaker_clip_set_place(
    wlmtk_dock_t *dock_ptr,
    wlmaker_dock_place_t place)
{
    wlmtk_dock_positioning_t pos = *wlmtk_dock_get_positioning(dock_ptr);
    if (pos.edge == place.edge && pos.anchor == place.anchor) return;
    pos.edge = place.edge;
    pos.anchor = place.anchor;
    if (!wlmtk_dock_set_positioning(dock_ptr, &pos)) {
        bs_log(BS_WARNING, "Failed to move dock %p to edge 0x%"PRIx32
               ", anchor 0x%"PRIx32, dock_ptr, place.edge, place.anchor);
    }
}

/* ------------------------------------------------------------------------- */
/** Computes the Clip's place for the quadrant the pointer is in. */
bool _wlmaker_clip_place_at_cursor(
    wlmaker_clip_t *clip_ptr,
    wlmaker_dock_place_t *place_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, c->x, c->y);
    if (NULL == wlr_output_ptr) return false;
    struct wlr_box box;
    wlr_output_layout_get_box(
        server_ptr->wlr_output_layout_ptr, wlr_output_ptr, &box);
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        clip_ptr->wlmtk_dock_ptr);
    *place_ptr = wlmaker_dock_place_at(
        &box, c->x, c->y,
        wlmaker_dock_place_vertical(
            (wlmaker_dock_place_t){ pos_ptr->edge, pos_ptr->anchor }));
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_accepts_motion: With the
 * button held, starts dragging beyond a threshold; while dragging, moves the
 * Clip to the corner of the quadrant the pointer is in (a live preview).
 */
bool _wlmaker_clip_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_clip_t,
        super_tile.super_container.super_element);

    if (clip_ptr->drag_pending) {
        struct wlr_cursor *c = wlmim_wlr_cursor(
            clip_ptr->server_ptr->input_manager_ptr);
        double dx = c->x - clip_ptr->drag_start_x;
        double dy = c->y - clip_ptr->drag_start_y;
        if (!clip_ptr->dragging &&
            dx * dx + dy * dy >=
            _wlmaker_clip_drag_threshold * _wlmaker_clip_drag_threshold) {
            clip_ptr->dragging = true;
        }
        wlmaker_dock_place_t place;
        const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
            clip_ptr->wlmtk_dock_ptr);
        if (clip_ptr->dragging &&
            _wlmaker_clip_place_at_cursor(clip_ptr, &place) &&
            (place.edge != pos_ptr->edge || place.anchor != pos_ptr->anchor) &&
            NULL == clip_ptr->preview_event_source_ptr) {
            // Not while dispatching the pointer event: Moving re-adds tiles.
            clip_ptr->preview_place = place;
            clip_ptr->preview_event_source_ptr = wl_event_loop_add_idle(
                wl_display_get_event_loop(
                    clip_ptr->server_ptr->wl_display_ptr),
                _wlmaker_clip_apply_preview,
                clip_ptr);
        }
    }

    return clip_ptr->orig_super_element_vmt.pointer_accepts_motion(
        element_ptr, motion_event_ptr);
}

/* ------------------------------------------------------------------------- */
/** Moves the Clip to the preview's corner, and keeps the drag going. */
void _wlmaker_clip_apply_preview(void *data_ptr)
{
    wlmaker_clip_t *clip_ptr = data_ptr;
    clip_ptr->preview_event_source_ptr = NULL;
    if (!clip_ptr->dragging) return;

    // Re-adding the tile cancels the pointer grab: Restore it.
    _wlmaker_clip_set_place(clip_ptr->wlmtk_dock_ptr, clip_ptr->preview_place);
    _wlmaker_clip_arrange(clip_ptr);
    clip_ptr->drag_pending = true;
    clip_ptr->dragging = true;
    wlmtk_element_t *element_ptr = wlmtk_tile_element(&clip_ptr->super_tile);
    if (NULL != element_ptr->parent_container_ptr) {
        wlmtk_container_pointer_grab(
            element_ptr->parent_container_ptr, element_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_grab_cancel. Ends a drag. */
void _wlmaker_clip_pointer_grab_cancel(wlmtk_element_t *element_ptr)
{
    wlmaker_clip_t *clip_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_clip_t,
        super_tile.super_container.super_element);
    clip_ptr->drag_pending = false;
    clip_ptr->dragging = false;
    clip_ptr->orig_super_element_vmt.pointer_grab_cancel(element_ptr);
}

/* ------------------------------------------------------------------------- */
/** Ends a (pending) drag: Releases the pointer grab. */
void _wlmaker_clip_end_drag(wlmaker_clip_t *clip_ptr)
{
    wlmtk_element_t *element_ptr = wlmtk_tile_element(&clip_ptr->super_tile);
    clip_ptr->drag_pending = false;
    clip_ptr->dragging = false;
    if (NULL != clip_ptr->preview_event_source_ptr) {
        wl_event_source_remove(clip_ptr->preview_event_source_ptr);
        clip_ptr->preview_event_source_ptr = NULL;
    }
    if (NULL != element_ptr->parent_container_ptr) {
        wlmtk_container_pointer_grab_release(
            element_ptr->parent_container_ptr, element_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Unblocks SIGUSR1 in forked processes: Only wlmaker monitors it. */
void _wlmaker_clip_atfork_child(void)
{
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles SIGUSR1, sent by `wlmdock --clip` when it changed the Clip's
 * launchers (eg. one was removed): Updates the room left for them.
 */
int _wlmaker_clip_handle_sigusr1(__UNUSED__ int signal_number, void *data_ptr)
{
    _wlmaker_clip_reload_launchers(data_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Re-reads the launchers of the Clip and the Dock, and the Dock's place, from
 * the state: Updates the room the Clip leaves for its launchers, and where the
 * Dock's dock-apps start.
 */
void _wlmaker_clip_reload_launchers(wlmaker_clip_t *clip_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        server_ptr->files_ptr, server_ptr->state_fname_ptr);
    if (NULL == state_dict_ptr) return;
    // As `wlmdock --clip` shows them: Those with an icon.
    bspl_array_t *launchers_ptr = wlmaker_dock_launchers_load(
        server_ptr, "Clip");
    size_t spacers = NULL != launchers_ptr ? bspl_array_size(launchers_ptr) : 0;
    if (NULL != launchers_ptr) bspl_array_unref(launchers_ptr);
    wlmaker_dock_place_t place;
    if (wlmaker_dock_place_decode(
            bspl_dict_get_dict(state_dict_ptr, "Dock"), &place)) {
        clip_ptr->dock_place = place;
    }
    bspl_dict_unref(state_dict_ptr);

    bs_log(BS_INFO, "Clip %p: Room for %zu launchers.", clip_ptr, spacers);
    clip_ptr->clip_launchers = spacers;

    bspl_array_t *a = wlmaker_dock_launchers_load(server_ptr, "Dock");
    if (NULL != a) {
        clip_ptr->dock_launchers = bspl_array_size(a);
        if (NULL != clip_ptr->dock_side_ptr) {
            _wlmaker_clip_set_spacers(clip_ptr, true, clip_ptr->dock_launchers,
                                      &server_ptr->style_ptr->tile);
        }
        _wlmaker_clip_update_dock_side(clip_ptr);
        _wlmaker_clip_sync_drawers(clip_ptr, a);
        bspl_array_unref(a);
    } else {
        _wlmaker_clip_update_dock_side(clip_ptr);
    }
    _wlmaker_clip_arrange(clip_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Makes the drawers those of the Dock's launchers with `Drawer = Yes`, in
 * their order: Creates new ones, updates the others, destroys those gone
 * (their dock-apps go to the Clip).
 */
void _wlmaker_clip_sync_drawers(
    wlmaker_clip_t *clip_ptr,
    bspl_array_t *launchers_ptr)
{
    size_t n = bspl_array_size(launchers_ptr);
    wlmaker_drawer_t **new_ptr = logged_calloc(n + 1, sizeof(*new_ptr));
    if (NULL == new_ptr) return;
    size_t new_n = 0;
    for (size_t i = 0; i < n; ++i) {
        bspl_dict_t *d = bspl_dict_from_object(bspl_array_at(launchers_ptr, i));
        if (!wlmaker_drawer_is_entry(d)) continue;
        const char *id_ptr = bspl_dict_get_string_value(d, "Id");
        if (NULL == id_ptr) id_ptr = "";
        wlmaker_drawer_t *drawer_ptr = NULL;
        for (size_t j = 0; j < clip_ptr->drawers; ++j) {
            if (NULL != clip_ptr->drawers_ptr[j] &&
                0 == strcmp(wlmaker_drawer_id(clip_ptr->drawers_ptr[j]),
                            id_ptr)) {
                drawer_ptr = clip_ptr->drawers_ptr[j];
                clip_ptr->drawers_ptr[j] = NULL;
                break;
            }
        }
        if (NULL == drawer_ptr) {
            drawer_ptr = wlmaker_drawer_create(clip_ptr->server_ptr, id_ptr);
        }
        if (NULL == drawer_ptr) continue;
        new_ptr[new_n++] = drawer_ptr;
    }
    // Gone: Destroyed, before the others are updated (they may take over
    // their dock-apps).
    for (size_t j = 0; j < clip_ptr->drawers; ++j) {
        if (NULL != clip_ptr->drawers_ptr[j]) {
            wlmaker_drawer_destroy(clip_ptr->drawers_ptr[j]);
        }
    }
    if (NULL != clip_ptr->drawers_ptr) free(clip_ptr->drawers_ptr);
    clip_ptr->drawers_ptr = new_ptr;
    clip_ptr->drawers = new_n;

    size_t k = 0;
    for (size_t i = 0; i < n && k < new_n; ++i) {
        bspl_dict_t *d = bspl_dict_from_object(bspl_array_at(launchers_ptr, i));
        if (!wlmaker_drawer_is_entry(d)) continue;
        wlmaker_drawer_update(new_ptr[k++], d, i);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Creates the dock for the dock-apps attached to the Dock: Placed over the
 * Dock's column, after its main tile, with room for its launchers among the
 * dock-apps. Without a Dock in the state, there is none.
 */
bool _wlmaker_clip_create_dock_side(
    wlmaker_clip_t *clip_ptr,
    bspl_dict_t *state_dict_ptr,
    const wlmaker_config_style_t *style_ptr,
    struct wlr_output *wlr_output_ptr)
{
    if (!wlmaker_dock_place_decode(
            bspl_dict_get_dict(state_dict_ptr, "Dock"),
            &clip_ptr->dock_place)) return true;

    wlmtk_dock_positioning_t pos = {
        .edge = clip_ptr->dock_place.edge,
        .anchor = clip_ptr->dock_place.anchor,
        .overlay = true };
    clip_ptr->dock_side_ptr = wlmtk_dock_create(&pos, &style_ptr->dock);
    if (NULL == clip_ptr->dock_side_ptr) return false;
    wlmtk_element_set_visible(
        wlmtk_dock_element(clip_ptr->dock_side_ptr), true);
    clip_ptr->dock_dockapps_ptr = wlmaker_dockapps_create_with_key(
        clip_ptr->dock_side_ptr, NULL, clip_ptr->dockapps_fname_ptr, "Dock");
    if (NULL == clip_ptr->dock_dockapps_ptr) return false;
    wlmaker_dockapps_set_changed_callback(
        clip_ptr->dock_dockapps_ptr, _wlmaker_clip_handle_dockapps_changed,
        clip_ptr);
    bspl_array_t *defaults_ptr = bspl_dict_get_array(
        bspl_dict_get_dict(state_dict_ptr, "Dock"), "Dockapps");
    if (NULL != defaults_ptr) {
        clip_ptr->dock_default_dockapps_ptr = bspl_array_ref(defaults_ptr);
    }

    wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
        clip_ptr->server_ptr->desktop_ptr, WLMTK_DESKTOP_LAYER_TOP);
    if (NULL != wlr_output_ptr &&
        !wlmtk_layer_add_panel(layer_ptr,
                               wlmtk_dock_panel(clip_ptr->dock_side_ptr),
                               wlr_output_ptr)) return false;

    bspl_array_t *a = wlmaker_dock_launchers_load(
        clip_ptr->server_ptr, "Dock");
    if (NULL != a) {
        clip_ptr->dock_launchers = bspl_array_size(a);
        bspl_array_unref(a);
    }
    if (!_wlmaker_clip_set_spacers(clip_ptr, true, clip_ptr->dock_launchers,
                                   &style_ptr->tile)) return false;
    _wlmaker_clip_update_dock_side(clip_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Places the Dock's dock-apps (and the room for its launchers) right after
 * the Dock's main tile: At the Dock's corner, one tile further.
 */
void _wlmaker_clip_update_dock_side(wlmaker_clip_t *clip_ptr)
{
    if (NULL == clip_ptr->dock_side_ptr) return;
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    int pitch = server_ptr->style_ptr->tile.size +
        server_ptr->style_ptr->dock.margin.width;
    wlmtk_dock_positioning_t pos = *wlmtk_dock_get_positioning(
        clip_ptr->dock_side_ptr);
    pos.edge = clip_ptr->dock_place.edge;
    pos.anchor = clip_ptr->dock_place.anchor;
    pos.margin = pitch;

    // Where the Dock is: It may be pushed away from the corner by others.
    struct wlr_box b, o;
    struct wlr_output *wlr_output_ptr = wlmbe_primary_output(
        server_ptr->wlr_output_layout_ptr);
    if (NULL != server_ptr->dock_layer_panel_ptr && NULL != wlr_output_ptr &&
        wlmaker_layer_panel_get_box(server_ptr->dock_layer_panel_ptr, &b)) {
        wlr_output_layout_get_box(
            server_ptr->wlr_output_layout_ptr, wlr_output_ptr, &o);
        switch (pos.anchor) {
        case WLR_EDGE_TOP: pos.margin += b.y - o.y; break;
        case WLR_EDGE_BOTTOM:
            pos.margin += o.y + o.height - (b.y + b.height); break;
        case WLR_EDGE_LEFT: pos.margin += b.x - o.x; break;
        case WLR_EDGE_RIGHT:
            pos.margin += o.x + o.width - (b.x + b.width); break;
        default: break;
        }
        if (0 > pos.margin) pos.margin = 0;
    }

    const wlmtk_dock_positioning_t *cur_ptr = wlmtk_dock_get_positioning(
        clip_ptr->dock_side_ptr);
    if (cur_ptr->edge == pos.edge && cur_ptr->anchor == pos.anchor &&
        cur_ptr->margin == pos.margin) return;
    if (!wlmtk_dock_set_positioning(clip_ptr->dock_side_ptr, &pos)) {
        bs_log(BS_WARNING, "Failed to place the Dock's dock-apps at edge "
               "0x%"PRIx32", anchor 0x%"PRIx32, pos.edge, pos.anchor);
    }
}

/* ------------------------------------------------------------------------- */
/** Decodes Window Maker's `Lowered` and `AutoRaiseLower` of a dock. */
wlmaker_dock_level_t _wlmaker_clip_decode_level(bspl_dict_t *dict_ptr)
{
    if (wlmaker_dock_launcher_flag(dict_ptr, "AutoRaiseLower")) {
        return WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER;
    }
    if (wlmaker_dock_launcher_flag(dict_ptr, "Lowered")) {
        return WLMAKER_DOCK_LEVEL_NORMAL;
    }
    return WLMAKER_DOCK_LEVEL_KEEP_ON_TOP;
}

/* ------------------------------------------------------------------------- */
/** Puts the Dock's (or the Clip's) surfaces in the layer of its level. */
void _wlmaker_clip_apply_level(wlmaker_clip_t *clip_ptr, bool dock)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    wlmtk_desktop_layer_t layer = wlmaker_clip_layer(server_ptr, dock);
    if (dock) {
        if (NULL != clip_ptr->dock_side_ptr) {
            _wlmaker_clip_move_panel(
                clip_ptr, wlmtk_dock_panel(clip_ptr->dock_side_ptr), layer);
        }
        if (NULL != server_ptr->dock_layer_panel_ptr) {
            wlmaker_layer_panel_update_layer(server_ptr->dock_layer_panel_ptr);
        }
        for (size_t i = 0; i < clip_ptr->drawers; ++i) {
            wlmaker_drawer_update_layer(clip_ptr->drawers_ptr[i]);
        }
    } else {
        _wlmaker_clip_move_panel(
            clip_ptr, wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr), layer);
        _wlmaker_clip_replace_lanes(clip_ptr);
        if (NULL != server_ptr->clip_layer_panel_ptr) {
            wlmaker_layer_panel_update_layer(server_ptr->clip_layer_panel_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Moves the panel to `layer`, on the output it is on. */
void _wlmaker_clip_move_panel(
    wlmaker_clip_t *clip_ptr,
    wlmtk_panel_t *panel_ptr,
    wlmtk_desktop_layer_t layer)
{
    wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
        clip_ptr->server_ptr->desktop_ptr, layer);
    wlmtk_layer_t *current_ptr = wlmtk_panel_get_layer(panel_ptr);
    if (layer_ptr == current_ptr || NULL == layer_ptr) return;
    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &clip_ptr->output_description,
        clip_ptr->server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(
            clip_ptr->server_ptr->wlr_output_layout_ptr);
    }
    if (NULL != current_ptr) wlmtk_layer_remove_panel(current_ptr, panel_ptr);
    if (NULL != wlr_output_ptr) {
        wlmtk_layer_add_panel(layer_ptr, panel_ptr, wlr_output_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** @return Whether the pointer is on the Dock (or the Clip). */
bool _wlmaker_clip_pointer_on(wlmaker_clip_t *clip_ptr, bool dock)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    if (NULL == server_ptr->input_manager_ptr) return false;
    struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
    wlmaker_dock_column_t column;
    if (!wlmaker_clip_column(server_ptr, dock, &column)) return false;
    // The Clip's grid: On the Clip, or on any of its icons.
    if (column.grid) {
        struct wlr_box b = { .x = column.x, .y = column.y,
                             .width = column.size, .height = column.size };
        return wlr_box_contains_point(&b, c->x, c->y) ||
            SIZE_MAX != wlmaker_clip_grid_icon_at_point(
                server_ptr, c->x, c->y, NULL, NULL);
    }
    size_t n = 1 + column.launchers + column.dockapps;
    struct wlr_box box = { .x = column.x, .y = column.y,
                           .width = column.size, .height = column.size };
    int length = (n - 1) * column.pitch;
    switch (column.place.anchor) {
    case WLR_EDGE_TOP: box.height += length; break;
    case WLR_EDGE_BOTTOM: box.y -= length; box.height += length; break;
    case WLR_EDGE_LEFT: box.width += length; break;
    case WLR_EDGE_RIGHT: box.x -= length; box.width += length; break;
    default: break;
    }
    if (wlr_box_contains_point(&box, c->x, c->y)) return true;
    // An open drawer is part of the Dock.
    for (size_t i = 0; dock && i < clip_ptr->drawers; ++i) {
        struct wlr_box row;
        if (wlmaker_drawer_row_box(clip_ptr->drawers_ptr[i], &row) &&
            wlr_box_contains_point(&row, c->x, c->y)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/** Raises or lowers the dock, when auto-raising. */
static void _wlmaker_clip_level_timer(wlmaker_clip_t *clip_ptr, bool dock)
{
    int i = dock ? 1 : 0;
    if (NULL != clip_ptr->level_timer_ptr[i]) {
        wl_event_source_remove(clip_ptr->level_timer_ptr[i]);
        clip_ptr->level_timer_ptr[i] = NULL;
    }
    if (WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER != clip_ptr->level[i]) return;
    bool on = _wlmaker_clip_pointer_on(clip_ptr, dock);
    if (on != clip_ptr->raised[i]) {
        clip_ptr->raised[i] = on;
        _wlmaker_clip_apply_level(clip_ptr, dock);
    }
    // Raised: Moving the surfaces to the other layer may lose the pointer's
    // "leave". Checks again, to lower it once the pointer is away.
    if (clip_ptr->raised[i]) {
        clip_ptr->level_timer_ptr[i] = wl_event_loop_add_timer(
            wl_display_get_event_loop(clip_ptr->server_ptr->wl_display_ptr),
            dock ? _wlmaker_clip_handle_level_timer_dock :
            _wlmaker_clip_handle_level_timer_clip,
            clip_ptr);
        if (NULL != clip_ptr->level_timer_ptr[i]) {
            wl_event_source_timer_update(clip_ptr->level_timer_ptr[i], 1000);
        }
    }
}

/** Timer callback: Auto-raise or lower the Clip. */
int _wlmaker_clip_handle_level_timer_clip(void *data_ptr)
{
    _wlmaker_clip_level_timer(data_ptr, false);
    return 0;
}

/** Timer callback: Auto-raise or lower the Dock. */
int _wlmaker_clip_handle_level_timer_dock(void *data_ptr)
{
    _wlmaker_clip_level_timer(data_ptr, true);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Stores the number of dock-apps attached to the Dock as `DockappTiles` in
 * the state's `Dock` section: For others placing tiles after them, eg. a
 * system tray.
 */
void _wlmaker_clip_publish_dock_tiles(wlmaker_clip_t *clip_ptr)
{
    if (NULL == clip_ptr->dock_dockapps_ptr) return;
    long n = wlmaker_dockapps_size(clip_ptr->dock_dockapps_ptr);
    if (n == clip_ptr->published_dock_tiles) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", n);
    bspl_dict_t *d = bspl_dict_create();
    bspl_string_t *s = bspl_string_create(buf);
    if (NULL != d && NULL != s &&
        bspl_dict_add(d, "DockappTiles", bspl_object_from_string(s)) &&
        wlmaker_state_store_section(clip_ptr->server_ptr->files_ptr,
                                    clip_ptr->server_ptr->state_fname_ptr,
                                    "Dock", d)) {
        clip_ptr->published_dock_tiles = n;
    }
    if (NULL != s) bspl_string_unref(s);
    if (NULL != d) bspl_dict_unref(d);
}

/* ------------------------------------------------------------------------- */
/**
 * Sets the number of spacer tiles of the Clip (or the Dock): The room for
 * the launchers `wlmdock` shows there, among the dock-apps.
 */
bool _wlmaker_clip_set_spacers(
    wlmaker_clip_t *clip_ptr,
    bool dock,
    size_t spacers,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    wlmtk_dock_t *wlmtk_dock_ptr = dock ?
        clip_ptr->dock_side_ptr : clip_ptr->wlmtk_dock_ptr;
    wlmtk_tile_t **tiles_ptr_ptr = dock ?
        &clip_ptr->dock_spacer_tiles_ptr : &clip_ptr->spacer_tiles_ptr;
    size_t *n_ptr = dock ?
        &clip_ptr->dock_spacer_tiles : &clip_ptr->spacer_tiles;
    if (NULL == wlmtk_dock_ptr) return true;
    if (spacers == *n_ptr) return true;

    for (size_t i = 0; i < *n_ptr; ++i) {
        wlmtk_tile_t *t_ptr = &(*tiles_ptr_ptr)[i];
        if (NULL != wlmtk_tile_element(t_ptr)->parent_container_ptr) {
            wlmtk_dock_remove_tile(wlmtk_dock_ptr, t_ptr);
        }
        wlmtk_tile_fini(t_ptr);
    }
    if (NULL != *tiles_ptr_ptr) free(*tiles_ptr_ptr);
    *tiles_ptr_ptr = NULL;
    *n_ptr = 0;
    if (0 < spacers) {
        *tiles_ptr_ptr = logged_calloc(spacers, sizeof(wlmtk_tile_t));
        if (NULL == *tiles_ptr_ptr) return false;
    }
    for (size_t i = 0; i < spacers; ++i) {
        wlmtk_tile_t *t_ptr = &(*tiles_ptr_ptr)[i];
        if (!_wlmaker_clip_spacer_init(t_ptr, tile_style_ptr)) break;
        *n_ptr = i + 1;
        wlmtk_element_set_visible(wlmtk_tile_element(t_ptr), true);
        // Placed among the dock-apps by _wlmaker_clip_arrange().
        wlmtk_dock_add_tile(wlmtk_dock_ptr, t_ptr);
    }
    bool rv = _wlmaker_clip_update_spacers(clip_ptr, tile_style_ptr);
    _wlmaker_clip_arrange(clip_ptr);
    return rv && *n_ptr == spacers;
}

/* ------------------------------------------------------------------------- */
/**
 * Finds where the Clip's grid is: The position of the Clip's tile and the
 * box of its output, in layout coordinates.
 * @return false if there is no output.
 */
bool _wlmaker_clip_grid_frame(
    wlmaker_clip_t *clip_ptr,
    int *x_ptr,
    int *y_ptr,
    struct wlr_box *output_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    wlmtk_element_get_absolute_position(
        wlmtk_tile_element(&clip_ptr->super_tile), x_ptr, y_ptr);
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, *x_ptr + 1, *y_ptr + 1);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_output_description_first_fnmatch(
            &clip_ptr->output_description, server_ptr->wlr_output_layout_ptr);
    }
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(server_ptr->wlr_output_layout_ptr);
    }
    if (NULL == wlr_output_ptr) return false;
    wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr,
                              wlr_output_ptr, output_ptr);
    return !wlr_box_empty(output_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Computes the Clip's grid (see @ref wlmaker_clip_t::grid): The cells
 * stored for its launchers and dock-apps, those kept; the others get free
 * ones in the column's former order (as before the grid: a state without
 * cells looks the same).
 */
void _wlmaker_clip_update_grid(wlmaker_clip_t *clip_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    size_t nl = clip_ptr->clip_launchers;
    size_t nd = NULL != clip_ptr->dockapps_ptr ?
        wlmaker_dockapps_size(clip_ptr->dockapps_ptr) : 0;
    size_t n = nl + nd;
    wlmaker_clip_cell_t *cells_ptr = NULL;
    bool *stored_ptr = NULL;
    size_t *order_ptr = NULL;
    if (0 < n) {
        cells_ptr = logged_calloc(n, sizeof(wlmaker_clip_cell_t));
        stored_ptr = logged_calloc(n, sizeof(bool));
        order_ptr = logged_calloc(n, sizeof(size_t));
        if (NULL == cells_ptr || NULL == stored_ptr || NULL == order_ptr) {
            // Keeps the grid as it was.
            if (NULL != cells_ptr) free(cells_ptr);
            if (NULL != stored_ptr) free(stored_ptr);
            if (NULL != order_ptr) free(order_ptr);
            return;
        }
    }

    // The cells stored: Launchers as `wlmdock --clip` shows them.
    bspl_array_t *a = 0 < nl ?
        wlmaker_dock_launchers_load(server_ptr, "Clip") : NULL;
    for (size_t i = 0; i < nl; ++i) {
        bspl_dict_t *d = NULL;
        if (NULL != a && i < bspl_array_size(a)) {
            d = bspl_dict_from_object(bspl_array_at(a, i));
        }
        stored_ptr[i] = NULL != d &&
            wlmaker_dock_launcher_cell(d, &cells_ptr[i]);
    }
    if (NULL != a) bspl_array_unref(a);
    for (size_t j = 0; j < nd; ++j) {
        stored_ptr[nl + j] = wlmaker_dockapps_cell(
            clip_ptr->dockapps_ptr,
            wlmaker_dockapps_tile_at(clip_ptr->dockapps_ptr, j),
            &cells_ptr[nl + j]);
    }

    // The former order of the column: Dock-apps after their launchers.
    wlmaker_dock_column_t column = {
        .launchers = nl, .dockapps = nd,
        .dockapp_positions = NULL != clip_ptr->dockapps_ptr ?
        wlmaker_dockapps_positions(clip_ptr->dockapps_ptr, nl) : 0 };
    for (size_t p = 0; p < n; ++p) {
        size_t index;
        bool dockapp = wlmaker_dock_column_dockapp_at(&column, p, &index);
        order_ptr[p] = dockapp ? nl + index : index;
    }

    wlmaker_clip_grid_t grid = { .n = n, .cells_ptr = cells_ptr };
    int x, y;
    struct wlr_box o;
    if (_wlmaker_clip_grid_frame(clip_ptr, &x, &y, &o)) {
        const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
            clip_ptr->wlmtk_dock_ptr);
        int size = clip_ptr->super_tile.style.size;
        wlmaker_clip_grid_bounds(
            (wlmaker_dock_place_t){ pos_ptr->edge, pos_ptr->anchor },
            x, y, size, size + server_ptr->style_ptr->dock.margin.width,
            &o, &grid.lanes, &grid.slots);
    }
    wlmaker_clip_grid_resolve(&grid, stored_ptr, order_ptr, NULL);
    free(order_ptr);

    if (NULL != clip_ptr->grid.cells_ptr) free(clip_ptr->grid.cells_ptr);
    if (NULL != clip_ptr->grid_derived_ptr) free(clip_ptr->grid_derived_ptr);
    clip_ptr->grid = grid;
    // Kept as: Whether each icon has a cell of its own stored.
    clip_ptr->grid_derived_ptr = stored_ptr;
    for (size_t i = 0; i < n; ++i) stored_ptr[i] = !stored_ptr[i];
}

/* ------------------------------------------------------------------------- */
/**
 * Keeps `spacers` empty tiles of `*tiles_ptr_ptr` in `dock_ptr`: Adds new
 * ones (at the end; ordered later) or removes those beyond.
 */
bool _wlmaker_clip_pool_spacers(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t **tiles_ptr_ptr,
    size_t *n_ptr,
    size_t spacers,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    if (spacers == *n_ptr) return true;
    for (size_t i = 0; i < *n_ptr; ++i) {
        wlmtk_tile_t *t_ptr = &(*tiles_ptr_ptr)[i];
        if (NULL != wlmtk_tile_element(t_ptr)->parent_container_ptr) {
            wlmtk_dock_remove_tile(dock_ptr, t_ptr);
        }
        wlmtk_tile_fini(t_ptr);
    }
    if (NULL != *tiles_ptr_ptr) free(*tiles_ptr_ptr);
    *tiles_ptr_ptr = NULL;
    *n_ptr = 0;
    if (0 == spacers) return true;
    *tiles_ptr_ptr = logged_calloc(spacers, sizeof(wlmtk_tile_t));
    if (NULL == *tiles_ptr_ptr) return false;
    // A freshly created buffer is all zero, ie. fully transparent.
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        tile_style_ptr->size, tile_style_ptr->size);
    for (size_t i = 0; i < spacers; ++i) {
        wlmtk_tile_t *t_ptr = &(*tiles_ptr_ptr)[i];
        if (!_wlmaker_clip_spacer_init(t_ptr, tile_style_ptr)) break;
        *n_ptr = i + 1;
        if (NULL != wlr_buffer_ptr) {
            wlmtk_tile_set_background_buffer(t_ptr, wlr_buffer_ptr);
        }
        wlmtk_element_set_visible(wlmtk_tile_element(t_ptr), true);
        wlmtk_dock_add_tile(dock_ptr, t_ptr);
    }
    if (NULL != wlr_buffer_ptr) wlr_buffer_drop(wlr_buffer_ptr);
    return *n_ptr == spacers;
}

/* ------------------------------------------------------------------------- */
/** @return The dock (the Clip's, or a lane's) holding `tile_ptr`, or NULL. */
static wlmtk_dock_t *_wlmaker_clip_dock_holding(
    wlmaker_clip_t *clip_ptr,
    wlmtk_tile_t *tile_ptr)
{
    if (0 <= wlmtk_dock_tile_index(clip_ptr->wlmtk_dock_ptr, tile_ptr)) {
        return clip_ptr->wlmtk_dock_ptr;
    }
    for (size_t i = 0; i < clip_ptr->lanes; ++i) {
        wlmtk_dock_t *d_ptr = clip_ptr->lanes_ptr[i].dock_ptr;
        if (NULL != d_ptr && 0 <= wlmtk_dock_tile_index(d_ptr, tile_ptr)) {
            return d_ptr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Moves `tile_ptr` into `dock_ptr`, from whichever of the Clip's has it. */
static void _wlmaker_clip_move_tile_to(
    wlmaker_clip_t *clip_ptr,
    wlmtk_tile_t *tile_ptr,
    wlmtk_dock_t *dock_ptr)
{
    wlmtk_dock_t *from_ptr = _wlmaker_clip_dock_holding(clip_ptr, tile_ptr);
    if (from_ptr == dock_ptr) return;
    if (NULL != from_ptr) wlmtk_dock_remove_tile(from_ptr, tile_ptr);
    // The dock's box sets the position along it only.
    wlmtk_element_set_position(wlmtk_tile_element(tile_ptr), 0, 0);
    wlmtk_dock_add_tile(dock_ptr, tile_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmaker_dockapps_host_t::attach: Into the Clip's dock. */
void _wlmaker_clip_host_attach(void *ud_ptr, wlmtk_tile_t *tile_ptr)
{
    wlmaker_clip_t *clip_ptr = ud_ptr;
    wlmtk_dock_add_tile(clip_ptr->wlmtk_dock_ptr, tile_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmaker_dockapps_host_t::detach: From its lane's dock. */
void _wlmaker_clip_host_detach(void *ud_ptr, wlmtk_tile_t *tile_ptr)
{
    wlmaker_clip_t *clip_ptr = ud_ptr;
    wlmtk_dock_t *dock_ptr = _wlmaker_clip_dock_holding(clip_ptr, tile_ptr);
    if (NULL != dock_ptr) wlmtk_dock_remove_tile(dock_ptr, tile_ptr);
}

/* ------------------------------------------------------------------------- */
/** Destroys the lane's dock and its spacers. Must hold no dock-app. */
void _wlmaker_clip_destroy_lane(
    wlmaker_clip_t *clip_ptr,
    wlmaker_clip_lane_t *lane_ptr)
{
    if (NULL == lane_ptr->dock_ptr) return;
    _wlmaker_clip_pool_spacers(lane_ptr->dock_ptr, &lane_ptr->spacers_ptr,
                               &lane_ptr->spacers, 0,
                               &clip_ptr->super_tile.style);
    // Anything else (a dock-app, when shutting down): Back to the Clip's.
    while (0 < wlmtk_dock_tiles(lane_ptr->dock_ptr)) {
        wlmtk_tile_t *t_ptr = NULL;
        for (size_t j = 0; NULL == t_ptr && NULL != clip_ptr->dockapps_ptr &&
                 j < wlmaker_dockapps_size(clip_ptr->dockapps_ptr); ++j) {
            wlmtk_tile_t *c_ptr = wlmaker_dockapps_tile_at(
                clip_ptr->dockapps_ptr, j);
            if (0 <= wlmtk_dock_tile_index(lane_ptr->dock_ptr, c_ptr)) {
                t_ptr = c_ptr;
            }
        }
        if (NULL == t_ptr) break;
        _wlmaker_clip_move_tile_to(clip_ptr, t_ptr, clip_ptr->wlmtk_dock_ptr);
    }
    wlmtk_layer_t *l_ptr = wlmtk_panel_get_layer(
        wlmtk_dock_panel(lane_ptr->dock_ptr));
    if (NULL != l_ptr) {
        wlmtk_layer_remove_panel(l_ptr, wlmtk_dock_panel(lane_ptr->dock_ptr));
    }
    wlmtk_dock_destroy(lane_ptr->dock_ptr);
    lane_ptr->dock_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** @return The positioning of lane `lane`: The Clip's, `lane` tiles away. */
static wlmtk_dock_positioning_t _wlmaker_clip_lane_positioning(
    wlmaker_clip_t *clip_ptr,
    int lane)
{
    wlmtk_dock_positioning_t pos = *wlmtk_dock_get_positioning(
        clip_ptr->wlmtk_dock_ptr);
    int pitch = clip_ptr->super_tile.style.size +
        clip_ptr->server_ptr->style_ptr->dock.margin.width;
    pos.edge_offset = pos.edge_offset + lane * pitch;
    pos.overlay = true;
    return pos;
}

/* ------------------------------------------------------------------------- */
/**
 * Puts the lanes' docks where the Clip's is: Its layer, its output. After
 * the Clip's level or output changed.
 */
void _wlmaker_clip_replace_lanes(wlmaker_clip_t *clip_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    wlmtk_layer_t *layer_ptr = wlmtk_panel_get_layer(
        wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr));
    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &clip_ptr->output_description, server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(server_ptr->wlr_output_layout_ptr);
    }
    for (size_t i = 0; i < clip_ptr->lanes; ++i) {
        if (NULL == clip_ptr->lanes_ptr[i].dock_ptr) continue;
        wlmtk_panel_t *p_ptr = wlmtk_dock_panel(clip_ptr->lanes_ptr[i].dock_ptr);
        wlmtk_layer_t *current_ptr = wlmtk_panel_get_layer(p_ptr);
        if (NULL != current_ptr) wlmtk_layer_remove_panel(current_ptr, p_ptr);
        if (NULL != layer_ptr && NULL != wlr_output_ptr) {
            wlmtk_layer_add_panel(layer_ptr, p_ptr, wlr_output_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Creates the dock of lane `lane` (1 ..): Beside the Clip's, `lane` away. */
static bool _wlmaker_clip_create_lane(
    wlmaker_clip_t *clip_ptr,
    wlmaker_clip_lane_t *lane_ptr,
    int lane)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    wlmtk_dock_positioning_t pos = _wlmaker_clip_lane_positioning(
        clip_ptr, lane);
    lane_ptr->dock_ptr = wlmtk_dock_create(&pos, &server_ptr->style_ptr->dock);
    if (NULL == lane_ptr->dock_ptr) return false;
    wlmtk_element_set_visible(wlmtk_dock_element(lane_ptr->dock_ptr), true);
    // Where the Clip is: Its layer, its output.
    wlmtk_panel_t *clip_panel_ptr = wlmtk_dock_panel(clip_ptr->wlmtk_dock_ptr);
    wlmtk_layer_t *layer_ptr = wlmtk_panel_get_layer(clip_panel_ptr);
    if (NULL == layer_ptr) {
        layer_ptr = wlmtk_desktop_get_layer(
            server_ptr->desktop_ptr,
            wlmaker_clip_layer(server_ptr, false));
    }
    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &clip_ptr->output_description, server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(server_ptr->wlr_output_layout_ptr);
    }
    if (NULL != layer_ptr && NULL != wlr_output_ptr) {
        wlmtk_layer_add_panel(layer_ptr, wlmtk_dock_panel(lane_ptr->dock_ptr),
                              wlr_output_ptr);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Arranges the Clip's tiles on its grid: Its column (lane 0) holds the Clip,
 * then a tile per slot: a dock-app, or a spacer (room for a launcher, or a
 * hole). Each further lane with dock-apps has a dock of its own, beside.
 */
void _wlmaker_clip_arrange_grid(wlmaker_clip_t *clip_ptr)
{
    if (NULL == clip_ptr->dockapps_ptr) return;
    _wlmaker_clip_update_grid(clip_ptr);
    const wlmaker_clip_grid_t *g_ptr = &clip_ptr->grid;
    size_t nl = clip_ptr->clip_launchers;
    size_t nd = g_ptr->n - nl;
    const struct wlmtk_tile_style *ts_ptr = &clip_ptr->super_tile.style;

    // Lanes holding dock-apps.
    size_t lanes = 0;
    for (size_t j = 0; j < nd; ++j) {
        int l = g_ptr->cells_ptr[nl + j].lane;
        if (0 < l && (size_t)l > lanes) lanes = l;
    }
    if (lanes > clip_ptr->lanes) {
        wlmaker_clip_lane_t *l_ptr = realloc(
            clip_ptr->lanes_ptr, lanes * sizeof(wlmaker_clip_lane_t));
        if (NULL == l_ptr) {
            bs_log(BS_ERROR, "Failed realloc() for the Clip's lanes.");
            lanes = clip_ptr->lanes;
        } else {
            memset(&l_ptr[clip_ptr->lanes], 0,
                   (lanes - clip_ptr->lanes) * sizeof(wlmaker_clip_lane_t));
            clip_ptr->lanes_ptr = l_ptr;
            clip_ptr->lanes = lanes;
        }
    }
    for (size_t l = 1; l <= clip_ptr->lanes; ++l) {
        bool used = false;
        for (size_t j = 0; !used && j < nd; ++j) {
            used = (int)l == g_ptr->cells_ptr[nl + j].lane;
        }
        wlmaker_clip_lane_t *lane_ptr = &clip_ptr->lanes_ptr[l - 1];
        if (used && NULL == lane_ptr->dock_ptr &&
            !_wlmaker_clip_create_lane(clip_ptr, lane_ptr, l)) {
            bs_log(BS_ERROR, "Failed to create lane %zu of the Clip.", l);
        }
        // Where the Clip is now: Eg. moved to another corner.
        if (NULL != lane_ptr->dock_ptr) {
            wlmtk_dock_positioning_t pos = _wlmaker_clip_lane_positioning(
                clip_ptr, l);
            const wlmtk_dock_positioning_t *cur_ptr =
                wlmtk_dock_get_positioning(lane_ptr->dock_ptr);
            if (0 != memcmp(&pos, cur_ptr, sizeof(pos)) &&
                !wlmtk_dock_set_positioning(lane_ptr->dock_ptr, &pos)) {
                // Another orientation: A new dock.
                _wlmaker_clip_destroy_lane(clip_ptr, lane_ptr);
                if (used) _wlmaker_clip_create_lane(clip_ptr, lane_ptr, l);
            }
        }
    }

    // Each dock-app into the dock of its lane.
    for (size_t j = 0; j < nd; ++j) {
        int l = g_ptr->cells_ptr[nl + j].lane;
        wlmtk_dock_t *dock_ptr = clip_ptr->wlmtk_dock_ptr;
        if (0 < l && (size_t)l <= clip_ptr->lanes &&
            NULL != clip_ptr->lanes_ptr[l - 1].dock_ptr) {
            dock_ptr = clip_ptr->lanes_ptr[l - 1].dock_ptr;
        }
        _wlmaker_clip_move_tile_to(
            clip_ptr, wlmaker_dockapps_tile_at(clip_ptr->dockapps_ptr, j),
            dock_ptr);
    }

    // Lanes without dock-apps go away; the others get their order.
    for (size_t l = 0; l <= clip_ptr->lanes; ++l) {
        wlmtk_dock_t *dock_ptr;
        wlmtk_tile_t **spacers_ptr_ptr;
        size_t *spacers_ptr;
        if (0 == l) {
            dock_ptr = clip_ptr->wlmtk_dock_ptr;
            spacers_ptr_ptr = &clip_ptr->spacer_tiles_ptr;
            spacers_ptr = &clip_ptr->spacer_tiles;
        } else {
            wlmaker_clip_lane_t *lane_ptr = &clip_ptr->lanes_ptr[l - 1];
            dock_ptr = lane_ptr->dock_ptr;
            spacers_ptr_ptr = &lane_ptr->spacers_ptr;
            spacers_ptr = &lane_ptr->spacers;
        }
        if (NULL == dock_ptr) continue;
        // The column reaches its last icon; another lane its last dock-app
        // (its launchers are drawn by `wlmdock --clip`).
        int last = 0, dockapps = 0;
        for (size_t i = 0; i < g_ptr->n; ++i) {
            const wlmaker_clip_cell_t *c_ptr = &g_ptr->cells_ptr[i];
            if ((int)l != c_ptr->lane) continue;
            if (i >= nl) ++dockapps;
            if (0 == l || i >= nl) last = BS_MAX(last, c_ptr->slot);
        }
        if (0 < l && 0 == dockapps) {
            _wlmaker_clip_destroy_lane(clip_ptr, &clip_ptr->lanes_ptr[l - 1]);
            continue;
        }
        int first = (0 == l) ? 1 : 0;
        size_t spacers = (size_t)BS_MAX(0, last + 1 - first - dockapps);
        if (!_wlmaker_clip_pool_spacers(dock_ptr, spacers_ptr_ptr, spacers_ptr,
                                        spacers, ts_ptr)) {
            bs_log(BS_WARNING, "Clip lane %zu: Failed to keep %zu spacers.",
                   l, spacers);
        }
        size_t k = 0;
        for (int slot = first; slot <= last; ++slot) {
            wlmaker_clip_cell_t c = { .lane = l, .slot = slot };
            size_t i = wlmaker_clip_grid_icon_at(g_ptr, &c, SIZE_MAX);
            wlmtk_tile_t *tile_ptr = NULL;
            if (SIZE_MAX != i && i >= nl) {
                tile_ptr = wlmaker_dockapps_tile_at(
                    clip_ptr->dockapps_ptr, i - nl);
            } else if (k < *spacers_ptr) {
                tile_ptr = &(*spacers_ptr_ptr)[k++];
            }
            if (NULL != tile_ptr) {
                wlmtk_dock_move_tile(dock_ptr, tile_ptr, slot);
            }
        }
    }
    // Lanes past the last used: Dropped.
    while (0 < clip_ptr->lanes &&
           NULL == clip_ptr->lanes_ptr[clip_ptr->lanes - 1].dock_ptr) {
        --clip_ptr->lanes;
    }
    if (0 == clip_ptr->lanes && NULL != clip_ptr->lanes_ptr) {
        free(clip_ptr->lanes_ptr);
        clip_ptr->lanes_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Arranges the tiles of the Clip and of the Dock's dock-apps: The room for
 * each launcher (a spacer) and the dock-apps, in the column's order. Then
 * publishes the layout for `wlmdock`.
 */
void _wlmaker_clip_arrange(wlmaker_clip_t *clip_ptr)
{
    if (clip_ptr->arranging) return;
    clip_ptr->arranging = true;
    // The Clip: On its grid, as Window Maker's.
    _wlmaker_clip_arrange_grid(clip_ptr);
    for (int dock = 1; dock < 2; ++dock) {
        wlmaker_dock_column_t column;
        if (!wlmaker_clip_column_id(
                clip_ptr->server_ptr,
                dock ? WLMAKER_COLUMN_DOCK : WLMAKER_COLUMN_CLIP,
                &column)) continue;
        wlmtk_dock_t *wlmtk_dock_ptr = dock ?
            clip_ptr->dock_side_ptr : clip_ptr->wlmtk_dock_ptr;
        wlmaker_dockapps_t *dockapps_ptr = dock ?
            clip_ptr->dock_dockapps_ptr : clip_ptr->dockapps_ptr;
        wlmtk_tile_t *spacers_ptr = dock ?
            clip_ptr->dock_spacer_tiles_ptr : clip_ptr->spacer_tiles_ptr;
        size_t spacers = dock ?
            clip_ptr->dock_spacer_tiles : clip_ptr->spacer_tiles;
        if (NULL == wlmtk_dock_ptr || NULL == dockapps_ptr) continue;
        // The Clip's own tile comes first; the Dock's is `wlmdock`'s.
        size_t first = dock ? 0 : 1;
        for (size_t p = 0; p < column.launchers + column.dockapps; ++p) {
            size_t index;
            wlmtk_tile_t *tile_ptr = NULL;
            if (wlmaker_dock_column_dockapp_at(&column, p, &index)) {
                tile_ptr = wlmaker_dockapps_tile_at(dockapps_ptr, index);
            } else if (index < spacers) {
                tile_ptr = &spacers_ptr[index];
            }
            if (NULL != tile_ptr) {
                wlmtk_dock_move_tile(wlmtk_dock_ptr, tile_ptr, first + p);
            }
        }
    }
    // The drawers stick out at their tile on the Dock.
    for (size_t i = 0; i < clip_ptr->drawers; ++i) {
        wlmaker_drawer_update_position(clip_ptr->drawers_ptr[i]);
    }
    _wlmaker_clip_publish_layout(clip_ptr);
    clip_ptr->arranging = false;
}

/* ------------------------------------------------------------------------- */
/** The dock-apps of the Clip or the Dock changed: Arranges them. */
void _wlmaker_clip_handle_dockapps_changed(void *ud_ptr)
{
    _wlmaker_clip_arrange(ud_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds a string to an array. */
static bool _wlmaker_clip_push_string(bspl_array_t *a, const char *str_ptr)
{
    bspl_string_t *s_ptr = bspl_string_create(str_ptr);
    bool rv = NULL != s_ptr &&
        bspl_array_push_back(a, bspl_object_from_string(s_ptr));
    if (NULL != s_ptr) bspl_string_unref(s_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Adds a string to a dict. */
static bool _wlmaker_clip_add_string(
    bspl_dict_t *d,
    const char *key_ptr,
    const char *str_ptr)
{
    bspl_string_t *s_ptr = bspl_string_create(str_ptr);
    bool rv = NULL != s_ptr &&
        bspl_dict_add(d, key_ptr, bspl_object_from_string(s_ptr));
    if (NULL != s_ptr) bspl_string_unref(s_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Adds the layout of the Clip's grid to `dict_ptr`, as section "Clip":
 * `Cells`, the cell of each launcher `wlmdock --clip` shows; `Origin`, the
 * slot its surface starts at (1, after the Clip; 0 with a launcher beside
 * the Clip); `Positions` (the slot of the dock-apps in the column) and
 * `DockappCells` for other clients.
 */
static bool _wlmaker_clip_grid_layout_section(
    wlmaker_clip_t *clip_ptr,
    bspl_dict_t *dict_ptr)
{
    const wlmaker_clip_grid_t *g_ptr = &clip_ptr->grid;
    size_t nl = BS_MIN(clip_ptr->clip_launchers, g_ptr->n);
    bspl_dict_t *section_ptr = bspl_dict_create();
    bspl_array_t *cells_ptr = bspl_array_create();
    bspl_dict_t *positions_ptr = bspl_dict_create();
    bspl_dict_t *dockapp_cells_ptr = bspl_dict_create();
    bool rv = NULL != section_ptr && NULL != cells_ptr &&
        NULL != positions_ptr && NULL != dockapp_cells_ptr;
    char buf[32];
    int origin = 1;
    for (size_t i = 0; rv && i < nl; ++i) {
        wlmaker_clip_cell_format(&g_ptr->cells_ptr[i], buf, sizeof(buf));
        rv = _wlmaker_clip_push_string(cells_ptr, buf);
        if (0 == g_ptr->cells_ptr[i].slot) origin = 0;
    }
    for (size_t i = nl; rv && i < g_ptr->n; ++i) {
        const char *name_ptr = wlmaker_dockapps_name(
            clip_ptr->dockapps_ptr,
            wlmaker_dockapps_tile_at(clip_ptr->dockapps_ptr, i - nl));
        if (NULL == name_ptr) continue;
        wlmaker_clip_cell_format(&g_ptr->cells_ptr[i], buf, sizeof(buf));
        rv = _wlmaker_clip_add_string(dockapp_cells_ptr, name_ptr, buf);
        if (rv && 0 == g_ptr->cells_ptr[i].lane) {
            snprintf(buf, sizeof(buf), "%d", g_ptr->cells_ptr[i].slot);
            rv = _wlmaker_clip_add_string(positions_ptr, name_ptr, buf);
        }
    }
    snprintf(buf, sizeof(buf), "%d", origin);
    rv = rv &&
        bspl_dict_add(section_ptr, "Cells",
                      bspl_object_from_array(cells_ptr)) &&
        _wlmaker_clip_add_string(section_ptr, "Origin", buf) &&
        bspl_dict_add(section_ptr, "Positions",
                      bspl_object_from_dict(positions_ptr)) &&
        bspl_dict_add(section_ptr, "DockappCells",
                      bspl_object_from_dict(dockapp_cells_ptr)) &&
        bspl_dict_add(dict_ptr, "Clip", bspl_object_from_dict(section_ptr));
    if (NULL != dockapp_cells_ptr) bspl_dict_unref(dockapp_cells_ptr);
    if (NULL != positions_ptr) bspl_dict_unref(positions_ptr);
    if (NULL != cells_ptr) bspl_array_unref(cells_ptr);
    if (NULL != section_ptr) bspl_dict_unref(section_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Adds the layout of the column `id` as `section_ptr` to `dict_ptr`. */
static bool _wlmaker_clip_layout_section(
    wlmaker_clip_t *clip_ptr,
    int id,
    const char *section_ptr,
    bspl_dict_t *dict_ptr)
{
    wlmaker_dock_column_t column;
    if (!wlmaker_clip_column_id(clip_ptr->server_ptr, id, &column)) {
        return true;
    }
    wlmaker_dockapps_t *dockapps_ptr = wlmaker_clip_dockapps_id(
        clip_ptr->server_ptr, id);
    bspl_dict_t *section_dict_ptr = bspl_dict_create();
    bspl_array_t *gaps_ptr = bspl_array_create();
    bspl_dict_t *positions_ptr = bspl_dict_create();
    bool rv = NULL != section_dict_ptr && NULL != gaps_ptr &&
        NULL != positions_ptr;
    // For each launcher: The dock-apps right before it.
    size_t gap = 0;
    for (size_t p = 0;
         rv && p < column.launchers + column.dockapps;
         ++p) {
        size_t index;
        char buf[32];
        if (wlmaker_dock_column_dockapp_at(&column, p, &index)) {
            ++gap;
            const char *name_ptr = NULL == dockapps_ptr ? NULL :
                wlmaker_dockapps_name(
                    dockapps_ptr,
                    wlmaker_dockapps_tile_at(dockapps_ptr, index));
            if (NULL == name_ptr) continue;
            // Slot from the anchor: 0 is the Dock's main tile, or the Clip.
            snprintf(buf, sizeof(buf), "%zu", 1 + p);
            bspl_string_t *v = bspl_string_create(buf);
            rv = NULL != v && bspl_dict_add(positions_ptr, name_ptr,
                                            bspl_object_from_string(v));
            if (NULL != v) bspl_string_unref(v);
            continue;
        }
        snprintf(buf, sizeof(buf), "%zu", gap);
        gap = 0;
        bspl_string_t *v = bspl_string_create(buf);
        rv = NULL != v && bspl_array_push_back(gaps_ptr,
                                                bspl_object_from_string(v));
        if (NULL != v) bspl_string_unref(v);
    }
    rv = rv &&
        bspl_dict_add(section_dict_ptr, "Gaps",
                      bspl_object_from_array(gaps_ptr)) &&
        bspl_dict_add(section_dict_ptr, "Positions",
                      bspl_object_from_dict(positions_ptr)) &&
        bspl_dict_add(dict_ptr, section_ptr,
                      bspl_object_from_dict(section_dict_ptr));
    if (NULL != positions_ptr) bspl_dict_unref(positions_ptr);
    if (NULL != gaps_ptr) bspl_array_unref(gaps_ptr);
    if (NULL != section_dict_ptr) bspl_dict_unref(section_dict_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Publishes how launchers and dock-apps share the Dock and the Clip (see
 * @ref wlmaker_dock_layout_fname): `wlmdock` leaves room for the dock-apps
 * among its launchers. Tells `wlmdock` when it changed.
 */
void _wlmaker_clip_publish_layout(wlmaker_clip_t *clip_ptr)
{
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    // Not before the Clip is complete.
    if (server_ptr->clip_ptr != clip_ptr) return;
    bspl_dict_t *dict_ptr = bspl_dict_create();
    if (NULL == dict_ptr) return;
    bs_dynbuf_t dynbuf = {};
    bool rv = _wlmaker_clip_layout_section(
        clip_ptr, WLMAKER_COLUMN_DOCK, "Dock", dict_ptr) &&
        _wlmaker_clip_grid_layout_section(clip_ptr, dict_ptr) &&
        bs_dynbuf_init(&dynbuf, 1024, SIZE_MAX) &&
        bspl_object_write(bspl_object_from_dict(dict_ptr), &dynbuf) &&
        bs_dynbuf_append_char(&dynbuf, '\0');
    bspl_dict_unref(dict_ptr);
    if (!rv) {
        bs_dynbuf_fini(&dynbuf);
        return;
    }
    const char *text_ptr = dynbuf.data_ptr;
    if (NULL != clip_ptr->published_layout_ptr &&
        0 == strcmp(clip_ptr->published_layout_ptr, text_ptr)) {
        bs_dynbuf_fini(&dynbuf);
        return;
    }
    char *fname_ptr = wlmaker_dock_layout_fname(server_ptr->wl_socket_name_ptr);
    if (NULL != fname_ptr) {
        // Without the terminating NUL.
        dynbuf.length -= 1;
        if (bs_dynbuf_write_file(&dynbuf, fname_ptr, S_IRUSR | S_IWUSR)) {
            if (NULL != clip_ptr->published_layout_ptr) {
                free(clip_ptr->published_layout_ptr);
            }
            clip_ptr->published_layout_ptr = logged_strdup(text_ptr);
            _wlmaker_clip_signal_wlmdock(server_ptr);
        } else {
            bs_log(BS_WARNING, "Failed to write the docks' layout to \"%s\"",
                   fname_ptr);
        }
        free(fname_ptr);
    }
    bs_dynbuf_fini(&dynbuf);
}

/* == Unit tests =========================================================== */

static void test_draw_tile(bs_test_t *test_ptr);
static void test_resolve_image(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t wlmaker_clip_test_cases[] = {
    { true, "draw_tile", test_draw_tile },
    { true, "resolve_image", test_resolve_image },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_clip_test_set = BS_TEST_SET(
    true, "clip", wlmaker_clip_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests that the clip tile is drawn correctly. */
void test_draw_tile(bs_test_t *test_ptr)
{
    static const struct wlmtk_tile_style style = {
        .fill = {
            .type = WLMTK_STYLE_COLOR_DGRADIENT,
            .param = { .dgradient = { .from = 0xffa6a6b6, .to = 0xff515561 } }
        },
        .bezel_width = 2,
        .size = 64
    };
    struct wlr_buffer* wlr_buffer_ptr;
    bs_gfxbuf_t *gfxbuf_ptr;

    wlr_buffer_ptr = _wlmaker_clip_create_tile(&style, false, false);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, wlr_buffer_ptr);
    gfxbuf_ptr = bs_gfxbuf_from_wlr_buffer(wlr_buffer_ptr);
    BS_TEST_VERIFY_GFXBUF_EQUALS_PNG(
        test_ptr, gfxbuf_ptr, "clip_raised.png");
    wlr_buffer_drop(wlr_buffer_ptr);

    wlr_buffer_ptr = _wlmaker_clip_create_tile(&style, true, true);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, wlr_buffer_ptr);
    gfxbuf_ptr = bs_gfxbuf_from_wlr_buffer(wlr_buffer_ptr);
    BS_TEST_VERIFY_GFXBUF_EQUALS_PNG(
        test_ptr, gfxbuf_ptr, "clip_pressed.png");
    wlr_buffer_drop(wlr_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests finding the Clip's image: `Icon`, by name or path, and fallbacks. */
void test_resolve_image(bs_test_t *test_ptr)
{
    bs_test_setenv(test_ptr, "XDG_DATA_DIRS", WLMAKER_SOURCE_DIR "/share");
    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, files_ptr);

    // A name in wlmaker/icons.
    char *p = _wlmaker_clip_resolve_image(files_ptr, "dock-56x56.png");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
    BS_TEST_VERIFY_STREQ(
        test_ptr, "dock-56x56.png", p + strlen(p) - strlen("dock-56x56.png"));
    free(p);

    // A path.
    p = _wlmaker_clip_resolve_image(
        files_ptr, WLMAKER_SOURCE_DIR "/share/wlmaker/icons/chrome-56x56.png");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
    BS_TEST_VERIFY_STREQ(
        test_ptr, "chrome-56x56.png",
        p + strlen(p) - strlen("chrome-56x56.png"));
    free(p);

    // Missing, or no `Icon`: The default icon. (Without a build-time icon).
    if ('\0' == *WLMAKER_CLIP_DEFAULT_ICON) {
        const char *icons[] = { "does-not-exist.png", "/does/not/exist", NULL };
        for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
            p = _wlmaker_clip_resolve_image(files_ptr, icons[i]);
            BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
            BS_TEST_VERIFY_STREQ(
                test_ptr, "clip-56x56.png",
                p + strlen(p) - strlen("clip-56x56.png"));
            free(p);
        }
    }

    wlm_util_files_destroy(files_ptr);
}

/* == End of clip.c ======================================================== */
