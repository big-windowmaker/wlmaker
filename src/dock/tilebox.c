/* ========================================================================= */
/**
 * @file tilebox.c
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

#include "tilebox.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "libbase/libbase.h"
#include "toolkit/toolkit.h"
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** State of the tile box. */
struct _wlmdock_tilebox {
    /** Box holding the tiles. */
    wlmtk_box_t               tile_box;
    /** Whether to add tiles in 'reverse'. */
    bool                      reverse;
    /** List of tiles, via @ref wlmtk_dlnode_from_tile. */
    bs_dllist_t               tiles;
    /** Orientation of @ref wlmdock_tilebox_t::tile_box. */
    wlmtk_box_orientation_t   orientation;
    /** Margin between tiles. */
    struct wlmtk_margin_style margin;

    /** Whether the tiles are on a grid: Lanes of @ref wlmdock_tilebox_t::grid_box. */
    bool                      grid;
    /** Holds the lanes, across: The only element of the tile box. */
    wlmtk_box_t               grid_box;
    /** The lanes: Boxes along the edge, each with a tile or empty per cell. */
    wlmtk_box_t               *lanes_ptr;
    /** Number of lanes. */
    size_t                    lanes;
    /** Empty tiles of the grid. */
    wlmtk_tile_t              *fillers_ptr;
    /** Number of fillers. */
    size_t                    fillers;
    /** The grid's tiles, as given. */
    wlmtk_tile_t              **grid_tiles_ptr;
    /** Their cells. */
    wlmdock_tilebox_cell_t    *grid_cells_ptr;
    /** Number of the grid's tiles. */
    size_t                    grid_n;
    /** First slot. */
    int                       origin;
    /** Edge the lanes go away from. */
    uint32_t                  edge;
    /** Style of the empty tiles. */
    struct wlmtk_tile_style   filler_style;
};

static void _wlmdock_tilebox_clear_grid(wlmdock_tilebox_t *tilebox_ptr);
static bool _wlmdock_tilebox_build_grid(wlmdock_tilebox_t *tilebox_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmdock_tilebox_t *wlmdock_tilebox_create(
    wlmtk_box_orientation_t orientation,
    bool reverse,
    const struct wlmtk_dock_style *style_ptr)
{
    wlmdock_tilebox_t *tilebox_ptr = logged_calloc(1, sizeof(*tilebox_ptr));
    if (NULL == tilebox_ptr) return NULL;
    tilebox_ptr->reverse = reverse;
    tilebox_ptr->orientation = orientation;
    tilebox_ptr->margin = style_ptr->margin;

    if (!wlmtk_box_init(
            &tilebox_ptr->tile_box,
            orientation,
            &style_ptr->margin)) {
        wlmdock_tilebox_destroy(tilebox_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(wlmtk_box_element(&tilebox_ptr->tile_box), true);

    return tilebox_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmdock_tilebox_destroy(wlmdock_tilebox_t *tilebox_ptr)
{
    _wlmdock_tilebox_clear_grid(tilebox_ptr);
    if (NULL != tilebox_ptr->grid_tiles_ptr) free(tilebox_ptr->grid_tiles_ptr);
    if (NULL != tilebox_ptr->grid_cells_ptr) free(tilebox_ptr->grid_cells_ptr);
    wlmtk_box_fini(&tilebox_ptr->tile_box);
    free(tilebox_ptr);
}

/* ------------------------------------------------------------------------- */
wlmtk_element_t *wlmdock_tilebox_element(wlmdock_tilebox_t *tilebox_ptr)
{
    return wlmtk_box_element(&tilebox_ptr->tile_box);
}

/* ------------------------------------------------------------------------- */
wlmtk_container_t *wlmdock_tilebox_container(wlmdock_tilebox_t *tilebox_ptr)
{
    return &tilebox_ptr->tile_box.super_container;
}

/* ------------------------------------------------------------------------- */
void wlmdock_tilebox_add_tile(wlmdock_tilebox_t *tilebox_ptr,
                              wlmtk_tile_t *tile_ptr)
{
    BS_ASSERT(NULL == wlmtk_tile_element(tile_ptr)->parent_container_ptr);
    if (tilebox_ptr->reverse) {
        wlmtk_box_add_element_front(
            &tilebox_ptr->tile_box,
            wlmtk_tile_element(tile_ptr));
        bs_dllist_push_front(
            &tilebox_ptr->tiles,
            wlmtk_dlnode_from_tile(tile_ptr));
    } else {
        wlmtk_box_add_element_back(
            &tilebox_ptr->tile_box,
            wlmtk_tile_element(tile_ptr));
        bs_dllist_push_back(
            &tilebox_ptr->tiles,
            wlmtk_dlnode_from_tile(tile_ptr));
    }
}

/* ------------------------------------------------------------------------- */
void wlmdock_tilebox_remove_tile(wlmdock_tilebox_t *tilebox_ptr,
                                 wlmtk_tile_t *tile_ptr)
{
    if (tilebox_ptr->grid) {
        // In its lane, if laid out.
        wlmtk_container_t *p_ptr =
            wlmtk_tile_element(tile_ptr)->parent_container_ptr;
        for (size_t l = 0; NULL != p_ptr && l < tilebox_ptr->lanes; ++l) {
            if (&tilebox_ptr->lanes_ptr[l].element_container != p_ptr) continue;
            wlmtk_box_remove_element(&tilebox_ptr->lanes_ptr[l],
                                     wlmtk_tile_element(tile_ptr));
            break;
        }
        bs_dllist_remove(&tilebox_ptr->tiles, wlmtk_dlnode_from_tile(tile_ptr));
        for (size_t i = 0; i < tilebox_ptr->grid_n; ++i) {
            if (tilebox_ptr->grid_tiles_ptr[i] == tile_ptr) {
                tilebox_ptr->grid_tiles_ptr[i] = NULL;
            }
        }
        return;
    }
    BS_ASSERT(
        &tilebox_ptr->tile_box.element_container ==
        wlmtk_tile_element(tile_ptr)->parent_container_ptr);
    wlmtk_box_remove_element(
        &tilebox_ptr->tile_box,
        wlmtk_tile_element(tile_ptr));
    bs_dllist_remove(
        &tilebox_ptr->tiles,
        wlmtk_dlnode_from_tile(tile_ptr));
}

/* ------------------------------------------------------------------------- */
struct wlr_box wlmdock_tilebox_tile_box(wlmdock_tilebox_t *tilebox_ptr,
                                        wlmtk_tile_t *tile_ptr)
{
    struct wlr_box box = wlmtk_element_get_dimensions_box(
        wlmtk_tile_element(tile_ptr));
    // Up the containers, to the tile box: On a grid, through the lanes.
    wlmtk_element_t *root_ptr = wlmtk_box_element(&tilebox_ptr->tile_box);
    for (wlmtk_element_t *e_ptr = wlmtk_tile_element(tile_ptr);
         NULL != e_ptr && e_ptr != root_ptr;
         e_ptr = NULL != e_ptr->parent_container_ptr ?
             &e_ptr->parent_container_ptr->super_element : NULL) {
        int x, y;
        wlmtk_element_get_position(e_ptr, &x, &y);
        box.x += x;
        box.y += y;
    }
    return box;
}

/* ------------------------------------------------------------------------- */
size_t wlmdock_tilebox_tiles(wlmdock_tilebox_t *tilebox_ptr,
                             wlmtk_tile_t **tiles_ptr,
                             size_t max_tiles)
{
    size_t n = 0;
    for (bs_dllist_node_t *dlnode_ptr = tilebox_ptr->reverse ?
             tilebox_ptr->tiles.tail_ptr : tilebox_ptr->tiles.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = tilebox_ptr->reverse ?
             dlnode_ptr->prev_ptr : dlnode_ptr->next_ptr) {
        if (n < max_tiles) tiles_ptr[n] = wlmtk_tile_from_dlnode(dlnode_ptr);
        ++n;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
void wlmdock_tilebox_set_reverse(wlmdock_tilebox_t *tilebox_ptr, bool reverse)
{
    if (tilebox_ptr->reverse == reverse) return;
    if (tilebox_ptr->grid) {
        tilebox_ptr->reverse = reverse;
        _wlmdock_tilebox_build_grid(tilebox_ptr);
        return;
    }
    size_t n = bs_dllist_size(&tilebox_ptr->tiles);
    wlmtk_tile_t **tiles_ptr = NULL;
    if (0 < n) {
        tiles_ptr = logged_calloc(n, sizeof(wlmtk_tile_t*));
        if (NULL == tiles_ptr) return;
    }
    wlmdock_tilebox_tiles(tilebox_ptr, tiles_ptr, n);
    for (size_t i = 0; i < n; ++i) {
        wlmdock_tilebox_remove_tile(tilebox_ptr, tiles_ptr[i]);
    }
    tilebox_ptr->reverse = reverse;
    for (size_t i = 0; i < n; ++i) {
        wlmdock_tilebox_add_tile(tilebox_ptr, tiles_ptr[i]);
    }
    if (NULL != tiles_ptr) free(tiles_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmdock_tilebox_set_grid(
    wlmdock_tilebox_t *tilebox_ptr,
    wlmtk_tile_t **tiles_ptr,
    const wlmdock_tilebox_cell_t *cells_ptr,
    size_t n,
    int origin,
    uint32_t edge,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    _wlmdock_tilebox_clear_grid(tilebox_ptr);
    if (NULL != tilebox_ptr->grid_tiles_ptr) free(tilebox_ptr->grid_tiles_ptr);
    if (NULL != tilebox_ptr->grid_cells_ptr) free(tilebox_ptr->grid_cells_ptr);
    tilebox_ptr->grid_tiles_ptr = NULL;
    tilebox_ptr->grid_cells_ptr = NULL;
    tilebox_ptr->grid_n = 0;
    tilebox_ptr->grid = false;
    if (0 == n) return true;

    tilebox_ptr->grid_tiles_ptr = logged_calloc(n, sizeof(wlmtk_tile_t*));
    tilebox_ptr->grid_cells_ptr = logged_calloc(
        n, sizeof(wlmdock_tilebox_cell_t));
    if (NULL == tilebox_ptr->grid_tiles_ptr ||
        NULL == tilebox_ptr->grid_cells_ptr) return false;
    memcpy(tilebox_ptr->grid_tiles_ptr, tiles_ptr, n * sizeof(wlmtk_tile_t*));
    memcpy(tilebox_ptr->grid_cells_ptr, cells_ptr,
           n * sizeof(wlmdock_tilebox_cell_t));
    tilebox_ptr->grid_n = n;
    tilebox_ptr->origin = origin;
    tilebox_ptr->edge = edge;
    tilebox_ptr->filler_style = *tile_style_ptr;
    tilebox_ptr->grid = true;
    // Listed in their order from the anchor, as other tiles.
    for (size_t i = 0; i < n; ++i) {
        BS_ASSERT(NULL == wlmtk_tile_element(tiles_ptr[i])->parent_container_ptr);
        if (tilebox_ptr->reverse) {
            bs_dllist_push_front(&tilebox_ptr->tiles,
                                 wlmtk_dlnode_from_tile(tiles_ptr[i]));
        } else {
            bs_dllist_push_back(&tilebox_ptr->tiles,
                                wlmtk_dlnode_from_tile(tiles_ptr[i]));
        }
    }
    return _wlmdock_tilebox_build_grid(tilebox_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmdock_tilebox_set_edge(wlmdock_tilebox_t *tilebox_ptr, uint32_t edge)
{
    if (tilebox_ptr->edge == edge) return;
    tilebox_ptr->edge = edge;
    if (tilebox_ptr->grid) _wlmdock_tilebox_build_grid(tilebox_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmdock_tilebox_is_grid(wlmdock_tilebox_t *tilebox_ptr)
{
    return tilebox_ptr->grid;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Takes the grid apart: Lanes and empty tiles. The tiles stay listed. */
void _wlmdock_tilebox_clear_grid(wlmdock_tilebox_t *tilebox_ptr)
{
    for (size_t l = 0; l < tilebox_ptr->lanes; ++l) {
        wlmtk_box_t *lane_ptr = &tilebox_ptr->lanes_ptr[l];
        wlmtk_container_t *c_ptr = &lane_ptr->element_container;
        while (NULL != c_ptr->elements.head_ptr) {
            wlmtk_box_remove_element(
                lane_ptr, wlmtk_element_from_dlnode(c_ptr->elements.head_ptr));
        }
        wlmtk_box_remove_element(&tilebox_ptr->grid_box,
                                 wlmtk_box_element(lane_ptr));
        wlmtk_box_fini(lane_ptr);
    }
    if (NULL != tilebox_ptr->lanes_ptr) free(tilebox_ptr->lanes_ptr);
    tilebox_ptr->lanes_ptr = NULL;
    if (0 < tilebox_ptr->lanes) {
        wlmtk_box_remove_element(&tilebox_ptr->tile_box,
                                 wlmtk_box_element(&tilebox_ptr->grid_box));
        wlmtk_box_fini(&tilebox_ptr->grid_box);
    }
    tilebox_ptr->lanes = 0;
    for (size_t i = 0; i < tilebox_ptr->fillers; ++i) {
        wlmtk_tile_fini(&tilebox_ptr->fillers_ptr[i]);
    }
    if (NULL != tilebox_ptr->fillers_ptr) free(tilebox_ptr->fillers_ptr);
    tilebox_ptr->fillers_ptr = NULL;
    tilebox_ptr->fillers = 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Lays out the grid's tiles: A lane per distance from the edge, each with
 * a tile per slot from the origin to the last one (an empty one, if none
 * is there), so that all lanes line up.
 */
bool _wlmdock_tilebox_build_grid(wlmdock_tilebox_t *tilebox_ptr)
{
    _wlmdock_tilebox_clear_grid(tilebox_ptr);
    size_t n = tilebox_ptr->grid_n;
    int lanes = 0, slots = 0;
    for (size_t i = 0; i < n; ++i) {
        if (NULL == tilebox_ptr->grid_tiles_ptr[i]) continue;
        const wlmdock_tilebox_cell_t *c_ptr = &tilebox_ptr->grid_cells_ptr[i];
        lanes = BS_MAX(lanes, c_ptr->lane + 1);
        slots = BS_MAX(slots, c_ptr->slot - tilebox_ptr->origin + 1);
    }
    if (0 >= lanes || 0 >= slots) return true;
    // At most a large screen full: No runaway from a broken layout.
    if (64 < lanes || 256 < slots) return false;

    wlmtk_tile_t **at_ptr = logged_calloc(lanes * slots, sizeof(wlmtk_tile_t*));
    if (NULL == at_ptr) return false;
    size_t empty = lanes * slots;
    for (size_t i = 0; i < n; ++i) {
        wlmtk_tile_t *t_ptr = tilebox_ptr->grid_tiles_ptr[i];
        const wlmdock_tilebox_cell_t *c_ptr = &tilebox_ptr->grid_cells_ptr[i];
        int s = c_ptr->slot - tilebox_ptr->origin;
        if (NULL == t_ptr || 0 > c_ptr->lane || 0 > s) continue;
        wlmtk_tile_t **a_ptr = &at_ptr[c_ptr->lane * slots + s];
        if (NULL != *a_ptr) continue;
        *a_ptr = t_ptr;
        --empty;
    }

    // Lanes are separated by room of the margin's width, not drawn.
    wlmtk_box_orientation_t across = (WLMTK_BOX_VERTICAL ==
                                      tilebox_ptr->orientation) ?
        WLMTK_BOX_HORIZONTAL : WLMTK_BOX_VERTICAL;
    struct wlmtk_margin_style clear = { .width = tilebox_ptr->margin.width };
    tilebox_ptr->lanes_ptr = logged_calloc(lanes, sizeof(wlmtk_box_t));
    tilebox_ptr->fillers_ptr = 0 < empty ?
        logged_calloc(empty, sizeof(wlmtk_tile_t)) : NULL;
    bool rv = NULL != tilebox_ptr->lanes_ptr &&
        (0 == empty || NULL != tilebox_ptr->fillers_ptr) &&
        wlmtk_box_init(&tilebox_ptr->grid_box, across, &clear);
    if (!rv) {
        free(at_ptr);
        if (NULL != tilebox_ptr->lanes_ptr) free(tilebox_ptr->lanes_ptr);
        tilebox_ptr->lanes_ptr = NULL;
        if (NULL != tilebox_ptr->fillers_ptr) free(tilebox_ptr->fillers_ptr);
        tilebox_ptr->fillers_ptr = NULL;
        return false;
    }
    wlmtk_element_set_visible(wlmtk_box_element(&tilebox_ptr->grid_box), true);
    wlmtk_box_add_element_back(&tilebox_ptr->tile_box,
                               wlmtk_box_element(&tilebox_ptr->grid_box));
    // A freshly created buffer is all zero, ie. fully transparent.
    struct wlr_buffer *clear_ptr = bs_gfxbuf_create_wlr_buffer(
        tilebox_ptr->filler_style.size, tilebox_ptr->filler_style.size);

    bool far_edge = WLR_EDGE_RIGHT == tilebox_ptr->edge ||
        WLR_EDGE_BOTTOM == tilebox_ptr->edge;
    for (int l = 0; l < lanes; ++l) {
        wlmtk_box_t *lane_ptr = &tilebox_ptr->lanes_ptr[l];
        if (!wlmtk_box_init(lane_ptr, tilebox_ptr->orientation,
                            &tilebox_ptr->margin)) break;
        tilebox_ptr->lanes = l + 1;
        wlmtk_element_set_visible(wlmtk_box_element(lane_ptr), true);
        for (int s = 0; s < slots; ++s) {
            wlmtk_tile_t *t_ptr = at_ptr[l * slots + s];
            if (NULL == t_ptr && tilebox_ptr->fillers < empty) {
                t_ptr = &tilebox_ptr->fillers_ptr[tilebox_ptr->fillers];
                if (!wlmtk_tile_init(t_ptr, &tilebox_ptr->filler_style)) {
                    t_ptr = NULL;
                } else {
                    ++tilebox_ptr->fillers;
                    if (NULL != clear_ptr) {
                        wlmtk_tile_set_background_buffer(t_ptr, clear_ptr);
                    }
                    wlmtk_tile_element(t_ptr)->placeholder = true;
                    wlmtk_element_set_visible(wlmtk_tile_element(t_ptr), true);
                }
            }
            if (NULL == t_ptr) continue;
            // Slots from the anchor: At the far end, if reversed.
            if (tilebox_ptr->reverse) {
                wlmtk_box_add_element_front(lane_ptr, wlmtk_tile_element(t_ptr));
            } else {
                wlmtk_box_add_element_back(lane_ptr, wlmtk_tile_element(t_ptr));
            }
        }
        // Lanes from the edge: Lane 0 at the right (bottom) for that edge.
        if (far_edge) {
            wlmtk_box_add_element_front(&tilebox_ptr->grid_box,
                                        wlmtk_box_element(lane_ptr));
        } else {
            wlmtk_box_add_element_back(&tilebox_ptr->grid_box,
                                       wlmtk_box_element(lane_ptr));
        }
    }
    if (NULL != clear_ptr) wlr_buffer_drop(clear_ptr);
    free(at_ptr);
    return tilebox_ptr->lanes == (size_t)lanes;
}

/* == Unit tests =========================================================== */

static void test_grid(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmdock_tilebox_test_cases[] = {
    { true, "grid", test_grid },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmdock_tilebox_test_set = BS_TEST_SET(
    true, "tilebox", _wlmdock_tilebox_test_cases);

/* ------------------------------------------------------------------------- */
/** Tiles on a grid, for the Clip at the left and the right edge. */
void test_grid(bs_test_t *test_ptr)
{
    struct wlmtk_dock_style style = { .margin = { .width = 1 } };
    struct wlmtk_tile_style ts = { .size = 64 };
    wlmdock_tilebox_t *tb_ptr = wlmdock_tilebox_create(
        WLMTK_BOX_VERTICAL, false, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, tb_ptr);
    wlmtk_tile_t t[3];
    wlmtk_tile_t *tiles[3];
    for (int i = 0; i < 3; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, wlmtk_tile_init(&t[i], &ts));
        wlmtk_element_set_visible(wlmtk_tile_element(&t[i]), true);
        tiles[i] = &t[i];
    }
    // Below the Clip; beside the Clip; two below and two across.
    wlmdock_tilebox_cell_t cells[3] = { { 0, 1 }, { 1, 0 }, { 2, 2 } };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmdock_tilebox_set_grid(
                            tb_ptr, tiles, cells, 3, 0, WLR_EDGE_LEFT, &ts));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmdock_tilebox_is_grid(tb_ptr));
    wlmtk_element_layout(wlmdock_tilebox_element(tb_ptr));
    struct wlr_box b = wlmdock_tilebox_tile_box(tb_ptr, &t[0]);
    BS_TEST_VERIFY_EQ(test_ptr, 0, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 65, b.y);
    b = wlmdock_tilebox_tile_box(tb_ptr, &t[1]);
    BS_TEST_VERIFY_EQ(test_ptr, 65, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 0, b.y);
    b = wlmdock_tilebox_tile_box(tb_ptr, &t[2]);
    BS_TEST_VERIFY_EQ(test_ptr, 130, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 130, b.y);
    // All 3x3 cells are there: 6 empty ones.
    BS_TEST_VERIFY_EQ(test_ptr, 6u, tb_ptr->fillers);
    wlmtk_tile_t *listed[3];
    BS_TEST_VERIFY_EQ(test_ptr, 3u, wlmdock_tilebox_tiles(tb_ptr, listed, 3));
    BS_TEST_VERIFY_EQ(test_ptr, &t[0], listed[0]);
    BS_TEST_VERIFY_EQ(test_ptr, &t[2], listed[2]);

    // At the right edge: Lanes go left; lane 0 is the rightmost.
    wlmdock_tilebox_set_edge(tb_ptr, WLR_EDGE_RIGHT);
    wlmtk_element_layout(wlmdock_tilebox_element(tb_ptr));
    b = wlmdock_tilebox_tile_box(tb_ptr, &t[0]);
    BS_TEST_VERIFY_EQ(test_ptr, 130, b.x);
    b = wlmdock_tilebox_tile_box(tb_ptr, &t[2]);
    BS_TEST_VERIFY_EQ(test_ptr, 0, b.x);
    // Anchored at the bottom: Slot 0 is the lowest.
    wlmdock_tilebox_set_reverse(tb_ptr, true);
    wlmtk_element_layout(wlmdock_tilebox_element(tb_ptr));
    b = wlmdock_tilebox_tile_box(tb_ptr, &t[1]);
    BS_TEST_VERIFY_EQ(test_ptr, 130, b.y);
    b = wlmdock_tilebox_tile_box(tb_ptr, &t[2]);
    BS_TEST_VERIFY_EQ(test_ptr, 0, b.y);

    for (int i = 0; i < 3; ++i) wlmdock_tilebox_remove_tile(tb_ptr, &t[i]);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmdock_tilebox_set_grid(
                            tb_ptr, NULL, NULL, 0, 0, WLR_EDGE_LEFT, &ts));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmdock_tilebox_is_grid(tb_ptr));
    for (int i = 0; i < 3; ++i) wlmtk_tile_fini(&t[i]);
    wlmdock_tilebox_destroy(tb_ptr);
}

/* == End of tilebox.c ===================================================== */
