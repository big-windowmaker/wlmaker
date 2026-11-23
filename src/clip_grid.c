/* ========================================================================= */
/**
 * @file clip_grid.c
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
 *
 * The Clip's icons as Window Maker places them: On a grid around the Clip,
 * each in its own cell, next to another one (wDockSnapIcon, case WM_CLIP),
 * also beside each other and with empty cells between them.
 */

#include "clip_grid.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

static bool _wlmaker_clip_grid_has_neighbour(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own);
static bool _wlmaker_clip_grid_taken(
    const wlmaker_clip_cell_t *cells_ptr,
    const bool *placed_ptr,
    size_t n,
    const wlmaker_clip_cell_t *cell_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_cell_parse(const char *str_ptr, wlmaker_clip_cell_t *cell_ptr)
{
    if (NULL == str_ptr) return false;
    char *end_ptr;
    long lane = strtol(str_ptr, &end_ptr, 10);
    if (end_ptr == str_ptr || ',' != *end_ptr) return false;
    const char *s_ptr = end_ptr + 1;
    long slot = strtol(s_ptr, &end_ptr, 10);
    if (end_ptr == s_ptr || '\0' != *end_ptr) return false;
    // At most a large screen of tiles: No runaway from a broken file.
    if (0 > lane || 0 > slot || 1024 <= lane || 1024 <= slot) return false;
    cell_ptr->lane = lane;
    cell_ptr->slot = slot;
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_cell_format(
    const wlmaker_clip_cell_t *cell_ptr,
    char *buf_ptr,
    size_t size)
{
    snprintf(buf_ptr, size, "%d,%d", cell_ptr->lane, cell_ptr->slot);
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_grid_bounds(
    wlmaker_dock_place_t place,
    int x,
    int y,
    int size,
    int pitch,
    const struct wlr_box *output_ptr,
    int *lanes_ptr,
    int *slots_ptr)
{
    *lanes_ptr = 0;
    *slots_ptr = 0;
    if (0 >= pitch || wlr_box_empty(output_ptr)) return;
    // Tiles from the Clip on, towards the far side: Fully on the output.
    int right = (output_ptr->x + output_ptr->width - size - x) / pitch + 1;
    int left = (x - output_ptr->x) / pitch + 1;
    int down = (output_ptr->y + output_ptr->height - size - y) / pitch + 1;
    int up = (y - output_ptr->y) / pitch + 1;
    if (x < output_ptr->x || x + size > output_ptr->x + output_ptr->width) {
        right = left = 0;
    }
    if (y < output_ptr->y || y + size > output_ptr->y + output_ptr->height) {
        down = up = 0;
    }
    bool vertical = wlmaker_dock_place_vertical(place);
    int across = vertical ?
        (WLR_EDGE_RIGHT == place.edge ? left : right) :
        (WLR_EDGE_BOTTOM == place.edge ? up : down);
    int along = vertical ?
        (WLR_EDGE_BOTTOM == place.anchor ? up : down) :
        (WLR_EDGE_RIGHT == place.anchor ? left : right);
    *lanes_ptr = BS_MAX(0, across);
    *slots_ptr = BS_MAX(0, along);
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_cell_position(
    wlmaker_dock_place_t place,
    int x,
    int y,
    int pitch,
    const wlmaker_clip_cell_t *cell_ptr,
    int *cell_x_ptr,
    int *cell_y_ptr)
{
    int across = cell_ptr->lane * pitch;
    int along = cell_ptr->slot * pitch;
    if (wlmaker_dock_place_vertical(place)) {
        *cell_x_ptr = (WLR_EDGE_RIGHT == place.edge) ? x - across : x + across;
        *cell_y_ptr = (WLR_EDGE_BOTTOM == place.anchor) ? y - along : y + along;
    } else {
        *cell_y_ptr = (WLR_EDGE_BOTTOM == place.edge) ? y - across : y + across;
        *cell_x_ptr = (WLR_EDGE_RIGHT == place.anchor) ? x - along : x + along;
    }
}

/* ------------------------------------------------------------------------- */
wlmaker_clip_cell_t wlmaker_clip_cell_at(
    wlmaker_dock_place_t place,
    int x,
    int y,
    int size,
    int pitch,
    int tile_x,
    int tile_y)
{
    wlmaker_clip_cell_t cell = { .lane = -1, .slot = -1 };
    if (0 >= pitch) return cell;
    // As wDockSnapIcon: The C division truncates towards zero.
    int offset = size / 2;
    int ex_x = (tile_x < x) ?
        (tile_x - offset - x) / pitch : (tile_x + offset - x) / pitch;
    int ex_y = (tile_y < y) ?
        (tile_y - offset - y) / pitch : (tile_y + offset - y) / pitch;
    if (wlmaker_dock_place_vertical(place)) {
        cell.lane = (WLR_EDGE_RIGHT == place.edge) ? -ex_x : ex_x;
        cell.slot = (WLR_EDGE_BOTTOM == place.anchor) ? -ex_y : ex_y;
    } else {
        cell.lane = (WLR_EDGE_BOTTOM == place.edge) ? -ex_y : ex_y;
        cell.slot = (WLR_EDGE_RIGHT == place.anchor) ? -ex_x : ex_x;
    }
    return cell;
}

/* ------------------------------------------------------------------------- */
wlmaker_clip_cell_t wlmaker_clip_cell_towards(
    wlmaker_dock_place_t place,
    const wlmaker_clip_cell_t *cell_ptr,
    uint32_t direction)
{
    int dx = 0, dy = 0;
    if (WLR_EDGE_LEFT == direction) dx = -1;
    if (WLR_EDGE_RIGHT == direction) dx = 1;
    if (WLR_EDGE_TOP == direction) dy = -1;
    if (WLR_EDGE_BOTTOM == direction) dy = 1;
    wlmaker_clip_cell_t c = *cell_ptr;
    if (wlmaker_dock_place_vertical(place)) {
        c.lane += (WLR_EDGE_RIGHT == place.edge) ? -dx : dx;
        c.slot += (WLR_EDGE_BOTTOM == place.anchor) ? -dy : dy;
    } else {
        c.lane += (WLR_EDGE_BOTTOM == place.edge) ? -dy : dy;
        c.slot += (WLR_EDGE_RIGHT == place.anchor) ? -dx : dx;
    }
    return c;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_grid_contains(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr)
{
    return 0 <= cell_ptr->lane && cell_ptr->lane < grid_ptr->lanes &&
        0 <= cell_ptr->slot && cell_ptr->slot < grid_ptr->slots;
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_clip_grid_icon_at(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t except)
{
    for (size_t i = 0; i < grid_ptr->n; ++i) {
        if (i == except) continue;
        if (grid_ptr->cells_ptr[i].lane == cell_ptr->lane &&
            grid_ptr->cells_ptr[i].slot == cell_ptr->slot) return i;
    }
    return SIZE_MAX;
}

/* ------------------------------------------------------------------------- */
wlmaker_clip_grid_snap_t wlmaker_clip_grid_snap(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own)
{
    // The Clip's own tile, or another icon's.
    if ((0 == cell_ptr->lane && 0 == cell_ptr->slot) ||
        SIZE_MAX != wlmaker_clip_grid_icon_at(grid_ptr, cell_ptr, own)) {
        return WLMAKER_CLIP_GRID_OCCUPIED;
    }
    // Back on its own cell: It stays, also without a neighbour now (Window
    // Maker would detach it then, eg. after its neighbour was removed).
    if (own < grid_ptr->n &&
        grid_ptr->cells_ptr[own].lane == cell_ptr->lane &&
        grid_ptr->cells_ptr[own].slot == cell_ptr->slot) {
        return WLMAKER_CLIP_GRID_ATTACH;
    }
    // Window Maker: !onScreen() and no neighbour return False.
    if (!wlmaker_clip_grid_contains(grid_ptr, cell_ptr) ||
        !_wlmaker_clip_grid_has_neighbour(grid_ptr, cell_ptr, own)) {
        return WLMAKER_CLIP_GRID_OUTSIDE;
    }
    return WLMAKER_CLIP_GRID_ATTACH;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_grid_column_end(
    const wlmaker_clip_grid_t *grid_ptr,
    wlmaker_clip_cell_t *cell_ptr,
    size_t own)
{
    if (0 != cell_ptr->lane || 0 >= cell_ptr->slot ||
        !wlmaker_clip_grid_contains(grid_ptr, cell_ptr)) return false;
    // The last icon of the column; the Clip's own tile is slot 0.
    int last = 0;
    for (size_t i = 0; i < grid_ptr->n; ++i) {
        if (i == own || 0 != grid_ptr->cells_ptr[i].lane) continue;
        last = BS_MAX(last, grid_ptr->cells_ptr[i].slot);
    }
    if (cell_ptr->slot <= last) return false;
    wlmaker_clip_cell_t c = { .lane = 0, .slot = last + 1 };
    if (!wlmaker_clip_grid_contains(grid_ptr, &c)) return false;
    *cell_ptr = c;
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_clip_grid_free_cell(
    const wlmaker_clip_grid_t *grid_ptr,
    wlmaker_clip_cell_t *cell_ptr)
{
    for (int lane = 0; lane < grid_ptr->lanes; ++lane) {
        for (int slot = 0; slot < grid_ptr->slots; ++slot) {
            wlmaker_clip_cell_t c = { .lane = lane, .slot = slot };
            if (0 == lane && 0 == slot) continue;
            if (SIZE_MAX != wlmaker_clip_grid_icon_at(grid_ptr, &c, SIZE_MAX)) {
                continue;
            }
            // Next to another one: The column has no holes left by this.
            if (!_wlmaker_clip_grid_has_neighbour(grid_ptr, &c, SIZE_MAX)) {
                continue;
            }
            *cell_ptr = c;
            return true;
        }
    }
    // Full: Past the end of the column. Kept, not shown on the output.
    cell_ptr->lane = 0;
    cell_ptr->slot = BS_MAX(1, grid_ptr->slots);
    while (SIZE_MAX != wlmaker_clip_grid_icon_at(grid_ptr, cell_ptr, SIZE_MAX)) {
        ++cell_ptr->slot;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
void wlmaker_clip_grid_resolve(
    wlmaker_clip_grid_t *grid_ptr,
    const bool *stored_ptr,
    const size_t *order_ptr,
    bool *derived_ptr)
{
    size_t n = grid_ptr->n;
    if (0 == n) return;
    bool *placed_ptr = logged_calloc(n, sizeof(bool));
    wlmaker_clip_cell_t *stored_cells_ptr = logged_calloc(
        n, sizeof(wlmaker_clip_cell_t));
    if (NULL == placed_ptr || NULL == stored_cells_ptr) {
        // Out of memory: The column's order, as before the grid.
        for (size_t k = 0; k < n; ++k) {
            grid_ptr->cells_ptr[order_ptr[k]] = (wlmaker_clip_cell_t){
                .lane = 0, .slot = 1 + (int)k };
            if (NULL != derived_ptr) derived_ptr[order_ptr[k]] = true;
        }
        if (NULL != placed_ptr) free(placed_ptr);
        if (NULL != stored_cells_ptr) free(stored_cells_ptr);
        return;
    }
    memcpy(stored_cells_ptr, grid_ptr->cells_ptr,
           n * sizeof(wlmaker_clip_cell_t));

    // Stored cells, first come first served.
    for (size_t k = 0; k < n; ++k) {
        size_t i = order_ptr[k];
        const wlmaker_clip_cell_t *c_ptr = &stored_cells_ptr[i];
        if (!stored_ptr[i] || (0 == c_ptr->lane && 0 == c_ptr->slot) ||
            !wlmaker_clip_grid_contains(grid_ptr, c_ptr) ||
            _wlmaker_clip_grid_taken(grid_ptr->cells_ptr, placed_ptr, n, c_ptr)) {
            continue;
        }
        grid_ptr->cells_ptr[i] = *c_ptr;
        placed_ptr[i] = true;
    }

    // The others: The first free slot of the column, in the column's order.
    // Searched among the placed ones only.
    for (size_t k = 0; k < n; ++k) {
        size_t i = order_ptr[k];
        if (NULL != derived_ptr) derived_ptr[i] = !placed_ptr[i];
        if (placed_ptr[i]) continue;
        wlmaker_clip_cell_t c = { .lane = 0, .slot = 1 };
        bool found = false;
        for (int lane = 0; !found && lane < BS_MAX(1, grid_ptr->lanes);
             ++lane) {
            for (int slot = (0 == lane) ? 1 : 0;
                 !found && slot < grid_ptr->slots; ++slot) {
                c = (wlmaker_clip_cell_t){ .lane = lane, .slot = slot };
                found = !_wlmaker_clip_grid_taken(
                    grid_ptr->cells_ptr, placed_ptr, n, &c);
            }
        }
        if (!found) {
            // Off the output: Past the column's end, not lost.
            c = (wlmaker_clip_cell_t){ .lane = 0,
                                       .slot = BS_MAX(1, grid_ptr->slots) };
            while (_wlmaker_clip_grid_taken(
                       grid_ptr->cells_ptr, placed_ptr, n, &c)) ++c.slot;
        }
        grid_ptr->cells_ptr[i] = c;
        placed_ptr[i] = true;
    }
    free(stored_cells_ptr);
    free(placed_ptr);
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_grid_max_lane(const wlmaker_clip_grid_t *grid_ptr)
{
    int max = -1;
    for (size_t i = 0; i < grid_ptr->n; ++i) {
        max = BS_MAX(max, grid_ptr->cells_ptr[i].lane);
    }
    return max;
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_grid_max_slot(const wlmaker_clip_grid_t *grid_ptr, int lane)
{
    int max = 0;
    for (size_t i = 0; i < grid_ptr->n; ++i) {
        if (grid_ptr->cells_ptr[i].lane != lane) continue;
        max = BS_MAX(max, grid_ptr->cells_ptr[i].slot);
    }
    return max;
}

/* ------------------------------------------------------------------------- */
int wlmaker_clip_grid_min_slot(const wlmaker_clip_grid_t *grid_ptr, int lane)
{
    int min = INT_MAX;
    for (size_t i = 0; i < grid_ptr->n; ++i) {
        if (grid_ptr->cells_ptr[i].lane != lane ||
            0 == grid_ptr->cells_ptr[i].slot) continue;
        min = BS_MIN(min, grid_ptr->cells_ptr[i].slot);
    }
    return min;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * @return Whether the Clip or an icon other than `own` is within
 *     @ref WLMAKER_CLIP_ATTACH_VICINITY of `cell_ptr`.
 */
bool _wlmaker_clip_grid_has_neighbour(
    const wlmaker_clip_grid_t *grid_ptr,
    const wlmaker_clip_cell_t *cell_ptr,
    size_t own)
{
    // The Clip, at 0, 0: Icon 0 of Window Maker's icon_array.
    if (abs(cell_ptr->lane) <= WLMAKER_CLIP_ATTACH_VICINITY &&
        abs(cell_ptr->slot) <= WLMAKER_CLIP_ATTACH_VICINITY) return true;
    for (size_t i = 0; i < grid_ptr->n; ++i) {
        if (i == own) continue;
        const wlmaker_clip_cell_t *c_ptr = &grid_ptr->cells_ptr[i];
        if (abs(c_ptr->lane - cell_ptr->lane) <= WLMAKER_CLIP_ATTACH_VICINITY &&
            abs(c_ptr->slot - cell_ptr->slot) <= WLMAKER_CLIP_ATTACH_VICINITY) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/** @return Whether one of the placed cells is `cell_ptr`, or it's the Clip. */
bool _wlmaker_clip_grid_taken(
    const wlmaker_clip_cell_t *cells_ptr,
    const bool *placed_ptr,
    size_t n,
    const wlmaker_clip_cell_t *cell_ptr)
{
    if (0 == cell_ptr->lane && 0 == cell_ptr->slot) return true;
    for (size_t i = 0; i < n; ++i) {
        if (placed_ptr[i] &&
            cells_ptr[i].lane == cell_ptr->lane &&
            cells_ptr[i].slot == cell_ptr->slot) return true;
    }
    return false;
}

/* == Unit tests =========================================================== */

static void test_cell_parse(bs_test_t *test_ptr);
static void test_geometry(bs_test_t *test_ptr);
static void test_snap(bs_test_t *test_ptr);
static void test_resolve(bs_test_t *test_ptr);
static void test_free_cell(bs_test_t *test_ptr);
static void test_column_end(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_clip_grid_test_cases[] = {
    { true, "cell_parse", test_cell_parse },
    { true, "geometry", test_geometry },
    { true, "snap", test_snap },
    { true, "resolve", test_resolve },
    { true, "free_cell", test_free_cell },
    { true, "column_end", test_column_end },
    BS_TEST_CASE_SENTINEL()
};

/* ------------------------------------------------------------------------- */
/** A drop down the Clip's column, past its icons, goes to its end. */
void test_column_end(bs_test_t *test_ptr)
{
    // The Clip, launchers at (0,1) and (0,2), a dock-app beside at (1,0).
    wlmaker_clip_cell_t cells[] = { { 0, 1 }, { 0, 2 }, { 1, 0 } };
    wlmaker_clip_grid_t g = { .n = 3, .cells_ptr = cells,
                              .lanes = 4, .slots = 10 };
    wlmaker_clip_cell_t c = { 0, 8 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_clip_grid_column_end(&g, &c, SIZE_MAX));
    BS_TEST_VERIFY_EQ(test_ptr, 0, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 3, c.slot);
    // The icon moved: Its own cell does not count.
    c = (wlmaker_clip_cell_t){ 0, 8 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_clip_grid_column_end(&g, &c, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.slot);
    // Not in the column, or off the output: Not taken.
    c = (wlmaker_clip_cell_t){ 2, 8 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_clip_grid_column_end(&g, &c, SIZE_MAX));
    c = (wlmaker_clip_cell_t){ 0, 12 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_clip_grid_column_end(&g, &c, SIZE_MAX));
    // Between icons of the column: The grid's own rules.
    c = (wlmaker_clip_cell_t){ 0, 1 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_clip_grid_column_end(&g, &c, SIZE_MAX));
}

const bs_test_set_t wlmaker_clip_grid_test_set = BS_TEST_SET(
    true, "clip_grid", _wlmaker_clip_grid_test_cases);

/* ------------------------------------------------------------------------- */
/** Parsing and formatting the stored cells; broken ones are refused. */
void test_cell_parse(bs_test_t *test_ptr)
{
    wlmaker_clip_cell_t c = { .lane = 7, .slot = 7 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_clip_cell_parse("2,3", &c));
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 3, c.slot);
    char buf[32];
    wlmaker_clip_cell_format(&c, buf, sizeof(buf));
    BS_TEST_VERIFY_STREQ(test_ptr, "2,3", buf);

    static const char *bad[] = {
        NULL, "", "2", "2,", ",3", "2;3", "-1,0", "0,-1", "2,3x", "a,b",
        "99999,0" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        c = (wlmaker_clip_cell_t){ .lane = 7, .slot = 7 };
        BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_clip_cell_parse(bad[i], &c));
        BS_TEST_VERIFY_EQ(test_ptr, 7, c.lane);
    }
}

/* ------------------------------------------------------------------------- */
/** Cells to positions and back, for the Clip in each corner. */
void test_geometry(bs_test_t *test_ptr)
{
    struct wlr_box o = { .x = 100, .y = 0, .width = 1280, .height = 900 };
    int lanes, slots, x, y;

    // Top left: Lanes go right, slots down.
    wlmaker_dock_place_t tl = { WLR_EDGE_LEFT, WLR_EDGE_TOP };
    wlmaker_clip_grid_bounds(tl, 100, 0, 64, 65, &o, &lanes, &slots);
    BS_TEST_VERIFY_EQ(test_ptr, 19, lanes);  // (1280 - 64) / 65 + 1.
    BS_TEST_VERIFY_EQ(test_ptr, 13, slots);  // (900 - 64) / 65 + 1.
    wlmaker_clip_cell_t c = { .lane = 2, .slot = 3 };
    wlmaker_clip_cell_position(tl, 100, 0, 65, &c, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, 230, x);
    BS_TEST_VERIFY_EQ(test_ptr, 195, y);
    c = wlmaker_clip_cell_at(tl, 100, 0, 64, 65, 230, 195);
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 3, c.slot);
    // Nearest: Less than half a tile off still counts for that cell.
    c = wlmaker_clip_cell_at(tl, 100, 0, 64, 65, 230 + 30, 195 - 31);
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 3, c.slot);
    c = wlmaker_clip_cell_at(tl, 100, 0, 64, 65, 230 + 34, 195);
    BS_TEST_VERIFY_EQ(test_ptr, 3, c.lane);
    // Left of the Clip: Off the grid.
    c = wlmaker_clip_cell_at(tl, 100, 0, 64, 65, 100 - 40, 0);
    BS_TEST_VERIFY_EQ(test_ptr, -1, c.lane);

    // Bottom right: Lanes go left, slots up.
    wlmaker_dock_place_t br = { WLR_EDGE_RIGHT, WLR_EDGE_BOTTOM };
    int cx = 100 + 1280 - 64, cy = 900 - 64;
    wlmaker_clip_grid_bounds(br, cx, cy, 64, 65, &o, &lanes, &slots);
    BS_TEST_VERIFY_EQ(test_ptr, 19, lanes);
    BS_TEST_VERIFY_EQ(test_ptr, 13, slots);
    c = (wlmaker_clip_cell_t){ .lane = 1, .slot = 2 };
    wlmaker_clip_cell_position(br, cx, cy, 65, &c, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, cx - 65, x);
    BS_TEST_VERIFY_EQ(test_ptr, cy - 130, y);
    c = wlmaker_clip_cell_at(br, cx, cy, 64, 65, cx - 65, cy - 130);
    BS_TEST_VERIFY_EQ(test_ptr, 1, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.slot);

    // At the top edge, anchored right: Lanes go down, slots left.
    wlmaker_dock_place_t tr = { WLR_EDGE_TOP, WLR_EDGE_RIGHT };
    c = (wlmaker_clip_cell_t){ .lane = 1, .slot = 2 };
    wlmaker_clip_cell_position(tr, cx, 0, 65, &c, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, cx - 130, x);
    BS_TEST_VERIFY_EQ(test_ptr, 65, y);
    c = wlmaker_clip_cell_at(tr, cx, 0, 64, 65, cx - 130, 65);
    BS_TEST_VERIFY_EQ(test_ptr, 1, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.slot);

    // Neighbours on the screen: Right of a cell at the right edge is a
    // lane less; above, at the bottom anchor, a slot more.
    c = (wlmaker_clip_cell_t){ .lane = 1, .slot = 2 };
    wlmaker_clip_cell_t n = wlmaker_clip_cell_towards(br, &c, WLR_EDGE_RIGHT);
    BS_TEST_VERIFY_EQ(test_ptr, 0, n.lane);
    n = wlmaker_clip_cell_towards(br, &c, WLR_EDGE_TOP);
    BS_TEST_VERIFY_EQ(test_ptr, 3, n.slot);
    n = wlmaker_clip_cell_towards(tl, &c, WLR_EDGE_RIGHT);
    BS_TEST_VERIFY_EQ(test_ptr, 2, n.lane);
    n = wlmaker_clip_cell_towards(tr, &c, WLR_EDGE_BOTTOM);
    BS_TEST_VERIFY_EQ(test_ptr, 2, n.lane);
}

/* ------------------------------------------------------------------------- */
/** Window Maker's rules for the Clip: Free, on the output, next to one. */
void test_snap(bs_test_t *test_ptr)
{
    // The Clip, a launcher below it, a dock-app beside it.
    wlmaker_clip_cell_t cells[] = { { 0, 1 }, { 1, 0 } };
    wlmaker_clip_grid_t g = { .n = 2, .cells_ptr = cells,
                              .lanes = 4, .slots = 5 };
    wlmaker_clip_cell_t c;

    // Beside the dock-app, in the same row: Allowed, as on X11.
    c = (wlmaker_clip_cell_t){ 2, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Below the dock-app, beside the launcher.
    c = (wlmaker_clip_cell_t){ 1, 1 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Diagonally next to the dock-app.
    c = (wlmaker_clip_cell_t){ 2, 1 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Below the launcher: The column goes on.
    c = (wlmaker_clip_cell_t){ 0, 2 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Two cells away from all: Not attached (detaches).
    c = (wlmaker_clip_cell_t){ 3, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    c = (wlmaker_clip_cell_t){ 0, 3 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Taken: The launcher's cell, and the Clip's.
    c = (wlmaker_clip_cell_t){ 0, 1 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OCCUPIED,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    c = (wlmaker_clip_cell_t){ 0, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OCCUPIED,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Off the output, though next to one.
    cells[1] = (wlmaker_clip_cell_t){ 3, 0 };
    c = (wlmaker_clip_cell_t){ 4, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    c = (wlmaker_clip_cell_t){ -1, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));

    // The dragged icon is not its own neighbour, but may go back.
    c = (wlmaker_clip_cell_t){ 3, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, 1));
    c = (wlmaker_clip_cell_t){ 3, 1 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, 1));
    cells[1] = (wlmaker_clip_cell_t){ 1, 0 };
    c = (wlmaker_clip_cell_t){ 1, 0 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, 1));
    // Dragging the dock-app away from its neighbour: It can't keep it.
    cells[0] = (wlmaker_clip_cell_t){ 2, 2 };
    cells[1] = (wlmaker_clip_cell_t){ 2, 3 };
    c = (wlmaker_clip_cell_t){ 2, 4 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, 1));
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, SIZE_MAX));
    // Alone, far from the others: Back on its own cell, it stays.
    cells[1] = (wlmaker_clip_cell_t){ 3, 4 };
    c = (wlmaker_clip_cell_t){ 3, 4 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, 1));
    // Next to the other one, diagonally: Attaches; away from all: Not.
    c = (wlmaker_clip_cell_t){ 3, 3 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_ATTACH,
                      wlmaker_clip_grid_snap(&g, &c, 1));
    c = (wlmaker_clip_cell_t){ 0, 4 };
    BS_TEST_VERIFY_EQ(test_ptr, WLMAKER_CLIP_GRID_OUTSIDE,
                      wlmaker_clip_grid_snap(&g, &c, 1));
}

/* ------------------------------------------------------------------------- */
/** Stored cells are kept; old states get the column; conflicts resolved. */
void test_resolve(bs_test_t *test_ptr)
{
    // An old state: No cells. The column, in its order (launcher 0,
    // dock-app 2, launcher 1).
    wlmaker_clip_cell_t cells[4] = {};
    bool stored[4] = {};
    bool derived[4] = {};
    size_t order[4] = { 0, 2, 1 };
    wlmaker_clip_grid_t g = { .n = 3, .cells_ptr = cells,
                              .lanes = 4, .slots = 6 };
    wlmaker_clip_grid_resolve(&g, stored, order, derived);
    BS_TEST_VERIFY_EQ(test_ptr, 0, cells[0].lane);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[0].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 2, cells[2].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 3, cells[1].slot);
    BS_TEST_VERIFY_TRUE(test_ptr, derived[0] && derived[1] && derived[2]);

    // Stored cells, with a hole and beside each other. One without: It gets
    // the first free slot of the column.
    cells[0] = (wlmaker_clip_cell_t){ 0, 3 }; stored[0] = true;
    cells[1] = (wlmaker_clip_cell_t){ 1, 0 }; stored[1] = true;
    cells[2] = (wlmaker_clip_cell_t){ 2, 0 }; stored[2] = true;
    stored[3] = false;
    size_t order4[4] = { 0, 1, 2, 3 };
    g.n = 4;
    wlmaker_clip_grid_resolve(&g, stored, order4, derived);
    BS_TEST_VERIFY_EQ(test_ptr, 3, cells[0].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[1].lane);
    BS_TEST_VERIFY_EQ(test_ptr, 2, cells[2].lane);
    BS_TEST_VERIFY_EQ(test_ptr, 0, cells[3].lane);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[3].slot);
    BS_TEST_VERIFY_FALSE(test_ptr, derived[0] || derived[1] || derived[2]);
    BS_TEST_VERIFY_TRUE(test_ptr, derived[3]);

    // Two on the same cell (a hand-edited file), one on the Clip, one off
    // the output (a smaller screen now): The first keeps it, the others get
    // free slots. Nothing is lost.
    cells[0] = (wlmaker_clip_cell_t){ 1, 1 }; stored[0] = true;
    cells[1] = (wlmaker_clip_cell_t){ 1, 1 }; stored[1] = true;
    cells[2] = (wlmaker_clip_cell_t){ 0, 0 }; stored[2] = true;
    cells[3] = (wlmaker_clip_cell_t){ 9, 1 }; stored[3] = true;
    wlmaker_clip_grid_resolve(&g, stored, order4, derived);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[0].lane);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[0].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 0, cells[1].lane);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[1].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 2, cells[2].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 3, cells[3].slot);
    BS_TEST_VERIFY_TRUE(test_ptr, derived[1] && derived[2] && derived[3]);

    // More icons than fit: Past the column's end, each in its own cell.
    wlmaker_clip_grid_t tiny = { .n = 3, .cells_ptr = cells,
                                 .lanes = 1, .slots = 2 };
    memset(stored, 0, sizeof(stored));
    wlmaker_clip_grid_resolve(&tiny, stored, order4, derived);
    BS_TEST_VERIFY_EQ(test_ptr, 1, cells[0].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 2, cells[1].slot);
    BS_TEST_VERIFY_EQ(test_ptr, 3, cells[2].slot);
}

/* ------------------------------------------------------------------------- */
/** A new icon goes to the first free slot next to the others. */
void test_free_cell(bs_test_t *test_ptr)
{
    wlmaker_clip_cell_t cells[] = { { 0, 1 }, { 0, 3 }, { 1, 0 } };
    wlmaker_clip_grid_t g = { .n = 3, .cells_ptr = cells,
                              .lanes = 3, .slots = 4 };
    wlmaker_clip_cell_t c;
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_clip_grid_free_cell(&g, &c));
    BS_TEST_VERIFY_EQ(test_ptr, 0, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 2, c.slot);

    // Column full: Then the next lane.
    wlmaker_clip_cell_t col[] = { { 0, 1 }, { 0, 2 }, { 0, 3 } };
    g.cells_ptr = col;
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_clip_grid_free_cell(&g, &c));
    BS_TEST_VERIFY_EQ(test_ptr, 1, c.lane);
    BS_TEST_VERIFY_EQ(test_ptr, 0, c.slot);

    // All full: Past the end.
    wlmaker_clip_grid_t one = { .n = 3, .cells_ptr = col,
                                .lanes = 1, .slots = 4 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_clip_grid_free_cell(&one, &c));
    BS_TEST_VERIFY_EQ(test_ptr, 4, c.slot);
}

/* == End of clip_grid.c =================================================== */
