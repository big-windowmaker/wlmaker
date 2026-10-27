/* ========================================================================= */
/**
 * @file clip.h
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2023 Google LLC
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
 * Creates the wlmaker clip. A view, with server-bound surfaces, that act
 * as a workspace-local dock and a workspace pager.
 *
 * Corresponding Window Maker documentation:
 * http://www.windowmaker.org/docs/guidedtour/clip.html
 */
#ifndef __CLIP_H__
#define __CLIP_H__

#include <libbase/plist.h>
#include <libbase/libbase.h>

/** Forward definition: Clip handle. */
typedef struct _wlmaker_clip_t wlmaker_clip_t;

#include "config.h"
#include "dock_position.h"
#include "dockapps.h"
#include "context_menu.h"
#include "server.h"

/** Stacking of the Dock or the Clip: Window Maker's "Dock position". */
typedef enum {
    /** Above the windows ("Keep on Top"). The default. */
    WLMAKER_DOCK_LEVEL_KEEP_ON_TOP,
    /** Below the windows ("Normal"). */
    WLMAKER_DOCK_LEVEL_NORMAL,
    /** Below, raised while the pointer is on it ("Auto raise & lower"). */
    WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER,
} wlmaker_dock_level_t;

/** Column of the Clip, for @ref wlmaker_clip_column_id. */
#define WLMAKER_COLUMN_CLIP 0
/** Column of the Dock. Columns from 2 on are the drawers, in their order. */
#define WLMAKER_COLUMN_DOCK 1

/**
 * A column of tiles: The Dock (its main tile, then its launchers, then the
 * dock-apps attached to it), the Clip (the Clip, its launchers, then its
 * dock-apps), or a drawer (its tile on the Dock, then its launchers and its
 * dock-apps, sticking out of the Dock). For placing tiles dragged onto them.
 */
typedef struct _wlmaker_dock_column_t {
    /** Which column: @ref WLMAKER_COLUMN_CLIP, _DOCK, or a drawer's. */
    int                       id;
    /** Where the dock is. */
    wlmaker_dock_place_t      place;
    /** Position of the tile nearest the anchor, in layout coordinates. */
    int                       x;
    /** Position of the tile nearest the anchor, in layout coordinates. */
    int                       y;
    /** Size of a tile. */
    int                       size;
    /** Distance from one tile to the next. */
    int                       pitch;
    /** Number of launchers, after the first tile. */
    size_t                    launchers;
    /** Number of dock-apps, after the launchers. */
    size_t                    dockapps;
    /** The Dock: Bit `i` is set if launcher `i` is a drawer. */
    uint64_t                  drawers;
    /** The output. */
    struct wlr_box            output;
} wlmaker_dock_column_t;

#include "drawer.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the Clip. Needs the server to be up with workspaces running.
 *
 * @param server_ptr
 * @param state_dict_ptr
 * @param style_ptr
 *
 * @return Pointer to the Clip handle, or NULL on error.
 */
wlmaker_clip_t *wlmaker_clip_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *state_dict_ptr,
    const wlmaker_config_style_t *style_ptr);

/**
 * Destroys the Clip.
 *
 * @param clip_ptr
 */
void wlmaker_clip_destroy(wlmaker_clip_t *clip_ptr);

/**
 * Moves the Dock (state section "Dock", shown by `wlmdock`) or the Clip to
 * another corner, keeping both out of the same one (the other one takes the
 * moved one's former corner). Stores the result in the state file, moves the
 * Clip, and tells `wlmdock` processes to re-read their position.
 *
 * @param server_ptr
 * @param section_ptr         "Dock" or "Clip".
 * @param place
 */
void wlmaker_clip_move_dock(
    wlmaker_server_t *server_ptr,
    const char *section_ptr,
    wlmaker_dock_place_t place);

/**
 * Adds the items moving the Clip to the other side, and to the other end, to
 * a context menu. Like on the Clip's own menu. For the menus of tiles that
 * are part of the Clip, eg. the dock-apps.
 *
 * @param server_ptr
 * @param context_menu_ptr
 *
 * @return true on success, or if there is no Clip.
 */
bool wlmaker_clip_add_move_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr);

/**
 * The launchers of the Dock or the Clip changed (eg. in the state file):
 * Updates the room the Clip leaves for its launchers, where the Dock's
 * dock-apps start, and tells `wlmdock` to re-read them.
 *
 * @param server_ptr
 */
void wlmaker_clip_launchers_changed(wlmaker_server_t *server_ptr);

/**
 * The Dock's surface (`wlmdock`) moved or changed: Places the dock-apps
 * attached to the Dock after its launchers.
 *
 * @param server_ptr
 */
void wlmaker_clip_dock_surface_changed(wlmaker_server_t *server_ptr);

/**
 * Places a dock-app's tile: On the Dock, if it was attached there before
 * (it is in the Dock's order), else on the Clip.
 *
 * @param server_ptr
 * @param tile_ptr
 * @param name_ptr            Name of the dock-app. May be NULL.
 *
 * @return true on success.
 */
bool wlmaker_clip_add_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    const char *name_ptr);

/** Removes the dock-app's tile from the Dock or the Clip, if there. */
void wlmaker_clip_remove_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr);

/**
 * @return The column of the placement holding `tile_ptr`, or -1.
 */
int wlmaker_clip_dockapps_column_of(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr);

/** @return The placement of the dock-apps of column `id`, or NULL. */
wlmaker_dockapps_t *wlmaker_clip_dockapps_id(
    wlmaker_server_t *server_ptr,
    int id);

/** @return Number of columns: The Clip, the Dock, and the drawers. */
int wlmaker_clip_columns(wlmaker_server_t *server_ptr);

/** @return The drawer of column `id`, or NULL if not a drawer's column. */
wlmaker_drawer_t *wlmaker_clip_drawer(wlmaker_server_t *server_ptr, int id);

/** @return The drawer at the Dock's launcher `index`, or NULL. */
wlmaker_drawer_t *wlmaker_clip_drawer_at(
    wlmaker_server_t *server_ptr,
    size_t index);

/** @return The column of `drawer_ptr`, or -1. */
int wlmaker_clip_drawer_column(
    wlmaker_server_t *server_ptr,
    wlmaker_drawer_t *drawer_ptr);

/**
 * Fills `column_ptr` with the geometry of column `id`.
 *
 * @return false if there is no such column.
 */
bool wlmaker_clip_column_id(
    wlmaker_server_t *server_ptr,
    int id,
    wlmaker_dock_column_t *column_ptr);

/** @return The placement holding `tile_ptr`: The Clip's, Dock's, or NULL. */
wlmaker_dockapps_t *wlmaker_clip_dockapps_of(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr);

/** @return The placement of the dock-apps of the Dock (or the Clip). */
wlmaker_dockapps_t *wlmaker_clip_dockapps(
    wlmaker_server_t *server_ptr,
    bool dock);

/**
 * Attaches a dock-app to column `column` (the Clip, the Dock or a drawer)
 * at `index`, counted from the anchor among the dock-apps there. Moves it
 * within, or from another column. The order is stored.
 */
void wlmaker_clip_move_dockapp(
    wlmaker_server_t *server_ptr,
    wlmtk_tile_t *tile_ptr,
    int column,
    size_t index);

/**
 * Fills `column_ptr` with the geometry of the Dock's (or the Clip's)
 * column of tiles.
 *
 * @return false if there is no such dock.
 */
bool wlmaker_clip_column(
    wlmaker_server_t *server_ptr,
    bool dock,
    wlmaker_dock_column_t *column_ptr);

/** @return The level of the Dock (or the Clip). */
wlmaker_dock_level_t wlmaker_clip_get_level(
    wlmaker_server_t *server_ptr,
    bool dock);

/**
 * Sets the level of the Dock (or the Clip): Stores it in the state, as
 * Window Maker's `Lowered` and `AutoRaiseLower`, and moves the dock (and its
 * launchers' and dock-apps' surfaces) to the layer.
 */
void wlmaker_clip_set_level(
    wlmaker_server_t *server_ptr,
    bool dock,
    wlmaker_dock_level_t level);

/**
 * @return The layer the Dock's (or the Clip's) surfaces belong in now: The
 *     top layer, unless lowered.
 */
wlmtk_desktop_layer_t wlmaker_clip_layer(
    wlmaker_server_t *server_ptr,
    bool dock);

/** The pointer is on the Dock (or the Clip): Raises it, if auto-raising. */
void wlmaker_clip_dock_hovered(
    wlmaker_server_t *server_ptr,
    bool dock);

/** The pointer left the Dock (or the Clip): Lowers it, if auto-raising. */
void wlmaker_clip_dock_left(
    wlmaker_server_t *server_ptr,
    bool dock);

/** @return Path of the dock-apps state file (`Dockapps.plist`), or NULL. */
const char *wlmaker_clip_dockapps_fname(wlmaker_server_t *server_ptr);

/** Unit test set. */
extern const bs_test_set_t wlmaker_clip_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __CLIP_H__ */
/* == End of clip.h ======================================================== */
