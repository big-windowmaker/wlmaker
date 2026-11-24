/* ========================================================================= */
/**
 * @file tile_drag.c
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

#include "tile_drag.h"

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "dock_fx.h"
#include "dock_launchers.h"
#include "dockapps.h"
#include "i18n.h"
#include "input/manager.h"

/* == Declarations ========================================================= */

/** State of a tile being dragged. */
struct _wlmaker_tile_drag_t {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** What is dragged. */
    wlmaker_tile_drag_source_t source;
    /** Name of the dock-app, copied. */
    char                      *name_ptr;
    /** Where the tile was. */
    wlmaker_tile_drag_target_t origin;
    /** Whether @ref wlmaker_tile_drag_t::origin is known. */
    bool                      has_origin;
    /** Where it attaches, if dropped now. */
    wlmaker_tile_drag_target_t target;
    /** Whether it attaches somewhere, if dropped now. */
    bool                      has_target;
    /** Whether it was dropped: Slides, and is then applied. */
    bool                      dropped;
    /** Whether it holds the keyboard: Esc cancels. */
    bool                      key_grab;
    /** The column that is full, over which the tile is. -1: None. */
    int                       full_column;
    /** Where the full column ends: Its last tile, in layout coordinates. */
    struct wlr_box            full_box;
    /** The output of the full column. */
    struct wlr_box            full_area;
    /** Whether the balloon telling the column is full is shown. */
    bool                      full_balloon;
    /**
     * Whether the tile is over a cell of the Clip's grid that is taken: It
     * does not attach there, and dropped there it goes back.
     */
    bool                      over_taken;
};

static bool _wlmaker_tile_drag_find_target(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y,
    wlmaker_tile_drag_target_t *target_ptr);
static bool _wlmaker_tile_drag_may_detach(wlmaker_tile_drag_t *tile_drag_ptr);
static bool _wlmaker_tile_drag_modifier_held(
    wlmaker_tile_drag_t *tile_drag_ptr);
static void _wlmaker_tile_drag_slid(void *ud_ptr);
static void _wlmaker_tile_drag_apply(wlmaker_tile_drag_t *tile_drag_ptr);
static void _wlmaker_tile_drag_remove(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y);
static bool _wlmaker_tile_drag_try_column(
    wlmaker_tile_drag_t *tile_drag_ptr,
    int id,
    bool own,
    double x,
    double y,
    wlmaker_tile_drag_target_t *target_ptr);
static void _wlmaker_tile_drag_destroy(wlmaker_tile_drag_t *tile_drag_ptr);
static int _wlmaker_tile_drag_slot_position(
    const wlmaker_dock_column_t *column_ptr,
    size_t slot,
    int *y_ptr);
static bool _wlmaker_tile_drag_key(
    xkb_keysym_t keysym,
    uint32_t modifiers,
    void *ud_ptr);
static void _wlmaker_tile_drag_count_before(
    const wlmaker_dock_column_t *column_ptr,
    size_t position,
    size_t own_position,
    size_t *launchers_ptr,
    size_t *dockapps_ptr);
static void _wlmaker_tile_drag_place_dockapps(
    wlmaker_tile_drag_t *tile_drag_ptr);
static void _wlmaker_tile_drag_full_balloon(
    wlmaker_tile_drag_t *tile_drag_ptr,
    bool over_full);
static size_t _wlmaker_tile_drag_grid_icon(
    const wlmaker_tile_drag_source_t *source_ptr,
    size_t launchers);
static bool _wlmaker_tile_drag_can_push(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own);
static bool _wlmaker_tile_drag_apply_grid(wlmaker_tile_drag_t *tile_drag_ptr);

/* == Data ================================================================= */

/** Window Maker's DOCK_DETTACH_THRESHOLD: Tiles away before detaching. */
static const int _wlmaker_tile_drag_detach_threshold = 3;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_tile_drag_t *wlmaker_tile_drag_start(
    wlmaker_server_t *server_ptr,
    const wlmaker_tile_drag_source_t *source_ptr,
    double x,
    double y)
{
    if (NULL != server_ptr->tile_drag_ptr) {
        wlmaker_tile_drag_cancel(server_ptr->tile_drag_ptr);
    }
    if (NULL == server_ptr->dock_fx_ptr || NULL == source_ptr->image_ptr) {
        return NULL;
    }
    wlmaker_tile_drag_t *tile_drag_ptr = logged_calloc(
        1, sizeof(*tile_drag_ptr));
    if (NULL == tile_drag_ptr) return NULL;
    tile_drag_ptr->server_ptr = server_ptr;
    tile_drag_ptr->source = *source_ptr;
    tile_drag_ptr->full_column = -1;
    wlr_buffer_lock(tile_drag_ptr->source.image_ptr);
    if (NULL != source_ptr->launcher_ptr) {
        bspl_dict_ref(source_ptr->launcher_ptr);
    }
    if (NULL != source_ptr->name_ptr) {
        tile_drag_ptr->name_ptr = logged_strdup(source_ptr->name_ptr);
    }
    tile_drag_ptr->source.name_ptr = tile_drag_ptr->name_ptr;
    server_ptr->tile_drag_ptr = tile_drag_ptr;

    // Esc cancels, unless something else holds the keyboard.
    if (NULL != server_ptr->input_manager_ptr &&
        !wlmim_has_key_grab(server_ptr->input_manager_ptr)) {
        wlmim_set_key_grab(server_ptr->input_manager_ptr,
                           _wlmaker_tile_drag_key, server_ptr);
        tile_drag_ptr->key_grab = true;
    }

    // Where it was: Its slot in the column. A new launcher has none.
    wlmaker_dock_column_t column;
    size_t grid_launchers = 0;
    const wlmaker_clip_grid_t *grid_ptr = wlmaker_clip_grid(
        server_ptr, &grid_launchers);
    size_t icon = _wlmaker_tile_drag_grid_icon(source_ptr, grid_launchers);
    if (WLMAKER_COLUMN_CLIP == source_ptr->column && NULL != grid_ptr &&
        icon < grid_ptr->n &&
        wlmaker_clip_column_id(server_ptr, source_ptr->column, &column)) {
        // From the Clip: Its cell on the grid.
        tile_drag_ptr->origin = (wlmaker_tile_drag_target_t){
            .column = source_ptr->column,
            .index = source_ptr->index,
            .grid = true,
            .cell = grid_ptr->cells_ptr[icon] };
        wlmaker_clip_cell_position(
            column.place, column.x, column.y, column.pitch,
            &tile_drag_ptr->origin.cell,
            &tile_drag_ptr->origin.x, &tile_drag_ptr->origin.y);
        tile_drag_ptr->has_origin = true;
        tile_drag_ptr->target = tile_drag_ptr->origin;
        tile_drag_ptr->has_target = true;
        if (WLMAKER_TILE_DRAG_LAUNCHER == source_ptr->kind) {
            struct wlr_buffer *empty_ptr = wlmaker_dock_tile_buffer(
                &server_ptr->style_ptr->tile, NULL, NULL);
            wlmaker_dock_fx_cover_show(
                server_ptr->dock_fx_ptr, empty_ptr,
                tile_drag_ptr->origin.x, tile_drag_ptr->origin.y);
            if (NULL != empty_ptr) wlr_buffer_drop(empty_ptr);
        }
    } else if (WLMAKER_COLUMN_NONE != source_ptr->column &&
        wlmaker_clip_column_id(server_ptr, source_ptr->column, &column)) {
        size_t position = (WLMAKER_TILE_DRAG_DOCKAPP == source_ptr->kind) ?
            wlmaker_dock_column_dockapp_position(&column, source_ptr->index) :
            wlmaker_dock_column_launcher_position(&column, source_ptr->index);
        size_t slot = 1 + position;
        tile_drag_ptr->origin = (wlmaker_tile_drag_target_t){
            .column = source_ptr->column,
            .index = source_ptr->index,
            .slot = slot,
            .position = position };
        _wlmaker_tile_drag_count_before(
            &column, position, position,
            &tile_drag_ptr->origin.launchers_before,
            &tile_drag_ptr->origin.dockapps_before);
        tile_drag_ptr->origin.x = _wlmaker_tile_drag_slot_position(
            &column, slot, &tile_drag_ptr->origin.y);
        tile_drag_ptr->has_origin = true;
        tile_drag_ptr->target = tile_drag_ptr->origin;
        tile_drag_ptr->has_target = true;

        // A launcher is drawn by `wlmdock` (or the drawer): Cover it with an
        // empty tile.
        if (WLMAKER_TILE_DRAG_LAUNCHER == source_ptr->kind) {
            struct wlr_buffer *empty_ptr = wlmaker_dock_tile_buffer(
                &server_ptr->style_ptr->tile, NULL, NULL);
            wlmaker_dock_fx_cover_show(
                server_ptr->dock_fx_ptr, empty_ptr,
                tile_drag_ptr->origin.x, tile_drag_ptr->origin.y);
            if (NULL != empty_ptr) wlr_buffer_drop(empty_ptr);
        }
    }
    if (NULL != source_ptr->lift) {
        source_ptr->lift(source_ptr->lift_ud_ptr, true);
    }
    wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, NULL);
    wlmaker_dock_fx_drag_show(
        server_ptr->dock_fx_ptr, source_ptr->image_ptr,
        x - source_ptr->grab_x, y - source_ptr->grab_y);
    wlmaker_tile_drag_motion(tile_drag_ptr, x, y);
    return tile_drag_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_tile_drag_motion(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y)
{
    if (tile_drag_ptr->dropped) return;
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    wlmaker_dock_fx_drag_show(
        server_ptr->dock_fx_ptr, NULL,
        x - tile_drag_ptr->source.grab_x, y - tile_drag_ptr->source.grab_y);

    wlmaker_tile_drag_target_t target = {};
    tile_drag_ptr->full_column = -1;
    tile_drag_ptr->over_taken = false;
    bool found = _wlmaker_tile_drag_find_target(tile_drag_ptr, x, y, &target);
    if (found) {
        tile_drag_ptr->target = target;
        tile_drag_ptr->has_target = true;
    } else if (!_wlmaker_tile_drag_may_detach(tile_drag_ptr)) {
        // Window Maker: Stays where it was last attached.
    } else if (tile_drag_ptr->over_taken) {
        // Over a taken cell: No ghost there; dropped, it goes back.
        tile_drag_ptr->has_target = false;
    } else {
        tile_drag_ptr->has_target = false;
    }
    _wlmaker_tile_drag_full_balloon(tile_drag_ptr, !found);

    if (tile_drag_ptr->has_target) {
        wlmaker_dock_fx_ghost_show(
            server_ptr->dock_fx_ptr,
            wlmaker_dock_fx_ghost_visible(server_ptr->dock_fx_ptr) ?
            NULL : tile_drag_ptr->source.image_ptr,
            tile_drag_ptr->target.x, tile_drag_ptr->target.y);
    } else {
        wlmaker_dock_fx_ghost_hide(server_ptr->dock_fx_ptr);
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_tile_drag_drop(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y)
{
    if (tile_drag_ptr->dropped) return;
    wlmaker_tile_drag_motion(tile_drag_ptr, x, y);
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    tile_drag_ptr->dropped = true;
    wlmaker_dock_fx_ghost_hide(server_ptr->dock_fx_ptr);

    if (!tile_drag_ptr->has_target && tile_drag_ptr->over_taken) {
        // Dropped on another icon of the Clip: Refused, it goes back. Not
        // lost by a slip of the hand.
        bs_log(BS_INFO, "Cell of the Clip taken: Tile drag refused.");
        wlmaker_tile_drag_cancel(tile_drag_ptr);
        return;
    }
    if (!tile_drag_ptr->has_target && 0 <= tile_drag_ptr->full_column) {
        // Dropped on a full column: Refused. It goes back where it was.
        bs_log(BS_INFO, "Column %d is full: Tile drag refused.",
               tile_drag_ptr->full_column);
        wlmaker_tile_drag_cancel(tile_drag_ptr);
        return;
    }
    if (!tile_drag_ptr->has_target) {
        wlmaker_dock_fx_drag_hide(server_ptr->dock_fx_ptr);
        wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
        _wlmaker_tile_drag_remove(tile_drag_ptr, x, y);
        _wlmaker_tile_drag_destroy(tile_drag_ptr);
        return;
    }
    // Slides into its slot, then gets attached there.
    wlmaker_dock_fx_slide(
        server_ptr->dock_fx_ptr,
        tile_drag_ptr->target.x, tile_drag_ptr->target.y,
        _wlmaker_tile_drag_slid, tile_drag_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_tile_drag_cancel(wlmaker_tile_drag_t *tile_drag_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    wlmaker_dock_fx_ghost_hide(server_ptr->dock_fx_ptr);
    wlmaker_dock_fx_drag_hide(server_ptr->dock_fx_ptr);
    wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
    if (NULL != tile_drag_ptr->source.lift) {
        tile_drag_ptr->source.lift(tile_drag_ptr->source.lift_ud_ptr, false);
    }
    _wlmaker_tile_drag_destroy(tile_drag_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_tile_drag_forget_tile(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr)
{
    wlmaker_tile_drag_t *tile_drag_ptr = server_ptr->tile_drag_ptr;
    if (NULL == tile_drag_ptr || NULL == tile_ptr ||
        tile_drag_ptr->source.tile_ptr != tile_ptr) return;
    tile_drag_ptr->source.lift = NULL;
    wlmaker_tile_drag_cancel(tile_drag_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_tile_drag_snap(
    const wlmaker_dock_column_t *column_ptr,
    wlmaker_tile_drag_kind_t kind,
    bool own_column,
    size_t own_position,
    int x,
    int y,
    wlmaker_tile_drag_target_t *target_ptr)
{
    target_ptr->full = false;
    int pitch = column_ptr->pitch;
    if (0 >= pitch) return false;
    // As wDockSnapIcon: The slot nearest the tile's position.
    int offset = column_ptr->size / 2;
    int ex_x = (x < column_ptr->x) ?
        (x - offset - column_ptr->x) / pitch :
        (x + offset - column_ptr->x) / pitch;
    int ex_y = (y < column_ptr->y) ?
        (y - offset - column_ptr->y) / pitch :
        (y + offset - column_ptr->y) / pitch;

    int along, across;
    switch (column_ptr->place.edge) {
    case WLR_EDGE_LEFT:
    case WLR_EDGE_RIGHT:
        along = (WLR_EDGE_BOTTOM == column_ptr->place.anchor) ? -ex_y : ex_y;
        across = ex_x;
        break;
    default:
        along = (WLR_EDGE_RIGHT == column_ptr->place.anchor) ? -ex_x : ex_x;
        across = ex_y;
        break;
    }
    if (own_column) {
        if (abs(across) > _wlmaker_tile_drag_detach_threshold) return false;
    } else if (0 != across) {
        return false;
    }
    if (0 > along) return false;
    if (!own_column) own_position = SIZE_MAX;

    // A tile more: The last one must stay on the output.
    const struct wlr_box *o_ptr = &column_ptr->output;
    if (!own_column && !wlr_box_empty(o_ptr)) {
        int lx, ly;
        lx = _wlmaker_tile_drag_slot_position(
            column_ptr, 1 + column_ptr->launchers + column_ptr->dockapps, &ly);
        if (lx < o_ptr->x || ly < o_ptr->y ||
            lx + column_ptr->size > o_ptr->x + o_ptr->width ||
            ly + column_ptr->size > o_ptr->y + o_ptr->height) {
            target_ptr->full = true;
            return false;
        }
    }

    size_t position;
    if (column_ptr->mixed) {
        // Launchers and dock-apps share the column: Any slot after the first
        // tile, up to right after the last one (the tile's own, if from
        // here).
        size_t n = column_ptr->launchers + column_ptr->dockapps;
        size_t max_position = (SIZE_MAX != own_position && 0 < n) ? n - 1 : n;
        position = (1 > along) ? 0 : (size_t)along - 1;
        if (position > max_position) position = max_position;
        _wlmaker_tile_drag_count_before(
            column_ptr, position, own_position,
            &target_ptr->launchers_before, &target_ptr->dockapps_before);
        target_ptr->index = (WLMAKER_TILE_DRAG_LAUNCHER == kind) ?
            target_ptr->launchers_before : target_ptr->dockapps_before;
    } else {
        // Launchers come right after the first tile; dock-apps after them.
        // Within its own area, a tile keeps one of the existing slots.
        size_t first, count;
        if (WLMAKER_TILE_DRAG_LAUNCHER == kind) {
            first = 1;
            count = column_ptr->launchers;
        } else {
            first = 1 + column_ptr->launchers;
            count = column_ptr->dockapps;
        }
        size_t max_index = (own_column && 0 < count) ? count - 1 : count;
        size_t index = (along < (int)first) ? 0 : (size_t)along - first;
        if (index > max_index) index = max_index;
        target_ptr->index = index;
        position = first - 1 + index;
        bool launcher = WLMAKER_TILE_DRAG_LAUNCHER == kind;
        target_ptr->launchers_before = launcher ? index :
            column_ptr->launchers;
        target_ptr->dockapps_before = launcher ? 0 : index;
    }
    target_ptr->position = position;
    target_ptr->slot = 1 + position;
    target_ptr->x = _wlmaker_tile_drag_slot_position(
        column_ptr, target_ptr->slot, &target_ptr->y);

    // Not off the output.
    if (!wlr_box_empty(o_ptr) &&
        (target_ptr->x < o_ptr->x || target_ptr->y < o_ptr->y ||
         target_ptr->x + column_ptr->size > o_ptr->x + o_ptr->width ||
         target_ptr->y + column_ptr->size > o_ptr->y + o_ptr->height)) {
        return false;
    }
    return true;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** @return The x position of `slot`, and its y position in `y_ptr`. */
int _wlmaker_tile_drag_slot_position(
    const wlmaker_dock_column_t *column_ptr,
    size_t slot,
    int *y_ptr)
{
    int d = (int)slot * column_ptr->pitch;
    int x = column_ptr->x, y = column_ptr->y;
    switch (column_ptr->place.anchor) {
    case WLR_EDGE_TOP: y += d; break;
    case WLR_EDGE_BOTTOM: y -= d; break;
    case WLR_EDGE_LEFT: x += d; break;
    case WLR_EDGE_RIGHT: x -= d; break;
    default: break;
    }
    *y_ptr = y;
    return x;
}

/* ------------------------------------------------------------------------- */
/**
 * Counts the launchers and the dock-apps of the column that will be before
 * a tile at `position`, leaving out the tile at `own_position` (SIZE_MAX:
 * none), which is the one moved.
 */
void _wlmaker_tile_drag_count_before(
    const wlmaker_dock_column_t *column_ptr,
    size_t position,
    size_t own_position,
    size_t *launchers_ptr,
    size_t *dockapps_ptr)
{
    size_t launchers = 0, dockapps = 0, before = 0;
    size_t n = column_ptr->launchers + column_ptr->dockapps;
    for (size_t p = 0; p < n && before < position; ++p) {
        if (p == own_position) continue;
        if (wlmaker_dock_column_dockapp_at(column_ptr, p, NULL)) {
            ++dockapps;
        } else {
            ++launchers;
        }
        ++before;
    }
    *launchers_ptr = launchers;
    *dockapps_ptr = dockapps;
}

/* ------------------------------------------------------------------------- */
/**
 * Finds where the tile attaches, for the pointer at `x`, `y`: The column it
 * comes from first, then the other one.
 */
bool _wlmaker_tile_drag_find_target(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y,
    wlmaker_tile_drag_target_t *target_ptr)
{
    // Window Maker: With the move modifier held, a docked icon detaches.
    // Not for dock-apps: They are dragged with the modifier, or by the
    // tile's border.
    if (WLMAKER_TILE_DRAG_LAUNCHER == tile_drag_ptr->source.kind &&
        !tile_drag_ptr->source.locked &&
        _wlmaker_tile_drag_modifier_held(tile_drag_ptr)) return false;

    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    int own = tile_drag_ptr->source.column;
    // The column it comes from first: With Window Maker's threshold.
    if (WLMAKER_COLUMN_NONE != own &&
        _wlmaker_tile_drag_try_column(tile_drag_ptr, own, true, x, y,
                                      target_ptr)) return true;
    // A drawer's tile stays on the Dock.
    if (tile_drag_ptr->source.drawer) return false;
    // Then the open drawers, the Dock and the Clip.
    for (int id = 2; id < wlmaker_clip_columns(server_ptr); ++id) {
        wlmaker_drawer_t *d_ptr = wlmaker_clip_drawer(server_ptr, id);
        if (id == own || NULL == d_ptr || wlmaker_drawer_collapsed(d_ptr)) {
            continue;
        }
        if (_wlmaker_tile_drag_try_column(tile_drag_ptr, id, false, x, y,
                                          target_ptr)) return true;
    }
    static const int docks[2] = { WLMAKER_COLUMN_DOCK, WLMAKER_COLUMN_CLIP };
    for (int i = 0; i < 2; ++i) {
        if (docks[i] == own) continue;
        if (_wlmaker_tile_drag_try_column(tile_drag_ptr, docks[i], false, x, y,
                                          target_ptr)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/**
 * Tries to attach the tile to column `id`, for the pointer at `x`, `y`. On
 * the Dock, with the pointer on a drawer's tile: Into that drawer, which
 * opens (as Window Maker's Autoexpand).
 */
bool _wlmaker_tile_drag_try_column(
    wlmaker_tile_drag_t *tile_drag_ptr,
    int id,
    bool own,
    double x,
    double y,
    wlmaker_tile_drag_target_t *target_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    int tx = x - tile_drag_ptr->source.grab_x;
    int ty = y - tile_drag_ptr->source.grab_y;
    wlmaker_dock_column_t column;
    if (!wlmaker_clip_column_id(server_ptr, id, &column)) return false;

    if (WLMAKER_COLUMN_DOCK == id && !tile_drag_ptr->source.drawer) {
        for (int d = 2; d < wlmaker_clip_columns(server_ptr); ++d) {
            wlmaker_drawer_t *d_ptr = wlmaker_clip_drawer(server_ptr, d);
            wlmaker_dock_column_t dc;
            if (NULL == d_ptr || !wlmaker_drawer_column(d_ptr, &dc)) continue;
            struct wlr_box box = { .x = dc.x, .y = dc.y,
                                   .width = dc.size, .height = dc.size };
            if (!wlr_box_contains_point(&box, x, y)) continue;
            wlmaker_drawer_hovered(d_ptr);
            bool own = d == tile_drag_ptr->source.column;
            if (!wlmaker_tile_drag_snap(
                    &dc, tile_drag_ptr->source.kind, own,
                    own && tile_drag_ptr->has_origin ?
                    tile_drag_ptr->origin.position : SIZE_MAX,
                    dc.x, dc.y, target_ptr)) return false;
            target_ptr->column = d;
            return true;
        }
    }
    if (column.grid) {
        // The Clip: On its grid, as Window Maker's wDockSnapIcon for WM_CLIP.
        size_t launchers = 0;
        const wlmaker_clip_grid_t *g_ptr = wlmaker_clip_grid(
            server_ptr, &launchers);
        if (NULL == g_ptr) return false;
        bool push = false;
        size_t own_icon = (own && tile_drag_ptr->has_origin) ?
            _wlmaker_tile_drag_grid_icon(&tile_drag_ptr->source, launchers) :
            SIZE_MAX;
        wlmaker_clip_cell_t cell = wlmaker_clip_cell_at(
            column.place, column.x, column.y, column.size, column.pitch,
            tx, ty);
        switch (wlmaker_clip_grid_snap(g_ptr, &cell, own_icon)) {
        case WLMAKER_CLIP_GRID_ATTACH:
            break;
        case WLMAKER_CLIP_GRID_OCCUPIED:
            // Another icon there: It takes that cell, and the icons from
            // there on move down to the first free cell. Without room
            // (or on the Clip itself): Refused.
            if (!_wlmaker_tile_drag_can_push(g_ptr, &cell, own_icon)) {
                tile_drag_ptr->over_taken = true;
                return false;
            }
            push = true;
            break;
        default:
            // Down the Clip's column, past its icons: At its end.
            if (!wlmaker_clip_grid_column_end(g_ptr, &cell, own_icon)) {
                return false;
            }
            break;
        }
        *target_ptr = (wlmaker_tile_drag_target_t){
            .column = id, .grid = true, .cell = cell, .push = push };
        if (own) {
            target_ptr->index = tile_drag_ptr->source.index;
        } else {
            target_ptr->index = (WLMAKER_TILE_DRAG_LAUNCHER ==
                                 tile_drag_ptr->source.kind) ?
                column.launchers : column.dockapps;
        }
        wlmaker_clip_cell_position(column.place, column.x, column.y,
                                   column.pitch, &cell,
                                   &target_ptr->x, &target_ptr->y);
        return true;
    }
    if (!wlmaker_tile_drag_snap(
            &column, tile_drag_ptr->source.kind, own,
            own && tile_drag_ptr->has_origin ?
            tile_drag_ptr->origin.position : SIZE_MAX,
            tx, ty, target_ptr)) {
        if (target_ptr->full && 0 > tile_drag_ptr->full_column) {
            // Over a full column: Tell, at its last tile.
            size_t last = column.launchers + column.dockapps;
            tile_drag_ptr->full_column = id;
            tile_drag_ptr->full_box.x = _wlmaker_tile_drag_slot_position(
                &column, last, &tile_drag_ptr->full_box.y);
            tile_drag_ptr->full_box.width = column.size;
            tile_drag_ptr->full_box.height = column.size;
            tile_drag_ptr->full_area = column.output;
        }
        return false;
    }
    target_ptr->column = id;
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * @return Whether the tile may be dropped off the docks: Not if locked. For
 *     a running application, only with the move modifier held (Window
 *     Maker's MOD_MASK). Dock-apps always may: They are removed.
 */
bool _wlmaker_tile_drag_may_detach(wlmaker_tile_drag_t *tile_drag_ptr)
{
    if (tile_drag_ptr->source.locked) return false;
    if (WLMAKER_TILE_DRAG_DOCKAPP == tile_drag_ptr->source.kind) return true;
    if (!tile_drag_ptr->source.running) return true;
    return _wlmaker_tile_drag_modifier_held(tile_drag_ptr);
}

/* ------------------------------------------------------------------------- */
/** @return Whether the move modifier (as for moving windows) is held. */
bool _wlmaker_tile_drag_modifier_held(wlmaker_tile_drag_t *tile_drag_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    uint32_t modifier = 0;
    if (NULL != server_ptr->input_manager_ptr) {
        modifier = wlmim_cursor_options(
            server_ptr->input_manager_ptr)->move_window_modifier;
    }
    struct wlr_keyboard *kb_ptr = NULL;
    if (NULL != server_ptr->wlr_seat_ptr) {
        kb_ptr = wlr_seat_get_keyboard(server_ptr->wlr_seat_ptr);
    }
    if (0 == modifier || NULL == kb_ptr) return false;
    return modifier == (wlr_keyboard_get_modifiers(kb_ptr) & modifier);
}

/* ------------------------------------------------------------------------- */
/** The tile slid into its slot: Attaches it there. */
void _wlmaker_tile_drag_slid(void *ud_ptr)
{
    wlmaker_tile_drag_t *tile_drag_ptr = ud_ptr;
    _wlmaker_tile_drag_apply(tile_drag_ptr);
    _wlmaker_tile_drag_destroy(tile_drag_ptr);
}

/* ------------------------------------------------------------------------- */
/** Attaches the tile at the target. */
void _wlmaker_tile_drag_apply(wlmaker_tile_drag_t *tile_drag_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    wlmaker_tile_drag_source_t *s_ptr = &tile_drag_ptr->source;
    wlmaker_tile_drag_target_t *t_ptr = &tile_drag_ptr->target;
    bool moved = s_ptr->column != t_ptr->column || s_ptr->index != t_ptr->index;
    // In the Dock and the Clip, the place among the others of the other
    // kind may change too.
    bool reordered = moved || !tile_drag_ptr->has_origin ||
        t_ptr->position != tile_drag_ptr->origin.position;
    wlmaker_dock_column_t column;
    bool mixed = wlmaker_clip_column_id(server_ptr, t_ptr->column, &column) &&
        column.mixed;
    if (t_ptr->grid && _wlmaker_tile_drag_apply_grid(tile_drag_ptr)) return;

    if (WLMAKER_TILE_DRAG_DOCKAPP == s_ptr->kind) {
        if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
        if (reordered && NULL != s_ptr->tile_ptr) {
            // The others stay where they are among the launchers.
            wlmaker_clip_freeze_dockapps(server_ptr, s_ptr->column);
            wlmaker_clip_freeze_dockapps(server_ptr, t_ptr->column);
            if (moved) {
                wlmaker_clip_move_dockapp(server_ptr, s_ptr->tile_ptr,
                                          t_ptr->column, t_ptr->index);
            }
            if (mixed) {
                wlmaker_clip_set_dockapp_after(
                    server_ptr, s_ptr->tile_ptr, t_ptr->launchers_before);
            }
        }
        wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
        return;
    }

    if (NULL != s_ptr->launcher_ptr) {
        // A new launcher, from an application's icon: Inserted there.
        bs_log(BS_INFO, "Docking \"%s\" in column %d at %zu (slot %zu)",
               wlmaker_dock_launcher_value(s_ptr->launcher_ptr, "CommandLine"),
               t_ptr->column, t_ptr->index, t_ptr->slot);
        wlmaker_clip_freeze_dockapps(server_ptr, t_ptr->column);
        wlmaker_dock_launchers_edit(server_ptr, t_ptr->column, t_ptr->index,
                                    s_ptr->launcher_ptr);
        if (mixed) _wlmaker_tile_drag_place_dockapps(tile_drag_ptr);
        wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
        if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
        return;
    }
    if (reordered) {
        wlmaker_clip_freeze_dockapps(server_ptr, s_ptr->column);
        wlmaker_clip_freeze_dockapps(server_ptr, t_ptr->column);
        if (moved) {
            wlmaker_dock_launchers_move(
                server_ptr, s_ptr->column, s_ptr->index,
                t_ptr->column, t_ptr->index);
        }
        if (mixed) _wlmaker_tile_drag_place_dockapps(tile_drag_ptr);
    }
    // `wlmdock` shows the launcher where it now is.
    wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
    if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
}

/* ------------------------------------------------------------------------- */
/**
 * Shows (or hides) the balloon telling that the column the tile is over is
 * full, as found by the last @ref _wlmaker_tile_drag_find_target.
 */
void _wlmaker_tile_drag_full_balloon(
    wlmaker_tile_drag_t *tile_drag_ptr,
    bool over_full)
{
    wlmaker_dock_fx_t *fx_ptr = tile_drag_ptr->server_ptr->dock_fx_ptr;
    if (NULL == fx_ptr) return;
    if (!over_full || 0 > tile_drag_ptr->full_column) {
        if (tile_drag_ptr->full_balloon) {
            wlmaker_dock_fx_balloon_leave(fx_ptr, tile_drag_ptr);
            tile_drag_ptr->full_balloon = false;
        }
        return;
    }
    if (tile_drag_ptr->full_balloon) return;
    const char *text_ptr = N_("The drawer is full");
    if (WLMAKER_COLUMN_DOCK == tile_drag_ptr->full_column) {
        text_ptr = N_("The Dock is full");
    } else if (WLMAKER_COLUMN_CLIP == tile_drag_ptr->full_column) {
        text_ptr = N_("The Clip is full");
    }
    wlmaker_dock_fx_balloon_enter(
        fx_ptr, tile_drag_ptr, wlmaker_i18n(text_ptr),
        &tile_drag_ptr->full_box, &tile_drag_ptr->full_area);
    tile_drag_ptr->full_balloon = true;
}

/* ------------------------------------------------------------------------- */
/**
 * A launcher was put at the target, in the Dock or the Clip: The dock-apps
 * that were before the target stay before it, the others after it.
 */
void _wlmaker_tile_drag_place_dockapps(wlmaker_tile_drag_t *tile_drag_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    wlmaker_tile_drag_target_t *t_ptr = &tile_drag_ptr->target;
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_id(
        server_ptr, t_ptr->column);
    if (NULL == d_ptr) return;
    size_t i = t_ptr->index;
    for (size_t j = 0; j < wlmaker_dockapps_size(d_ptr); ++j) {
        wlmtk_tile_t *tile_ptr = wlmaker_dockapps_tile_at(d_ptr, j);
        size_t after = wlmaker_dockapps_after_launchers(d_ptr, tile_ptr);
        if (j < t_ptr->dockapps_before && after > i) {
            wlmaker_clip_set_dockapp_after(server_ptr, tile_ptr, i);
        } else if (j >= t_ptr->dockapps_before && after <= i) {
            wlmaker_clip_set_dockapp_after(server_ptr, tile_ptr, i + 1);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Removes the tile dropped off the docks at `x`, `y`: With Window Maker's
 * kaboom, unless the application is running. A dock-app is terminated, and
 * remembered as removed.
 */
void _wlmaker_tile_drag_remove(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    wlmaker_tile_drag_source_t *s_ptr = &tile_drag_ptr->source;
    int tx = x - s_ptr->grab_x, ty = y - s_ptr->grab_y;
    // The other icons of the Clip stay in their cells.
    if (WLMAKER_COLUMN_CLIP == s_ptr->column) {
        wlmaker_clip_freeze_grid(server_ptr);
    }

    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, x, y);
    struct wlr_box area = {};
    if (NULL != wlr_output_ptr) {
        wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr,
                                  wlr_output_ptr, &area);
    }

    if (WLMAKER_TILE_DRAG_DOCKAPP == s_ptr->kind) {
        bs_log(BS_INFO, "Dock-app \"%s\" dropped off the docks: Removing.",
               NULL != s_ptr->name_ptr ? s_ptr->name_ptr : "(unknown)");
        wlmaker_dock_fx_kaboom(server_ptr->dock_fx_ptr, s_ptr->image_ptr,
                               tx, ty, &area);
        if (NULL != s_ptr->name_ptr) {
            wlmaker_dockapps_set_removed(
                wlmaker_clip_dockapps_fname(server_ptr), s_ptr->name_ptr, true);
        }
        if (NULL != s_ptr->tile_ptr) {
            wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(
                server_ptr, s_ptr->tile_ptr);
            if (NULL != d_ptr) wlmaker_dockapps_detach(d_ptr, s_ptr->tile_ptr);
            if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
        }
        if (1 < s_ptr->pid && getpid() != s_ptr->pid &&
            0 != kill(s_ptr->pid, SIGTERM)) {
            bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%"PRIdMAX", SIGTERM)",
                   (intmax_t)s_ptr->pid);
        }
        return;
    }

    // An application's icon, not docked: It stays an application's icon.
    if (NULL != s_ptr->launcher_ptr) {
        if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
        return;
    }
    // A launcher: Kaboom, unless its application runs (Window Maker).
    if (!s_ptr->running) {
        wlmaker_dock_fx_kaboom(server_ptr->dock_fx_ptr, s_ptr->image_ptr,
                               tx, ty, &area);
    }
    wlmaker_dock_launchers_edit(server_ptr, s_ptr->column, s_ptr->index, NULL);
}

/* ------------------------------------------------------------------------- */
/** @return Whether a drop at the taken `cell_ptr` can move the icons down. */
bool _wlmaker_tile_drag_can_push(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own)
{
    if (0 == grid_ptr->n) return false;
    wlmaker_clip_cell_t *cells_ptr = logged_calloc(
        grid_ptr->n, sizeof(wlmaker_clip_cell_t));
    if (NULL == cells_ptr) return false;
    bool rv = wlmaker_clip_grid_push(grid_ptr, cell_ptr, own, cells_ptr);
    free(cells_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** @return The icon of the source on the Clip's grid (launchers first). */
size_t _wlmaker_tile_drag_grid_icon(
    const wlmaker_tile_drag_source_t *source_ptr,
    size_t launchers)
{
    if (WLMAKER_COLUMN_CLIP != source_ptr->column) return SIZE_MAX;
    return (WLMAKER_TILE_DRAG_DOCKAPP == source_ptr->kind) ?
        launchers + source_ptr->index : source_ptr->index;
}

/* ------------------------------------------------------------------------- */
/**
 * Attaches the tile at the target cell of the Clip's grid. First, the cells
 * of the Clip's icons are stored (those of a state from before the grid):
 * The others stay where they are.
 * @return true if done.
 */
bool _wlmaker_tile_drag_apply_grid(wlmaker_tile_drag_t *tile_drag_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    wlmaker_tile_drag_source_t *s_ptr = &tile_drag_ptr->source;
    wlmaker_tile_drag_target_t *t_ptr = &tile_drag_ptr->target;
    size_t launchers = 0;
    if (NULL == wlmaker_clip_grid(server_ptr, &launchers)) return false;
    wlmaker_clip_freeze_grid(server_ptr);
    // Where it comes from stays as it is, too.
    if (WLMAKER_COLUMN_CLIP != s_ptr->column &&
        WLMAKER_COLUMN_NONE != s_ptr->column) {
        wlmaker_clip_freeze_dockapps(server_ptr, s_ptr->column);
    }
    char buf[32];
    wlmaker_clip_cell_format(&t_ptr->cell, buf, sizeof(buf));
    if (t_ptr->push) {
        // The icons from the cell on move down, making room.
        bs_log(BS_INFO, "Clip's cell %s taken: Moving the icons from there "
               "down.", buf);
        wlmaker_clip_push_icons(
            server_ptr, &t_ptr->cell,
            tile_drag_ptr->has_origin ?
            _wlmaker_tile_drag_grid_icon(s_ptr, launchers) : SIZE_MAX);
    }

    if (WLMAKER_TILE_DRAG_DOCKAPP == s_ptr->kind) {
        if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
        if (NULL != s_ptr->tile_ptr) {
            bs_log(BS_INFO, "Dock-app \"%s\" to the Clip's cell %s",
                   NULL != s_ptr->name_ptr ? s_ptr->name_ptr : "(unknown)",
                   buf);
            wlmaker_clip_place_dockapp(server_ptr, s_ptr->tile_ptr,
                                       &t_ptr->cell);
        }
        wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
        return true;
    }

    const wlmaker_clip_grid_t *g_ptr = wlmaker_clip_grid(
        server_ptr, &launchers);
    if (NULL != s_ptr->launcher_ptr) {
        // A new launcher, from an application's icon.
        bs_log(BS_INFO, "Docking \"%s\" in the Clip's cell %s",
               wlmaker_dock_launcher_value(s_ptr->launcher_ptr, "CommandLine"),
               buf);
        bspl_dict_t *d = wlmaker_dock_launcher_with_cell(
            s_ptr->launcher_ptr, &t_ptr->cell);
        if (NULL != d) {
            wlmaker_dock_launchers_edit(server_ptr, WLMAKER_COLUMN_CLIP,
                                        launchers, d);
            bspl_dict_unref(d);
        }
    } else if (WLMAKER_COLUMN_CLIP == s_ptr->column) {
        // Within the Clip: Only its cell changes.
        bs_log(BS_INFO, "Launcher %zu to the Clip's cell %s", s_ptr->index,
               buf);
        if (NULL != g_ptr && s_ptr->index < launchers &&
            launchers <= g_ptr->n) {
            wlmaker_clip_cell_t *cells_ptr = logged_calloc(
                launchers, sizeof(wlmaker_clip_cell_t));
            bool *set_ptr = logged_calloc(launchers, sizeof(bool));
            if (NULL != cells_ptr && NULL != set_ptr) {
                memcpy(cells_ptr, g_ptr->cells_ptr,
                       launchers * sizeof(wlmaker_clip_cell_t));
                cells_ptr[s_ptr->index] = t_ptr->cell;
                set_ptr[s_ptr->index] = true;
                wlmaker_dock_launchers_set_cells(
                    server_ptr, cells_ptr, set_ptr, launchers);
            }
            if (NULL != set_ptr) free(set_ptr);
            if (NULL != cells_ptr) free(cells_ptr);
        }
    } else {
        // From the Dock or a drawer: Into the Clip, at the cell.
        bs_log(BS_INFO, "Launcher %zu of column %d to the Clip's cell %s",
               s_ptr->index, s_ptr->column, buf);
        wlmaker_dock_launchers_move_cell(
            server_ptr, s_ptr->column, s_ptr->index,
            WLMAKER_COLUMN_CLIP, launchers, &t_ptr->cell);
    }
    wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
    if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Keys while dragging: Esc cancels, the tile goes back. */
bool _wlmaker_tile_drag_key(
    xkb_keysym_t keysym,
    __UNUSED__ uint32_t modifiers,
    void *ud_ptr)
{
    wlmaker_server_t *server_ptr = ud_ptr;
    wlmaker_tile_drag_t *tile_drag_ptr = server_ptr->tile_drag_ptr;
    if (XKB_KEY_Escape != keysym || NULL == tile_drag_ptr) return false;
    // Dropped: It slides into its slot. Too late.
    if (tile_drag_ptr->dropped) return true;
    bs_log(BS_INFO, "Tile drag cancelled (Esc).");
    wlmaker_tile_drag_cancel(tile_drag_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Destroys the drag. */
void _wlmaker_tile_drag_destroy(wlmaker_tile_drag_t *tile_drag_ptr)
{
    wlmaker_server_t *server_ptr = tile_drag_ptr->server_ptr;
    if (server_ptr->tile_drag_ptr == tile_drag_ptr) {
        server_ptr->tile_drag_ptr = NULL;
    }
    if (tile_drag_ptr->key_grab && NULL != server_ptr->input_manager_ptr) {
        wlmim_set_key_grab(server_ptr->input_manager_ptr, NULL, NULL);
    }
    if (tile_drag_ptr->full_balloon && NULL != server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, tile_drag_ptr);
    }
    if (NULL != tile_drag_ptr->source.launcher_ptr) {
        bspl_dict_unref(tile_drag_ptr->source.launcher_ptr);
        tile_drag_ptr->source.launcher_ptr = NULL;
    }
    // Drawers opened while dragging close again, with Autocollapse.
    for (int id = 2; NULL != server_ptr->clip_ptr &&
             id < wlmaker_clip_columns(server_ptr); ++id) {
        wlmaker_drawer_t *d_ptr = wlmaker_clip_drawer(server_ptr, id);
        if (NULL != d_ptr) wlmaker_drawer_left(d_ptr);
    }
    if (NULL != tile_drag_ptr->source.image_ptr) {
        wlr_buffer_unlock(tile_drag_ptr->source.image_ptr);
    }
    if (NULL != tile_drag_ptr->name_ptr) free(tile_drag_ptr->name_ptr);
    free(tile_drag_ptr);
}

/* == Unit tests =========================================================== */

static void test_snap(bs_test_t *test_ptr);
static void test_snap_mixed(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_tile_drag_test_cases[] = {
    { true, "snap", test_snap },
    { true, "snap_mixed", test_snap_mixed },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_tile_drag_test_set = BS_TEST_SET(
    true, "tile_drag", _wlmaker_tile_drag_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests where dragged tiles attach, as Window Maker's wDockSnapIcon. */
void test_snap(bs_test_t *test_ptr)
{
    // The Dock at the top right of 1920x1080: Main tile, 2 launchers, 1 app.
    wlmaker_dock_column_t dock = {
        .place = { WLR_EDGE_RIGHT, WLR_EDGE_TOP },
        .x = 1856, .y = 0, .size = 64, .pitch = 65,
        .launchers = 2, .dockapps = 1,
        .output = { .width = 1920, .height = 1080 } };
    wlmaker_tile_drag_target_t t;

    // A dock-app from the Clip, right over the Dock's 4th slot: Attaches to
    // the end of the Dock's dock-apps.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                            1850, 4 * 65 + 10, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 4, t.slot);
    BS_TEST_VERIFY_EQ(test_ptr, 1856, t.x);
    BS_TEST_VERIFY_EQ(test_ptr, 260, t.y);
    // Far down the column: Still the end.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                            1856, 900, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    // Over the launchers: The first dock-app slot.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                            1856, 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 3, t.slot);
    // One tile to the left: Not over the Dock, not attached.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &dock, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                             1856 - 65, 260, &t));
    // ... unless it comes from the Dock: Up to 3 tiles away.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, true, 0,
                            1856 - 3 * 65, 260, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &dock, WLMAKER_TILE_DRAG_DOCKAPP, true, 0,
                             1856 - 5 * 65, 260, &t));

    // A launcher over the main tile: The first launcher slot.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_LAUNCHER, true, 0,
                            1856, 0, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.slot);
    // Within its own launchers, it keeps one of the 2 slots.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_LAUNCHER, true, 0,
                            1856, 500, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    // From the Clip, it can go after them.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_LAUNCHER, false, SIZE_MAX,
                            1856, 500, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);

    // The Clip at the bottom left: Slots go up from the anchor.
    wlmaker_dock_column_t clip = {
        .place = { WLR_EDGE_LEFT, WLR_EDGE_BOTTOM },
        .x = 0, .y = 1016, .size = 64, .pitch = 65,
        .launchers = 0, .dockapps = 2,
        .output = { .width = 1920, .height = 1080 } };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, true, 0,
                            5, 1016 - 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1016 - 65, t.y);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                            5, 1016 - 3 * 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);
    // Below the Clip (off the output): Not attached.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &clip, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                             5, 1100, &t));
}

/* ------------------------------------------------------------------------- */
/**
 * Tests the Dock and the Clip, where launchers and dock-apps share one order:
 * Any slot is reachable, for either kind, up to right after the last tile.
 */
void test_snap_mixed(bs_test_t *test_ptr)
{
    // The Clip at the top left: Clip, L0, D0 (wlmeyes), L1, D1 (zones).
    wlmaker_dock_column_t clip = {
        .place = { WLR_EDGE_LEFT, WLR_EDGE_TOP },
        .x = 0, .y = 0, .size = 64, .pitch = 65,
        .launchers = 2, .dockapps = 2,
        .dockapp_positions = (1 << 1) | (1 << 3),
        .mixed = true,
        .output = { .width = 1920, .height = 1080 } };
    wlmaker_tile_drag_target_t t;
    size_t i;

    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_column_dockapp_at(&clip, 0, &i));
    BS_TEST_VERIFY_EQ(test_ptr, 0, i);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_column_dockapp_at(&clip, 1, &i));
    BS_TEST_VERIFY_EQ(test_ptr, 0, i);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_column_dockapp_at(&clip, 2, &i));
    BS_TEST_VERIFY_EQ(test_ptr, 1, i);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_column_dockapp_at(&clip, 3, &i));
    BS_TEST_VERIFY_EQ(test_ptr, 1, i);
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dock_column_launcher_position(&clip, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 3, wlmaker_dock_column_dockapp_position(&clip, 1));

    // A new launcher (from an appicon) after the last dock-app: Slot 5.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, false, SIZE_MAX,
                            5, 5 * 65 + 3, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 5, t.slot);
    BS_TEST_VERIFY_EQ(test_ptr, 4, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.launchers_before);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.dockapps_before);
    // Far below: Still right after the last.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, false, SIZE_MAX,
                            5, 900, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 5, t.slot);
    // Between D0 and L1 (over L1's slot 3): Launcher index 1, after D0.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, false, SIZE_MAX,
                            5, 3 * 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.dockapps_before);
    // Over D0's slot 2: Before D0.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, false, SIZE_MAX,
                            5, 2 * 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.dockapps_before);
    // Over the Clip itself: The first slot.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, false, SIZE_MAX,
                            5, 0, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);

    // L0 (position 0) moved within: Up to the last slot, after D1.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, true, 0,
                            5, 900, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 3, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.launchers_before);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.dockapps_before);
    // ... or between D0 and L1: Still launcher 0, but after D0.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_LAUNCHER, true, 0,
                            5, 2 * 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.dockapps_before);

    // D1 (position 3) moved to the first slot: Before all launchers.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, true, 3,
                            5, 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.launchers_before);
    // D0 (position 1) moved after L1: Index 0 among the others, 2 launchers.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, true, 1,
                            5, 3 * 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.launchers_before);
    // A dock-app from the Dock, at the very end of the Clip.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, false, SIZE_MAX,
                            5, 900, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 4, t.position);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.launchers_before);

    // A full Clip: No room for one more (refused, and told). Its own tiles
    // still move within.
    wlmaker_dock_column_t full = clip;
    full.output.height = 5 * 65;
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &full, WLMAKER_TILE_DRAG_LAUNCHER, false,
                             SIZE_MAX, 5, 65, &t));
    BS_TEST_VERIFY_TRUE(test_ptr, t.full);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &full, WLMAKER_TILE_DRAG_LAUNCHER, true, 0,
                            5, 4 * 65, &t));
    BS_TEST_VERIFY_FALSE(test_ptr, t.full);
    BS_TEST_VERIFY_EQ(test_ptr, 3, t.position);

    // A drawer: Launchers first. Sections, as before.
    wlmaker_dock_column_t drawer = clip;
    drawer.mixed = false;
    wlmaker_dock_column_set_sections(&drawer);
    BS_TEST_VERIFY_EQ(test_ptr, (1 << 2) | (1 << 3), drawer.dockapp_positions);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &drawer, WLMAKER_TILE_DRAG_LAUNCHER, false,
                            SIZE_MAX, 5, 900, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.position);
}

/* == End of tile_drag.c =================================================== */
