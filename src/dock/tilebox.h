/* ========================================================================= */
/**
 * @file tilebox.h
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser <kaeser@gubbe.ch>
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
#ifndef __WLMAKER_DOCK_TILEBOX_H__
#define __WLMAKER_DOCK_TILEBOX_H__

#include <stdbool.h>
#include <stdint.h>
#include <libbase/libbase.h>
#include <toolkit/toolkit.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** State of the tile box. */
typedef struct _wlmdock_tilebox wlmdock_tilebox_t;

/**
 * Creates a tile box.
 *
 * @param orientation
 * @param reverse
 * @param style_ptr
 *
 * @return Handle to the tile box' state, or NULL on error.
 */
wlmdock_tilebox_t *wlmdock_tilebox_create(
    wlmtk_box_orientation_t orientation,
    bool reverse,
    const struct wlmtk_dock_style *style_ptr);

/**
 * Destroys the tile box.
 *
 * @param tilebox_ptr
 */
void wlmdock_tilebox_destroy(wlmdock_tilebox_t *tilebox_ptr);

/**
 * Returns the tilebox' root @ref wlmtk_element_t.
 *
 * @param tilebox_ptr
 *
 * @return Pointer to the super element of @ref wlmdock_tilebox_t::tile_box.
 */
wlmtk_element_t *wlmdock_tilebox_element(wlmdock_tilebox_t *tilebox_ptr);

/**
 * Returns the tilebox' root @ref wlmtk_container_t.
 *
 * @param tilebox_ptr
 *
 * @return Pointer to the super container of @ref wlmdock_tilebox_t::tile_box.
 */
wlmtk_container_t *wlmdock_tilebox_container(wlmdock_tilebox_t *tilebox_ptr);

/**
 * Adds the tile to the tile box.
 *
 * @param tilebox_ptr
 * @param tile_ptr
 */
void wlmdock_tilebox_add_tile(
    wlmdock_tilebox_t *tilebox_ptr,
    wlmtk_tile_t *tile_ptr);

/**
 * Removes the tile from the tilebox.
 *
 * @param tilebox_ptr
 * @param tile_ptr            Tile to remove. Must have been added to the tile
 *                            box before.
 */
void wlmdock_tilebox_remove_tile(
    wlmdock_tilebox_t *tilebox_ptr,
    wlmtk_tile_t *tile_ptr);

/**
 * Sets whether tiles are stacked from the end (eg. for a dock anchored at
 * the bottom). The tiles keep their order, as counted from the anchor.
 *
 * @param tilebox_ptr
 * @param reverse
 */
void wlmdock_tilebox_set_reverse(wlmdock_tilebox_t *tilebox_ptr, bool reverse);

/**
 * Returns the tiles in order from the anchor.
 *
 * @param tilebox_ptr
 * @param tiles_ptr           Array to fill, of `max_tiles` elements.
 * @param max_tiles
 *
 * @return The number of tiles. May be larger than `max_tiles`.
 */
size_t wlmdock_tilebox_tiles(wlmdock_tilebox_t *tilebox_ptr,
                             wlmtk_tile_t **tiles_ptr,
                             size_t max_tiles);

/**
 * Returns where the tile is, in coordinates of the tile box' element.
 *
 * @param tilebox_ptr
 * @param tile_ptr
 *
 * @return The tile's box.
 */
struct wlr_box wlmdock_tilebox_tile_box(wlmdock_tilebox_t *tilebox_ptr,
                                        wlmtk_tile_t *tile_ptr);

/** A cell of the grid: Lane from the edge, slot from the anchor. */
typedef struct {
    /** Distance from the edge, in tiles. */
    int                       lane;
    /** Distance from the anchor, in tiles. */
    int                       slot;
} wlmdock_tilebox_cell_t;

/**
 * Lays the tiles out on a grid, as the Clip's: Each tile in its cell, the
 * cells without one left empty (transparent). Lanes go away from `edge`,
 * slots away from the anchor (see @ref wlmdock_tilebox_set_reverse), from
 * slot `origin` on. Replaces the tiles added before, which must have been
 * removed. With `n` 0, the tile box is a plain box again.
 *
 * @param tilebox_ptr
 * @param tiles_ptr           The tiles; listed by @ref wlmdock_tilebox_tiles
 *                            in this order.
 * @param cells_ptr           Their cells. Two on a cell: The later is left
 *                            out of the layout.
 * @param n
 * @param origin
 * @param edge
 * @param tile_style_ptr      Style of the empty cells.
 * @return true on success.
 */
bool wlmdock_tilebox_set_grid(
    wlmdock_tilebox_t *tilebox_ptr,
    wlmtk_tile_t **tiles_ptr,
    const wlmdock_tilebox_cell_t *cells_ptr,
    size_t n,
    int origin,
    uint32_t edge,
    const struct wlmtk_tile_style *tile_style_ptr);

/** Sets the edge the grid's lanes go away from. Lays them out again. */
void wlmdock_tilebox_set_edge(wlmdock_tilebox_t *tilebox_ptr, uint32_t edge);

/** @return Whether the tile box lays out a grid. */
bool wlmdock_tilebox_is_grid(wlmdock_tilebox_t *tilebox_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmdock_tilebox_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // __WLMAKER_DOCK_TILEBOX_H__
/* == End of tilebox.h ===================================================== */
