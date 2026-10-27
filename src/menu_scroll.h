/* ========================================================================= */
/**
 * @file menu_scroll.h
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
#ifndef __WLMAKER_MENU_SCROLL_H__
#define __WLMAKER_MENU_SCROLL_H__

#include <stdbool.h>

#include <libbase/libbase.h>

#define WLR_USE_UNSTABLE
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

#include "toolkit/toolkit.h"

/** Forward declaration: Scrolls a menu at the screen's edge. */
typedef struct _wlmaker_menu_scroll_t wlmaker_menu_scroll_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Scrolls (slides) a menu that extends beyond the usable area, like Window
 * Maker does: While the menu is open and the pointer is at the edge of the
 * output, next to the menu, the menu's window moves in steps until its far
 * end is within the usable area. With all its open submenus.
 *
 * @param server_ptr
 * @param window_ptr          The window holding the menu.
 * @param menu_ptr            The (top-level) menu.
 *
 * @return The scroller, or NULL on error. Must be destroyed by
 *     @ref wlmaker_menu_scroll_destroy, before the window and menu.
 */
wlmaker_menu_scroll_t *wlmaker_menu_scroll_create(
    wlmaker_server_t *server_ptr,
    wlmtk_window_t *window_ptr,
    wlmtk_menu_t *menu_ptr);

/** Destroys the scroller. */
void wlmaker_menu_scroll_destroy(wlmaker_menu_scroll_t *menu_scroll_ptr);

/**
 * Computes how far to scroll the menu, like Window Maker.
 *
 * Horizontally, if the pointer is at the left or right edge of the output,
 * within the vertical span of the menu, and the menu extends beyond that
 * side of the usable area: Towards that side's other end, at most `step`.
 * Vertically likewise.
 *
 * @param output_ptr          The output's box: Its edges are where to scroll.
 * @param area_ptr            The usable area, where the menu should get to.
 * @param extents_ptr         The menu with its open submenus.
 * @param x                   The pointer's position.
 * @param y                   The pointer's position.
 * @param step                Maximum distance to move.
 * @param dx_ptr              Set to the horizontal movement.
 * @param dy_ptr              Set to the vertical movement.
 *
 * @return true if the menu shall move.
 */
bool wlmaker_menu_scroll_amount(
    const struct wlr_box *output_ptr,
    const struct wlr_box *area_ptr,
    const struct wlr_box *extents_ptr,
    double x,
    double y,
    int step,
    int *dx_ptr,
    int *dy_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_menu_scroll_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_MENU_SCROLL_H__ */
/* == End of menu_scroll.h ================================================= */
