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
static bool _wlmaker_tile_drag_move_launcher(
    wlmaker_server_t *server_ptr,
    bool from_dock,
    size_t from_index,
    bool to_dock,
    size_t to_index);
static void _wlmaker_tile_drag_destroy(wlmaker_tile_drag_t *tile_drag_ptr);
static int _wlmaker_tile_drag_slot_position(
    const wlmaker_dock_column_t *column_ptr,
    size_t slot,
    int *y_ptr);

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
    wlr_buffer_lock(tile_drag_ptr->source.image_ptr);
    if (NULL != source_ptr->name_ptr) {
        tile_drag_ptr->name_ptr = logged_strdup(source_ptr->name_ptr);
    }
    tile_drag_ptr->source.name_ptr = tile_drag_ptr->name_ptr;
    server_ptr->tile_drag_ptr = tile_drag_ptr;

    // Where it was: Its slot in the column.
    wlmaker_dock_column_t column;
    if (wlmaker_clip_column(server_ptr, source_ptr->from_dock, &column)) {
        size_t slot = 1 + source_ptr->index;
        if (WLMAKER_TILE_DRAG_DOCKAPP == source_ptr->kind) {
            slot += column.launchers;
        }
        tile_drag_ptr->origin = (wlmaker_tile_drag_target_t){
            .dock = source_ptr->from_dock,
            .index = source_ptr->index,
            .slot = slot };
        tile_drag_ptr->origin.x = _wlmaker_tile_drag_slot_position(
            &column, slot, &tile_drag_ptr->origin.y);
        tile_drag_ptr->has_origin = true;
        tile_drag_ptr->target = tile_drag_ptr->origin;
        tile_drag_ptr->has_target = true;

        // A launcher is drawn by `wlmdock`: Cover it with an empty tile.
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

    wlmaker_tile_drag_target_t target;
    if (_wlmaker_tile_drag_find_target(tile_drag_ptr, x, y, &target)) {
        tile_drag_ptr->target = target;
        tile_drag_ptr->has_target = true;
    } else if (!_wlmaker_tile_drag_may_detach(tile_drag_ptr)) {
        // Window Maker: Stays where it was last attached.
    } else {
        tile_drag_ptr->has_target = false;
    }

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
    int x,
    int y,
    wlmaker_tile_drag_target_t *target_ptr)
{
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
    target_ptr->slot = first + index;
    target_ptr->x = _wlmaker_tile_drag_slot_position(
        column_ptr, target_ptr->slot, &target_ptr->y);

    // Not off the output.
    const struct wlr_box *o_ptr = &column_ptr->output;
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
 * Finds where the tile attaches, for the pointer at `x`, `y`: The column it
 * comes from first, then the other one.
 */
bool _wlmaker_tile_drag_find_target(
    wlmaker_tile_drag_t *tile_drag_ptr,
    double x,
    double y,
    wlmaker_tile_drag_target_t *target_ptr)
{
    int tx = x - tile_drag_ptr->source.grab_x;
    int ty = y - tile_drag_ptr->source.grab_y;
    // Window Maker: With the move modifier held, a docked icon detaches.
    // Not for dock-apps: They are dragged with the modifier, or by the
    // tile's border.
    if (WLMAKER_TILE_DRAG_LAUNCHER == tile_drag_ptr->source.kind &&
        !tile_drag_ptr->source.locked &&
        _wlmaker_tile_drag_modifier_held(tile_drag_ptr)) return false;

    bool docks[2] = { tile_drag_ptr->source.from_dock,
                      !tile_drag_ptr->source.from_dock };
    for (int i = 0; i < 2; ++i) {
        wlmaker_dock_column_t column;
        if (!wlmaker_clip_column(tile_drag_ptr->server_ptr, docks[i], &column)) {
            continue;
        }
        bool own = (0 == i);
        // The dragged tile is no longer counted in its own area.
        if (wlmaker_tile_drag_snap(&column, tile_drag_ptr->source.kind, own,
                                   tx, ty, target_ptr)) {
            target_ptr->dock = docks[i];
            return true;
        }
    }
    return false;
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
    bool moved = s_ptr->from_dock != t_ptr->dock || s_ptr->index != t_ptr->index;

    if (WLMAKER_TILE_DRAG_DOCKAPP == s_ptr->kind) {
        if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
        if (moved && NULL != s_ptr->tile_ptr) {
            wlmaker_clip_move_dockapp(server_ptr, s_ptr->tile_ptr,
                                      t_ptr->dock, t_ptr->index);
        }
        wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
        return;
    }

    if (moved) {
        _wlmaker_tile_drag_move_launcher(
            server_ptr, s_ptr->from_dock, s_ptr->index,
            t_ptr->dock, t_ptr->index);
    }
    // `wlmdock` shows the launcher where it now is.
    wlmaker_dock_fx_cover_hide(server_ptr->dock_fx_ptr);
    if (NULL != s_ptr->lift) s_ptr->lift(s_ptr->lift_ud_ptr, false);
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

    // A launcher: Kaboom, unless its application runs (Window Maker).
    if (!s_ptr->running) {
        wlmaker_dock_fx_kaboom(server_ptr->dock_fx_ptr, s_ptr->image_ptr,
                               tx, ty, &area);
    }
    const char *section_ptr = s_ptr->from_dock ? "Dock" : "Clip";
    bspl_array_t *a = wlmaker_dock_launchers_load(server_ptr, section_ptr);
    bspl_array_t *n = bspl_array_create();
    bool rv = NULL != a && NULL != n;
    for (size_t i = 0; rv && i < bspl_array_size(a); ++i) {
        if (i == s_ptr->index) continue;
        rv = bspl_array_push_back(n, bspl_array_at(a, i));
    }
    if (rv) wlmaker_dock_launchers_store(server_ptr, section_ptr, n);
    if (NULL != n) bspl_array_unref(n);
    if (NULL != a) bspl_array_unref(a);
}

/* ------------------------------------------------------------------------- */
/** Moves a launcher in the state: Within the Dock or the Clip, or across. */
bool _wlmaker_tile_drag_move_launcher(
    wlmaker_server_t *server_ptr,
    bool from_dock,
    size_t from_index,
    bool to_dock,
    size_t to_index)
{
    const char *from_ptr = from_dock ? "Dock" : "Clip";
    const char *to_ptr = to_dock ? "Dock" : "Clip";
    bspl_array_t *from_a = wlmaker_dock_launchers_load(server_ptr, from_ptr);
    bspl_array_t *to_a = (from_dock == to_dock) ? NULL :
        wlmaker_dock_launchers_load(server_ptr, to_ptr);
    bspl_array_t *new_from = bspl_array_create();
    bspl_array_t *new_to = (from_dock == to_dock) ? NULL : bspl_array_create();
    bool rv = NULL != from_a && NULL != new_from &&
        from_index < bspl_array_size(from_a) &&
        (from_dock == to_dock || (NULL != to_a && NULL != new_to));
    bspl_object_t *moved_ptr = rv ? bspl_array_at(from_a, from_index) : NULL;

    // Without the moved one; and for a move within, with it at `to_index`.
    for (size_t i = 0, j = 0; rv && i < bspl_array_size(from_a); ++i) {
        if (i == from_index) continue;
        if (from_dock == to_dock && j == to_index) {
            rv = bspl_array_push_back(new_from, moved_ptr);
            ++j;
        }
        rv = rv && bspl_array_push_back(new_from, bspl_array_at(from_a, i));
        ++j;
    }
    if (rv && from_dock == to_dock &&
        bspl_array_size(new_from) < bspl_array_size(from_a)) {
        rv = bspl_array_push_back(new_from, moved_ptr);
    }
    if (rv && from_dock != to_dock) {
        size_t n = bspl_array_size(to_a);
        for (size_t i = 0; rv && i <= n; ++i) {
            if (i == to_index || (i == n && to_index > n)) {
                rv = bspl_array_push_back(new_to, moved_ptr);
            }
            if (rv && i < n) {
                rv = bspl_array_push_back(new_to, bspl_array_at(to_a, i));
            }
        }
    }
    if (rv && NULL != new_to) {
        rv = wlmaker_dock_launchers_store(server_ptr, to_ptr, new_to);
    }
    if (rv) rv = wlmaker_dock_launchers_store(server_ptr, from_ptr, new_from);

    if (NULL != new_to) bspl_array_unref(new_to);
    if (NULL != new_from) bspl_array_unref(new_from);
    if (NULL != to_a) bspl_array_unref(to_a);
    if (NULL != from_a) bspl_array_unref(from_a);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Destroys the drag. */
void _wlmaker_tile_drag_destroy(wlmaker_tile_drag_t *tile_drag_ptr)
{
    if (tile_drag_ptr->server_ptr->tile_drag_ptr == tile_drag_ptr) {
        tile_drag_ptr->server_ptr->tile_drag_ptr = NULL;
    }
    if (NULL != tile_drag_ptr->source.image_ptr) {
        wlr_buffer_unlock(tile_drag_ptr->source.image_ptr);
    }
    if (NULL != tile_drag_ptr->name_ptr) free(tile_drag_ptr->name_ptr);
    free(tile_drag_ptr);
}

/* == Unit tests =========================================================== */

static void test_snap(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_tile_drag_test_cases[] = {
    { true, "snap", test_snap },
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
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, false,
                            1850, 4 * 65 + 10, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 4, t.slot);
    BS_TEST_VERIFY_EQ(test_ptr, 1856, t.x);
    BS_TEST_VERIFY_EQ(test_ptr, 260, t.y);
    // Far down the column: Still the end.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, false,
                            1856, 900, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    // Over the launchers: The first dock-app slot.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, false,
                            1856, 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 3, t.slot);
    // One tile to the left: Not over the Dock, not attached.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &dock, WLMAKER_TILE_DRAG_DOCKAPP, false,
                             1856 - 65, 260, &t));
    // ... unless it comes from the Dock: Up to 3 tiles away.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_DOCKAPP, true,
                            1856 - 3 * 65, 260, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &dock, WLMAKER_TILE_DRAG_DOCKAPP, true,
                             1856 - 5 * 65, 260, &t));

    // A launcher over the main tile: The first launcher slot.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_LAUNCHER, true,
                            1856, 0, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.slot);
    // Within its own launchers, it keeps one of the 2 slots.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_LAUNCHER, true,
                            1856, 500, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.index);
    // From the Clip, it can go after them.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &dock, WLMAKER_TILE_DRAG_LAUNCHER, false,
                            1856, 500, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);

    // The Clip at the bottom left: Slots go up from the anchor.
    wlmaker_dock_column_t clip = {
        .place = { WLR_EDGE_LEFT, WLR_EDGE_BOTTOM },
        .x = 0, .y = 1016, .size = 64, .pitch = 65,
        .launchers = 0, .dockapps = 2,
        .output = { .width = 1920, .height = 1080 } };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, true,
                            5, 1016 - 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.index);
    BS_TEST_VERIFY_EQ(test_ptr, 1016 - 65, t.y);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_tile_drag_snap(
                            &clip, WLMAKER_TILE_DRAG_DOCKAPP, false,
                            5, 1016 - 3 * 65, &t));
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.index);
    // Below the Clip (off the output): Not attached.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_tile_drag_snap(
                             &clip, WLMAKER_TILE_DRAG_DOCKAPP, false,
                             5, 1100, &t));
}

/* == End of tile_drag.c =================================================== */
