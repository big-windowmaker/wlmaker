/* ========================================================================= */
/**
 * @file drawer.c
 *
 * @copyright
 * Copyright (c) 2026 Big WindowMaker contributors
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

#include "drawer.h"

#include <cairo.h>
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "action.h"
#include "backend/backend.h"
#include "clip.h"
#include "dock_fx.h"
#include "dock_launchers.h"
#include "dock_menu.h"
#include "input/manager.h"
#include "tile_drag.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** A launcher in a drawer: A tile of the compositor. */
typedef struct {
    /** The tile. */
    wlmtk_tile_t              super_tile;
    /** Original virtual method table of the tile's element. */
    wlmtk_element_vmt_t       orig_element_vmt;
    /** Back-link. */
    wlmaker_drawer_t          *drawer_ptr;
    /** Index among the drawer's launchers. */
    size_t                    index;
    /** The launcher's entry. */
    bspl_dict_t               *dict_ptr;
    /** Path of the icon. */
    char                      *icon_path_ptr;
    /** The icon. */
    wlmtk_image_t             *image_ptr;
    /** Overlay: Window Maker's dots, or the stipple while launching. */
    wlmtk_buffer_t            overlay_buffer;
    /** Whether the application runs. */
    bool                      running;
    /** Whether launching, until a window shows up. */
    bool                      launching;
    /** Ends launching, if it takes too long. */
    struct wl_event_source    *launching_timer_ptr;

    /** Whether the left button is held: May become a drag. */
    bool                      pressed;
    /** Where the button was pressed, in layout coordinates. */
    double                    press_lx;
    /** Where the button was pressed, in layout coordinates. */
    double                    press_ly;
    /** Where the button was pressed, on the tile. */
    int                       press_x;
    /** Where the button was pressed, on the tile. */
    int                       press_y;
    /** The drag, once started. */
    wlmaker_tile_drag_t       *tile_drag_ptr;
    /** Time of the last click, for double-clicks. */
    uint32_t                  last_click_msec;
    /** Whether the balloon is (to be) shown. */
    bool                      balloon_entered;
    /** Listener for @ref wlmtk_element_events_t::pointer_leave. */
    struct wl_listener        pointer_leave_listener;
} wlmaker_drawer_launcher_t;

/** State of a drawer. */
struct _wlmaker_drawer_t {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** The `Id` of the drawer. */
    char                      *id_ptr;
    /** Index among the Dock's launchers. */
    size_t                    launcher_index;
    /** The row of icons: A dock. */
    wlmtk_dock_t              *row_ptr;
    /** The launchers. */
    wlmaker_drawer_launcher_t **launchers_ptr;
    /** Number of launchers. */
    size_t                    launchers;
    /** The launchers' entries as text: Rebuilt only when it changes. */
    char                      *launchers_text_ptr;
    /** The drawer's dock-apps. */
    wlmaker_dockapps_t        *dockapps_ptr;
    /** Whether closed. */
    bool                      collapsed;
    /** Whether it closes itself. */
    bool                      autocollapse;
    /** Timer for opening, or closing. */
    struct wl_event_source    *timer_ptr;
    /** Whether the timer is to open it (Window Maker's auto_expand_magic). */
    bool                      expanding;
    /** Refreshes the launchers' "running" look while open. */
    struct wl_event_source    *refresh_timer_ptr;
};

static wlmaker_drawer_launcher_t *_wlmaker_drawer_launcher_create(
    wlmaker_drawer_t *drawer_ptr,
    bspl_dict_t *dict_ptr,
    size_t index);
static void _wlmaker_drawer_launcher_destroy(
    wlmaker_drawer_launcher_t *launcher_ptr);
static void _wlmaker_drawer_launcher_update_overlay(
    wlmaker_drawer_launcher_t *launcher_ptr);
static void _wlmaker_drawer_launcher_launch(
    wlmaker_drawer_launcher_t *launcher_ptr);
static bool _wlmaker_drawer_launcher_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);
static bool _wlmaker_drawer_launcher_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr);
static void _wlmaker_drawer_launcher_pointer_grab_cancel(
    wlmtk_element_t *element_ptr);
static void _wlmaker_drawer_launcher_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static int _wlmaker_drawer_launcher_handle_launching_timer(void *data_ptr);

static void _wlmaker_drawer_set_launchers(
    wlmaker_drawer_t *drawer_ptr,
    bspl_array_t *array_ptr);
static int _wlmaker_drawer_handle_timer(void *data_ptr);
static int _wlmaker_drawer_handle_refresh_timer(void *data_ptr);
static bool _wlmaker_drawer_pointer_on(wlmaker_drawer_t *drawer_ptr);
static bool _wlmaker_drawer_flag(bspl_dict_t *dict_ptr, const char *key_ptr,
                                 bool default_value);
static void _wlmaker_drawer_set_collapsed(wlmaker_drawer_t *drawer_ptr,
                                          bool collapsed);

/** The drawer launcher's extension of the tile's element. */
static const wlmtk_element_vmt_t _wlmaker_drawer_launcher_element_vmt = {
    .pointer_button = _wlmaker_drawer_launcher_pointer_button,
    .pointer_accepts_motion = _wlmaker_drawer_launcher_pointer_accepts_motion,
    .pointer_grab_cancel = _wlmaker_drawer_launcher_pointer_grab_cancel,
};

/** Window Maker's ClipAutoexpandDelay, in ms. */
static const int _wlmaker_drawer_expand_msec = 600;
/** Window Maker's ClipAutocollapseDelay, in ms. */
static const int _wlmaker_drawer_collapse_msec = 1000;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_drawer_t *wlmaker_drawer_create(
    wlmaker_server_t *server_ptr,
    const char *id_ptr)
{
    wlmaker_drawer_t *drawer_ptr = logged_calloc(1, sizeof(*drawer_ptr));
    if (NULL == drawer_ptr) return NULL;
    drawer_ptr->server_ptr = server_ptr;
    drawer_ptr->collapsed = true;
    drawer_ptr->autocollapse = true;
    drawer_ptr->id_ptr = logged_strdup(id_ptr);
    if (NULL == drawer_ptr->id_ptr) {
        wlmaker_drawer_destroy(drawer_ptr);
        return NULL;
    }

    wlmaker_dock_column_t dock;
    bool vertical = !wlmaker_clip_column(server_ptr, true, &dock) ||
        wlmaker_dock_place_vertical(dock.place);
    wlmtk_dock_positioning_t pos = {
        .edge = vertical ? WLR_EDGE_TOP : WLR_EDGE_LEFT,
        .anchor = vertical ? (WLR_EDGE_LEFT == dock.place.edge ?
                              WLR_EDGE_LEFT : WLR_EDGE_RIGHT) :
        (WLR_EDGE_TOP == dock.place.edge ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM),
        .overlay = true };
    drawer_ptr->row_ptr = wlmtk_dock_create(&pos, &server_ptr->style_ptr->dock);
    if (NULL == drawer_ptr->row_ptr) {
        wlmaker_drawer_destroy(drawer_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(wlmtk_dock_element(drawer_ptr->row_ptr), false);

    const char *fname_ptr = wlmaker_clip_dockapps_fname(server_ptr);
    char *key_ptr = bs_strdupf("Drawer.%s", id_ptr);
    if (NULL != key_ptr) {
        drawer_ptr->dockapps_ptr = wlmaker_dockapps_create_with_key(
            drawer_ptr->row_ptr, NULL, fname_ptr, key_ptr);
        free(key_ptr);
    }
    if (NULL == drawer_ptr->dockapps_ptr) {
        wlmaker_drawer_destroy(drawer_ptr);
        return NULL;
    }

    struct wlr_output *wlr_output_ptr = wlmbe_primary_output(
        server_ptr->wlr_output_layout_ptr);
    wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
        server_ptr->desktop_ptr, wlmaker_clip_layer(server_ptr, true));
    if (NULL != wlr_output_ptr && NULL != layer_ptr) {
        wlmtk_layer_add_panel(layer_ptr, wlmtk_dock_panel(drawer_ptr->row_ptr),
                              wlr_output_ptr);
    }
    bs_log(BS_INFO, "Created drawer %p, Id \"%s\".", drawer_ptr, id_ptr);
    return drawer_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_destroy(wlmaker_drawer_t *drawer_ptr)
{
    wlmaker_server_t *server_ptr = drawer_ptr->server_ptr;
    if (NULL != drawer_ptr->timer_ptr) {
        wl_event_source_remove(drawer_ptr->timer_ptr);
        drawer_ptr->timer_ptr = NULL;
    }
    if (NULL != drawer_ptr->refresh_timer_ptr) {
        wl_event_source_remove(drawer_ptr->refresh_timer_ptr);
        drawer_ptr->refresh_timer_ptr = NULL;
    }
    if (NULL != server_ptr->context_menu_ptr) {
        wlmaker_context_menu_cancel(server_ptr, drawer_ptr);
    }
    _wlmaker_drawer_set_launchers(drawer_ptr, NULL);

    // Its dock-apps go to the Clip: There are no free icons. Not when
    // shutting down (no Clip anymore).
    if (NULL != drawer_ptr->dockapps_ptr) {
        wlmaker_dockapps_t *clip_ptr = wlmaker_clip_dockapps_id(
            server_ptr, WLMAKER_COLUMN_CLIP);
        while (0 < wlmaker_dockapps_size(drawer_ptr->dockapps_ptr)) {
            wlmtk_tile_t *tile_ptr = wlmaker_dockapps_tile_at(
                drawer_ptr->dockapps_ptr, 0);
            const char *n = wlmaker_dockapps_name(drawer_ptr->dockapps_ptr,
                                                  tile_ptr);
            char *name_ptr = NULL != n ? logged_strdup(n) : NULL;
            if (NULL != clip_ptr && NULL != server_ptr->clip_ptr) {
                // Removed: Detached from the drawer, attached to the Clip.
                wlmaker_dockapps_detach(drawer_ptr->dockapps_ptr, tile_ptr);
                wlmaker_dockapps_insert(clip_ptr, tile_ptr, name_ptr,
                                        wlmaker_dockapps_size(clip_ptr));
            } else {
                // Shutting down: Stays in the drawer's order.
                wlmaker_dockapps_remove(drawer_ptr->dockapps_ptr, tile_ptr);
            }
            if (NULL != name_ptr) free(name_ptr);
        }
        wlmaker_dockapps_destroy(drawer_ptr->dockapps_ptr);
        drawer_ptr->dockapps_ptr = NULL;
    }

    if (NULL != drawer_ptr->row_ptr) {
        wlmtk_layer_t *l_ptr = wlmtk_panel_get_layer(
            wlmtk_dock_panel(drawer_ptr->row_ptr));
        if (NULL != l_ptr) {
            wlmtk_layer_remove_panel(l_ptr, wlmtk_dock_panel(drawer_ptr->row_ptr));
        }
        wlmtk_dock_destroy(drawer_ptr->row_ptr);
        drawer_ptr->row_ptr = NULL;
    }
    if (NULL != drawer_ptr->launchers_text_ptr) {
        free(drawer_ptr->launchers_text_ptr);
    }
    if (NULL != drawer_ptr->id_ptr) free(drawer_ptr->id_ptr);
    free(drawer_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_update(
    wlmaker_drawer_t *drawer_ptr,
    bspl_dict_t *dict_ptr,
    size_t launcher_index)
{
    drawer_ptr->launcher_index = launcher_index;
    drawer_ptr->autocollapse = _wlmaker_drawer_flag(
        dict_ptr, "AutoCollapse", true);

    // The launchers: Rebuilt when they changed.
    bspl_array_t *a = bspl_dict_get_array(dict_ptr, "Launchers");
    char *text_ptr = NULL;
    bs_dynbuf_t dynbuf = {};
    if (NULL != a && bs_dynbuf_init(&dynbuf, 1024, SIZE_MAX) &&
        bspl_object_write(bspl_object_from_array(a), &dynbuf) &&
        bs_dynbuf_append_char(&dynbuf, '\0')) {
        text_ptr = logged_strdup(dynbuf.data_ptr);
    }
    bs_dynbuf_fini(&dynbuf);
    if (NULL == text_ptr || NULL == drawer_ptr->launchers_text_ptr ||
        0 != strcmp(text_ptr, drawer_ptr->launchers_text_ptr)) {
        bspl_array_t *shown_ptr = wlmaker_dock_launchers_load_column(
            drawer_ptr->server_ptr,
            wlmaker_clip_drawer_column(drawer_ptr->server_ptr, drawer_ptr));
        _wlmaker_drawer_set_launchers(drawer_ptr, shown_ptr);
        if (NULL != shown_ptr) bspl_array_unref(shown_ptr);
        if (NULL != drawer_ptr->launchers_text_ptr) {
            free(drawer_ptr->launchers_text_ptr);
        }
        drawer_ptr->launchers_text_ptr = text_ptr;
    } else {
        free(text_ptr);
    }

    // Stored as closed or open, without Autocollapse.
    if (!drawer_ptr->autocollapse) {
        _wlmaker_drawer_set_collapsed(
            drawer_ptr, _wlmaker_drawer_flag(dict_ptr, "Collapsed", true));
    }
    wlmaker_drawer_update_position(drawer_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_update_position(wlmaker_drawer_t *drawer_ptr)
{
    wlmaker_dock_column_t column;
    if (!wlmaker_drawer_column(drawer_ptr, &column)) return;
    const struct wlr_box *o_ptr = &column.output;
    int margin_width = drawer_ptr->server_ptr->style_ptr->dock.margin.width;
    wlmtk_dock_positioning_t pos = *wlmtk_dock_get_positioning(
        drawer_ptr->row_ptr);
    // Next to the drawer's tile, away from the Dock's edge.
    switch (pos.anchor) {
    case WLR_EDGE_RIGHT:
        pos.margin = o_ptr->x + o_ptr->width - column.x + margin_width;
        pos.edge_offset = column.y - o_ptr->y;
        break;
    case WLR_EDGE_LEFT:
        pos.margin = column.x + column.size + margin_width - o_ptr->x;
        pos.edge_offset = column.y - o_ptr->y;
        break;
    case WLR_EDGE_BOTTOM:
        pos.margin = o_ptr->y + o_ptr->height - column.y + margin_width;
        pos.edge_offset = column.x - o_ptr->x;
        break;
    case WLR_EDGE_TOP:
        pos.margin = column.y + column.size + margin_width - o_ptr->y;
        pos.edge_offset = column.x - o_ptr->x;
        break;
    default:
        return;
    }
    const wlmtk_dock_positioning_t *cur_ptr = wlmtk_dock_get_positioning(
        drawer_ptr->row_ptr);
    if (cur_ptr->margin == pos.margin &&
        cur_ptr->edge_offset == pos.edge_offset) return;
    wlmtk_dock_set_positioning(drawer_ptr->row_ptr, &pos);
}

/* ------------------------------------------------------------------------- */
const char *wlmaker_drawer_id(wlmaker_drawer_t *drawer_ptr)
{
    return drawer_ptr->id_ptr;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_drawer_launcher_index(wlmaker_drawer_t *drawer_ptr)
{
    return drawer_ptr->launcher_index;
}

/* ------------------------------------------------------------------------- */
wlmaker_dockapps_t *wlmaker_drawer_dockapps(wlmaker_drawer_t *drawer_ptr)
{
    return drawer_ptr->dockapps_ptr;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_drawer_launchers(wlmaker_drawer_t *drawer_ptr)
{
    return drawer_ptr->launchers;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_drawer_empty(wlmaker_drawer_t *drawer_ptr)
{
    return 0 == drawer_ptr->launchers &&
        0 == wlmaker_dockapps_size(drawer_ptr->dockapps_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_drawer_collapsed(wlmaker_drawer_t *drawer_ptr)
{
    return drawer_ptr->collapsed;
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_set_collapsed(wlmaker_drawer_t *drawer_ptr, bool collapsed)
{
    if (collapsed == drawer_ptr->collapsed) return;
    _wlmaker_drawer_set_collapsed(drawer_ptr, collapsed);
    // Without Autocollapse, it stays so: Also after a restart.
    if (!drawer_ptr->autocollapse) {
        wlmaker_dock_launchers_set_drawer_flag(
            drawer_ptr->server_ptr, drawer_ptr->id_ptr, "Collapsed", collapsed);
    }
}

/* ------------------------------------------------------------------------- */
/** Opens (or closes) the drawer. */
void _wlmaker_drawer_set_collapsed(wlmaker_drawer_t *drawer_ptr, bool collapsed)
{
    if (collapsed == drawer_ptr->collapsed) return;
    drawer_ptr->collapsed = collapsed;
    wlmtk_element_set_visible(wlmtk_dock_element(drawer_ptr->row_ptr),
                              !collapsed);
    if (!collapsed) {
        // The launchers' look: Running or not, as of now.
        for (size_t i = 0; i < drawer_ptr->launchers; ++i) {
            _wlmaker_drawer_launcher_update_overlay(drawer_ptr->launchers_ptr[i]);
        }
        if (NULL == drawer_ptr->refresh_timer_ptr) {
            drawer_ptr->refresh_timer_ptr = wl_event_loop_add_timer(
                wl_display_get_event_loop(drawer_ptr->server_ptr->wl_display_ptr),
                _wlmaker_drawer_handle_refresh_timer, drawer_ptr);
        }
        if (NULL != drawer_ptr->refresh_timer_ptr) {
            wl_event_source_timer_update(drawer_ptr->refresh_timer_ptr, 2000);
        }
    } else if (NULL != drawer_ptr->refresh_timer_ptr) {
        wl_event_source_remove(drawer_ptr->refresh_timer_ptr);
        drawer_ptr->refresh_timer_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
bool wlmaker_drawer_autocollapse(wlmaker_drawer_t *drawer_ptr)
{
    return drawer_ptr->autocollapse;
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_hovered(wlmaker_drawer_t *drawer_ptr)
{
    if (!drawer_ptr->autocollapse) return;
    if (NULL == drawer_ptr->timer_ptr) {
        drawer_ptr->timer_ptr = wl_event_loop_add_timer(
            wl_display_get_event_loop(drawer_ptr->server_ptr->wl_display_ptr),
            _wlmaker_drawer_handle_timer, drawer_ptr);
        if (NULL == drawer_ptr->timer_ptr) return;
    }
    // Closed: Opens after the delay (from when the pointer came). Open:
    // Stays open.
    if (!drawer_ptr->collapsed) {
        wl_event_source_timer_update(drawer_ptr->timer_ptr, 0);
        drawer_ptr->expanding = false;
    } else if (!drawer_ptr->expanding) {
        wl_event_source_timer_update(drawer_ptr->timer_ptr,
                                     _wlmaker_drawer_expand_msec);
        drawer_ptr->expanding = true;
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_left(wlmaker_drawer_t *drawer_ptr)
{
    if (!drawer_ptr->autocollapse) return;
    if (NULL == drawer_ptr->timer_ptr) {
        drawer_ptr->timer_ptr = wl_event_loop_add_timer(
            wl_display_get_event_loop(drawer_ptr->server_ptr->wl_display_ptr),
            _wlmaker_drawer_handle_timer, drawer_ptr);
        if (NULL == drawer_ptr->timer_ptr) return;
    }
    drawer_ptr->expanding = false;
    wl_event_source_timer_update(
        drawer_ptr->timer_ptr,
        drawer_ptr->collapsed ? 0 : _wlmaker_drawer_collapse_msec);
}

/* ------------------------------------------------------------------------- */
void wlmaker_drawer_update_layer(wlmaker_drawer_t *drawer_ptr)
{
    wlmaker_server_t *server_ptr = drawer_ptr->server_ptr;
    wlmtk_panel_t *panel_ptr = wlmtk_dock_panel(drawer_ptr->row_ptr);
    wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
        server_ptr->desktop_ptr, wlmaker_clip_layer(server_ptr, true));
    wlmtk_layer_t *current_ptr = wlmtk_panel_get_layer(panel_ptr);
    if (layer_ptr == current_ptr || NULL == layer_ptr) return;
    struct wlr_output *wlr_output_ptr = wlmbe_primary_output(
        server_ptr->wlr_output_layout_ptr);
    if (NULL != current_ptr) wlmtk_layer_remove_panel(current_ptr, panel_ptr);
    if (NULL != wlr_output_ptr) {
        wlmtk_layer_add_panel(layer_ptr, panel_ptr, wlr_output_ptr);
    }
}

/* ------------------------------------------------------------------------- */
bool wlmaker_drawer_column(
    wlmaker_drawer_t *drawer_ptr,
    struct _wlmaker_dock_column_t *column_ptr)
{
    wlmaker_dock_column_t dock;
    if (!wlmaker_clip_column(drawer_ptr->server_ptr, true, &dock)) return false;
    int d = (1 + (int)drawer_ptr->launcher_index) * dock.pitch;
    int x = dock.x, y = dock.y;
    switch (dock.place.anchor) {
    case WLR_EDGE_TOP: y += d; break;
    case WLR_EDGE_BOTTOM: y -= d; break;
    case WLR_EDGE_LEFT: x += d; break;
    case WLR_EDGE_RIGHT: x -= d; break;
    default: break;
    }
    const wlmtk_dock_positioning_t *pos_ptr = wlmtk_dock_get_positioning(
        drawer_ptr->row_ptr);
    *column_ptr = (wlmaker_dock_column_t){
        .id = wlmaker_clip_drawer_column(drawer_ptr->server_ptr, drawer_ptr),
        .place = { pos_ptr->edge, pos_ptr->anchor },
        .x = x, .y = y, .size = dock.size, .pitch = dock.pitch,
        .launchers = drawer_ptr->launchers,
        .dockapps = wlmaker_dockapps_size(drawer_ptr->dockapps_ptr),
        .output = dock.output };
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_drawer_row_box(
    wlmaker_drawer_t *drawer_ptr,
    struct wlr_box *box_ptr)
{
    wlmaker_dock_column_t column;
    if (drawer_ptr->collapsed ||
        !wlmaker_drawer_column(drawer_ptr, &column)) return false;
    int n = column.launchers + column.dockapps;
    if (0 >= n) return false;
    int length = (n - 1) * column.pitch + column.size;
    *box_ptr = (struct wlr_box){
        .x = column.x, .y = column.y,
        .width = column.size, .height = column.size };
    switch (column.place.anchor) {
    case WLR_EDGE_TOP: box_ptr->y += column.pitch; box_ptr->height = length;
        break;
    case WLR_EDGE_BOTTOM: box_ptr->y -= length + column.pitch - column.size;
        box_ptr->height = length; break;
    case WLR_EDGE_LEFT: box_ptr->x += column.pitch; box_ptr->width = length;
        break;
    case WLR_EDGE_RIGHT: box_ptr->x -= length + column.pitch - column.size;
        box_ptr->width = length; break;
    default: return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_drawer_create_entry(
    bspl_array_t *launchers_ptr,
    const char *icon_ptr)
{
    // An Id not in use: The smallest number.
    unsigned id = 1;
    for (bool used = true; used; ) {
        used = false;
        for (size_t i = 0; NULL != launchers_ptr &&
                 i < bspl_array_size(launchers_ptr); ++i) {
            const char *v = bspl_dict_get_string_value(
                bspl_dict_from_object(bspl_array_at(launchers_ptr, i)), "Id");
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", id);
            if (NULL != v && 0 == strcmp(v, buf)) used = true;
        }
        if (used) ++id;
    }
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%u", id);
    bspl_object_t *o = bspl_create_object_from_plist_string(
        "{Drawer = Yes; CommandLine = \"\"; AutoCollapse = Yes; "
        "Launchers = ();}");
    bspl_dict_t *d = bspl_dict_from_object(o);
    if (NULL == d) {
        if (NULL != o) bspl_object_unref(o);
        return NULL;
    }
    bspl_string_t *id_ptr = bspl_string_create(id_buf);
    bspl_string_t *i_ptr = bspl_string_create(icon_ptr);
    bool rv = NULL != id_ptr && NULL != i_ptr &&
        bspl_dict_add(d, "Id", bspl_object_from_string(id_ptr)) &&
        bspl_dict_add(d, "Icon", bspl_object_from_string(i_ptr));
    if (NULL != id_ptr) bspl_string_unref(id_ptr);
    if (NULL != i_ptr) bspl_string_unref(i_ptr);
    if (!rv) {
        bspl_dict_unref(d);
        return NULL;
    }
    return d;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_drawer_is_entry(bspl_dict_t *dict_ptr)
{
    return _wlmaker_drawer_flag(dict_ptr, "Drawer", false);
}

/** Arguments for @ref _wlmaker_drawer_copy_entry. */
typedef struct {
    /** Destination. */
    bspl_dict_t               *dest_ptr;
    /** Key left out. */
    const char                *skip_ptr;
} _wlmaker_drawer_copy_arg_t;

/** Copies an entry, unless it is the key left out. */
static bool _wlmaker_drawer_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *ud_ptr)
{
    _wlmaker_drawer_copy_arg_t *arg_ptr = ud_ptr;
    if (0 == strcmp(key_ptr, arg_ptr->skip_ptr)) return true;
    return bspl_dict_add(arg_ptr->dest_ptr, key_ptr, object_ptr);
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_drawer_dict_with(
    bspl_dict_t *dict_ptr,
    const char *key_ptr,
    bspl_object_t *object_ptr)
{
    bspl_dict_t *d = bspl_dict_create();
    if (NULL == d) return NULL;
    _wlmaker_drawer_copy_arg_t arg = { .dest_ptr = d, .skip_ptr = key_ptr };
    if (!bspl_dict_foreach(dict_ptr, _wlmaker_drawer_copy_entry, &arg) ||
        (NULL != object_ptr && !bspl_dict_add(d, key_ptr, object_ptr))) {
        bspl_dict_unref(d);
        return NULL;
    }
    return d;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** @return The boolean `key_ptr` of the dict, or `default_value`. */
bool _wlmaker_drawer_flag(bspl_dict_t *dict_ptr, const char *key_ptr,
                          bool default_value)
{
    const char *v = NULL != dict_ptr ?
        bspl_dict_get_string_value(dict_ptr, key_ptr) : NULL;
    if (NULL == v) return default_value;
    return 0 == strcasecmp(v, "Yes") || 0 == strcasecmp(v, "True") ||
        0 == strcmp(v, "1");
}

/* ------------------------------------------------------------------------- */
/** Replaces the drawer's launchers with tiles for `array_ptr`. */
void _wlmaker_drawer_set_launchers(
    wlmaker_drawer_t *drawer_ptr,
    bspl_array_t *array_ptr)
{
    if (NULL != drawer_ptr->server_ptr->tile_drag_ptr) {
        for (size_t i = 0; i < drawer_ptr->launchers; ++i) {
            if (NULL != drawer_ptr->launchers_ptr[i]->tile_drag_ptr) {
                wlmaker_tile_drag_cancel(
                    drawer_ptr->launchers_ptr[i]->tile_drag_ptr);
            }
        }
    }
    for (size_t i = 0; i < drawer_ptr->launchers; ++i) {
        _wlmaker_drawer_launcher_destroy(drawer_ptr->launchers_ptr[i]);
    }
    if (NULL != drawer_ptr->launchers_ptr) free(drawer_ptr->launchers_ptr);
    drawer_ptr->launchers_ptr = NULL;
    drawer_ptr->launchers = 0;
    if (NULL == array_ptr || 0 == bspl_array_size(array_ptr)) return;

    size_t n = bspl_array_size(array_ptr);
    drawer_ptr->launchers_ptr = logged_calloc(n, sizeof(void*));
    if (NULL == drawer_ptr->launchers_ptr) return;
    for (size_t i = 0; i < n; ++i) {
        wlmaker_drawer_launcher_t *l_ptr = _wlmaker_drawer_launcher_create(
            drawer_ptr, bspl_dict_from_object(bspl_array_at(array_ptr, i)),
            drawer_ptr->launchers);
        if (NULL == l_ptr) continue;
        drawer_ptr->launchers_ptr[drawer_ptr->launchers++] = l_ptr;
        // Launchers first, nearest the drawer's tile; dock-apps after.
        wlmtk_dock_add_tile(drawer_ptr->row_ptr, &l_ptr->super_tile);
        wlmtk_dock_move_tile(drawer_ptr->row_ptr, &l_ptr->super_tile, l_ptr->index);
    }
}

/* ------------------------------------------------------------------------- */
/** Timer: Opens or closes the drawer (Autocollapse). */
int _wlmaker_drawer_handle_timer(void *data_ptr)
{
    wlmaker_drawer_t *drawer_ptr = data_ptr;
    drawer_ptr->expanding = false;
    if (drawer_ptr->collapsed) {
        wlmaker_drawer_set_collapsed(drawer_ptr, false);
    } else if (!_wlmaker_drawer_pointer_on(drawer_ptr) &&
               NULL == drawer_ptr->server_ptr->tile_drag_ptr) {
        wlmaker_drawer_set_collapsed(drawer_ptr, true);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Timer: Refreshes the launchers' look, while open. */
int _wlmaker_drawer_handle_refresh_timer(void *data_ptr)
{
    wlmaker_drawer_t *drawer_ptr = data_ptr;
    for (size_t i = 0; i < drawer_ptr->launchers; ++i) {
        _wlmaker_drawer_launcher_update_overlay(drawer_ptr->launchers_ptr[i]);
    }
    wl_event_source_timer_update(drawer_ptr->refresh_timer_ptr, 2000);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** @return Whether the pointer is on the drawer's tile, or its row. */
bool _wlmaker_drawer_pointer_on(wlmaker_drawer_t *drawer_ptr)
{
    wlmaker_server_t *server_ptr = drawer_ptr->server_ptr;
    if (NULL == server_ptr->input_manager_ptr) return false;
    struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
    wlmaker_dock_column_t column;
    if (!wlmaker_drawer_column(drawer_ptr, &column)) return false;
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
    return wlr_box_contains_point(&box, c->x, c->y);
}

/* ------------------------------------------------------------------------- */
/** Creates a launcher tile of the drawer. */
wlmaker_drawer_launcher_t *_wlmaker_drawer_launcher_create(
    wlmaker_drawer_t *drawer_ptr,
    bspl_dict_t *dict_ptr,
    size_t index)
{
    if (NULL == dict_ptr) return NULL;
    wlmaker_server_t *server_ptr = drawer_ptr->server_ptr;
    wlmaker_drawer_launcher_t *l_ptr = logged_calloc(1, sizeof(*l_ptr));
    if (NULL == l_ptr) return NULL;
    l_ptr->drawer_ptr = drawer_ptr;
    l_ptr->index = index;
    l_ptr->dict_ptr = bspl_dict_ref(dict_ptr);
    l_ptr->icon_path_ptr = wlmaker_dock_launcher_icon_path(
        server_ptr->files_ptr, wlmaker_dock_launcher_value(dict_ptr, "Icon"));
    const struct wlmtk_tile_style *ts_ptr = &server_ptr->style_ptr->tile;
    if (NULL == l_ptr->icon_path_ptr ||
        !wlmtk_tile_init(&l_ptr->super_tile, ts_ptr)) {
        if (NULL != l_ptr->icon_path_ptr) free(l_ptr->icon_path_ptr);
        bspl_dict_unref(l_ptr->dict_ptr);
        free(l_ptr);
        return NULL;
    }
    l_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&l_ptr->super_tile),
        &_wlmaker_drawer_launcher_element_vmt);
    wlmtk_element_set_visible(wlmtk_tile_element(&l_ptr->super_tile), true);
    wlmtk_util_connect_listener_signal(
        &wlmtk_tile_element(&l_ptr->super_tile)->events.pointer_leave,
        &l_ptr->pointer_leave_listener,
        _wlmaker_drawer_launcher_handle_pointer_leave);

    l_ptr->image_ptr = wlmtk_image_create_scaled(
        l_ptr->icon_path_ptr, ts_ptr->content_size, ts_ptr->content_size);
    if (NULL != l_ptr->image_ptr) {
        wlmtk_element_set_visible(wlmtk_image_element(l_ptr->image_ptr), true);
        wlmtk_tile_set_content(&l_ptr->super_tile,
                               wlmtk_image_element(l_ptr->image_ptr));
    }
    if (wlmtk_buffer_init(&l_ptr->overlay_buffer)) {
        wlmtk_element_set_visible(
            wlmtk_buffer_element(&l_ptr->overlay_buffer), true);
        _wlmaker_drawer_launcher_update_overlay(l_ptr);
        wlmtk_tile_set_overlay(&l_ptr->super_tile,
                               wlmtk_buffer_element(&l_ptr->overlay_buffer));
    }
    return l_ptr;
}

/* ------------------------------------------------------------------------- */
/** Destroys the launcher tile, after taking it out of the row. */
void _wlmaker_drawer_launcher_destroy(wlmaker_drawer_launcher_t *l_ptr)
{
    wlmaker_server_t *server_ptr = l_ptr->drawer_ptr->server_ptr;
    wlmtk_element_t *e = wlmtk_tile_element(&l_ptr->super_tile);
    if (NULL != server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, l_ptr);
    }
    wlmaker_context_menu_cancel(server_ptr, l_ptr);
    if (l_ptr->pressed && NULL != e->parent_container_ptr) {
        wlmtk_container_pointer_grab_release(e->parent_container_ptr, e);
    }
    if (NULL != e->parent_container_ptr) {
        wlmtk_dock_remove_tile(l_ptr->drawer_ptr->row_ptr, &l_ptr->super_tile);
    }
    if (NULL != l_ptr->launching_timer_ptr) {
        wl_event_source_remove(l_ptr->launching_timer_ptr);
    }
    wlmtk_util_disconnect_listener(&l_ptr->pointer_leave_listener);
    wlmtk_tile_set_overlay(&l_ptr->super_tile, NULL);
    wlmtk_buffer_fini(&l_ptr->overlay_buffer);
    wlmtk_tile_set_content(&l_ptr->super_tile, NULL);
    if (NULL != l_ptr->image_ptr) wlmtk_image_destroy(l_ptr->image_ptr);
    wlmtk_tile_fini(&l_ptr->super_tile);
    free(l_ptr->icon_path_ptr);
    bspl_dict_unref(l_ptr->dict_ptr);
    free(l_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the launcher's overlay, as `wlmdock` does: Window Maker's three dots
 * while its application is not running; stippled while launching.
 */
void _wlmaker_drawer_launcher_update_overlay(wlmaker_drawer_launcher_t *l_ptr)
{
    wlmaker_server_t *server_ptr = l_ptr->drawer_ptr->server_ptr;
    int s = l_ptr->super_tile.style.size;
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(s, s);
    if (NULL == wlr_buffer_ptr) return;
    const char *app_id_ptr = wlmaker_dock_launcher_value(l_ptr->dict_ptr, "AppID");
    l_ptr->running = 0 < wlmaker_dock_app_windows(server_ptr, app_id_ptr, NULL);
    if (l_ptr->running) l_ptr->launching = false;
    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return;
    }
    if (!l_ptr->running) {
        // Window Maker's make3Dots, as wlmdock draws them.
        static const int xs[3] = { 4, 9, 14 };
        cairo_set_antialias(cairo_ptr, CAIRO_ANTIALIAS_NONE);
        for (int i = 0; i < 3; ++i) {
            int x = xs[i], y = s - 6;
            cairo_set_source_rgb(cairo_ptr, 0, 0, 0);
            cairo_rectangle(cairo_ptr, x, y, 2, 1);
            cairo_rectangle(cairo_ptr, x, y + 1, 1, 1);
            cairo_fill(cairo_ptr);
            cairo_set_source_rgb(cairo_ptr, 1, 1, 1);
            cairo_rectangle(cairo_ptr, x + 2, y, 1, 2);
            cairo_rectangle(cairo_ptr, x + 1, y + 1, 1, 1);
            cairo_fill(cairo_ptr);
        }
    }
    cairo_surface_flush(cairo_get_target(cairo_ptr));
    if (l_ptr->launching) {
        bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(wlr_buffer_ptr);
        for (unsigned y = 0; y < g_ptr->height; ++y) {
            for (unsigned x = y % 2; x < g_ptr->width; x += 2) {
                g_ptr->data_ptr[y * g_ptr->width + x] = 0xff000000;
            }
        }
        cairo_surface_mark_dirty(cairo_get_target(cairo_ptr));
    }
    cairo_destroy(cairo_ptr);
    wlmtk_buffer_set(&l_ptr->overlay_buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Launches the launcher's command; shows it as launching. */
void _wlmaker_drawer_launcher_launch(wlmaker_drawer_launcher_t *l_ptr)
{
    wlmaker_server_t *server_ptr = l_ptr->drawer_ptr->server_ptr;
    const char *cmd_ptr = wlmaker_dock_launcher_value(l_ptr->dict_ptr,
                                                      "CommandLine");
    if (NULL == cmd_ptr || '\0' == *cmd_ptr) return;
    wlmaker_action_execute(server_ptr, WLMAKER_ACTION_EXECUTE, (char*)cmd_ptr);
    const char *app_id_ptr = wlmaker_dock_launcher_value(l_ptr->dict_ptr, "AppID");
    if (NULL == app_id_ptr || '\0' == *app_id_ptr) return;
    l_ptr->launching = true;
    if (NULL == l_ptr->launching_timer_ptr) {
        l_ptr->launching_timer_ptr = wl_event_loop_add_timer(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            _wlmaker_drawer_launcher_handle_launching_timer, l_ptr);
    }
    if (NULL != l_ptr->launching_timer_ptr) {
        wl_event_source_timer_update(l_ptr->launching_timer_ptr, 10000);
    }
    _wlmaker_drawer_launcher_update_overlay(l_ptr);
}

/* ------------------------------------------------------------------------- */
/** Timer: Launching took too long. */
int _wlmaker_drawer_launcher_handle_launching_timer(void *data_ptr)
{
    wlmaker_drawer_launcher_t *l_ptr = data_ptr;
    l_ptr->launching = false;
    _wlmaker_drawer_launcher_update_overlay(l_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** @return Whether launchers launch on a click (SingleClickLaunch). */
static bool _wlmaker_drawer_single_click(wlmaker_server_t *server_ptr)
{
    const char *v = bspl_dict_get_string_value(
        server_ptr->config_dict_ptr, "SingleClickLaunch");
    return NULL != v && (0 == strcasecmp(v, "Yes") || 0 == strcasecmp(v, "True"));
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_button. */
bool _wlmaker_drawer_launcher_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_drawer_launcher_t *l_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_drawer_launcher_t,
        super_tile.super_container.super_element);
    wlmaker_drawer_t *drawer_ptr = l_ptr->drawer_ptr;
    wlmaker_server_t *server_ptr = drawer_ptr->server_ptr;
    int column = wlmaker_clip_drawer_column(server_ptr, drawer_ptr);

    if (NULL != server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, NULL);
    }
    l_ptr->balloon_entered = false;

    if (BTN_RIGHT == button_event_ptr->button) {
        if (WLMTK_BUTTON_DOWN == button_event_ptr->type && 0 <= column) {
            wlmaker_dock_menu_open_launcher(server_ptr, column, l_ptr->index);
        }
        return true;
    }
    if (BTN_MIDDLE == button_event_ptr->button) {
        if (WLMTK_BUTTON_DOWN == button_event_ptr->type) {
            wlmaker_dock_launchers_paste_command(server_ptr, l_ptr->dict_ptr);
        }
        return true;
    }
    if (BTN_LEFT != button_event_ptr->button) return true;

    struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
    switch (button_event_ptr->type) {
    case WLMTK_BUTTON_DOWN:
        l_ptr->pressed = true;
        l_ptr->tile_drag_ptr = NULL;
        l_ptr->press_lx = c->x;
        l_ptr->press_ly = c->y;
        l_ptr->press_x = element_ptr->last_pointer_motion_event.x;
        l_ptr->press_y = element_ptr->last_pointer_motion_event.y;
        if (NULL != element_ptr->parent_container_ptr) {
            wlmtk_container_pointer_grab(element_ptr->parent_container_ptr,
                                         element_ptr);
        }
        break;
    case WLMTK_BUTTON_UP:
        if (!l_ptr->pressed) break;
        l_ptr->pressed = false;
        if (NULL != element_ptr->parent_container_ptr) {
            wlmtk_container_pointer_grab_release(
                element_ptr->parent_container_ptr, element_ptr);
        }
        if (NULL != l_ptr->tile_drag_ptr) {
            wlmaker_tile_drag_t *d_ptr = l_ptr->tile_drag_ptr;
            l_ptr->tile_drag_ptr = NULL;
            // May destroy this launcher (rebuilding the drawer).
            if (d_ptr == server_ptr->tile_drag_ptr) {
                wlmaker_tile_drag_drop(d_ptr, c->x, c->y);
            }
            return true;
        }
        // A click: Launches, or (on a double-click) shows the application.
        if (_wlmaker_drawer_single_click(server_ptr)) {
            _wlmaker_drawer_launcher_launch(l_ptr);
            break;
        }
        {
            uint32_t t = button_event_ptr->time_msec;
            uint64_t msec = 0;
            const char *msec_ptr = bspl_dict_get_string_value(
                bspl_dict_get_dict(server_ptr->config_dict_ptr, "Mouse"),
                "DoubleClickTime");
            if (NULL == msec_ptr || !bs_strconvert_uint64(msec_ptr, &msec, 10) ||
                0 == msec) msec = 300;
            bool double_click = 0 != l_ptr->last_click_msec &&
                t - l_ptr->last_click_msec <= msec;
            l_ptr->last_click_msec = double_click ? 0 : (t ? t : 1);
            if (!double_click) break;
            const char *app_id_ptr = wlmaker_dock_launcher_value(
                l_ptr->dict_ptr, "AppID");
            if (0 < wlmaker_dock_app_windows(server_ptr, app_id_ptr, NULL)) {
                wlmaker_dock_app_activate(server_ptr, app_id_ptr);
            } else {
                _wlmaker_drawer_launcher_launch(l_ptr);
            }
        }
        break;
    default:
        break;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_accepts_motion: Drags the
 * launcher beyond Window Maker's MOVE_THRESHOLD; shows its balloon; keeps
 * the drawer open.
 */
bool _wlmaker_drawer_launcher_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    wlmaker_drawer_launcher_t *l_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_drawer_launcher_t,
        super_tile.super_container.super_element);
    wlmaker_drawer_t *drawer_ptr = l_ptr->drawer_ptr;
    wlmaker_server_t *server_ptr = drawer_ptr->server_ptr;
    bool rv = l_ptr->orig_element_vmt.pointer_accepts_motion(
        element_ptr, motion_event_ptr);

    if (l_ptr->pressed) {
        struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
        if (NULL == l_ptr->tile_drag_ptr) {
            double dx = c->x - l_ptr->press_lx, dy = c->y - l_ptr->press_ly;
            if (dx * dx + dy * dy < 5 * 5) return rv;
            struct wlr_buffer *image_ptr = wlmaker_dock_tile_buffer(
                &l_ptr->super_tile.style, l_ptr->icon_path_ptr, NULL);
            if (NULL == image_ptr) return rv;
            wlmaker_tile_drag_source_t source = {
                .kind = WLMAKER_TILE_DRAG_LAUNCHER,
                .column = wlmaker_clip_drawer_column(server_ptr, drawer_ptr),
                .index = l_ptr->index,
                .image_ptr = image_ptr,
                .grab_x = l_ptr->press_x,
                .grab_y = l_ptr->press_y,
                .running = l_ptr->running,
                .locked = wlmaker_dock_launcher_flag(l_ptr->dict_ptr, "Lock") };
            l_ptr->tile_drag_ptr = wlmaker_tile_drag_start(
                server_ptr, &source, c->x, c->y);
            wlr_buffer_drop(image_ptr);
        } else if (l_ptr->tile_drag_ptr == server_ptr->tile_drag_ptr) {
            wlmaker_tile_drag_motion(l_ptr->tile_drag_ptr, c->x, c->y);
        }
        return rv;
    }

    // Not on this tile: Nothing to show.
    if (!rv) return rv;
    wlmaker_drawer_hovered(drawer_ptr);
    wlmaker_clip_dock_hovered(server_ptr, true);
    if (NULL == server_ptr->dock_fx_ptr || l_ptr->balloon_entered ||
        NULL != server_ptr->tile_drag_ptr) return rv;
    l_ptr->balloon_entered = true;
    char *text_ptr = wlmaker_dock_launcher_balloon_text(l_ptr->dict_ptr);
    struct wlr_box box = wlmtk_element_get_dimensions_box(element_ptr);
    int x, y;
    wlmtk_element_get_absolute_position(element_ptr, &x, &y);
    box.x += x;
    box.y += y;
    box.height -= 2;
    struct wlr_box area = {};
    struct wlr_output *o_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, box.x + 1, box.y + 1);
    if (NULL != o_ptr) {
        wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr, o_ptr, &area);
    }
    wlmaker_dock_fx_balloon_enter(server_ptr->dock_fx_ptr, l_ptr, text_ptr,
                                  &box, &area);
    if (NULL != text_ptr) free(text_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_grab_cancel. */
void _wlmaker_drawer_launcher_pointer_grab_cancel(wlmtk_element_t *element_ptr)
{
    wlmaker_drawer_launcher_t *l_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_drawer_launcher_t,
        super_tile.super_container.super_element);
    wlmaker_server_t *server_ptr = l_ptr->drawer_ptr->server_ptr;
    l_ptr->pressed = false;
    if (NULL != l_ptr->tile_drag_ptr &&
        l_ptr->tile_drag_ptr == server_ptr->tile_drag_ptr) {
        wlmaker_tile_drag_cancel(l_ptr->tile_drag_ptr);
    }
    l_ptr->tile_drag_ptr = NULL;
    l_ptr->orig_element_vmt.pointer_grab_cancel(element_ptr);
}

/* ------------------------------------------------------------------------- */
/** The pointer left the launcher: Balloon, and the drawer's Autocollapse. */
void _wlmaker_drawer_launcher_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_drawer_launcher_t *l_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_drawer_launcher_t, pointer_leave_listener);
    wlmaker_server_t *server_ptr = l_ptr->drawer_ptr->server_ptr;
    l_ptr->balloon_entered = false;
    if (NULL != server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, l_ptr);
    }
    wlmaker_drawer_left(l_ptr->drawer_ptr);
    wlmaker_clip_dock_left(server_ptr, true);
}

/* == Unit tests =========================================================== */

static void test_entry(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_drawer_test_cases[] = {
    { true, "entry", test_entry },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_drawer_test_set = BS_TEST_SET(
    true, "drawer", _wlmaker_drawer_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests creating a drawer's entry, and copying dicts with a key replaced. */
void test_entry(bs_test_t *test_ptr)
{
    bspl_object_t *o = bspl_create_object_from_plist_string(
        "({CommandLine = a; Icon = b;}, {Drawer = Yes; Id = 1;}, "
        "{Drawer = Yes; Id = 3;})");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    bspl_dict_t *d = wlmaker_drawer_create_entry(
        bspl_array_from_object(o), "drawer.png");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_drawer_is_entry(d));
    BS_TEST_VERIFY_STREQ(test_ptr, "2", bspl_dict_get_string_value(d, "Id"));
    BS_TEST_VERIFY_STREQ(test_ptr, "drawer.png",
                         bspl_dict_get_string_value(d, "Icon"));
    BS_TEST_VERIFY_STREQ(test_ptr, "", bspl_dict_get_string_value(
                             d, "CommandLine"));
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmaker_drawer_flag(d, "AutoCollapse", false));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_drawer_is_entry(
                             bspl_dict_from_object(
                                 bspl_array_at(bspl_array_from_object(o), 0))));

    bspl_dict_t *c = wlmaker_drawer_dict_with(
        d, "Icon", bspl_array_at(bspl_array_from_object(o), 0));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, c);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_get_dict(c, "Icon"));
    BS_TEST_VERIFY_STREQ(test_ptr, "2", bspl_dict_get_string_value(c, "Id"));
    bspl_dict_unref(c);
    c = wlmaker_drawer_dict_with(d, "Icon", NULL);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, bspl_dict_get(c, "Icon"));
    bspl_dict_unref(c);
    bspl_dict_unref(d);
    bspl_object_unref(o);
}

/* == End of drawer.c ====================================================== */
