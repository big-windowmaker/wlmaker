/* ========================================================================= */
/**
 * @file dock_position.h
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
#ifndef __WLMAKER_DOCK_POSITION_H__
#define __WLMAKER_DOCK_POSITION_H__

#include <stdbool.h>
#include <stdint.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Where a dock (the Dock, or the Clip) is placed: Its edge, and the anchor
 * along that edge. Together, they pick a corner of the output.
 */
typedef struct {
    /** Edge: WLR_EDGE_LEFT, _RIGHT (vertical dock), _TOP or _BOTTOM. */
    uint32_t                  edge;
    /** Anchor: Adjacent to the edge. */
    uint32_t                  anchor;
} wlmaker_dock_place_t;

/** @return Whether the dock at `place` is vertical (at a left/right edge). */
bool wlmaker_dock_place_vertical(wlmaker_dock_place_t place);

/** @return The corner of `place`, as a bitmask of two `enum wlr_edges`. */
uint32_t wlmaker_dock_place_corner(wlmaker_dock_place_t place);

/**
 * Computes the place for a corner, keeping the dock's orientation.
 *
 * @param corner              Bitmask of one of WLR_EDGE_LEFT, _RIGHT and one
 *                            of WLR_EDGE_TOP, _BOTTOM.
 * @param vertical
 *
 * @return The place.
 */
wlmaker_dock_place_t wlmaker_dock_place_from_corner(
    uint32_t corner, bool vertical);

/**
 * Computes the place for dropping a dock at (x, y): The corner of the
 * output's quadrant, ie. left/right by the horizontal and top/bottom by the
 * vertical half the point is in. The dock's orientation is kept.
 *
 * @param output_box_ptr
 * @param x
 * @param y
 * @param vertical
 *
 * @return The place.
 */
wlmaker_dock_place_t wlmaker_dock_place_at(
    const struct wlr_box *output_box_ptr,
    double x,
    double y,
    bool vertical);

/**
 * Keeps two docks out of the same corner: If `moved` ends up in the corner
 * of `other_ptr`, `other_ptr` moves to where `moved` came from.
 *
 * @param moved               New place of the moved dock.
 * @param moved_from          Former place of the moved dock.
 * @param other_ptr           Place of the other dock. Updated.
 *
 * @return true if `other_ptr` was updated.
 */
bool wlmaker_dock_place_resolve(
    wlmaker_dock_place_t moved,
    wlmaker_dock_place_t moved_from,
    wlmaker_dock_place_t *other_ptr);

/**
 * Reads `Edge` and `Anchor` from a section of the state.
 *
 * @param dict_ptr
 * @param place_ptr
 *
 * @return true if both were found and valid.
 */
bool wlmaker_dock_place_decode(
    bspl_dict_t *dict_ptr,
    wlmaker_dock_place_t *place_ptr);

/**
 * Creates a dict with `Edge` and `Anchor` of `place`, for storing.
 *
 * @param place
 *
 * @return The dict, or NULL on error.
 */
bspl_dict_t *wlmaker_dock_place_encode(wlmaker_dock_place_t place);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dock_position_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCK_POSITION_H__ */
/* == End of dock_position.h =============================================== */
