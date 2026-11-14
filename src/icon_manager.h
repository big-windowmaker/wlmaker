/* ========================================================================= */
/**
 * @file icon_manager.h
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
 */
#ifndef __ICON_MANAGER_H__
#define __ICON_MANAGER_H__

struct wl_display;

/** Forward declaration: Icon Manager handle. */
typedef struct _wlmaker_icon_manager_t wlmaker_icon_manager_t;

/** Forward declaration: Toplevel icon handle. */
typedef struct _wlmaker_toplevel_icon_t wlmaker_toplevel_icon_t;

#include <stddef.h>
#include <sys/types.h>

#include "task_list.h"

struct wlr_box;
struct wlr_layer_surface_v1;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates an icon manager.
 *
 * @param wl_display_ptr
 * @param server_ptr
 *
 * @return The handle of the icon manager or NULL on error. Must be destroyed
 *     by calling @ref wlmaker_icon_manager_destroy.
 */
wlmaker_icon_manager_t *wlmaker_icon_manager_create(
    struct wl_display *wl_display_ptr,
    wlmaker_server_t *server_ptr);

/**
 * Destroys the Icon Manager.
 *
 * @param icon_manager_ptr
 */
void wlmaker_icon_manager_destroy(
    wlmaker_icon_manager_t *icon_manager_ptr);

/**
 * Adopts a layer surface as a dock-app: Its surface becomes the content of
 * a tile, placed on the Dock, the Clip or a drawer like the dock-apps of the
 * icon protocol (by the client's name), and handled as they are: dragged,
 * removed, its menu. Its popups show above the dock. The surface keeps the
 * size it asks for (up to the tile's); the right button goes to it where it
 * takes input (its input region), the tile's menu elsewhere. Layer, anchor
 * and margins are ignored. For layer-shell clients with the namespace
 * "wlmaker-dockapp", eg. the system tray.
 *
 * @param icon_manager_ptr
 * @param wlr_layer_surface_v1_ptr
 *
 * @return true if adopted: The tile goes with the layer surface.
 */
bool wlmaker_icon_manager_adopt_layer_surface(
    wlmaker_icon_manager_t *icon_manager_ptr,
    struct wlr_layer_surface_v1 *wlr_layer_surface_v1_ptr);

struct wlr_xwayland_surface;

/**
 * Adopts an X11 dock-app's icon window (shown through XWayland) as a
 * dock-app: Its surface is the content of a tile, placed by `name_ptr` like
 * the other dock-apps, dragged and removed as they are. The X11 program gets
 * the pointer on its window, also the right button (the tile's menu is on
 * the border), as on Window Maker. The window is configured at the place it
 * is shown at, for its popups.
 *
 * @param icon_manager_ptr
 * @param wlr_xwayland_surface_ptr The icon window's surface.
 * @param name_ptr            The dock-app's name (WM_CLASS instance), or
 *                            NULL.
 * @param pid                 The dock-app's process, or 0.
 *
 * @return The tile's icon, or NULL if not adopted. Release it with
 *     @ref wlmaker_icon_manager_release_xwayland_icon when the window goes.
 */
wlmaker_toplevel_icon_t *wlmaker_icon_manager_adopt_xwayland_surface(
    wlmaker_icon_manager_t *icon_manager_ptr,
    struct wlr_xwayland_surface *wlr_xwayland_surface_ptr,
    const char *name_ptr,
    pid_t pid);

/**
 * Sets the shape of an adopted X11 icon window: Only these rectangles of
 * it (in its coordinates) are shown, the tile shows around them; as Window
 * Maker shows a shaped icon window (X Shape extension). NULL or none: All.
 *
 * @param toplevel_icon_ptr
 * @param rects_ptr
 * @param rects
 */
void wlmaker_icon_manager_set_xwayland_shape(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr,
    const struct wlr_box *rects_ptr,
    size_t rects);

/** Releases what @ref wlmaker_icon_manager_adopt_xwayland_surface adopted. */
void wlmaker_icon_manager_release_xwayland_icon(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __ICON_MANAGER_H__ */
/* == End of icon_manager.h ================================================ */
