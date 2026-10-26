/* ========================================================================= */
/**
 * @file dock.h
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
#ifndef __WLMTK_DOCK_H__
#define __WLMTK_DOCK_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "element.h"
#include "panel.h"
#include "style.h"
#include "tile.h"

/** Forward declaration: Dock handle. */
typedef struct _wlmtk_dock_t wlmtk_dock_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Positioning options for the dock. */
typedef struct {
    /** Principal edge the dock aligns to. Must not be WLR_EDGE_NONE. */
    enum wlr_edges            edge;
    /** Dock anchor, along @ref wlmtk_dock_positioning_t::edge. */
    enum wlr_edges            anchor;
    /** Distance from the anchor, in pixels. Optional, defaults to 0. */
    int                       margin;
    /**
     * Whether the dock is positioned relative to the full output, ignoring
     * other panels' exclusive zones, and without an exclusive zone of its own.
     * Eg. for placing launchers adjacent to the Clip. Optional.
     */
    bool                      overlay;
} wlmtk_dock_positioning_t;

/** Styling information for the dock. */
struct wlmtk_dock_style {
    /** The margin's style of the dock's box. */
    struct wlmtk_margin_style margin;
};

/**
 * Creates a dock. A dock contains icons, launchers and the likes.
 *
 * The dock is an implementation of a @ref wlmtk_panel_t.
 *
 * @param dock_positioning_ptr
 * @param style_ptr
 *
 * @return The dock handle, or NULL on error. Must be destroyed calling
 *     @ref wlmtk_dock_destroy.
 */
wlmtk_dock_t *wlmtk_dock_create(
    const wlmtk_dock_positioning_t *dock_positioning_ptr,
    const struct wlmtk_dock_style *style_ptr);

/**
 * Destroys the dock.
 *
 * @param dock_ptr
 */
void wlmtk_dock_destroy(wlmtk_dock_t *dock_ptr);

/**
 * Sets the style for the dock, including all contained tiles.
 *
 * @param dock_ptr
 * @param style_ptr
 * @param tile_style_ptr
 *
 * @return true on success.
 */
bool wlmtk_dock_set_style(
    wlmtk_dock_t *dock_ptr,
    const struct wlmtk_dock_style *style_ptr,
    const struct wlmtk_tile_style *tile_style_ptr);

/**
 * Adds the tile to the dock.
 *
 * @param dock_ptr
 * @param tile_ptr
 */
void wlmtk_dock_add_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr);

/**
 * Removes the tile from the dock.
 *
 * @param dock_ptr
 * @param tile_ptr
 */
void wlmtk_dock_remove_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr);

/** @return The current positioning of `dock_ptr`. */
const wlmtk_dock_positioning_t *wlmtk_dock_get_positioning(
    wlmtk_dock_t *dock_ptr);

/**
 * Moves the dock to another edge and anchor, eg. to another corner of the
 * output. The tiles keep their order, counted from the anchor.
 *
 * @param dock_ptr
 * @param positioning_ptr
 *
 * @return false if the positioning would change the dock's orientation:
 *     That is not supported.
 */
bool wlmtk_dock_set_positioning(
    wlmtk_dock_t *dock_ptr,
    const wlmtk_dock_positioning_t *positioning_ptr);

/** @return Number of tiles in the dock. */
size_t wlmtk_dock_tiles(wlmtk_dock_t *dock_ptr);

/**
 * Returns the index of `tile_ptr` in the dock, counted from the dock's anchor:
 * The tile nearest to the anchor has index 0.
 *
 * @param dock_ptr
 * @param tile_ptr
 *
 * @return The index, or -1 if `tile_ptr` is not in the dock.
 */
int wlmtk_dock_tile_index(wlmtk_dock_t *dock_ptr, wlmtk_tile_t *tile_ptr);

/**
 * Moves `tile_ptr` to position `index`, counted from the dock's anchor. An
 * index beyond the last tile moves it to the far end.
 *
 * @param dock_ptr
 * @param tile_ptr
 * @param index
 */
void wlmtk_dock_move_tile(
    wlmtk_dock_t *dock_ptr,
    wlmtk_tile_t *tile_ptr,
    size_t index);

/** @return Pointer to the superclass @ref wlmtk_panel_t of `dock_ptr`. */
wlmtk_panel_t *wlmtk_dock_panel(wlmtk_dock_t *dock_ptr);

/** @return Pointer to the superclass @ref wlmtk_element_t of `dock_ptr`. */
wlmtk_element_t *wlmtk_dock_element(wlmtk_dock_t *dock_ptr);

/** Plist decoding descriptor of the dock's style. */
extern const bspl_desc_t wlmtk_dock_style_desc[];

/** Unit tests for @ref wlmtk_dock_t. */
extern const bs_test_set_t wlmtk_dock_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMTK_DOCK_H__ */
/* == End of dock.h ======================================================== */
