/* ========================================================================= */
/**
 * @file menu.c
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

#include "menu.h"

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "base.h"
#include "input.h"
#include "util.h"

/* == Declarations ========================================================= */

/** State of the menu. */
struct _wlmtk_menu_t {
    /** Composed of a base element. */
    wlmtk_base_t              base;
    /** And, of a box, holding menu items. */
    wlmtk_box_t               box;
    /** Style of the menu. */
    const struct wlmtk_menu_style *style_ptr;
    /** Reference to the menu style. */
    wlmtk_menu_style_ref_t    *style_ref_ptr;

    /** Signals that can be raised by the menu. */
    wlmtk_menu_events_t       events;
    /** Virtual method table of @ref wlmtk_menu_t::base, before extending. */
    wlmtk_element_vmt_t       orig_base_element_vmt;

    /** List of menu items, via @ref wlmtk_menu_item_t::dlnode. */
    bs_dllist_t               items;
    /** The currently-highlighted menu item, or NULL if none. */
    wlmtk_menu_item_t         *highlighted_menu_item_ptr;
    /** If this is a submenu, this points to the parent menu item. */
    wlmtk_menu_item_t         *parent_menu_item_ptr;
    /** Current mode of the menu. */
    enum wlmtk_menu_mode      mode;
    /** Bounds for the submenus. See @ref wlmtk_menu_set_bounds. */
    struct wlr_box            bounds;
    /** Whether submenus prefer to open to the left. */
    bool                      opens_left;

    /**
     * Right-click mode: Whether a pointer position was recorded since the
     * menu was opened. See @ref wlmtk_menu_t::rightclick_moved.
     */
    bool                      rightclick_has_position;
    /** Right-click mode: The first pointer position seen after opening. */
    double                    rightclick_x;
    /** Right-click mode: The first pointer position seen after opening. */
    double                    rightclick_y;
    /**
     * Right-click mode: Whether the pointer moved since the menu was opened.
     * If the right button is released without the pointer having moved, it
     * was a click: The menu stays open, see @ref wlmtk_menu_t::sticky.
     */
    bool                      rightclick_moved;
    /**
     * Right-click mode: The menu was opened by a click, and stays open.
     * Items are then triggered by releasing either button over them, and a
     * button press outside the menu closes it.
     */
    bool                      sticky;
};

/** Type-safe holder for the menu style's reference counter. */
struct _wlmtk_menu_style_ref_t {
    /** Actual reference counter. */
    bs_ref_t                  reference;
};
/** Holds the reference and the style. */
struct wlmtk_menu_style_holder {
    /** Reference counter */
    struct _wlmtk_menu_style_ref_t msr;
    /** Style */
    struct wlmtk_menu_style   style;
};

static void _wlmtk_menu_eliminate_item(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static bool _wlmtk_menu_set_item_style(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmtk_menu_set_item_mode(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmtk_menu_update_item_shown(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);

static void _wlmtk_menu_box_element_destroy(wlmtk_element_t *element_ptr);

static void _wlmtk_menu_element_destroy(
    wlmtk_element_t *element_ptr);
static bool _wlmtk_menu_element_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);
static bool _wlmtk_menu_element_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr);
static bool _wlmtk_menu_sticky_pointer_button(
    wlmtk_menu_t *menu_ptr,
    const wlmtk_button_event_t *button_event_ptr);
static wlmtk_menu_item_t *_wlmtk_menu_leaf_item_under_pointer(
    wlmtk_menu_t *menu_ptr);
static void _wlmtk_menu_reset_rightclick(wlmtk_menu_t *menu_ptr);
static bool _wlmtk_menu_element_keyboard_sym(
    wlmtk_element_t *element_ptr,
    xkb_keysym_t keysym,
    enum xkb_key_direction direction,
    uint32_t modifiers);
static bs_dllist_node_t *_wlmtk_menu_this_or_next_non_disabled_dlnode(
    bs_dllist_node_t *dlnode_ptr,
    bs_dllist_node_iterator_t iterator);

static void _wlmtk_menu_style_destroy(bs_ref_t *ref_ptr);

/* == Data ================================================================= */

/** The superclass' element virtual method table. */
static const wlmtk_element_vmt_t _wlmtk_menu_element_vmt = {
    .keyboard_sym = _wlmtk_menu_element_keyboard_sym,
    .destroy = _wlmtk_menu_element_destroy,
    .pointer_button = _wlmtk_menu_element_pointer_button,
    .pointer_accepts_motion = _wlmtk_menu_element_pointer_accepts_motion,
};

/**
 * Distance (in pixels, per axis) the pointer must move in right-click mode
 * before releasing the right button triggers an item.
 */
static const double _wlmtk_menu_rightclick_move_threshold = 3.0;

/** Extra override for the box element. Does not have a dtor by default. */
static const wlmtk_element_vmt_t _wlmtk_menu_box_element_vmt = {
    .destroy = _wlmtk_menu_box_element_destroy
};

/** Descriptor for decoding the "Menu" dictionary. */
static const bspl_desc_t _wlmtk_menu_style_desc[] = {
    BSPL_DESC_DICT(
        "Item", true, struct wlmtk_menu_style, item, item,
        wlmtk_menu_item_style_desc),
    BSPL_DESC_DICT(
        "Margin", true, struct wlmtk_menu_style, margin, margin,
        wlmtk_style_margin_desc),
    BSPL_DESC_DICT(
        "Border", true, struct wlmtk_menu_style, border, border,
        wlmtk_style_margin_desc),
    BSPL_DESC_SENTINEL()
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmtk_menu_t *wlmtk_menu_create(wlmtk_menu_style_ref_t *style_ref_ptr)
{
    wlmtk_menu_t *menu_ptr = logged_calloc(1, sizeof(wlmtk_menu_t));
    if (NULL == menu_ptr) return NULL;
    menu_ptr->style_ref_ptr = style_ref_ptr;
    menu_ptr->style_ptr = wlmtk_menu_style_ref_retain(style_ref_ptr);

    if (!wlmtk_box_init(
            &menu_ptr->box,
            WLMTK_BOX_VERTICAL,
            &menu_ptr->style_ptr->margin)) {
        wlmtk_menu_destroy(menu_ptr);
        return NULL;
    }
    wlmtk_element_extend(
        wlmtk_box_element(&menu_ptr->box),
        &_wlmtk_menu_box_element_vmt);
    wlmtk_element_set_visible(wlmtk_box_element(&menu_ptr->box), true);

    if (!wlmtk_base_init(
            &menu_ptr->base,
            wlmtk_box_element(&menu_ptr->box))) {
        wlmtk_menu_destroy(menu_ptr);
        return NULL;
    }
    menu_ptr->orig_base_element_vmt = wlmtk_element_extend(
        wlmtk_base_element(&menu_ptr->base), &_wlmtk_menu_element_vmt);
    wlmtk_element_set_visible(wlmtk_base_element(&menu_ptr->base), false);

    wl_signal_init(&menu_ptr->events.open_changed);
    wl_signal_init(&menu_ptr->events.request_close);
    wl_signal_init(&menu_ptr->events.destroy);
    return menu_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_destroy(wlmtk_menu_t *menu_ptr)
{
    wl_signal_emit(&menu_ptr->events.destroy, NULL);

    // Must destroy the items before the base.
    bs_dllist_for_each(
        &menu_ptr->items,
        _wlmtk_menu_eliminate_item,
        menu_ptr);

    wlmtk_base_fini(&menu_ptr->base);

    if (NULL != menu_ptr->style_ref_ptr) {
        wlmtk_menu_style_ref_release(menu_ptr->style_ref_ptr);
    }
    free(menu_ptr);
}

/* ------------------------------------------------------------------------- */
wlmtk_element_t *wlmtk_menu_element(wlmtk_menu_t *menu_ptr)
{
    return wlmtk_base_element(&menu_ptr->base);
}

/* ------------------------------------------------------------------------- */
wlmtk_base_t *wlmtk_menu_base(wlmtk_menu_t *menu_ptr)
{
    return &menu_ptr->base;
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_events_t *wlmtk_menu_events(wlmtk_menu_t *menu_ptr)
{
    return &menu_ptr->events;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_menu_set_style(
    wlmtk_menu_t *menu_ptr,
    wlmtk_menu_style_ref_t *style_ref_ptr)
{
    wlmtk_menu_style_ref_t *old_ref_ptr = menu_ptr->style_ref_ptr;
    menu_ptr->style_ref_ptr = style_ref_ptr;
    menu_ptr->style_ptr = wlmtk_menu_style_ref_retain(style_ref_ptr);

    wlmtk_box_set_style(&menu_ptr->box, &menu_ptr->style_ptr->margin);

    bool rv = bs_dllist_all(
        &menu_ptr->items,
        _wlmtk_menu_set_item_style,
        style_ref_ptr);

    wlmtk_element_layout(wlmtk_menu_element(menu_ptr));
    wlmtk_element_invalidate_parent_layout(wlmtk_menu_element(menu_ptr));

    wlmtk_menu_style_ref_release(old_ref_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_set_open(wlmtk_menu_t *menu_ptr, bool opened)
{
    // Items of a closed menu hold no buffers; drawn when (re)opened. Also
    // when the menu's element was made visible by other means.
    if (wlmtk_menu_element(menu_ptr)->visible == opened) {
        bs_dllist_for_each(
            &menu_ptr->items, _wlmtk_menu_update_item_shown, NULL);
        return;
    }

    wlmtk_element_set_visible(wlmtk_menu_element(menu_ptr), opened);
    _wlmtk_menu_reset_rightclick(menu_ptr);
    bs_dllist_for_each(&menu_ptr->items, _wlmtk_menu_update_item_shown, NULL);

    if (NULL != menu_ptr->highlighted_menu_item_ptr) {
        wlmtk_menu_item_set_highlighted(
            menu_ptr->highlighted_menu_item_ptr, false);
        menu_ptr->highlighted_menu_item_ptr = NULL;
    }

    wl_signal_emit(&menu_ptr->events.open_changed, menu_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_menu_is_open(wlmtk_menu_t *menu_ptr)
{
    return wlmtk_menu_element(menu_ptr)->visible;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_set_mode(wlmtk_menu_t *menu_ptr,
                         enum wlmtk_menu_mode mode)
{
    if (menu_ptr->mode == mode) return;
    menu_ptr->mode = mode;
    _wlmtk_menu_reset_rightclick(menu_ptr);
    bs_dllist_for_each(
        &menu_ptr->items,
        _wlmtk_menu_set_item_mode,
        menu_ptr);
}

/* ------------------------------------------------------------------------- */
enum wlmtk_menu_mode wlmtk_menu_get_mode(wlmtk_menu_t *menu_ptr)
{
    return menu_ptr->mode;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_add_item(wlmtk_menu_t *menu_ptr,
                         wlmtk_menu_item_t *menu_item_ptr)
{
    bs_dllist_push_back(
        &menu_ptr->items,
        wlmtk_dlnode_from_menu_item(menu_item_ptr));
    wlmtk_box_add_element_back(
        &menu_ptr->box,
        wlmtk_menu_item_element(menu_item_ptr));
    wlmtk_menu_item_set_mode(menu_item_ptr, menu_ptr->mode);
    wlmtk_menu_item_set_parent_menu(menu_item_ptr, menu_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_remove_item(wlmtk_menu_t *menu_ptr,
                            wlmtk_menu_item_t *menu_item_ptr)
{
    if (menu_ptr->highlighted_menu_item_ptr == menu_item_ptr) {
        menu_ptr->highlighted_menu_item_ptr = NULL;
    }
    wlmtk_menu_item_set_parent_menu(menu_item_ptr, NULL);
    wlmtk_box_remove_element(
        &menu_ptr->box,
        wlmtk_menu_item_element(menu_item_ptr));
    bs_dllist_remove(
        &menu_ptr->items,
        wlmtk_dlnode_from_menu_item(menu_item_ptr));
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_set_parent_item(wlmtk_menu_t *menu_ptr,
                                wlmtk_menu_item_t *menu_item_ptr)
{
    menu_ptr->parent_menu_item_ptr = menu_item_ptr;
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_item_t *wlmtk_menu_get_parent_item(wlmtk_menu_t *menu_ptr)
{
    return menu_ptr->parent_menu_item_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_set_bounds(wlmtk_menu_t *menu_ptr,
                           const struct wlr_box *bounds_ptr)
{
    if (NULL == bounds_ptr) {
        menu_ptr->bounds = (struct wlr_box){};
    } else {
        menu_ptr->bounds = *bounds_ptr;
    }
}

/* ------------------------------------------------------------------------- */
struct wlr_box wlmtk_menu_get_bounds(wlmtk_menu_t *menu_ptr)
{
    while (NULL != menu_ptr->parent_menu_item_ptr &&
           NULL != wlmtk_menu_item_get_parent_menu(
               menu_ptr->parent_menu_item_ptr)) {
        menu_ptr = wlmtk_menu_item_get_parent_menu(
            menu_ptr->parent_menu_item_ptr);
    }
    return menu_ptr->bounds;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_set_opens_left(wlmtk_menu_t *menu_ptr, bool opens_left)
{
    menu_ptr->opens_left = opens_left;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_menu_opens_left(wlmtk_menu_t *menu_ptr)
{
    return menu_ptr->opens_left;
}

/* ------------------------------------------------------------------------- */
struct wlr_box wlmtk_menu_get_extents(wlmtk_menu_t *menu_ptr)
{
    wlmtk_element_t *e = wlmtk_menu_element(menu_ptr);
    int x, y;
    wlmtk_element_get_absolute_position(e, &x, &y);
    struct wlr_box box = wlmtk_element_get_dimensions_box(e);
    box.x += x;
    box.y += y;

    for (bs_dllist_node_t *dlnode_ptr = menu_ptr->items.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_menu_t *submenu_ptr = wlmtk_menu_item_get_submenu(
            wlmtk_menu_item_from_dlnode(dlnode_ptr));
        if (NULL == submenu_ptr || !wlmtk_menu_is_open(submenu_ptr)) continue;
        struct wlr_box sub = wlmtk_menu_get_extents(submenu_ptr);
        box = wlmtk_util_box_union(&box, &sub);
    }
    return box;
}

/* ------------------------------------------------------------------------- */
bool wlmtk_menu_place_submenu(
    const struct wlr_box *bounds_ptr,
    int item_left,
    int item_right,
    int item_top,
    int width,
    int height,
    bool prefer_left,
    int *x_ptr,
    int *y_ptr)
{
    *x_ptr = item_right;
    *y_ptr = item_top;
    if (NULL == bounds_ptr || wlr_box_empty(bounds_ptr)) return false;

    const int right = bounds_ptr->x + bounds_ptr->width;
    const int bottom = bounds_ptr->y + bounds_ptr->height;
    bool fits_right = item_right + width <= right;
    bool fits_left = item_left - width >= bounds_ptr->x;
    bool left;
    if (prefer_left && fits_left) {
        left = true;
    } else if (fits_right) {
        left = false;
    } else if (fits_left) {
        left = true;
    } else {
        // Fits on neither side: Where there is more room.
        left = item_left - bounds_ptr->x > right - item_right;
    }
    *x_ptr = left ? item_left - width : item_right;

    if (*y_ptr + height > bottom) *y_ptr = bottom - height;
    if (*y_ptr < bounds_ptr->y) *y_ptr = bounds_ptr->y;
    return left;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_request_item_highlight(
    wlmtk_menu_t *menu_ptr,
    wlmtk_menu_item_t *menu_item_ptr)
{
    if (menu_ptr->highlighted_menu_item_ptr == menu_item_ptr) return;

    if (NULL != menu_ptr->highlighted_menu_item_ptr) {
        wlmtk_menu_item_set_highlighted(
            menu_ptr->highlighted_menu_item_ptr, false);
        menu_ptr->highlighted_menu_item_ptr = NULL;
    }

    if (NULL != menu_item_ptr &&
        wlmtk_menu_item_set_highlighted(menu_item_ptr, true)) {
        menu_ptr->highlighted_menu_item_ptr = menu_item_ptr;
    }
}

/* ------------------------------------------------------------------------- */
size_t wlmtk_menu_items_size(wlmtk_menu_t *menu_ptr)
{
    return bs_dllist_size(&menu_ptr->items);
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_item_t *wlmtk_menu_item_at(wlmtk_menu_t *menu_ptr, size_t i)
{
    if (i >= bs_dllist_size(&menu_ptr->items)) return NULL;

    bs_dllist_node_t *dlnode_ptr = menu_ptr->items.head_ptr;
    while (i--) dlnode_ptr = bs_dllist_node_iterator_forward(dlnode_ptr);
    return wlmtk_menu_item_from_dlnode(dlnode_ptr);
}

/* ------------------------------------------------------------------------- */
struct wlmtk_menu_style *wlmtk_menu_style_create(void)
{
    struct wlmtk_menu_style_holder *sh = logged_calloc(1, sizeof(*sh));
    if (NULL == sh) return NULL;
    bs_ref_init(&sh->msr.reference, _wlmtk_menu_style_destroy);
    return &sh->style;
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_style_ref_t *wlmtk_menu_style_to_ref(
    struct wlmtk_menu_style *menu_style_ptr)
{
    // Guard clause.
    if (NULL == menu_style_ptr) return NULL;

    struct wlmtk_menu_style_holder *sh = BS_CONTAINER_OF(
        menu_style_ptr, struct wlmtk_menu_style_holder, style);
    return &sh->msr;
}

/* ------------------------------------------------------------------------- */
const struct wlmtk_menu_style *wlmtk_menu_style_ref_retain(
    wlmtk_menu_style_ref_t *ref_ptr)
{
    bs_ref_retain(&ref_ptr->reference);
    struct wlmtk_menu_style_holder *sh = BS_CONTAINER_OF(
        ref_ptr, struct wlmtk_menu_style_holder, msr.reference);
    return &sh->style;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_style_ref_release(wlmtk_menu_style_ref_t *ref_ptr)
{
    bs_ref_release(&ref_ptr->reference);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_menu_style_decode(
    bspl_object_t *object_ptr,
    __UNUSED__ const union bspl_desc_value *desc_value_ptr,
    void *value_ptr)
{
    struct wlmtk_menu_style **s = value_ptr;

    return bspl_decode_dict(
        bspl_dict_from_object(object_ptr),
        _wlmtk_menu_style_desc,
        *s);
}

/* ------------------------------------------------------------------------- */
bool wlmtk_menu_style_decode_init(void *dst_ptr)
{
    struct wlmtk_menu_style **s = dst_ptr;
    *s = wlmtk_menu_style_create();
    return *s != NULL;
}

/* ------------------------------------------------------------------------- */
void wlmtk_menu_style_decode_fini(void *dst_ptr)
{
    struct wlmtk_menu_style **s = dst_ptr;
    if (NULL != *s) {
        wlmtk_menu_style_ref_release(wlmtk_menu_style_to_ref(*s));
        *s = NULL;
    }
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Callback for bs_dllist_for_each: Removes item from items, destroys it. */
void _wlmtk_menu_eliminate_item(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_menu_item_t *item_ptr = wlmtk_menu_item_from_dlnode(dlnode_ptr);
    wlmtk_menu_t *menu_ptr = ud_ptr;

    wlmtk_menu_remove_item(menu_ptr, item_ptr);
    wlmtk_element_destroy(wlmtk_menu_item_element(item_ptr));
}

/* ------------------------------------------------------------------------- */
/**
 * Callback for bs_dllist_all: Sets the style for each menu item.
 *
 * @param dlnode_ptr
 * @param ud_ptr
 *
 * @return true on success.
 */
bool _wlmtk_menu_set_item_style(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_menu_item_t *item_ptr = wlmtk_menu_item_from_dlnode(dlnode_ptr);
    wlmtk_menu_style_ref_t *style_ref_ptr = ud_ptr;

    return wlmtk_menu_item_set_style(item_ptr, style_ref_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Callback for bs_dllist_for_each: Sets the menu mode for the item.
 *
 * @param dlnode_ptr
 * @param ud_ptr
 */
void _wlmtk_menu_set_item_mode(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_menu_item_set_mode(
        wlmtk_menu_item_from_dlnode(dlnode_ptr),
        ((wlmtk_menu_t*)ud_ptr)->mode);
}

/* ------------------------------------------------------------------------- */
/** Callback for bs_dllist_for_each: Updates the item for the menu's state. */
void _wlmtk_menu_update_item_shown(
    bs_dllist_node_t *dlnode_ptr,
    __UNUSED__ void *ud_ptr)
{
    wlmtk_menu_item_update_shown(wlmtk_menu_item_from_dlnode(dlnode_ptr));
}

/* ------------------------------------------------------------------------- */
/** Dtor for @ref wlmtk_menu_t::box. */
void _wlmtk_menu_box_element_destroy(wlmtk_element_t *element_ptr)
{
    wlmtk_box_t *box_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_box_t, super_container.super_element);
    wlmtk_box_fini(box_ptr);
}

/* ------------------------------------------------------------------------- */
/** Wraps to dtor. Implements @ref wlmtk_element_vmt_t::destroy. */
void _wlmtk_menu_element_destroy(
    wlmtk_element_t *element_ptr)
{
    wlmtk_menu_t *menu_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_menu_t, base.super_container.super_element);

    wlmtk_menu_destroy(menu_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * If the menu is in right-click mode, acts on right-button events and signals
 * the menu to close.
 *
 * Implementation of @ref wlmtk_element_vmt_t::pointer_button.
 *
 * @param element_ptr
 * @param button_event_ptr
 *
 * @return whether the button event was claimed.
 */
bool _wlmtk_menu_element_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmtk_menu_t *menu_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_menu_t, base.super_container.super_element);

    // Only the top-level menu tracks whether it was opened by a click: It
    // receives all pointer events also for its submenus.
    if (WLMTK_MENU_MODE_RIGHTCLICK == menu_ptr->mode &&
        NULL == menu_ptr->parent_menu_item_ptr) {
        if (!menu_ptr->sticky &&
            !menu_ptr->rightclick_moved &&
            BTN_RIGHT == button_event_ptr->button &&
            WLMTK_BUTTON_UP == button_event_ptr->type) {
            // Released where it was opened: It was a click. Stay open.
            menu_ptr->sticky = true;
            return true;
        }
        if (menu_ptr->sticky) {
            return _wlmtk_menu_sticky_pointer_button(
                menu_ptr, button_event_ptr);
        }
    }

    bool rv = menu_ptr->orig_base_element_vmt.pointer_button(
        element_ptr, button_event_ptr);

    if (WLMTK_MENU_MODE_RIGHTCLICK == menu_ptr->mode &&
        BTN_RIGHT == button_event_ptr->button) {
        wl_signal_emit(&menu_ptr->events.request_close, NULL);
        rv = true;
    }

    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_accepts_motion. Records
 * whether the pointer moved since the menu was opened, then calls the
 * parent's implementation.
 *
 * @param element_ptr
 * @param motion_event_ptr
 *
 * @return See @ref wlmtk_element_vmt_t::pointer_accepts_motion.
 */
bool _wlmtk_menu_element_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    wlmtk_menu_t *menu_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_menu_t, base.super_container.super_element);

    if (!menu_ptr->rightclick_has_position) {
        menu_ptr->rightclick_x = motion_event_ptr->x;
        menu_ptr->rightclick_y = motion_event_ptr->y;
        menu_ptr->rightclick_has_position = true;
    } else {
        double dx = motion_event_ptr->x - menu_ptr->rightclick_x;
        double dy = motion_event_ptr->y - menu_ptr->rightclick_y;
        if (dx >= _wlmtk_menu_rightclick_move_threshold ||
            -dx >= _wlmtk_menu_rightclick_move_threshold ||
            dy >= _wlmtk_menu_rightclick_move_threshold ||
            -dy >= _wlmtk_menu_rightclick_move_threshold) {
            menu_ptr->rightclick_moved = true;
        }
    }

    return menu_ptr->orig_base_element_vmt.pointer_accepts_motion(
        element_ptr, motion_event_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles button events for a right-click menu that was opened by a click.
 *
 * Releasing either button over an item triggers it, and closes the menu.
 * Pressing a button outside the menu closes it. All other events are
 * claimed, but ignored.
 *
 * @param menu_ptr
 * @param button_event_ptr
 *
 * @return true.
 */
bool _wlmtk_menu_sticky_pointer_button(
    wlmtk_menu_t *menu_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmtk_element_t *element_ptr = wlmtk_menu_element(menu_ptr);
    if (BTN_LEFT != button_event_ptr->button &&
        BTN_RIGHT != button_event_ptr->button) return true;

    if (WLMTK_BUTTON_DOWN == button_event_ptr->type &&
        !element_ptr->pointer_inside) {
        wl_signal_emit(&menu_ptr->events.request_close, NULL);
        return true;
    }

    if (WLMTK_BUTTON_UP == button_event_ptr->type &&
        element_ptr->pointer_inside &&
        NULL != _wlmtk_menu_leaf_item_under_pointer(menu_ptr)) {
        // Forward as right-button release: That triggers in right-click mode.
        wlmtk_button_event_t e = *button_event_ptr;
        e.button = BTN_RIGHT;
        menu_ptr->orig_base_element_vmt.pointer_button(element_ptr, &e);
        wl_signal_emit(&menu_ptr->events.request_close, NULL);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Finds the item under the pointer, descending into open submenus.
 *
 * @param menu_ptr
 *
 * @return The highlighted item under the pointer, if it has no submenu. NULL
 *     if there is no such item, eg. when the pointer is on the margin or on
 *     an item that opens a submenu.
 */
wlmtk_menu_item_t *_wlmtk_menu_leaf_item_under_pointer(wlmtk_menu_t *menu_ptr)
{
    while (NULL != menu_ptr) {
        wlmtk_menu_item_t *item_ptr = menu_ptr->highlighted_menu_item_ptr;
        if (NULL == item_ptr) return NULL;
        wlmtk_menu_t *submenu_ptr = wlmtk_menu_item_get_submenu(item_ptr);

        if (wlmtk_menu_item_element(item_ptr)->pointer_inside) {
            return NULL == submenu_ptr ? item_ptr : NULL;
        }
        if (NULL == submenu_ptr ||
            !wlmtk_menu_is_open(submenu_ptr) ||
            !wlmtk_menu_element(submenu_ptr)->pointer_inside) return NULL;
        menu_ptr = submenu_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Resets the state tracking a right-click, eg. when opening the menu. */
void _wlmtk_menu_reset_rightclick(wlmtk_menu_t *menu_ptr)
{
    menu_ptr->rightclick_has_position = false;
    menu_ptr->rightclick_moved = false;
    menu_ptr->sticky = false;
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::keyboard_sym. Translated keys. */
bool _wlmtk_menu_element_keyboard_sym(
    wlmtk_element_t *element_ptr,
    xkb_keysym_t keysym,
    enum xkb_key_direction direction,
    uint32_t modifiers)
{
    wlmtk_menu_t *menu_ptr = BS_CONTAINER_OF(
        element_ptr, wlmtk_menu_t, base.super_container.super_element);
    bs_dllist_node_t *dlnode_ptr = wlmtk_dlnode_from_menu_item(
        menu_ptr->highlighted_menu_item_ptr);
    bs_dllist_node_iterator_t node_iterator;

    // If there is a submenu with an already highlighted item: Forward key.
    wlmtk_menu_t *submenu_ptr = NULL;
    if (NULL != menu_ptr->highlighted_menu_item_ptr) {
        submenu_ptr = wlmtk_menu_item_get_submenu(
            menu_ptr->highlighted_menu_item_ptr);
        if (NULL != submenu_ptr &&
            NULL != submenu_ptr->highlighted_menu_item_ptr) {
            return _wlmtk_menu_element_keyboard_sym(
                &submenu_ptr->base.super_container.super_element,
                keysym,
                direction,
                modifiers);
        }
    }

    // Otherwise: Only interested in key press events, not release.
    if (direction != XKB_KEY_DOWN) return false;

    switch (keysym) {
    case XKB_KEY_Escape:
        if (NULL != wlmtk_menu_get_parent_item(menu_ptr)) {
            wlmtk_menu_request_item_highlight(menu_ptr, NULL);
        } else {
            wl_signal_emit(&menu_ptr->events.request_close, NULL);
        }
        return true;

    case XKB_KEY_Return:
        if (NULL != menu_ptr->highlighted_menu_item_ptr) {
            wlmtk_menu_item_trigger(menu_ptr->highlighted_menu_item_ptr);
        }
        return true;

    case XKB_KEY_Left:
        if (NULL == wlmtk_menu_get_parent_item(menu_ptr)) return false;
        wlmtk_menu_request_item_highlight(menu_ptr, NULL);
        return true;

    case XKB_KEY_Right:
        if (NULL != submenu_ptr &&
            NULL == submenu_ptr->highlighted_menu_item_ptr) {
            wlmtk_menu_item_t *item_ptr = wlmtk_menu_item_from_dlnode(
                _wlmtk_menu_this_or_next_non_disabled_dlnode(
                    submenu_ptr->items.head_ptr,
                    bs_dllist_node_iterator_forward));
            if (NULL != item_ptr) {
                wlmtk_menu_request_item_highlight(submenu_ptr, item_ptr);
                return true;
            }
        }
        return false;

    case XKB_KEY_Home:
        node_iterator = bs_dllist_node_iterator_forward;
        dlnode_ptr = menu_ptr->items.head_ptr;
        break;

    case XKB_KEY_End:
        node_iterator = bs_dllist_node_iterator_backward;
        dlnode_ptr = menu_ptr->items.tail_ptr;
        break;

    case XKB_KEY_Down:
        node_iterator = bs_dllist_node_iterator_forward;
        dlnode_ptr = _wlmtk_menu_this_or_next_non_disabled_dlnode(
            node_iterator(dlnode_ptr), node_iterator);
        if (NULL == dlnode_ptr) dlnode_ptr = menu_ptr->items.head_ptr;
        break;

    case XKB_KEY_Up:
        node_iterator = bs_dllist_node_iterator_backward;
        dlnode_ptr = _wlmtk_menu_this_or_next_non_disabled_dlnode(
            node_iterator(dlnode_ptr), node_iterator);
        if (NULL == dlnode_ptr) dlnode_ptr = menu_ptr->items.tail_ptr;
        break;

    default:
        return false;
    }

    wlmtk_menu_item_t *item_ptr = wlmtk_menu_item_from_dlnode(
        _wlmtk_menu_this_or_next_non_disabled_dlnode(
            dlnode_ptr, node_iterator));
    if (NULL != item_ptr) {
        wlmtk_menu_request_item_highlight(menu_ptr, item_ptr);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Returns the given-or-next non-disabled menu item, from `dlnode_ptr`.
 *
 * @param dlnode_ptr
 * @param iterator            Iterator to reach the "next" item.
 *
 * @return A pointer to @ref wlmtk_menu_item_t::dlnode of `dlnode_ptr` (or the
 *     the next non-disabled item), or NULL.
 */
bs_dllist_node_t *_wlmtk_menu_this_or_next_non_disabled_dlnode(
    bs_dllist_node_t *dlnode_ptr,
    bs_dllist_node_iterator_t iterator)
{
    wlmtk_menu_item_t *menu_item_ptr = wlmtk_menu_item_from_dlnode(dlnode_ptr);
    if (NULL == menu_item_ptr) return NULL;

    switch (wlmtk_menu_item_get_state(menu_item_ptr)) {
    case WLMTK_MENU_ITEM_ENABLED:
    case WLMTK_MENU_ITEM_HIGHLIGHTED:
        return dlnode_ptr;

    case WLMTK_MENU_ITEM_DISABLED:
        break;
    }
    return _wlmtk_menu_this_or_next_non_disabled_dlnode(
        iterator(dlnode_ptr), iterator);
}

/* ------------------------------------------------------------------------- */
/** Destroys the menu style. */
void _wlmtk_menu_style_destroy(bs_ref_t *ref_ptr)
{
    struct wlmtk_menu_style_holder *sh = BS_CONTAINER_OF(
        ref_ptr, struct wlmtk_menu_style_holder, msr.reference);
    free(sh);
}

/* == Unit tests =========================================================== */

static void test_pointer_highlight(bs_test_t *test_ptr);
static void test_set_mode(bs_test_t *test_ptr);
static void test_keyboard_navigation(bs_test_t *test_ptr);
static void test_keyboard_navigation_nested(bs_test_t *test_ptr);
static void test_rightclick(bs_test_t *test_ptr);
static void test_place_submenu(bs_test_t *test_ptr);
static void test_submenu_bounds(bs_test_t *test_ptr);

/** Test cases */
static const bs_test_case_t _wlmtk_menu_test_cases[] = {
    { 1, "pointer_highlight", test_pointer_highlight },
    { 1, "set_mode", test_set_mode },
    { 1, "keyboard_navigation", test_keyboard_navigation },
    { 1, "keyboard_navigation_nested", test_keyboard_navigation_nested },
    { 1, "rightclick", test_rightclick },
    { 1, "place_submenu", test_place_submenu },
    { 1, "submenu_bounds", test_submenu_bounds },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmtk_menu_test_set = BS_TEST_SET(
    true, "menu", _wlmtk_menu_test_cases);

/** For tests: Meu style to apply. */
static struct wlmtk_menu_style_holder _test_style_holder = {
    .msr = { .reference = { .count = 1 } },
    .style = {
        .margin = { .width = 2 },
        .border = { .width = 2 },
        .item = { .height = 10, .bezel_width=1, .width = 100 }
    }
};

/* ------------------------------------------------------------------------- */
/** Tests that pointer moves highlight the items. */
void test_pointer_highlight(bs_test_t *test_ptr)
{
    wlmtk_menu_t *menu_ptr = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, menu_ptr);
    wlmtk_element_t *me = wlmtk_menu_element(menu_ptr);
    wlmtk_element_set_visible(me, true);

    wlmtk_menu_item_t *i1 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i1);
    wlmtk_menu_add_item(menu_ptr, i1);
    wlmtk_menu_item_t *i2 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i2);
    wlmtk_menu_add_item(menu_ptr, i2);

    // Motion into first element: highlight it.
    wlmtk_pointer_motion_event_t e = { .x = 9, .y = 5 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_HIGHLIGHTED, wlmtk_menu_item_get_state(i1));
    BS_TEST_VERIFY_EQ(test_ptr, i1, menu_ptr->highlighted_menu_item_ptr);

    // Motion into second element: highlight that, un-highlight the other.
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 15 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_ENABLED, wlmtk_menu_item_get_state(i1));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_HIGHLIGHTED, wlmtk_menu_item_get_state(i2));
    BS_TEST_VERIFY_EQ(test_ptr, i2, menu_ptr->highlighted_menu_item_ptr);

    // Move into the margin area: Both are un-highlighted.
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 10 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_ENABLED, wlmtk_menu_item_get_state(i1));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_ENABLED, wlmtk_menu_item_get_state(i2));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, menu_ptr->highlighted_menu_item_ptr);

    // Move entirely outside: Both remain just enabled.
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 55 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_ENABLED, wlmtk_menu_item_get_state(i1));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_ENABLED, wlmtk_menu_item_get_state(i2));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, menu_ptr->highlighted_menu_item_ptr);

    // Back into second element: highlight that.
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 15 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_ENABLED, wlmtk_menu_item_get_state(i1));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_MENU_ITEM_HIGHLIGHTED, wlmtk_menu_item_get_state(i2));

    // Remove one item explicitly, the other one to destroy during cleanup.
    wlmtk_menu_remove_item(menu_ptr, i1);
    wlmtk_menu_item_destroy(i1);
    wlmtk_menu_destroy(menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests setting the menu's mode. */
void test_set_mode(bs_test_t *test_ptr)
{
    wlmtk_menu_t *menu_ptr = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, menu_ptr);

    wlmtk_util_test_listener_t destroy_test_listener;
    wlmtk_util_connect_test_listener(
        &wlmtk_menu_events(menu_ptr)->destroy,
        &destroy_test_listener);

    wlmtk_menu_item_t *item1_ptr = wlmtk_menu_item_create(
        &_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, item1_ptr);
    wlmtk_menu_add_item(menu_ptr, item1_ptr);

    // Setting the mode must propagate.
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_MODE_NORMAL,
        wlmtk_menu_item_get_mode(item1_ptr));
    wlmtk_menu_set_mode(menu_ptr, WLMTK_MENU_MODE_RIGHTCLICK);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_MODE_RIGHTCLICK,
        wlmtk_menu_item_get_mode(item1_ptr));

    // A new item must get the mode applied.
    wlmtk_menu_item_t *item2_ptr = wlmtk_menu_item_create(
        &_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, item2_ptr);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_MODE_NORMAL,
        wlmtk_menu_item_get_mode(item2_ptr));
    wlmtk_menu_add_item(menu_ptr, item2_ptr);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_MODE_RIGHTCLICK,
        wlmtk_menu_item_get_mode(item2_ptr));

    // Setting the mode must propagate to all.
    wlmtk_menu_set_mode(menu_ptr, WLMTK_MENU_MODE_NORMAL);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_MODE_NORMAL,
        wlmtk_menu_item_get_mode(item1_ptr));
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_MODE_NORMAL,
        wlmtk_menu_item_get_mode(item2_ptr));

    BS_TEST_VERIFY_EQ(test_ptr, 0, destroy_test_listener.calls);
    wlmtk_menu_destroy(menu_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 1, destroy_test_listener.calls);
}

/* ------------------------------------------------------------------------- */
/** Tests keyboard navigation within the menu. */
void test_keyboard_navigation(bs_test_t *test_ptr)
{
    static const wlmtk_menu_item_state_t HL = WLMTK_MENU_ITEM_HIGHLIGHTED;

    wlmtk_menu_t *menu_ptr = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, menu_ptr);
    wlmtk_element_t *me = wlmtk_menu_element(menu_ptr);
    wlmtk_element_set_visible(me, true);

    wlmtk_util_test_listener_t request_close_test_listener;
    wlmtk_util_connect_test_listener(
        &wlmtk_menu_events(menu_ptr)->request_close,
        &request_close_test_listener);

    // Test items: disabled, enabled, enabled, disabled, enabled.
    struct {
        wlmtk_menu_item_t *item;
        bool enabled;
        wlmtk_util_test_listener_t triggered_test_listener;
    } items[6] = {
        [1] = { .enabled = true },
        [2] = { .enabled = true },
        [4] = { .enabled = true },
    };
    for (int i = 0; i < 5; ++i) {
        items[i].item = wlmtk_menu_item_create(&_test_style_holder.msr);
        BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, items[i].item);
        wlmtk_util_connect_test_listener(
            &wlmtk_menu_item_events(items[i].item)->triggered,
            &items[i].triggered_test_listener);
        wlmtk_menu_add_item(menu_ptr, items[i].item);
        wlmtk_menu_item_set_enabled(items[i].item, items[i].enabled);
    }
    wlmtk_element_layout(wlmtk_menu_element(menu_ptr));

    // Move pointer over items[2].
    wlmtk_pointer_motion_event_t e = { .x = 9, .y = 25 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[2].item));

    // Down key: Moves down, items[3] is disabled => land at items[4].
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Down, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[4].item));

    // Down key once more: Wrap around, land at items[1].
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Down, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ( test_ptr, HL, wlmtk_menu_item_get_state(items[1].item));

    // Up key: Wraps around, land at items[4].
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Up, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[4].item));

    // Up key: Moves up once more, at items[2].
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Up, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[2].item));

    // End key: Jump to items[4].
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_End, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[4].item));

    // A motion, within items[2]. Re-gain focus there.
    e = (wlmtk_pointer_motion_event_t){ .x = 8, .y = 25 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[2].item));

    // Home key: Jump to items[1].
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Home, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(items[1].item));

    // Return key: Trigger items[1].
    wlmtk_util_clear_test_listener(&items[1].triggered_test_listener);
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Return, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, items[1].triggered_test_listener.calls);

    // Escape key: Request to close the menu.
    wlmtk_util_clear_test_listener(&request_close_test_listener);
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Escape, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, request_close_test_listener.calls);

    // Keys that were released: No trigger.
    BS_TEST_VERIFY_FALSE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Escape, XKB_KEY_UP, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, request_close_test_listener.calls);

    wlmtk_menu_destroy(menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests keyboard navigation with nested menus. */
void test_keyboard_navigation_nested(bs_test_t *test_ptr)
{
    static const wlmtk_menu_item_state_t HL = WLMTK_MENU_ITEM_HIGHLIGHTED;

    // Menu m0 with two items (i00, i01).
    wlmtk_menu_t *m0 = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, m0);
    wlmtk_menu_item_t *i00 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i00);
    wlmtk_menu_add_item(m0, i00);
    wlmtk_menu_item_t *i01 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i01);
    wlmtk_menu_add_item(m0, i01);

    wlmtk_util_test_listener_t request_close_test_listener;
    wlmtk_util_connect_test_listener(
        &wlmtk_menu_events(m0)->request_close,
        &request_close_test_listener);

    // i01 has submenu m1 with two items (i10, i11).
    wlmtk_menu_t *m1 = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, m1);
    wlmtk_menu_item_set_submenu(i01, m1);
    wlmtk_menu_item_t *i10 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i10);
    wlmtk_menu_add_item(m1, i10);
    wlmtk_menu_item_t *i11 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i11);
    wlmtk_menu_add_item(m1, i11);
    wlmtk_element_t *me = wlmtk_menu_element(m0);
    wlmtk_element_set_visible(me, true);

    // Home key, highlights i00.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Home, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i00));

    // Right key. Does not do anything.
    BS_TEST_VERIFY_FALSE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Right, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i00));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_is_open(m1));

    // Down key. Highlights i01, opens m1, but no highlight there.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Down, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i01));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_is_open(m1));
    BS_TEST_VERIFY_NEQ(test_ptr, HL, wlmtk_menu_item_get_state(i10));

    // Right key. Now highlights i10. i01 remains highlighted.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Right, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i01));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i10));

    // Down key. Now highlights i11. i01 remains highlighted.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Down, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i01));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i11));

    // Left key. Moves highlight back, but keeps submenu open.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Left, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i01));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_is_open(m1));
    BS_TEST_VERIFY_NEQ(test_ptr, HL, wlmtk_menu_item_get_state(i10));
    BS_TEST_VERIFY_NEQ(test_ptr, HL, wlmtk_menu_item_get_state(i11));

    // Right key. Highlights the submenu with i10 again.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Right, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i01));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i10));

    // Esc key, while submenu has highlight. Same action as left key.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Escape, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, HL, wlmtk_menu_item_get_state(i01));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_is_open(m1));
    BS_TEST_VERIFY_NEQ(test_ptr, HL, wlmtk_menu_item_get_state(i10));
    BS_TEST_VERIFY_NEQ(test_ptr, HL, wlmtk_menu_item_get_state(i11));
    BS_TEST_VERIFY_EQ(test_ptr, 0, request_close_test_listener.calls);

    // Esc key, once more. Must close.
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        wlmtk_element_keyboard_sym(me, XKB_KEY_Escape, XKB_KEY_DOWN, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, request_close_test_listener.calls);

    wlmtk_menu_destroy(m0);
}

/* ------------------------------------------------------------------------- */
/**
 * Tests right-click mode: Releasing the button after dragging onto an item
 * triggers it. Releasing it without having moved keeps the menu open, and
 * a later click on an item triggers, a click outside closes.
 */
void test_rightclick(bs_test_t *test_ptr)
{
    wlmtk_menu_t *menu_ptr = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, menu_ptr);
    wlmtk_element_t *me = wlmtk_menu_element(menu_ptr);
    wlmtk_menu_item_t *i1 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i1);
    wlmtk_menu_add_item(menu_ptr, i1);
    wlmtk_menu_item_t *i2 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i2);
    wlmtk_menu_add_item(menu_ptr, i2);

    wlmtk_util_test_listener_t t1, t2, close;
    wlmtk_util_connect_test_listener(
        &wlmtk_menu_item_events(i1)->triggered, &t1);
    wlmtk_util_connect_test_listener(
        &wlmtk_menu_item_events(i2)->triggered, &t2);
    wlmtk_util_connect_test_listener(
        &wlmtk_menu_events(menu_ptr)->request_close, &close);
    wlmtk_button_event_t rup = { .button = BTN_RIGHT, .type = WLMTK_BUTTON_UP };
    wlmtk_button_event_t ldown = {
        .button = BTN_LEFT, .type = WLMTK_BUTTON_DOWN };
    wlmtk_button_event_t lup = { .button = BTN_LEFT, .type = WLMTK_BUTTON_UP };
    wlmtk_pointer_motion_event_t e;

    wlmtk_menu_set_mode(menu_ptr, WLMTK_MENU_MODE_RIGHTCLICK);

    // Opened, pointer drags from first to second item, releases: Trigger.
    wlmtk_menu_set_open(menu_ptr, true);
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 5 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 15 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &rup));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t1.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t2.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 1, close.calls);
    wlmtk_util_clear_test_listener(&t2);
    wlmtk_util_clear_test_listener(&close);
    wlmtk_menu_set_open(menu_ptr, false);

    // Opened over the first item, released without moving: Stays open.
    wlmtk_menu_set_open(menu_ptr, true);
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 5 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    e = (wlmtk_pointer_motion_event_t){ .x = 10, .y = 6 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &rup));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t1.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 0, close.calls);

    // A click on the margin between items does nothing.
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 10 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &ldown));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &lup));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t1.calls + t2.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 0, close.calls);

    // A left click on the second item triggers it, and closes the menu.
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 15 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &ldown));
    BS_TEST_VERIFY_EQ(test_ptr, 0, close.calls);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &lup));
    BS_TEST_VERIFY_EQ(test_ptr, 0, t1.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t2.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 1, close.calls);
    wlmtk_util_clear_test_listener(&t2);
    wlmtk_util_clear_test_listener(&close);
    wlmtk_menu_set_open(menu_ptr, false);

    // Opened by a click, then a press outside the menu closes it.
    wlmtk_menu_set_open(menu_ptr, true);
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 5 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &rup));
    BS_TEST_VERIFY_EQ(test_ptr, 0, close.calls);
    e = (wlmtk_pointer_motion_event_t){ .x = 9, .y = 55 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_element_pointer_motion(me, &e));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_element_pointer_button(me, &ldown));
    BS_TEST_VERIFY_EQ(test_ptr, 1, close.calls);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t1.calls + t2.calls);

    wlmtk_util_disconnect_test_listener(&close);
    wlmtk_util_disconnect_test_listener(&t2);
    wlmtk_util_disconnect_test_listener(&t1);
    wlmtk_menu_destroy(menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests @ref wlmtk_menu_place_submenu. */
void test_place_submenu(bs_test_t *test_ptr)
{
    struct wlr_box b = { .x = 0, .y = 0, .width = 1000, .height = 700 };
    int x, y;

    // No bounds: Right of the item, aligned.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_place_submenu(
                             NULL, 900, 990, 650, 200, 300, false, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, 990, x);
    BS_TEST_VERIFY_EQ(test_ptr, 650, y);

    // Fits right.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_place_submenu(
                             &b, 100, 300, 50, 200, 100, false, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, 300, x);
    BS_TEST_VERIFY_EQ(test_ptr, 50, y);

    // Would extend beyond the right, and the bottom: Left, moved up.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_place_submenu(
                            &b, 700, 900, 650, 200, 300, false, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, 500, x);
    BS_TEST_VERIFY_EQ(test_ptr, 400, y);

    // Prefers left: Stays left, unless it does not fit there.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_place_submenu(
                            &b, 300, 500, 0, 200, 100, true, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, 100, x);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_place_submenu(
                             &b, 100, 300, 0, 200, 100, true, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, 300, x);

    // Fits nowhere: Where there is more room. Taller than the bounds: Top.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_place_submenu(
                            &b, 600, 700, 100, 650, 900, false, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, -50, x);
    BS_TEST_VERIFY_EQ(test_ptr, 0, y);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_place_submenu(
                             &b, 300, 400, 100, 650, 100, false, &x, &y));
    BS_TEST_VERIFY_EQ(test_ptr, 400, x);
}

/* ------------------------------------------------------------------------- */
/** A submenu opens to the left, when there is no room on the right. */
void test_submenu_bounds(bs_test_t *test_ptr)
{
    wlmtk_container_t *c = wlmtk_container_create_fake_parent();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, c);
    wlmtk_menu_t *m0 = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, m0);
    wlmtk_menu_item_t *i00 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i00);
    wlmtk_menu_add_item(m0, i00);
    wlmtk_menu_t *m1 = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, m1);
    wlmtk_menu_item_set_submenu(i00, m1);
    wlmtk_menu_item_t *i10 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i10);
    wlmtk_menu_add_item(m1, i10);
    wlmtk_menu_t *m2 = wlmtk_menu_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, m2);
    wlmtk_menu_item_set_submenu(i10, m2);
    wlmtk_menu_item_t *i20 = wlmtk_menu_item_create(&_test_style_holder.msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, i20);
    wlmtk_menu_add_item(m2, i20);

    wlmtk_element_t *me = wlmtk_menu_element(m0);
    wlmtk_container_add_element(c, me);
    wlmtk_element_set_visible(me, true);
    wlmtk_element_set_position(me, 300, 50);
    int ix, iy;
    wlmtk_element_get_absolute_position(
        wlmtk_menu_item_element(i00), &ix, &iy);
    BS_TEST_VERIFY_TRUE(test_ptr, 300 <= ix && ix < 310);
    BS_TEST_VERIFY_TRUE(test_ptr, 50 <= iy && iy < 60);
    int w = wlmtk_element_get_dimensions_box(wlmtk_menu_element(m1)).width;
    int h = wlmtk_element_get_dimensions_box(wlmtk_menu_element(m1)).height;
    BS_TEST_VERIFY_TRUE(test_ptr, 100 <= w && w < 120);

    // Room on the right (no bounds): Opens to the right of the item.
    int x, y;
    wlmtk_menu_request_item_highlight(m0, i00);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_is_open(m1));
    wlmtk_element_get_absolute_position(wlmtk_menu_element(m1), &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, ix + 100, x);
    BS_TEST_VERIFY_EQ(test_ptr, iy, y);
    wlmtk_menu_request_item_highlight(m0, NULL);

    // Bounds end at 450: Opens to the left of the item. And its submenu, too.
    struct wlr_box b = { .x = 0, .y = 0, .width = 450, .height = 700 };
    wlmtk_menu_set_bounds(m0, &b);
    wlmtk_menu_request_item_highlight(m0, i00);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_is_open(m1));
    wlmtk_element_get_absolute_position(wlmtk_menu_element(m1), &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, ix - w, x);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_opens_left(m1));
    wlmtk_menu_request_item_highlight(m1, i10);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_is_open(m2));
    wlmtk_element_get_absolute_position(wlmtk_menu_element(m2), &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, ix - 2 * w, x);
    struct wlr_box ext = wlmtk_menu_get_extents(m0);
    BS_TEST_VERIFY_EQ(test_ptr, ix - 2 * w, ext.x);
    BS_TEST_VERIFY_EQ(test_ptr, 300 + w, ext.x + ext.width);

    // Bounds end at 60 below: Moves up.
    wlmtk_menu_request_item_highlight(m0, NULL);
    b.height = 60;
    wlmtk_menu_set_bounds(m0, &b);
    wlmtk_menu_request_item_highlight(m0, i00);
    wlmtk_element_get_absolute_position(wlmtk_menu_element(m1), &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, 60 - h, y);

    wlmtk_container_remove_element(c, me);
    wlmtk_menu_destroy(m0);
    wlmtk_container_destroy_fake_parent(c);
}

/* == End of menu.c ======================================================== */
