/* ========================================================================= */
/**
 * @file drawer.h
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
#ifndef __WLMAKER_DRAWER_H__
#define __WLMAKER_DRAWER_H__

#include <stdbool.h>
#include <stddef.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

/** Forward declaration: One of Window Maker's drawers. */
typedef struct _wlmaker_drawer_t wlmaker_drawer_t;

#include "dockapps.h"
#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

struct _wlmaker_dock_column_t;
struct wlr_box;

/**
 * Creates a drawer, as Window Maker's: A tile on the Dock (shown by
 * `wlmdock`, from a launcher with `Drawer = Yes`) that opens a row of
 * icons, sticking out of the Dock: launchers (`Launchers` of the drawer's
 * entry), then dock-apps (stored as "Drawer.<Id>" in Dockapps.plist).
 *
 * @param server_ptr
 * @param id_ptr              The drawer's `Id`.
 *
 * @return The drawer, or NULL on error.
 */
wlmaker_drawer_t *wlmaker_drawer_create(
    wlmaker_server_t *server_ptr,
    const char *id_ptr);

/**
 * Destroys the drawer. Its dock-apps are placed on the Clip.
 *
 * @param drawer_ptr
 */
void wlmaker_drawer_destroy(wlmaker_drawer_t *drawer_ptr);

/**
 * Updates the drawer from its entry in the Dock's `Launchers`: Its icons,
 * options, and where it is.
 *
 * @param drawer_ptr
 * @param dict_ptr            The drawer's entry.
 * @param launcher_index      Its index among the Dock's launchers.
 */
void wlmaker_drawer_update(
    wlmaker_drawer_t *drawer_ptr,
    bspl_dict_t *dict_ptr,
    size_t launcher_index);

/** Places the drawer's row next to its tile, eg. after the Dock moved. */
void wlmaker_drawer_update_position(wlmaker_drawer_t *drawer_ptr);

/** @return The drawer's `Id`. */
const char *wlmaker_drawer_id(wlmaker_drawer_t *drawer_ptr);

/** @return The drawer's index among the Dock's launchers. */
size_t wlmaker_drawer_launcher_index(wlmaker_drawer_t *drawer_ptr);

/** @return The drawer's dock-apps. */
wlmaker_dockapps_t *wlmaker_drawer_dockapps(wlmaker_drawer_t *drawer_ptr);

/** @return Number of launchers in the drawer. */
size_t wlmaker_drawer_launchers(wlmaker_drawer_t *drawer_ptr);

/** @return Whether the drawer holds no icons. */
bool wlmaker_drawer_empty(wlmaker_drawer_t *drawer_ptr);

/** @return Whether the drawer is closed ("Collapsed"). */
bool wlmaker_drawer_collapsed(wlmaker_drawer_t *drawer_ptr);

/** Opens (or closes) the drawer. */
void wlmaker_drawer_set_collapsed(wlmaker_drawer_t *drawer_ptr, bool collapsed);

/** @return Whether the drawer closes itself ("Autocollapse"). */
bool wlmaker_drawer_autocollapse(wlmaker_drawer_t *drawer_ptr);

/**
 * The pointer is on the drawer's tile, or its row: With "Autocollapse",
 * opens after Window Maker's ClipAutoexpandDelay (600 ms), and stays open.
 */
void wlmaker_drawer_hovered(wlmaker_drawer_t *drawer_ptr);

/**
 * The pointer left the drawer's tile, or its row: With "Autocollapse",
 * closes after ClipAutocollapseDelay (1 s), unless back on it.
 */
void wlmaker_drawer_left(wlmaker_drawer_t *drawer_ptr);

/** Moves the drawer's row to the layer of the Dock's level. */
void wlmaker_drawer_update_layer(wlmaker_drawer_t *drawer_ptr);

/**
 * Fills the geometry of the drawer as a column of tiles: The drawer's tile
 * first, then its launchers and dock-apps, away from the Dock.
 *
 * @return false if there is no Dock.
 */
bool wlmaker_drawer_column(
    wlmaker_drawer_t *drawer_ptr,
    struct _wlmaker_dock_column_t *column_ptr);

/**
 * Fills `box_ptr` with the open drawer's row of icons (without its tile on
 * the Dock), in layout coordinates.
 *
 * @return false if closed, or empty.
 */
bool wlmaker_drawer_row_box(
    wlmaker_drawer_t *drawer_ptr,
    struct wlr_box *box_ptr);

/**
 * Creates the entry of a new drawer, for the Dock's `Launchers`: With an
 * `Id` not in `launchers_ptr`, the icon `icon_ptr`, and Autocollapse.
 *
 * @return The entry, or NULL on error.
 */
bspl_dict_t *wlmaker_drawer_create_entry(
    bspl_array_t *launchers_ptr,
    const char *icon_ptr);

/** @return Whether `dict_ptr` is the entry of a drawer (`Drawer = Yes`). */
bool wlmaker_drawer_is_entry(bspl_dict_t *dict_ptr);

/**
 * @return A copy of `dict_ptr` with `key_ptr` set to `object_ptr` (or
 *     removed, if NULL). Unref with bspl_dict_unref.
 */
bspl_dict_t *wlmaker_drawer_dict_with(
    bspl_dict_t *dict_ptr,
    const char *key_ptr,
    bspl_object_t *object_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_drawer_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DRAWER_H__ */
/* == End of drawer.h ====================================================== */
