/* ========================================================================= */
/**
 * @file clip_grid.h
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
#ifndef __WLMAKER_CLIP_GRID_H__
#define __WLMAKER_CLIP_GRID_H__

#include <stdbool.h>
#include <stddef.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

#include "dock_position.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * A cell of the Clip's grid, as Window Maker's `xindex`, `yindex` of the
 * icons attached to the Clip, but counted from the Clip's corner of the
 * output: The Clip itself is at 0, 0.
 */
typedef struct {
    /** Distance from the output's edge the Clip is at, in tiles. */
    int                       lane;
    /** Distance from the Clip's anchor, along the edge, in tiles. */
    int                       slot;
} wlmaker_clip_cell_t;

/** Where a tile dropped at a cell goes, see @ref wlmaker_clip_grid_snap. */
typedef enum {
    /** The cell is free, on the output, next to another icon: It attaches. */
    WLMAKER_CLIP_GRID_ATTACH,
    /**
     * The cell holds another icon (or the Clip): It does not attach there.
     * Dropped there, the tile goes back to where it was (Window Maker would
     * detach it; that lost dock-apps and launchers by a slip of the hand).
     */
    WLMAKER_CLIP_GRID_OCCUPIED,
    /**
     * Away from the Clip's icons, or off the output: Not attached, as
     * Window Maker's wDockSnapIcon returning False. Dropped there, the tile
     * detaches.
     */
    WLMAKER_CLIP_GRID_OUTSIDE,
} wlmaker_clip_grid_snap_t;

/**
 * The cells of the icons attached to the Clip: Launchers first (in the order
 * of the state's `Launchers`), then dock-apps (in their placement's order).
 */
typedef struct {
    /** Number of icons. */
    size_t                    n;
    /** Cell of each icon. */
    wlmaker_clip_cell_t       *cells_ptr;
    /** Lanes that fit on the output (all are fully on it). */
    int                       lanes;
    /** Slots that fit on the output. */
    int                       slots;
} wlmaker_clip_grid_t;

/** Window Maker's CLIP_ATTACH_VICINITY: Neighbours are this close. */
#define WLMAKER_CLIP_ATTACH_VICINITY 1

/**
 * Parses a cell as stored ("lane,slot").
 * @return false if `str_ptr` is NULL or not a cell.
 */
bool wlmaker_clip_cell_parse(const char *str_ptr, wlmaker_clip_cell_t *cell_ptr);

/** Formats the cell as stored, into `buf_ptr` of `size` bytes. */
void wlmaker_clip_cell_format(
    const wlmaker_clip_cell_t *cell_ptr,
    char *buf_ptr,
    size_t size);

/**
 * Computes how many lanes and slots of tiles at `x`, `y` (the Clip's tile,
 * `size` wide, every `pitch`) fit on `output_ptr`, for `place`.
 */
void wlmaker_clip_grid_bounds(
    wlmaker_dock_place_t place,
    int x,
    int y,
    int size,
    int pitch,
    const struct wlr_box *output_ptr,
    int *lanes_ptr,
    int *slots_ptr);

/**
 * Position of `cell_ptr`'s tile, in layout coordinates, for the Clip's tile
 * at `x`, `y`.
 */
void wlmaker_clip_cell_position(
    wlmaker_dock_place_t place,
    int x,
    int y,
    int pitch,
    const wlmaker_clip_cell_t *cell_ptr,
    int *cell_x_ptr,
    int *cell_y_ptr);

/**
 * The cell nearest to a tile with its top-left at `tile_x`, `tile_y`, as
 * Window Maker's wDockSnapIcon computes `ex_x`, `ex_y` (rounding to the
 * nearest; half a tile counts for the next one). May be outside the grid.
 */
wlmaker_clip_cell_t wlmaker_clip_cell_at(
    wlmaker_dock_place_t place,
    int x,
    int y,
    int size,
    int pitch,
    int tile_x,
    int tile_y);

/**
 * @return The cell next to `cell_ptr` on the screen, towards `direction`
 *     (one of WLR_EDGE_TOP, _BOTTOM, _LEFT, _RIGHT), for the Clip at `place`.
 */
wlmaker_clip_cell_t wlmaker_clip_cell_towards(
    wlmaker_dock_place_t place,
    const wlmaker_clip_cell_t *cell_ptr,
    uint32_t direction);

/** @return Whether `cell_ptr` is within the grid's bounds. */
bool wlmaker_clip_grid_contains(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr);

/**
 * @return The icon at `cell_ptr`, other than `except` (SIZE_MAX: none); or
 *     SIZE_MAX if there is none.
 */
size_t wlmaker_clip_grid_icon_at(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t except);

/**
 * Where a tile attaches, when dragged to `cell_ptr`, as Window Maker's
 * wDockSnapIcon for the Clip: To a free cell on the output that has a
 * neighbour (the Clip, or another icon than itself) within
 * @ref WLMAKER_CLIP_ATTACH_VICINITY, also diagonally.
 *
 * @param grid_ptr
 * @param cell_ptr
 * @param own                 The icon dragged, if attached to the Clip;
 *                            else SIZE_MAX. It may always go back to its
 *                            own cell.
 */
wlmaker_clip_grid_snap_t wlmaker_clip_grid_snap(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own);

/**
 * Finds a free cell for an icon newly attached to the Clip, without one: The
 * first free slot of the Clip's column, from the Clip on (the column is the
 * room kept off the windows), then the next lanes.
 * @return false if the grid is full; `cell_ptr` is then past its end.
 */
bool wlmaker_clip_grid_free_cell(
    const wlmaker_clip_grid_t *grid_ptr,
    wlmaker_clip_cell_t *cell_ptr);

/**
 * Gives the icons their cells: Those with a cell stored (`stored_ptr[i]`
 * true) keep it if it is within the grid and not taken by an icon before
 * them in `order_ptr`. The others get the free cells of
 * @ref wlmaker_clip_grid_free_cell, in `order_ptr` order: For a state
 * without any cells (before the grid), each icon gets the slot of the
 * Clip's column it had in the linear order.
 *
 * @param grid_ptr            With `n`, `cells_ptr` (the stored cells),
 *                            `lanes`, `slots` set. Cells are updated.
 * @param stored_ptr          For each icon: Whether its cell was stored.
 * @param order_ptr           The icons, in the column's order (a
 *                            permutation of 0 .. n-1).
 * @param derived_ptr         Set for each icon whose cell was not kept
 *                            (not stored, invalid, or off the output). May
 *                            be NULL.
 */
void wlmaker_clip_grid_resolve(
    wlmaker_clip_grid_t *grid_ptr,
    const bool *stored_ptr,
    const size_t *order_ptr,
    bool *derived_ptr);

/** @return The highest lane used, or -1 if there are no icons. */
int wlmaker_clip_grid_max_lane(const wlmaker_clip_grid_t *grid_ptr);

/** @return The highest slot used in `lane`, or 0 (the Clip's row). */
int wlmaker_clip_grid_max_slot(const wlmaker_clip_grid_t *grid_ptr, int lane);

/** @return The lowest slot used in `lane` other than 0, or INT_MAX. */
int wlmaker_clip_grid_min_slot(const wlmaker_clip_grid_t *grid_ptr, int lane);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_clip_grid_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_CLIP_GRID_H__ */
/* == End of clip_grid.h =================================================== */
