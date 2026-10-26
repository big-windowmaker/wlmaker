/* ========================================================================= */
/**
 * @file dock_position.c
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

#include "dock_position.h"

#include <string.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** Names of edges, as in the state file. */
static const bspl_enum_desc_t _wlmaker_dock_place_edges[] = {
    BSPL_ENUM("TOP", WLR_EDGE_TOP),
    BSPL_ENUM("BOTTOM", WLR_EDGE_BOTTOM),
    BSPL_ENUM("LEFT", WLR_EDGE_LEFT),
    BSPL_ENUM("RIGHT", WLR_EDGE_RIGHT),
    BSPL_ENUM_SENTINEL(),
};

/** Mask of the horizontal sides. */
static const uint32_t _wlmaker_dock_place_horizontal =
    WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
/** Mask of the vertical sides. */
static const uint32_t _wlmaker_dock_place_vertical_sides =
    WLR_EDGE_TOP | WLR_EDGE_BOTTOM;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_place_vertical(wlmaker_dock_place_t place)
{
    return 0 != (place.edge & _wlmaker_dock_place_horizontal);
}

/* ------------------------------------------------------------------------- */
uint32_t wlmaker_dock_place_corner(wlmaker_dock_place_t place)
{
    return place.edge | place.anchor;
}

/* ------------------------------------------------------------------------- */
wlmaker_dock_place_t wlmaker_dock_place_from_corner(
    uint32_t corner, bool vertical)
{
    uint32_t h = corner & _wlmaker_dock_place_horizontal;
    uint32_t v = corner & _wlmaker_dock_place_vertical_sides;
    if (vertical) return (wlmaker_dock_place_t){ .edge = h, .anchor = v };
    return (wlmaker_dock_place_t){ .edge = v, .anchor = h };
}

/* ------------------------------------------------------------------------- */
wlmaker_dock_place_t wlmaker_dock_place_at(
    const struct wlr_box *output_box_ptr,
    double x,
    double y,
    bool vertical)
{
    double cx = output_box_ptr->x + output_box_ptr->width / 2.0;
    double cy = output_box_ptr->y + output_box_ptr->height / 2.0;
    uint32_t corner = (x < cx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
        (y < cy ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
    return wlmaker_dock_place_from_corner(corner, vertical);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_place_resolve(
    wlmaker_dock_place_t moved,
    wlmaker_dock_place_t moved_from,
    wlmaker_dock_place_t *other_ptr)
{
    if (wlmaker_dock_place_corner(moved) !=
        wlmaker_dock_place_corner(*other_ptr)) return false;
    // Should the moved dock come from that corner too: Pick the corner across
    // the edge, so they still don't overlap.
    uint32_t corner = wlmaker_dock_place_corner(moved_from);
    if (corner == wlmaker_dock_place_corner(moved)) {
        corner ^= _wlmaker_dock_place_horizontal;
    }
    *other_ptr = wlmaker_dock_place_from_corner(
        corner, wlmaker_dock_place_vertical(*other_ptr));
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_place_decode(
    bspl_dict_t *dict_ptr,
    wlmaker_dock_place_t *place_ptr)
{
    if (NULL == dict_ptr) return false;
    int edge, anchor;
    const char *e = bspl_dict_get_string_value(dict_ptr, "Edge");
    const char *a = bspl_dict_get_string_value(dict_ptr, "Anchor");
    if (NULL == e || NULL == a ||
        !bspl_enum_name_to_value(_wlmaker_dock_place_edges, e, &edge) ||
        !bspl_enum_name_to_value(_wlmaker_dock_place_edges, a, &anchor)) {
        return false;
    }
    wlmaker_dock_place_t p = { .edge = edge, .anchor = anchor };
    // The anchor must be adjacent to the edge.
    if (wlmaker_dock_place_vertical(p) ==
        (0 != (p.anchor & _wlmaker_dock_place_horizontal))) return false;
    *place_ptr = p;
    return true;
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_dock_place_encode(wlmaker_dock_place_t place)
{
    const char *e = NULL, *a = NULL;
    if (!bspl_enum_value_to_name(_wlmaker_dock_place_edges, place.edge, &e) ||
        !bspl_enum_value_to_name(_wlmaker_dock_place_edges, place.anchor, &a)) {
        return NULL;
    }
    bspl_dict_t *dict_ptr = bspl_dict_create();
    bspl_string_t *es = bspl_string_create(e);
    bspl_string_t *as = bspl_string_create(a);
    bool rv = NULL != dict_ptr && NULL != es && NULL != as &&
        bspl_dict_add(dict_ptr, "Edge", bspl_object_from_string(es)) &&
        bspl_dict_add(dict_ptr, "Anchor", bspl_object_from_string(as));
    if (NULL != es) bspl_string_unref(es);
    if (NULL != as) bspl_string_unref(as);
    if (!rv && NULL != dict_ptr) {
        bspl_dict_unref(dict_ptr);
        dict_ptr = NULL;
    }
    return dict_ptr;
}

/* == Unit tests =========================================================== */

static void test_quadrants(bs_test_t *test_ptr);
static void test_resolve(bs_test_t *test_ptr);
static void test_codec(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmaker_dock_position_test_cases[] = {
    { true, "quadrants", test_quadrants },
    { true, "resolve", test_resolve },
    { true, "codec", test_codec },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dock_position_test_set = BS_TEST_SET(
    true, "dock_position", _wlmaker_dock_position_test_cases);

/* ------------------------------------------------------------------------- */
/** Maps the quadrants of an output to edge and anchor. */
void test_quadrants(bs_test_t *test_ptr)
{
    struct wlr_box o = { .x = 1280, .y = 0, .width = 1280, .height = 720 };
    wlmaker_dock_place_t p;

    // Vertical docks: Edge left/right, anchor top/bottom.
    p = wlmaker_dock_place_at(&o, 1300, 10, true);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_LEFT, p.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_TOP, p.anchor);
    p = wlmaker_dock_place_at(&o, 2500, 10, true);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, p.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_TOP, p.anchor);
    p = wlmaker_dock_place_at(&o, 1300, 700, true);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_LEFT, p.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_BOTTOM, p.anchor);
    p = wlmaker_dock_place_at(&o, 1920, 360, true);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, p.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_BOTTOM, p.anchor);

    // Horizontal docks: Edge top/bottom, anchor left/right.
    p = wlmaker_dock_place_at(&o, 2500, 700, false);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_BOTTOM, p.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, p.anchor);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_place_vertical(p));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT,
                      wlmaker_dock_place_corner(p));
}

/* ------------------------------------------------------------------------- */
/** Two docks don't share a corner. */
void test_resolve(bs_test_t *test_ptr)
{
    wlmaker_dock_place_t dock = { WLR_EDGE_RIGHT, WLR_EDGE_TOP };
    wlmaker_dock_place_t clip = { WLR_EDGE_LEFT, WLR_EDGE_TOP };
    wlmaker_dock_place_t moved;

    // Dock moved bottom-right: No conflict.
    moved = (wlmaker_dock_place_t){ WLR_EDGE_RIGHT, WLR_EDGE_BOTTOM };
    BS_TEST_VERIFY_FALSE(
        test_ptr, wlmaker_dock_place_resolve(moved, dock, &clip));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_LEFT, clip.edge);

    // Dock moved onto the Clip's corner: They swap.
    moved = (wlmaker_dock_place_t){ WLR_EDGE_LEFT, WLR_EDGE_TOP };
    BS_TEST_VERIFY_TRUE(
        test_ptr, wlmaker_dock_place_resolve(moved, dock, &clip));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, clip.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_TOP, clip.anchor);

    // A horizontal Clip keeps its orientation when swapped.
    wlmaker_dock_place_t hclip = { WLR_EDGE_BOTTOM, WLR_EDGE_RIGHT };
    moved = (wlmaker_dock_place_t){ WLR_EDGE_RIGHT, WLR_EDGE_BOTTOM };
    BS_TEST_VERIFY_TRUE(
        test_ptr, wlmaker_dock_place_resolve(moved, dock, &hclip));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_TOP, hclip.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, hclip.anchor);

    // Both in the same corner before: The other goes across.
    wlmaker_dock_place_t other = { WLR_EDGE_LEFT, WLR_EDGE_TOP };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_place_resolve(
                            other, other, &other));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, other.edge);
}

/* ------------------------------------------------------------------------- */
/** Decodes and encodes places. */
void test_codec(bs_test_t *test_ptr)
{
    bspl_object_t *o = bspl_create_object_from_plist_string(
        "{ Edge = RIGHT; Anchor = BOTTOM; }");
    wlmaker_dock_place_t p = {};
    BS_TEST_VERIFY_TRUE(
        test_ptr, wlmaker_dock_place_decode(bspl_dict_from_object(o), &p));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_RIGHT, p.edge);
    BS_TEST_VERIFY_EQ(test_ptr, WLR_EDGE_BOTTOM, p.anchor);
    bspl_object_unref(o);

    // The anchor must be adjacent.
    o = bspl_create_object_from_plist_string("{ Edge = RIGHT; Anchor = LEFT; }");
    BS_TEST_VERIFY_FALSE(
        test_ptr, wlmaker_dock_place_decode(bspl_dict_from_object(o), &p));
    bspl_object_unref(o);

    bspl_dict_t *d = wlmaker_dock_place_encode(
        (wlmaker_dock_place_t){ WLR_EDGE_LEFT, WLR_EDGE_TOP });
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_STREQ(test_ptr, "LEFT",
                         bspl_dict_get_string_value(d, "Edge"));
    BS_TEST_VERIFY_STREQ(test_ptr, "TOP",
                         bspl_dict_get_string_value(d, "Anchor"));
    bspl_dict_unref(d);
}

/* == End of dock_position.c =============================================== */
