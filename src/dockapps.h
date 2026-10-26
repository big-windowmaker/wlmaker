/* ========================================================================= */
/**
 * @file dockapps.h
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
#ifndef __DOCKAPPS_H__
#define __DOCKAPPS_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>

#include "toolkit/toolkit.h"

struct wl_client;

/** Forward declaration: Placement of dock-apps in a dock (the Clip). */
typedef struct _wlmaker_dockapps_t wlmaker_dockapps_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the dock-app placement for `dock_ptr`.
 *
 * Dock-apps are identified by a name, see @ref wlmaker_dockapps_client_name.
 * Their order (from the dock's anchor) follows the order of names in the file
 * at `state_fname_ptr`, if that exists; or `order_array_ptr` otherwise.
 * Dock-apps with names not listed are placed after the listed ones.
 *
 * @param dock_ptr
 * @param order_array_ptr     Array of strings, or NULL. Eg. `Clip.Dockapps`
 *                            from the State file.
 * @param state_fname_ptr     File to load and store the order, or NULL.
 *
 * @return A pointer to the dock-apps placement, or NULL on error. Must be
 *     destroyed by calling @ref wlmaker_dockapps_destroy.
 */
wlmaker_dockapps_t *wlmaker_dockapps_create(
    wlmtk_dock_t *dock_ptr,
    bspl_array_t *order_array_ptr,
    const char *state_fname_ptr);

/** Destroys the placement. Does not remove the tiles from the dock. */
void wlmaker_dockapps_destroy(wlmaker_dockapps_t *dockapps_ptr);

/**
 * Adds `tile_ptr` of dock-app `name_ptr` to the dock, at its place.
 *
 * @param dockapps_ptr
 * @param tile_ptr
 * @param name_ptr            May be NULL, then the tile is placed at the end.
 *
 * @return true on success.
 */
bool wlmaker_dockapps_add(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    const char *name_ptr);

/** Removes `tile_ptr` from the dock. */
void wlmaker_dockapps_remove(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr);

/**
 * Moves the dock-app's tile by `delta` positions, away from the anchor for
 * positive values. Stores the resulting order in the state file, if given.
 *
 * @param dockapps_ptr
 * @param tile_ptr
 * @param delta
 */
void wlmaker_dockapps_move(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    int delta);

/**
 * Converts a drag of the pointer (in pixels, along x and y) into a number of
 * positions to move by in the dock: Positive moves away from the anchor.
 *
 * @param dockapps_ptr
 * @param dx
 * @param dy
 * @param tile_pitch          Distance between two tiles, in pixels.
 *
 * @return The number of positions.
 */
int wlmaker_dockapps_drag_delta(
    wlmaker_dockapps_t *dockapps_ptr,
    double dx,
    double dy,
    int tile_pitch);

/**
 * Returns the move needed to shift the dock-app's tile by one place on the
 * screen, towards `direction`. For a right-click menu's "Move Up" etc.
 *
 * @param dockapps_ptr
 * @param tile_ptr
 * @param direction           One of WLR_EDGE_TOP, _BOTTOM, _LEFT, _RIGHT.
 *
 * @return The `delta` for @ref wlmaker_dockapps_move: +1 or -1. 0 if the
 *     direction is across the dock, or the tile is already at that end.
 */
int wlmaker_dockapps_delta_towards(
    wlmaker_dockapps_t *dockapps_ptr,
    wlmtk_tile_t *tile_ptr,
    uint32_t direction);

/**
 * Returns the name identifying the dock-app of `wl_client_ptr`: The name of
 * the client's executable (process name), eg. "wlmclock".
 *
 * @param wl_client_ptr
 *
 * @return A string to be free()-ed by the caller, or NULL.
 */
char *wlmaker_dockapps_client_name(struct wl_client *wl_client_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dockapps_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __DOCKAPPS_H__ */
/* == End of dockapps.h ==================================================== */
