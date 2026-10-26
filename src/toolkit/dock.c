/* ========================================================================= */
/**
 * @file dock.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2024 Google LLC
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

#include "dock.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "box.h"
#include "container.h"

/* == Declarations ========================================================= */

/** State of the toolkit dock. */
struct _wlmtk_dock_t {
    /** Parent class: The panel. */
    wlmtk_panel_t             super_panel;
    /** Original virtual method table of the panel's @ref wlmtk_element_t. */
    wlmtk_element_vmt_t       old_element_vmt;

    /** Positioning information for the panel. */
    wlmtk_panel_positioning_t panel_positioning;

    /** Copy of the positioning information this dock was called from. */
    wlmtk_dock_positioning_t  dock_positioning;
    /** Styling info of the dock. */
    struct wlmtk_dock_style   style;

    /** Principal element of the dock is a box, holding tiles. */
    wlmtk_box_t               tile_box;
    /** Extra holder for tiles, for direct access. */
    bs_dllist_t               tiles;
};

static uint32_t _wlmtk_dock_panel_request_size(
    wlmtk_panel_t *panel_ptr,
    int width,
    int height);
static void _wlmtk_dock_element_layout(wlmtk_element_t *element_ptr);

static wlmtk_box_orientation_t _wlmtk_dock_orientation(wlmtk_dock_t *dock_ptr);
static bool _wlmtk_dock_positioning(
    wlmtk_dock_t *dock_ptr,
    wlmtk_panel_positioning_t *panel_positioning_ptr);
static bool _wlmtk_dock_tile_set_style(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmtk_dock_tile_destroy(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);

/* == Data ================================================================= */

const bspl_desc_t wlmtk_dock_style_desc[] = {
    BSPL_DESC_DICT(
        "Margin", true, struct wlmtk_dock_style, margin, margin,
        wlmtk_style_margin_desc),
    BSPL_DESC_SENTINEL()
};

/** Virtual method table of the panel. */
static const wlmtk_panel_vmt_t _wlmtk_dock_panel_vmt = {
    .request_size = _wlmtk_dock_panel_request_size
};

/** Virtual method table of the panel's element. */
static const wlmtk_element_vmt_t _wlmtk_dock_element_vmt = {
    .layout = _wlmtk_dock_element_layout
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmtk_dock_t *wlmtk_dock_create(
    const wlmtk_dock_positioning_t *dock_positioning_ptr,
    const struct wlmtk_dock_style *style_ptr)
{
    wlmtk_dock_t *dock_ptr = logged_calloc(1, sizeof(*dock_ptr));
    if (NULL == dock_ptr) return NULL;
    dock_ptr->dock_positioning = *dock_positioning_ptr;
    dock_ptr->style = *style_ptr;

    if (!wlmtk_box_init(
            &dock_ptr->tile_box,
            _wlmtk_dock_orientation(dock_ptr),
            &dock_ptr->style.margin)) {
        wlmtk_dock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(wlmtk_box_element(&dock_ptr->tile_box), true);

    if (!_wlmtk_dock_positioning(
            dock_ptr,
            &dock_ptr->panel_positioning)) {
        wlmtk_dock_destroy(dock_ptr);
        return NULL;
    }

    if (!wlmtk_panel_init(
            &dock_ptr->super_panel,
            &dock_ptr->panel_positioning)) {
        bs_log(BS_ERROR, "Failed wlmtk_panel_init.");
        wlmtk_dock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_panel_extend(&dock_ptr->super_panel, &_wlmtk_dock_panel_vmt);
    dock_ptr->old_element_vmt = wlmtk_element_extend(
        wlmtk_dock_element(dock_ptr), &_wlmtk_dock_element_vmt);

    wlmtk_container_add_element(
        &dock_ptr->super_panel.super_container,
        wlmtk_box_element(&dock_ptr->tile_box));

    return dock_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_destroy(wlmtk_dock_t *dock_ptr)
{
    bs_dllist_for_each(&dock_ptr->tiles, _wlmtk_dock_tile_destroy, dock_ptr);

    if (wlmtk_box_element(&dock_ptr->tile_box)->parent_container_ptr) {
        wlmtk_container_remove_element(
            &dock_ptr->super_panel.super_container,
            wlmtk_box_element(&dock_ptr->tile_box));
        wlmtk_box_fini(&dock_ptr->tile_box);
    }

    wlmtk_panel_fini(&dock_ptr->super_panel);
    free(dock_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_dock_set_style(
    wlmtk_dock_t *dock_ptr,
    const struct wlmtk_dock_style *style_ptr,
    const struct wlmtk_tile_style *tile_style_ptr)
{
    wlmtk_box_set_style(&dock_ptr->tile_box, &style_ptr->margin);
    bool rv = bs_dllist_all(
        &dock_ptr->tiles,
        _wlmtk_dock_tile_set_style,
        (void*)tile_style_ptr);

    wlmtk_element_layout(wlmtk_box_element(&dock_ptr->tile_box));
    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
    return rv;
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_add_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr)
{
    BS_ASSERT(NULL == wlmtk_tile_element(tile_ptr)->parent_container_ptr);
    if (WLR_EDGE_TOP == dock_ptr->dock_positioning.anchor ||
        WLR_EDGE_LEFT == dock_ptr->dock_positioning.anchor) {
        wlmtk_box_add_element_back(
            &dock_ptr->tile_box,
            wlmtk_tile_element(tile_ptr));
        bs_dllist_push_back(
            &dock_ptr->tiles,
            wlmtk_dlnode_from_tile(tile_ptr));
    } else {
        wlmtk_box_add_element_front(
            &dock_ptr->tile_box,
            wlmtk_tile_element(tile_ptr));
        bs_dllist_push_front(
            &dock_ptr->tiles,
            wlmtk_dlnode_from_tile(tile_ptr));
    }

    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
}

/* ------------------------------------------------------------------------- */
const wlmtk_dock_positioning_t *wlmtk_dock_get_positioning(
    wlmtk_dock_t *dock_ptr)
{
    return &dock_ptr->dock_positioning;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_dock_set_positioning(
    wlmtk_dock_t *dock_ptr,
    const wlmtk_dock_positioning_t *positioning_ptr)
{
    wlmtk_dock_positioning_t old_positioning = dock_ptr->dock_positioning;
    wlmtk_box_orientation_t old_orientation = _wlmtk_dock_orientation(dock_ptr);
    dock_ptr->dock_positioning = *positioning_ptr;
    if (_wlmtk_dock_orientation(dock_ptr) != old_orientation) {
        dock_ptr->dock_positioning = old_positioning;
        return false;
    }

    // Collect tiles in order from the (former) anchor, then re-add: Adding
    // places a tile at the end away from the (new) anchor.
    size_t n = bs_dllist_size(&dock_ptr->tiles);
    wlmtk_tile_t **tiles_ptr = NULL;
    if (0 < n) {
        tiles_ptr = logged_calloc(n, sizeof(wlmtk_tile_t*));
        if (NULL == tiles_ptr) {
            dock_ptr->dock_positioning = old_positioning;
            return false;
        }
    }
    bool from_head = (WLR_EDGE_TOP == old_positioning.anchor ||
                      WLR_EDGE_LEFT == old_positioning.anchor);
    size_t pos = 0;
    for (bs_dllist_node_t *dlnode_ptr = from_head ?
             dock_ptr->tiles.head_ptr : dock_ptr->tiles.tail_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = from_head ? dlnode_ptr->next_ptr : dlnode_ptr->prev_ptr) {
        tiles_ptr[pos++] = wlmtk_tile_from_dlnode(dlnode_ptr);
    }
    for (size_t i = 0; i < n; ++i) {
        wlmtk_box_remove_element(
            &dock_ptr->tile_box, wlmtk_tile_element(tiles_ptr[i]));
        bs_dllist_remove(&dock_ptr->tiles, wlmtk_dlnode_from_tile(tiles_ptr[i]));
    }
    for (size_t i = 0; i < n; ++i) wlmtk_dock_add_tile(dock_ptr, tiles_ptr[i]);
    if (NULL != tiles_ptr) free(tiles_ptr);

    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
    return true;
}

/* ------------------------------------------------------------------------- */
size_t wlmtk_dock_tiles(wlmtk_dock_t *dock_ptr)
{
    return bs_dllist_size(&dock_ptr->tiles);
}

/* ------------------------------------------------------------------------- */
int wlmtk_dock_tile_index(wlmtk_dock_t *dock_ptr, wlmtk_tile_t *tile_ptr)
{
    // @ref wlmtk_dock_t::tiles is in visual order (top/left first). Tiles are
    // added at the end away from the anchor.
    bool from_head = (WLR_EDGE_TOP == dock_ptr->dock_positioning.anchor ||
                      WLR_EDGE_LEFT == dock_ptr->dock_positioning.anchor);
    int index = 0;
    for (bs_dllist_node_t *dlnode_ptr = from_head ?
             dock_ptr->tiles.head_ptr : dock_ptr->tiles.tail_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = from_head ? dlnode_ptr->next_ptr : dlnode_ptr->prev_ptr) {
        if (wlmtk_tile_from_dlnode(dlnode_ptr) == tile_ptr) return index;
        ++index;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_move_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t index)
{
    int current = wlmtk_dock_tile_index(dock_ptr, tile_ptr);
    size_t n = bs_dllist_size(&dock_ptr->tiles);
    if (0 > current) return;
    if (index >= n) index = n - 1;
    if ((size_t)current == index) return;

    // Collect tiles in order from the anchor, without `tile_ptr`.
    wlmtk_tile_t **tiles_ptr = logged_calloc(n, sizeof(wlmtk_tile_t*));
    if (NULL == tiles_ptr) return;
    bool from_head = (WLR_EDGE_TOP == dock_ptr->dock_positioning.anchor ||
                      WLR_EDGE_LEFT == dock_ptr->dock_positioning.anchor);
    size_t pos = 0;
    for (bs_dllist_node_t *dlnode_ptr = from_head ?
             dock_ptr->tiles.head_ptr : dock_ptr->tiles.tail_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = from_head ? dlnode_ptr->next_ptr : dlnode_ptr->prev_ptr) {
        wlmtk_tile_t *t_ptr = wlmtk_tile_from_dlnode(dlnode_ptr);
        if (t_ptr != tile_ptr) tiles_ptr[pos++] = t_ptr;
    }
    // Insert `tile_ptr` at `index`.
    memmove(&tiles_ptr[index + 1], &tiles_ptr[index],
            (n - 1 - index) * sizeof(wlmtk_tile_t*));
    tiles_ptr[index] = tile_ptr;

    // Re-add all tiles in that order. Adding places a tile at the far end.
    for (size_t i = 0; i < n; ++i) {
        wlmtk_box_remove_element(
            &dock_ptr->tile_box, wlmtk_tile_element(tiles_ptr[i]));
        bs_dllist_remove(&dock_ptr->tiles, wlmtk_dlnode_from_tile(tiles_ptr[i]));
    }
    for (size_t i = 0; i < n; ++i) wlmtk_dock_add_tile(dock_ptr, tiles_ptr[i]);
    free(tiles_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmtk_dock_remove_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr)
{
    BS_ASSERT(
        &dock_ptr->tile_box.element_container ==
        wlmtk_tile_element(tile_ptr)->parent_container_ptr);
    wlmtk_box_remove_element(
        &dock_ptr->tile_box,
        wlmtk_tile_element(tile_ptr));
    bs_dllist_remove(
        &dock_ptr->tiles,
        wlmtk_dlnode_from_tile(tile_ptr));

    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
}

/* ------------------------------------------------------------------------- */
wlmtk_panel_t *wlmtk_dock_panel(wlmtk_dock_t *dock_ptr)
{
    return &dock_ptr->super_panel;
}

/* ------------------------------------------------------------------------- */
wlmtk_element_t *wlmtk_dock_element(wlmtk_dock_t *dock_ptr)
{
    return wlmtk_panel_element(wlmtk_dock_panel(dock_ptr));
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Recomputes the dock's size -- and also commits it.
 *
 * @param panel_ptr
 * @param width
 * @param height
 *
 * @return 0
 */
uint32_t _wlmtk_dock_panel_request_size(
    wlmtk_panel_t *panel_ptr,
    __UNUSED__ int width,
    __UNUSED__ int height)
{
    wlmtk_dock_t *dock_ptr = BS_CONTAINER_OF(
        panel_ptr, wlmtk_dock_t, super_panel);

    wlmtk_panel_positioning_t panel_positioning = {};
    if (!_wlmtk_dock_positioning(dock_ptr, &panel_positioning)) {
        bs_log(BS_ERROR, "Panel %p invalid positioning data.", panel_ptr);
        return 0;
    }

    wlmtk_panel_commit(panel_ptr, 0, &panel_positioning);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Redraw the dock. We also (re)request size, ie. the positioning. */
void _wlmtk_dock_element_layout(wlmtk_element_t *element_ptr)
{
    wlmtk_dock_t *dock_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_dock_t, super_panel.super_container.super_element);
    dock_ptr->old_element_vmt.layout(element_ptr);
    _wlmtk_dock_panel_request_size(&dock_ptr->super_panel, 0, 0);
}

/* ------------------------------------------------------------------------- */
/** Derives the box' orientation for the dock. */
wlmtk_box_orientation_t _wlmtk_dock_orientation(wlmtk_dock_t *dock_ptr)
{
    switch (dock_ptr->dock_positioning.edge) {
    case WLR_EDGE_LEFT:
    case WLR_EDGE_RIGHT:
        return WLMTK_BOX_VERTICAL;

    case WLR_EDGE_TOP:
    case WLR_EDGE_BOTTOM:
        return WLMTK_BOX_HORIZONTAL;

    default:
        bs_log(BS_FATAL, "wlmtk_dock %p: Invalid edge %"PRIx32,
               dock_ptr, dock_ptr->dock_positioning.edge);
    }
    /* Not reached. */
    return WLMTK_BOX_HORIZONTAL;
}

/* ------------------------------------------------------------------------- */
/** Fills the panel positioning parameters from the dock's. */
bool _wlmtk_dock_positioning(
    wlmtk_dock_t *dock_ptr,
    wlmtk_panel_positioning_t *panel_positioning_ptr)
{
    struct wlr_box box = wlmtk_element_get_dimensions_box(
        wlmtk_box_element(&dock_ptr->tile_box));

    switch (dock_ptr->dock_positioning.edge) {
    case WLR_EDGE_LEFT:
    case WLR_EDGE_RIGHT:
        panel_positioning_ptr->anchor = dock_ptr->dock_positioning.edge;
        panel_positioning_ptr->desired_width = BS_MAX(box.width, 1);

        if (dock_ptr->dock_positioning.anchor != WLR_EDGE_TOP &&
            dock_ptr->dock_positioning.anchor != WLR_EDGE_BOTTOM) {
            bs_log(BS_ERROR, "wlmtk_dock_t anchor must be adjacent to edge: "
                   "anchor %"PRIx32", edge %"PRIx32,
                   dock_ptr->dock_positioning.anchor,
                   dock_ptr->dock_positioning.edge);
            return false;
        }
        panel_positioning_ptr->anchor |= dock_ptr->dock_positioning.anchor;

        // The layer protocol requires a non-zero value for panels not spanning
        // the entire height. Go with a one-tile dimension, as long as there's
        // no tiles yet.
        panel_positioning_ptr->desired_height = BS_MAX(box.height, 1);
        panel_positioning_ptr->exclusive_zone = BS_MAX(box.width, 1);
        break;

    case WLR_EDGE_TOP:
    case WLR_EDGE_BOTTOM:
        panel_positioning_ptr->anchor = dock_ptr->dock_positioning.edge;
        panel_positioning_ptr->desired_height = BS_MAX(box.height, 1);
        if (dock_ptr->dock_positioning.anchor != WLR_EDGE_LEFT &&
            dock_ptr->dock_positioning.anchor != WLR_EDGE_RIGHT) {
            bs_log(BS_ERROR, "wlmtk_dock_t anchor must be adjacent to edge: "
                   "anchor %"PRIx32", edge %"PRIx32,
                   dock_ptr->dock_positioning.anchor,
                   dock_ptr->dock_positioning.edge);
            return false;
        }
        panel_positioning_ptr->anchor |= dock_ptr->dock_positioning.anchor;

        // The layer protocol requires a non-zero value for panels not spanning
        // the entire width. Go with a one-tile dimension, as long as there's
        // no tiles yet.
        panel_positioning_ptr->desired_width = BS_MAX(box.width, 1);
        panel_positioning_ptr->exclusive_zone = BS_MAX(box.height, 1);
        break;

    default:
        bs_log(BS_ERROR, "Unexpected wlmtk_dock_t positioning edge 0x%"PRIx32,
               dock_ptr->dock_positioning.edge);
        return false;
    }

    // Distance from the anchor.
    switch (dock_ptr->dock_positioning.anchor) {
    case WLR_EDGE_TOP:
        panel_positioning_ptr->margin_top = dock_ptr->dock_positioning.margin;
        break;
    case WLR_EDGE_BOTTOM:
        panel_positioning_ptr->margin_bottom = dock_ptr->dock_positioning.margin;
        break;
    case WLR_EDGE_LEFT:
        panel_positioning_ptr->margin_left = dock_ptr->dock_positioning.margin;
        break;
    case WLR_EDGE_RIGHT:
        panel_positioning_ptr->margin_right = dock_ptr->dock_positioning.margin;
        break;
    default:
        break;
    }

    panel_positioning_ptr->exclusive_edge = dock_ptr->dock_positioning.edge;
    return true;
}

/* ------------------------------------------------------------------------- */
/** Iterator to bs_dllist_all(): Sets style for a tile in the iterator. */
bool _wlmtk_dock_tile_set_style(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_tile_t *tile_ptr = wlmtk_tile_from_dlnode(dlnode_ptr);
    const struct wlmtk_tile_style *tile_style_ptr = ud_ptr;
    return wlmtk_tile_set_style(tile_ptr, tile_style_ptr);
}

/* ------------------------------------------------------------------------- */
/** Iterator to bs_dllist_for_each(): Removes & destroys the tile at node. */
void _wlmtk_dock_tile_destroy(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_tile_t *tile_ptr = wlmtk_tile_from_dlnode(dlnode_ptr);
    wlmtk_dock_t *dock_ptr = ud_ptr;
    wlmtk_dock_remove_tile(dock_ptr, tile_ptr);
    wlmtk_element_destroy(wlmtk_tile_element(tile_ptr));
}

/* == Unit tests =========================================================== */

static void test_create_destroy(bs_test_t *test_ptr);
static void test_move_tile(bs_test_t *test_ptr);
static void test_set_positioning(bs_test_t *test_ptr);

/** Test cases */
static const bs_test_case_t _wlmtk_dock_test_cases[] = {
    { 1, "create_destroy", test_create_destroy },
    { 1, "move_tile", test_move_tile },
    { 1, "set_positioning", test_set_positioning },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmtk_dock_test_set = BS_TEST_SET(
    true, "dock", _wlmtk_dock_test_cases);

/* ------------------------------------------------------------------------- */
/** Exercises ctor and dtor. */
 void test_create_destroy(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT,
        .anchor = WLR_EDGE_BOTTOM,
    };
    struct wlmtk_dock_style style = {};

    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_EDGE_LEFT | WLR_EDGE_BOTTOM,
        dock_ptr->super_panel.positioning.anchor);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        1,
        dock_ptr->super_panel.positioning.desired_width);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        1,
        dock_ptr->super_panel.positioning.desired_height);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_BOX_VERTICAL,
        dock_ptr->tile_box.orientation);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dock_ptr);
    wlmtk_dock_destroy(dock_ptr);
}


/* ------------------------------------------------------------------------- */
/** Tests indexing and moving tiles, for both anchor directions. */
void test_move_tile(bs_test_t *test_ptr)
{
    static const enum wlr_edges anchors[] = { WLR_EDGE_TOP, WLR_EDGE_BOTTOM };
    for (size_t a = 0; a < 2; ++a) {
        wlmtk_dock_positioning_t pos = {
            .edge = WLR_EDGE_LEFT, .anchor = anchors[a] };
        struct wlmtk_dock_style style = {};
        wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
        BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);

        struct wlmtk_tile_style ts = { .size = 64 };
        wlmtk_tile_t t[3];
        for (int i = 0; i < 3; ++i) {
            BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, wlmtk_tile_init(&t[i], &ts));
            wlmtk_element_set_visible(wlmtk_tile_element(&t[i]), true);
            wlmtk_dock_add_tile(dock_ptr, &t[i]);
        }
        BS_TEST_VERIFY_EQ(test_ptr, 3, wlmtk_dock_tiles(dock_ptr));
        BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &t[0]));
        BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_dock_tile_index(dock_ptr, &t[2]));

        // Move last to the anchor: order 2, 0, 1.
        wlmtk_dock_move_tile(dock_ptr, &t[2], 0);
        BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &t[2]));
        BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tile_index(dock_ptr, &t[0]));
        BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_dock_tile_index(dock_ptr, &t[1]));

        // Move first far beyond the end: order 0, 1, 2.
        wlmtk_dock_move_tile(dock_ptr, &t[2], 10);
        BS_TEST_VERIFY_EQ(test_ptr, 0, wlmtk_dock_tile_index(dock_ptr, &t[0]));
        BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_dock_tile_index(dock_ptr, &t[1]));
        BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_dock_tile_index(dock_ptr, &t[2]));

        // Visual position: Index 0 nearest the anchor.
        int x0, y0, x2, y2;
        wlmtk_element_get_position(wlmtk_tile_element(&t[0]), &x0, &y0);
        wlmtk_element_get_position(wlmtk_tile_element(&t[2]), &x2, &y2);
        if (WLR_EDGE_TOP == anchors[a]) {
            BS_TEST_VERIFY_TRUE(test_ptr, y0 < y2);
        } else {
            BS_TEST_VERIFY_TRUE(test_ptr, y0 > y2);
        }

        for (int i = 0; i < 3; ++i) {
            wlmtk_dock_remove_tile(dock_ptr, &t[i]);
            wlmtk_tile_fini(&t[i]);
        }
        wlmtk_dock_destroy(dock_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Moves the dock to other corners: Order from the anchor is kept. */
void test_set_positioning(bs_test_t *test_ptr)
{
    wlmtk_dock_positioning_t pos = {
        .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP };
    struct wlmtk_dock_style style = {};
    wlmtk_dock_t *dock_ptr = wlmtk_dock_create(&pos, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);

    struct wlmtk_tile_style ts = { .size = 64 };
    wlmtk_tile_t t[3];
    for (int i = 0; i < 3; ++i) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, wlmtk_tile_init(&t[i], &ts));
        wlmtk_element_set_visible(wlmtk_tile_element(&t[i]), true);
        wlmtk_dock_add_tile(dock_ptr, &t[i]);
    }

    // To the bottom-right corner: t[0] still nearest the anchor, now lowest.
    pos = (wlmtk_dock_positioning_t){
        .edge = WLR_EDGE_RIGHT, .anchor = WLR_EDGE_BOTTOM };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_dock_set_positioning(dock_ptr, &pos));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT,
                      wlmtk_dock_get_positioning(dock_ptr)->edge);
    for (int i = 0; i < 3; ++i) {
        BS_TEST_VERIFY_EQ(test_ptr, i, wlmtk_dock_tile_index(dock_ptr, &t[i]));
    }
    int x0, y0, x2, y2;
    wlmtk_element_get_position(wlmtk_tile_element(&t[0]), &x0, &y0);
    wlmtk_element_get_position(wlmtk_tile_element(&t[2]), &x2, &y2);
    BS_TEST_VERIFY_TRUE(test_ptr, y0 > y2);

    // Back to the top: Order kept.
    pos.anchor = WLR_EDGE_TOP;
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_dock_set_positioning(dock_ptr, &pos));
    for (int i = 0; i < 3; ++i) {
        BS_TEST_VERIFY_EQ(test_ptr, i, wlmtk_dock_tile_index(dock_ptr, &t[i]));
    }

    // Changing the orientation is refused.
    pos = (wlmtk_dock_positioning_t){
        .edge = WLR_EDGE_BOTTOM, .anchor = WLR_EDGE_LEFT };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_dock_set_positioning(dock_ptr, &pos));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT,
                      wlmtk_dock_get_positioning(dock_ptr)->edge);

    for (int i = 0; i < 3; ++i) {
        wlmtk_dock_remove_tile(dock_ptr, &t[i]);
        wlmtk_tile_fini(&t[i]);
    }
    wlmtk_dock_destroy(dock_ptr);
}

/* == End of dock.c ======================================================== */
