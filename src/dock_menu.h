/* ========================================================================= */
/**
 * @file dock_menu.h
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
#ifndef __WLMAKER_DOCK_MENU_H__
#define __WLMAKER_DOCK_MENU_H__

#include <stdbool.h>
#include <sys/types.h>

#include <libbase/libbase.h>

#include "context_menu.h"
#include "dockapp_prefs.h"
#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** What a docked icon is, for the items of its menu. */
typedef struct {
    /** Application ID: Its windows are hidden, brought here or killed. */
    const char                *app_id_ptr;
    /** Command, for "Launch". May be NULL. */
    const char                *command_ptr;
    /** Process of a dock-app, for "Kill". 0 if not a dock-app. */
    pid_t                     pid;
    /** Name of the dock-app, for "Settings...". NULL if not a dock-app. */
    const char                *dockapp_name_ptr;
    /** The column: The Clip, the Dock, or a drawer's (@ref WLMAKER_COLUMN_CLIP). */
    int                       column;
    /** Index of the launcher: -1 for the Dock's main tile, or a dock-app. */
    int                       launcher_index;
    /** Whether a drawer's tile, on the Dock: "Remove drawer", not "Kill". */
    bool                      drawer;
    /** A dock-app's preferences: Its "Settings..." submenu. May be NULL. */
    wlmaker_dockapp_prefs_t   *prefs_ptr;
} wlmaker_dock_menu_icon_t;

/**
 * Opens Window Maker's menu for a launcher of the Dock or the Clip (shown
 * by `wlmdock`), or of a drawer; or for the Dock's main tile.
 *
 * @param server_ptr
 * @param column              The column, see @ref WLMAKER_COLUMN_CLIP.
 * @param index               Index of the launcher. -1 for the main tile.
 */
void wlmaker_dock_menu_open_launcher(
    wlmaker_server_t *server_ptr,
    int column,
    int index);

/**
 * Adds Window Maker's first items of the Dock's menu ("Dock position", "Add
 * a drawer"), the Clip's ("Clip Options") or a drawer's ("Drawer options",
 * "Remove Icon"), for the icon `icon_ptr`.
 *
 * @return true on success.
 */
bool wlmaker_dock_menu_add_dock_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    const wlmaker_dock_menu_icon_t *icon_ptr);

/**
 * Adds Window Maker's items for a docked icon: "Launch", "Bring Here",
 * "Hide", "Settings..." and "Kill" (or "Remove drawer", for a drawer's
 * tile). Those that don't apply are disabled.
 *
 * @return true on success.
 */
bool wlmaker_dock_menu_add_icon_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    const wlmaker_dock_menu_icon_t *icon_ptr);

/**
 * @return Path of the logo for dialogs: The icon of the Dock's main tile,
 *     as Window Maker's panels show its logo. To be free()-ed, or NULL.
 */
char *wlmaker_dock_menu_logo_path(wlmaker_server_t *server_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dock_menu_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCK_MENU_H__ */
/* == End of dock_menu.h =================================================== */
