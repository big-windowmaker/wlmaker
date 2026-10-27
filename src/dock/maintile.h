/* ========================================================================= */
/**
 * @file maintile.h
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
#ifndef __WLMDOCK_MAINTILE_H__
#define __WLMDOCK_MAINTILE_H__

#include <libbase/libbase.h>

#include "toolkit/toolkit.h"
#include "util/files.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Forward declaration: The Dock's main tile. */
typedef struct _wlmdock_maintile_t wlmdock_maintile_t;

/** Handler for a right-click on the main tile, eg. to open its menu. */
typedef void (*wlmdock_maintile_menu_handler_t)(
    wlmdock_maintile_t *maintile_ptr,
    void *ud_ptr);

/**
 * Creates the Dock's main tile: Like the Dock's top tile on Window Maker, it
 * is always there (and can't be removed), carries the Dock's menu and is the
 * handle for dragging the Dock. Shows an icon.
 *
 * @param style_ptr
 * @param icon_ptr            Icon file: A path, or a name looked up in
 *                            `${XDG_DATA_DIRS}/wlmaker/icons`. May be NULL.
 *                            If not found, the icon configured at build time
 *                            (`WLMDOCK_DEFAULT_ICON`) or `dock-56x56.png` is
 *                            used; failing all, the tile has no icon.
 * @param files_ptr
 *
 * @return The main tile, or NULL on error.
 */
wlmdock_maintile_t *wlmdock_maintile_create(
    const struct wlmtk_tile_style *style_ptr,
    const char *icon_ptr,
    wlm_util_files_t *files_ptr);

/** Destroys the main tile. */
void wlmdock_maintile_destroy(wlmdock_maintile_t *maintile_ptr);

/** @return The @ref wlmtk_tile_t superclass of `maintile_ptr`. */
wlmtk_tile_t *wlmdock_maintile_tile(wlmdock_maintile_t *maintile_ptr);

/** @return The main tile for `tile_ptr`, if it is one. Else NULL. */
wlmdock_maintile_t *wlmdock_maintile_from_tile(wlmtk_tile_t *tile_ptr);

/** Sets the handler for a right-click on the main tile. */
void wlmdock_maintile_set_menu_handler(
    wlmdock_maintile_t *maintile_ptr,
    wlmdock_maintile_menu_handler_t handler,
    void *ud_ptr);

/**
 * Resolves an icon file: A path (starting with '/' or '~'), or a name looked
 * up in `${XDG_DATA_DIRS}/wlmaker/icons`.
 *
 * @param icon_ptr
 * @param files_ptr
 *
 * @return The path of an existing file, to be free()-ed. NULL if not found.
 */
char *wlmdock_resolve_icon(const char *icon_ptr, wlm_util_files_t *files_ptr);

/** Unit test set. */
extern const bs_test_set_t wlmdock_maintile_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // __WLMDOCK_MAINTILE_H__
/* == End of maintile.h ==================================================== */
