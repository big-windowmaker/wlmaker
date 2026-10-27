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
    /** Whether on the Dock (else on the Clip). */
    bool                      dock;
    /** Index of the launcher: -1 for the Dock's main tile, or a dock-app. */
    int                       launcher_index;
    /** A dock-app's preferences: Its "Settings..." submenu. May be NULL. */
    wlmaker_dockapp_prefs_t   *prefs_ptr;
} wlmaker_dock_menu_icon_t;

/**
 * Opens Window Maker's menu for a launcher of the Dock or the Clip (shown
 * by `wlmdock`), or for the Dock's main tile.
 *
 * @param server_ptr
 * @param dock                Whether on the Dock (else on the Clip).
 * @param index               Index of the launcher. -1 for the main tile.
 */
void wlmaker_dock_menu_open_launcher(
    wlmaker_server_t *server_ptr,
    bool dock,
    int index);

/**
 * Adds Window Maker's first items of the Dock's menu ("Dock position", "Add
 * a drawer") or the Clip's ("Clip Options").
 *
 * @return true on success.
 */
bool wlmaker_dock_menu_add_dock_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    bool dock);

/**
 * Adds Window Maker's items for a docked icon: "Launch", "Bring Here",
 * "Hide", "Settings..." and "Kill". Those that don't apply are disabled.
 *
 * @return true on success.
 */
bool wlmaker_dock_menu_add_icon_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    const wlmaker_dock_menu_icon_t *icon_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dock_menu_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCK_MENU_H__ */
/* == End of dock_menu.h =================================================== */
