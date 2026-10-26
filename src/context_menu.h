/* ========================================================================= */
/**
 * @file context_menu.h
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
#ifndef __WLMAKER_CONTEXT_MENU_H__
#define __WLMAKER_CONTEXT_MENU_H__

#include <stdbool.h>

#include <libbase/libbase.h>

#include "toolkit/toolkit.h"

/** Forward declaration: A context (right-click) menu. */
typedef struct _wlmaker_context_menu_t wlmaker_context_menu_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Callback for a triggered item of a context menu. */
typedef void (*wlmaker_context_menu_callback_t)(void *ud_ptr);

/**
 * Creates a context menu: A right-click menu, drawn like the root menu, for
 * an element like the Clip or a dock-app's tile. It is opened by @ref
 * wlmaker_context_menu_open and destroys itself once closed.
 *
 * @param server_ptr
 * @param owner_ptr           Identifies the element the menu is for. See
 *                            @ref wlmaker_context_menu_cancel.
 * @param title_ptr           Title of the menu's window.
 *
 * @return The context menu, or NULL on error.
 */
wlmaker_context_menu_t *wlmaker_context_menu_create(
    wlmaker_server_t *server_ptr,
    void *owner_ptr,
    const char *title_ptr);

/** Destroys the context menu. */
void wlmaker_context_menu_destroy(wlmaker_context_menu_t *context_menu_ptr);

/**
 * Adds an item to the context menu.
 *
 * @param context_menu_ptr
 * @param text_ptr
 * @param callback            Called with `ud_ptr` when the item is triggered.
 *                            The menu is closed afterwards.
 * @param ud_ptr
 *
 * @return true on success.
 */
bool wlmaker_context_menu_add_item(
    wlmaker_context_menu_t *context_menu_ptr,
    const char *text_ptr,
    wlmaker_context_menu_callback_t callback,
    void *ud_ptr);

/**
 * Opens the context menu in right-click mode, at the pointer's position and
 * within the usable area of the output: Not covering docks and panels.
 *
 * The server takes ownership: An already open context menu is closed. The
 * menu is destroyed once it gets closed.
 *
 * @param context_menu_ptr
 */
void wlmaker_context_menu_open(wlmaker_context_menu_t *context_menu_ptr);

/**
 * Closes the server's open context menu, if it was opened for `owner_ptr`.
 * To call when the owner goes away, since the items refer to it.
 *
 * @param server_ptr
 * @param owner_ptr           Or NULL, to close any open context menu.
 */
void wlmaker_context_menu_cancel(wlmaker_server_t *server_ptr,
                                 void *owner_ptr);

/** Returns the toolkit menu of the context menu. */
wlmtk_menu_t *wlmaker_context_menu_menu(
    wlmaker_context_menu_t *context_menu_ptr);

/**
 * Computes the position for a box of `width` x `height` at (x, y), moved
 * such that it stays within `area_ptr` (or at its top-left corner, if it is
 * larger).
 *
 * @param area_ptr
 * @param x
 * @param y
 * @param width
 * @param height
 *
 * @return The box at its position.
 */
struct wlr_box wlmaker_context_menu_confine(
    const struct wlr_box *area_ptr,
    int x, int y, int width, int height);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_context_menu_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_CONTEXT_MENU_H__ */
/* == End of context_menu.h ================================================ */
