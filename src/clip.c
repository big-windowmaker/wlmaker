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
#include "backend/output_config.h"
#include "context_menu.h"
#include "dockapps.h"
#include "dock_position.h"
#include "i18n.h"
#include "input/manager.h"
#include "toolkit/toolkit.h"
#include "util/files.h"

/* == Declarations ========================================================= */

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
     * Empty tiles after the clip, one per element of `Clip.Launchers`: Room
     * for the launchers shown there by `wlmdock --clip`. Dock-apps follow.
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
static bool _wlmaker_clip_set_spacers(
    wlmaker_clip_t *clip_ptr,
    size_t spacers,
    const struct wlmtk_tile_style *tile_style_ptr);
static void _wlmaker_clip_atfork_child(void);
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

    bspl_array_t *launchers_ptr = bspl_dict_get_array(dict_ptr, "Launchers");
    clip_ptr->spacer_tiles = (NULL != launchers_ptr) ?
        bspl_array_size(launchers_ptr) : 0;
    if (0 < clip_ptr->spacer_tiles) {
        clip_ptr->spacer_tiles_ptr = logged_calloc(
            clip_ptr->spacer_tiles, sizeof(wlmtk_tile_t));
        if (NULL == clip_ptr->spacer_tiles_ptr) {
            clip_ptr->spacer_tiles = 0;
            wlmaker_clip_destroy(clip_ptr);
            return NULL;
        }
        for (size_t i = 0; i < clip_ptr->spacer_tiles; ++i) {
            if (!wlmtk_tile_init(&clip_ptr->spacer_tiles_ptr[i],
                                 &style_ptr->tile)) {
                clip_ptr->spacer_tiles = i;
                wlmaker_clip_destroy(clip_ptr);
                return NULL;
            }
            wlmtk_element_set_visible(
                wlmtk_tile_element(&clip_ptr->spacer_tiles_ptr[i]), true);
            wlmtk_dock_add_tile(
                clip_ptr->wlmtk_dock_ptr, &clip_ptr->spacer_tiles_ptr[i]);
        }
        if (!_wlmaker_clip_update_spacers(clip_ptr, &style_ptr->tile)) {
            wlmaker_clip_destroy(clip_ptr);
            return NULL;
        }
    }

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
    char *fname_ptr = wlm_util_files_xdg_config_fname(
        server_ptr->files_ptr, "Dockapps.plist");
    clip_ptr->dockapps_ptr = wlmaker_dockapps_create(
        clip_ptr->wlmtk_dock_ptr,
        bspl_dict_get_array(dict_ptr, "Dockapps"),
        fname_ptr);
    if (NULL != fname_ptr) free(fname_ptr);
    if (NULL == clip_ptr->dockapps_ptr) {
        wlmaker_clip_destroy(clip_ptr);
        return NULL;
    }
    server_ptr->dockapps_ptr = clip_ptr->dockapps_ptr;

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
    bs_log(BS_INFO, "Created clip %p", clip_ptr);
    return clip_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_destroy(wlmaker_clip_t *clip_ptr)
{
    if (NULL != clip_ptr->server_ptr) {
        wlmaker_context_menu_cancel(clip_ptr->server_ptr, clip_ptr);
    }
    if (NULL != clip_ptr->preview_event_source_ptr) {
        wl_event_source_remove(clip_ptr->preview_event_source_ptr);
        clip_ptr->preview_event_source_ptr = NULL;
    }
    if (NULL != clip_ptr->sigusr1_event_source_ptr) {
        wl_event_source_remove(clip_ptr->sigusr1_event_source_ptr);
        clip_ptr->sigusr1_event_source_ptr = NULL;
    }
    if (NULL != clip_ptr->server_ptr) {
        clip_ptr->server_ptr->clip_dock_ptr = NULL;
        clip_ptr->server_ptr->dockapps_ptr = NULL;
    }
    if (NULL != clip_ptr->dockapps_ptr) {
        wlmaker_dockapps_destroy(clip_ptr->dockapps_ptr);
        clip_ptr->dockapps_ptr = NULL;
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
    // wlmdock (the Dock, and the Clip's launchers) re-reads the state file
    // on SIGUSR1.
    struct wl_list *clients_ptr = wl_display_get_client_list(
        server_ptr->wl_display_ptr);
    struct wl_client *wl_client_ptr;
    wl_client_for_each(wl_client_ptr, clients_ptr) {
        char *name_ptr = wlmaker_dockapps_client_name(wl_client_ptr);
        if (NULL == name_ptr) continue;
        if (0 == strcmp(name_ptr, "wlmdock")) {
            pid_t pid = 0;
            wl_client_get_credentials(wl_client_ptr, &pid, NULL, NULL);
            if (1 < pid && 0 != kill(pid, SIGUSR1)) {
                bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%"PRIdMAX
                       ", SIGUSR1)", (intmax_t)pid);
            }
        }
        free(name_ptr);
    }
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

    _wlmaker_clip_signal_wlmdock(server_ptr);
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
        _wlmaker_clip_set_spacers(clip_ptr, bspl_array_size(a),
                                  &server_ptr->style_ptr->tile);
        // `wlmdock --clip` shows them.
        _wlmaker_clip_signal_wlmdock(server_ptr);
    }
    if (NULL != d) bspl_dict_unref(d);
    bspl_array_unref(a);
}

/** Moves the Clip to the corner `corner` XOR `flip`. */
static void _wlmaker_clip_flip(wlmaker_clip_t *clip_ptr, uint32_t flip)
{
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        clip_ptr->wlmtk_dock_ptr);
    wlmaker_dock_place_t place = { pos_ptr->edge, pos_ptr->anchor };
    place = wlmaker_dock_place_from_corner(
        wlmaker_dock_place_corner(place) ^ flip,
        wlmaker_dock_place_vertical(place));
    wlmaker_clip_move_dock(clip_ptr->server_ptr, "Clip", place);
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
        server_ptr, clip_ptr, "Clip");
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
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        clip_ptr->wlmtk_dock_ptr);
    uint32_t corner = pos_ptr->edge | pos_ptr->anchor;
    rv = rv && wlmaker_context_menu_add_item(
        cm_ptr,
        wlmaker_i18n((corner & WLR_EDGE_LEFT) ?
                     "Move to Right Side" : "Move to Left Side"),
        _wlmaker_clip_menu_flip_side, clip_ptr);
    rv = rv && wlmaker_context_menu_add_item(
        cm_ptr,
        wlmaker_i18n((corner & WLR_EDGE_TOP) ? "Move to Bottom" : "Move to Top"),
        _wlmaker_clip_menu_flip_end, clip_ptr);
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
#ifdef WLMAKER_SOURCE_DIR
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

    wlmtk_dock_set_style(
        clip_ptr->wlmtk_dock_ptr,
        &style_ptr->dock,
        &style_ptr->tile);

    // Need to apply the button state (again) after @ref wlmtk_dock_set_style,
    // since the latter will apply the generic tile backend. Which the tile
    // overwrites.
    _wlmaker_clip_apply_button_state(clip_ptr);
    _wlmaker_clip_update_spacers(clip_ptr, &style_ptr->tile);
}

/* ------------------------------------------------------------------------- */
/** Gives the spacer tiles a transparent background. */
bool _wlmaker_clip_update_spacers(
    wlmaker_clip_t *clip_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    if (0 == clip_ptr->spacer_tiles) return true;
    // A freshly created buffer is all zero, ie. fully transparent.
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        tile_style_ptr->size, tile_style_ptr->size);
    if (NULL == wlr_buffer_ptr) return false;
    bool rv = true;
    for (size_t i = 0; i < clip_ptr->spacer_tiles; ++i) {
        rv &= wlmtk_tile_set_background_buffer(
            &clip_ptr->spacer_tiles_ptr[i], wlr_buffer_ptr);
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
    wlmaker_clip_t *clip_ptr = data_ptr;
    wlmaker_server_t *server_ptr = clip_ptr->server_ptr;
    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        server_ptr->files_ptr, server_ptr->state_fname_ptr);
    bspl_array_t *launchers_ptr = bspl_dict_get_array(
        bspl_dict_get_dict(state_dict_ptr, "Clip"), "Launchers");
    size_t spacers = NULL != launchers_ptr ? bspl_array_size(launchers_ptr) : 0;
    bspl_dict_unref(state_dict_ptr);

    bs_log(BS_INFO, "Clip %p: Room for %zu launchers.", clip_ptr, spacers);
    _wlmaker_clip_set_spacers(clip_ptr, spacers, &server_ptr->style_ptr->tile);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Sets the number of empty tiles after the Clip, for its launchers. */
bool _wlmaker_clip_set_spacers(
    wlmaker_clip_t *clip_ptr,
    size_t spacers,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    if (spacers == clip_ptr->spacer_tiles) return true;

    for (size_t i = 0; i < clip_ptr->spacer_tiles; ++i) {
        wlmtk_tile_t *t_ptr = &clip_ptr->spacer_tiles_ptr[i];
        if (NULL != wlmtk_tile_element(t_ptr)->parent_container_ptr) {
            wlmtk_dock_remove_tile(clip_ptr->wlmtk_dock_ptr, t_ptr);
        }
        wlmtk_tile_fini(t_ptr);
    }
    if (NULL != clip_ptr->spacer_tiles_ptr) free(clip_ptr->spacer_tiles_ptr);
    clip_ptr->spacer_tiles_ptr = NULL;
    clip_ptr->spacer_tiles = 0;
    if (0 == spacers) return true;

    clip_ptr->spacer_tiles_ptr = logged_calloc(spacers, sizeof(wlmtk_tile_t));
    if (NULL == clip_ptr->spacer_tiles_ptr) return false;
    for (size_t i = 0; i < spacers; ++i) {
        wlmtk_tile_t *t_ptr = &clip_ptr->spacer_tiles_ptr[i];
        if (!wlmtk_tile_init(t_ptr, tile_style_ptr)) break;
        clip_ptr->spacer_tiles = i + 1;
        wlmtk_element_set_visible(wlmtk_tile_element(t_ptr), true);
        // Right after the Clip, before the dock-apps.
        wlmtk_dock_add_tile(clip_ptr->wlmtk_dock_ptr, t_ptr);
        wlmtk_dock_move_tile(clip_ptr->wlmtk_dock_ptr, t_ptr, 1 + i);
    }
    return _wlmaker_clip_update_spacers(clip_ptr, tile_style_ptr);
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
