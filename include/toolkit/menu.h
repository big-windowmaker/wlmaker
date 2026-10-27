/* ========================================================================= */
/**
 * @file menu.h
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
#ifndef __WLMTK_MENU_H__
#define __WLMTK_MENU_H__

/** Forward declaration: Menu handle. */
typedef struct _wlmtk_menu_t wlmtk_menu_t;

/** Forward declaration: Reference to the menu's style. */
typedef struct _wlmtk_menu_style_ref_t wlmtk_menu_style_ref_t;

enum wlmtk_menu_mode;  // IWYU pragma: keep

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

#include "base.h"
#include "box.h"  // IWYU pragma: keep
#include "element.h"
#include "menu_item.h"  // IWYU pragma: keep
#include "style.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Style definition for the menu. */
struct wlmtk_menu_style  {
    /** Margin. */
    struct wlmtk_margin_style margin;
    /** Border. */
    struct wlmtk_margin_style border;
    /** Item's style. */
    struct wlmtk_menu_item_style item;
};

/** Modes of the menu. */
enum wlmtk_menu_mode {
    /** Normal (window) mode of a menu: Left button click triggers items. */
    WLMTK_MENU_MODE_NORMAL,
    /**
     * Right-click mode of menu: Menu is invoked while right button is pressed.
     * Releasing the right button triggers items.
     */
    WLMTK_MENU_MODE_RIGHTCLICK
};

/** Events of the popup menu. */
typedef struct {
    /**
     * The menu was opened or closed. Check through @ref wlmtk_menu_is_open.
     * The signal's argument is a pointer to the menu that opened/closed.
     */
    struct wl_signal          open_changed;
    /** Popup menu requests to be closed. */
    struct wl_signal          request_close;
    /** The dtor has been called. */
    struct wl_signal          destroy;
} wlmtk_menu_events_t;

/**
 * Creates a menu.
 *
 * @param style_ref_ptr
 *
 * @return Pointer to the menu state or NULL on error.
 */
wlmtk_menu_t *wlmtk_menu_create(
    wlmtk_menu_style_ref_t *style_ref_ptr);

/**
 * Destroys the menu.
 *
 * @param menu_ptr
 */
void wlmtk_menu_destroy(wlmtk_menu_t *menu_ptr);

/** @return pointer to the menu's @ref wlmtk_element_t superclass. */
wlmtk_element_t *wlmtk_menu_element(wlmtk_menu_t *menu_ptr);

/** @return pointer to the menu's @ref wlmtk_base_t superclass. */
wlmtk_base_t *wlmtk_menu_base(wlmtk_menu_t *menu_ptr);

/** @return a pointer to @ref wlmtk_menu_t::events. */
wlmtk_menu_events_t *wlmtk_menu_events(wlmtk_menu_t *menu_ptr);

/**
 * Sets the menu's style.
 *
 * @param menu_ptr
 * @param style_ref_ptr       Reference to the new style.
 *
 * @return true on success.
 */
bool wlmtk_menu_set_style(
    wlmtk_menu_t *menu_ptr,
    wlmtk_menu_style_ref_t *style_ref_ptr);

/**
 * Opens the menu: Makes it visible or invisible, and resets state
 * if needed.
 *
 * @param menu_ptr
 * @param opened
 */
void wlmtk_menu_set_open(wlmtk_menu_t *menu_ptr, bool opened);

/** @return whether the menu is open, ie. visible. */
bool wlmtk_menu_is_open(wlmtk_menu_t *menu_ptr);

/**
 * Sets the mode of the menu.
 *
 * @param menu_ptr
 * @param mode
 */
void wlmtk_menu_set_mode(wlmtk_menu_t *menu_ptr,
                         enum wlmtk_menu_mode mode);

/** @return mode of the menu. @see wlmtk_menu_set_mode. */
enum wlmtk_menu_mode wlmtk_menu_get_mode(wlmtk_menu_t *menu_ptr);

/**
 * Adds a menu item to the menu.
 *
 * @param menu_ptr
 * @param menu_item_ptr
 */
void wlmtk_menu_add_item(wlmtk_menu_t *menu_ptr,
                         wlmtk_menu_item_t *menu_item_ptr);

/**
 * Removes a menu item from the menu.
 *
 * @param menu_ptr
 * @param menu_item_ptr
 */
void wlmtk_menu_remove_item(wlmtk_menu_t *menu_ptr,
                            wlmtk_menu_item_t *menu_item_ptr);

/**
 * Registers `menu_item_ptr` as this submenu's parent item.
 *
 * @protected Must be called only from @ref wlmtk_menu_item_set_submenu.
 *
 * @param menu_ptr
 * @param menu_item_ptr
 */
void wlmtk_menu_set_parent_item(wlmtk_menu_t *menu_ptr,
                                wlmtk_menu_item_t *menu_item_ptr);

/** @return the parent menu item, if this is a submenu. NULL otherwise. */
wlmtk_menu_item_t *wlmtk_menu_get_parent_item(wlmtk_menu_t *menu_ptr);

/**
 * Sets the bounds for the menu's submenus, eg. the usable area of the output.
 * In the coordinates of @ref wlmtk_element_get_absolute_position. Like on
 * Window Maker: A submenu opens to the left of its item, if it would extend
 * beyond the right of the bounds; and moves up, to not extend beyond the
 * bottom.
 *
 * @param menu_ptr
 * @param bounds_ptr          The bounds, or NULL for none.
 */
void wlmtk_menu_set_bounds(wlmtk_menu_t *menu_ptr,
                           const struct wlr_box *bounds_ptr);

/**
 * @return The bounds for the submenus of `menu_ptr`: Those set for the
 *     top-level menu. An empty box, if there are none.
 */
struct wlr_box wlmtk_menu_get_bounds(wlmtk_menu_t *menu_ptr);

/**
 * Sets whether the menu's submenus prefer to open to the left. Like on
 * Window Maker, a submenu that opened to the left, opens its submenus to the
 * left, too.
 *
 * @protected Called when placing the submenu of a @ref wlmtk_menu_item_t.
 *
 * @param menu_ptr
 * @param opens_left
 */
void wlmtk_menu_set_opens_left(wlmtk_menu_t *menu_ptr, bool opens_left);

/** @return Whether the submenus prefer to open to the left. */
bool wlmtk_menu_opens_left(wlmtk_menu_t *menu_ptr);

/**
 * @return The box covered by the menu and its open submenus, in the
 *     coordinates of @ref wlmtk_element_get_absolute_position.
 */
struct wlr_box wlmtk_menu_get_extents(wlmtk_menu_t *menu_ptr);

/**
 * Computes where to place a submenu of `width` x `height` for an item: To
 * the right of the item, or to the left if it does not fit there (or if
 * `prefer_left` and it fits on the left). If it fits on neither side, on the
 * side with more room. Aligned with the item's top, but moved up to not
 * extend beyond the bottom, and down to not extend beyond the top.
 *
 * @param bounds_ptr          The bounds. If empty: To the right, aligned.
 * @param item_left           Left edge of the item.
 * @param item_right          Right edge of the item.
 * @param item_top            Top edge of the item.
 * @param width
 * @param height
 * @param prefer_left
 * @param x_ptr               Set to the left edge of the submenu.
 * @param y_ptr               Set to the top edge of the submenu.
 *
 * @return true if the submenu opens to the left.
 */
bool wlmtk_menu_place_submenu(
    const struct wlr_box *bounds_ptr,
    int item_left,
    int item_right,
    int item_top,
    int width,
    int height,
    bool prefer_left,
    int *x_ptr,
    int *y_ptr);

/**
 * Requests that menu_item_ptr be highlighted.
 *
 * @param menu_ptr
 * @param menu_item_ptr
 */
void wlmtk_menu_request_item_highlight(
    wlmtk_menu_t *menu_ptr,
    wlmtk_menu_item_t *menu_item_ptr);

/**
 * @param menu_ptr
 *
 * @return the size of @ref wlmtk_menu_t::items.
 */
size_t wlmtk_menu_items_size(wlmtk_menu_t *menu_ptr);

/**
 * @param menu_ptr
 * @param i
 *
 * @return the i-th element of @ref wlmtk_menu_t::items, or NULL if i is out
 *     of bounds.
 */
wlmtk_menu_item_t *wlmtk_menu_item_at(wlmtk_menu_t *menu_ptr, size_t i);

/** Creates a holder for the menu style, with initialized reference.
 *
 * @return Pointer to @ref wlmtk_menu_style. To destroy, call
 *     @ref wlmtk_menu_style_ref_release on @ref wlmtk_menu_style_to_ref.
 */
struct wlmtk_menu_style *wlmtk_menu_style_create(void);

/** @return the @ref wlmtk_menu_style_ref_t for the style. */
wlmtk_menu_style_ref_t *wlmtk_menu_style_to_ref(
    struct wlmtk_menu_style *menu_style_ptr);

/** Retains a reference. @return a read-only style. */
const struct wlmtk_menu_style *wlmtk_menu_style_ref_retain(
    wlmtk_menu_style_ref_t *ref_ptr);
/** Releases a reference. */
void wlmtk_menu_style_ref_release(wlmtk_menu_style_ref_t *ref_ptr);

/** Decodes dict at `object_ptr` into @ref wlmtk_menu_style at `value_ptr` */
bool wlmtk_menu_style_decode(
    bspl_object_t *object_ptr,
   const union bspl_desc_value *desc_value_ptr,
    void *value_ptr);
/** Initializer. */
bool wlmtk_menu_style_decode_init(void *dst_ptr);
/** Fini. */
void wlmtk_menu_style_decode_fini(void *dst_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmtk_menu_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMTK_MENU_H__ */
/* == End of menu.h ======================================================== */
