/* ========================================================================= */
/**
 * @file context_menu.c
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

#include "context_menu.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

#include "clip.h"
#include "drawer.h"
#include "input/manager.h"
#include "menu_scroll.h"

/* == Declarations ========================================================= */

/** State of a context menu. */
struct _wlmaker_context_menu_t {
    /** Back-link to server. */
    wlmaker_server_t          *server_ptr;
    /** The element the menu is for. */
    void                      *owner_ptr;

    /** The menu. */
    wlmtk_menu_t              *menu_ptr;
    /** The window holding the menu, as for the root menu. */
    wlmtk_window_t            *window_ptr;

    /** Scrolls the menu at the edge of the output. */
    wlmaker_menu_scroll_t     *menu_scroll_ptr;

    /** Whether the menu was opened, and is owned by the server. */
    bool                      opened;
    /** Deferred destruction, once closed. */
    struct wl_event_source    *idle_destroy_event_source_ptr;

    /** Listener for @ref wlmtk_menu_events_t::open_changed. */
    struct wl_listener        menu_open_changed_listener;
    /** Listener for @ref wlmtk_menu_events_t::request_close. */
    struct wl_listener        menu_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::request_close. */
    struct wl_listener        window_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        window_set_activated_listener;
};

/** An item of the context menu. */
typedef struct {
    /** The menu item. */
    wlmtk_menu_item_t         *menu_item_ptr;
    /** Back-link to the context menu. */
    wlmaker_context_menu_t    *context_menu_ptr;
    /** Callback, when triggered. */
    wlmaker_context_menu_callback_t callback;
    /** Argument to @ref wlmaker_context_menu_item_t::callback. */
    void                      *ud_ptr;
    /** Releases @ref wlmaker_context_menu_item_t::ud_ptr. Or NULL. */
    wlmaker_context_menu_free_t free_fn;

    /** Listener for @ref wlmtk_menu_item_events_t::triggered. */
    struct wl_listener        triggered_listener;
    /** Listener for @ref wlmtk_menu_item_events_t::destroy. */
    struct wl_listener        destroy_listener;
} wlmaker_context_menu_item_t;

static void _wlmaker_context_menu_close(
    wlmaker_context_menu_t *context_menu_ptr);
static wlmaker_context_menu_item_t *_wlmaker_context_menu_add(
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *parent_ptr,
    const char *text_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    wlmaker_context_menu_callback_t callback,
    void *ud_ptr,
    wlmaker_context_menu_free_t free_fn);
static void _wlmaker_context_menu_handle_open_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_context_menu_handle_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_context_menu_handle_window_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_context_menu_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_context_menu_idle_destroy(void *data_ptr);

static void _wlmaker_context_menu_item_handle_triggered(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_context_menu_item_handle_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_context_menu_t *wlmaker_context_menu_create(
    wlmaker_server_t *server_ptr,
    void *owner_ptr,
    const char *title_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = logged_calloc(
        1, sizeof(*context_menu_ptr));
    if (NULL == context_menu_ptr) return NULL;
    context_menu_ptr->server_ptr = server_ptr;
    context_menu_ptr->owner_ptr = owner_ptr;

    context_menu_ptr->menu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == context_menu_ptr->menu_ptr) {
        wlmaker_context_menu_destroy(context_menu_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(context_menu_ptr->menu_ptr)->open_changed,
        &context_menu_ptr->menu_open_changed_listener,
        _wlmaker_context_menu_handle_open_changed);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(context_menu_ptr->menu_ptr)->request_close,
        &context_menu_ptr->menu_request_close_listener,
        _wlmaker_context_menu_handle_request_close);

    context_menu_ptr->window_ptr = wlmtk_window_create(
        wlmtk_menu_element(context_menu_ptr->menu_ptr),
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == context_menu_ptr->window_ptr) {
        wlmaker_context_menu_destroy(context_menu_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(context_menu_ptr->window_ptr)->request_close,
        &context_menu_ptr->window_request_close_listener,
        _wlmaker_context_menu_handle_window_request_close);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(context_menu_ptr->window_ptr)->set_activated,
        &context_menu_ptr->window_set_activated_listener,
        _wlmaker_context_menu_handle_window_set_activated);
    wlmtk_window_set_title(context_menu_ptr->window_ptr, title_ptr);
    // The window shows its content: Closed, until opened as right-click menu.
    wlmtk_menu_set_open(context_menu_ptr->menu_ptr, false);
    wlmtk_window_set_server_side_decorated(context_menu_ptr->window_ptr, true);

    context_menu_ptr->menu_scroll_ptr = wlmaker_menu_scroll_create(
        server_ptr, context_menu_ptr->window_ptr, context_menu_ptr->menu_ptr);
    if (NULL == context_menu_ptr->menu_scroll_ptr) {
        wlmaker_context_menu_destroy(context_menu_ptr);
        return NULL;
    }
    return context_menu_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_context_menu_destroy(wlmaker_context_menu_t *context_menu_ptr)
{
    if (context_menu_ptr->opened &&
        context_menu_ptr->server_ptr->context_menu_ptr == context_menu_ptr) {
        context_menu_ptr->server_ptr->context_menu_ptr = NULL;
    }

    if (NULL != context_menu_ptr->menu_scroll_ptr) {
        wlmaker_menu_scroll_destroy(context_menu_ptr->menu_scroll_ptr);
        context_menu_ptr->menu_scroll_ptr = NULL;
    }

    if (NULL != context_menu_ptr->idle_destroy_event_source_ptr) {
        wl_event_source_remove(context_menu_ptr->idle_destroy_event_source_ptr);
        context_menu_ptr->idle_destroy_event_source_ptr = NULL;
    }

    if (NULL != context_menu_ptr->window_ptr) {
        wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
            context_menu_ptr->window_ptr);
        if (NULL != workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr,
                                         context_menu_ptr->window_ptr);
        }
        wlmtk_util_disconnect_listener(
            &context_menu_ptr->window_request_close_listener);
        wlmtk_util_disconnect_listener(
            &context_menu_ptr->window_set_activated_listener);
        wlmtk_window_destroy(context_menu_ptr->window_ptr);
        context_menu_ptr->window_ptr = NULL;
    }

    if (NULL != context_menu_ptr->menu_ptr) {
        wlmtk_util_disconnect_listener(
            &context_menu_ptr->menu_request_close_listener);
        wlmtk_util_disconnect_listener(
            &context_menu_ptr->menu_open_changed_listener);
        wlmtk_menu_destroy(context_menu_ptr->menu_ptr);
        context_menu_ptr->menu_ptr = NULL;
    }
    free(context_menu_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_context_menu_add_item(
    wlmaker_context_menu_t *context_menu_ptr,
    const char *text_ptr,
    wlmaker_context_menu_callback_t callback,
    void *ud_ptr)
{
    return NULL != _wlmaker_context_menu_add(
        context_menu_ptr, NULL, text_ptr, WLMTK_MENU_ITEM_INDICATOR_NONE,
        false, callback, ud_ptr, NULL);
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_t *wlmaker_context_menu_add_submenu(
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *parent_ptr,
    const char *text_ptr)
{
    wlmaker_context_menu_item_t *item_ptr = _wlmaker_context_menu_add(
        context_menu_ptr, parent_ptr, text_ptr,
        WLMTK_MENU_ITEM_INDICATOR_NONE, false, NULL, NULL, NULL);
    if (NULL == item_ptr) return NULL;

    wlmtk_menu_t *submenu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(
            context_menu_ptr->server_ptr->style_ptr->menu_style_ptr));
    if (NULL == submenu_ptr) return NULL;
    // The item owns the submenu, and destroys it along.
    wlmtk_menu_item_set_submenu(item_ptr->menu_item_ptr, submenu_ptr);
    return submenu_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_context_menu_add_option(
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *parent_ptr,
    const char *text_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    wlmaker_context_menu_callback_t callback,
    void *ud_ptr,
    wlmaker_context_menu_free_t free_fn)
{
    return NULL != _wlmaker_context_menu_add(
        context_menu_ptr, parent_ptr, text_ptr, indicator, checked,
        callback, ud_ptr, free_fn);
}

/* ------------------------------------------------------------------------- */
void wlmaker_context_menu_open(wlmaker_context_menu_t *context_menu_ptr)
{
    wlmaker_server_t *server_ptr = context_menu_ptr->server_ptr;

    // There is only one open context menu at a time.
    wlmaker_context_menu_cancel(server_ptr, NULL);
    server_ptr->context_menu_ptr = context_menu_ptr;
    context_menu_ptr->opened = true;

    wlmtk_workspace_t *workspace_ptr = wlmtk_desktop_get_current_workspace(
        server_ptr->desktop_ptr);
    struct wlr_cursor *wlr_cursor_ptr = wlmim_wlr_cursor(
        server_ptr->input_manager_ptr);
    int x = wlr_cursor_ptr->x, y = wlr_cursor_ptr->y;

    wlmtk_workspace_map_window(workspace_ptr, context_menu_ptr->window_ptr);
    wlmtk_window_set_properties(context_menu_ptr->window_ptr,
                                WLMTK_WINDOW_PROPERTY_RIGHTCLICK);
    wlmtk_workspace_set_window_position(
        workspace_ptr, context_menu_ptr->window_ptr, x, y);

    // Keep it off the docks and panels: Within the usable area.
    struct wlr_box bbox = wlmtk_window_get_bounding_box(
        context_menu_ptr->window_ptr);
    struct wlr_box area = wlmtk_desktop_get_usable_area(
        server_ptr->desktop_ptr,
        wlmaker_server_get_output_at_cursor(server_ptr));
    // The bounding box is in workspace coordinates.
    struct wlr_box pos = wlmaker_context_menu_confine(
        &area, bbox.x, bbox.y, bbox.width, bbox.height);
    int wx = x + pos.x - bbox.x, wy = y + pos.y - bbox.y;
    wlmtk_workspace_set_window_position(
        workspace_ptr, context_menu_ptr->window_ptr, wx, wy);
    wlmtk_menu_set_bounds(context_menu_ptr->menu_ptr, &area);

    wlmtk_menu_set_mode(context_menu_ptr->menu_ptr,
                        WLMTK_MENU_MODE_RIGHTCLICK);
    wlmtk_menu_set_open(context_menu_ptr->menu_ptr, true);

    // Not over an open drawer (which is above the windows): Below, or
    // above it. With its items, the menu has its full size now.
    bbox = wlmtk_window_get_bounding_box(context_menu_ptr->window_ptr);
    struct wlr_box moved = bbox;
    for (int id = 2; NULL != server_ptr->clip_ptr &&
             id < wlmaker_clip_columns(server_ptr); ++id) {
        wlmaker_drawer_t *d_ptr = wlmaker_clip_drawer(server_ptr, id);
        struct wlr_box row, i;
        if (NULL == d_ptr || !wlmaker_drawer_row_box(d_ptr, &row) ||
            !wlr_box_intersection(&i, &row, &moved)) continue;
        moved.y = row.y + row.height;
        if (moved.y + moved.height > area.y + area.height) {
            moved.y = row.y - moved.height;
        }
    }
    if (moved.y != bbox.y) {
        wlmtk_workspace_set_window_position(
            workspace_ptr, context_menu_ptr->window_ptr,
            wx, wy + moved.y - bbox.y);
    }
    // Keyboard focus: The menu's keys work, and Escape closes it.
    wlmtk_workspace_activate_window(workspace_ptr, context_menu_ptr->window_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_context_menu_cancel(wlmaker_server_t *server_ptr,
                                 void *owner_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = server_ptr->context_menu_ptr;
    if (NULL == context_menu_ptr) return;
    if (NULL != owner_ptr && context_menu_ptr->owner_ptr != owner_ptr) return;
    wlmaker_context_menu_destroy(context_menu_ptr);
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_t *wlmaker_context_menu_menu(
    wlmaker_context_menu_t *context_menu_ptr)
{
    return context_menu_ptr->menu_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_box wlmaker_context_menu_confine(
    const struct wlr_box *area_ptr,
    int x, int y, int width, int height)
{
    struct wlr_box box = { .x = x, .y = y, .width = width, .height = height };
    if (wlr_box_empty(area_ptr)) return box;

    if (box.x + box.width > area_ptr->x + area_ptr->width) {
        box.x = area_ptr->x + area_ptr->width - box.width;
    }
    if (box.x < area_ptr->x) box.x = area_ptr->x;
    if (box.y + box.height > area_ptr->y + area_ptr->height) {
        box.y = area_ptr->y + area_ptr->height - box.height;
    }
    if (box.y < area_ptr->y) box.y = area_ptr->y;
    return box;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Adds an item to `parent_ptr`, or to the context menu's menu.
 *
 * @return The item, or NULL on error. `free_fn` is then called already.
 */
wlmaker_context_menu_item_t *_wlmaker_context_menu_add(
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *parent_ptr,
    const char *text_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    wlmaker_context_menu_callback_t callback,
    void *ud_ptr,
    wlmaker_context_menu_free_t free_fn)
{
    if (NULL == parent_ptr) parent_ptr = context_menu_ptr->menu_ptr;
    wlmaker_context_menu_item_t *item_ptr = logged_calloc(
        1, sizeof(*item_ptr));
    if (NULL == item_ptr) {
        if (NULL != free_fn) free_fn(ud_ptr);
        return NULL;
    }
    item_ptr->context_menu_ptr = context_menu_ptr;
    item_ptr->callback = callback;
    item_ptr->ud_ptr = ud_ptr;
    item_ptr->free_fn = free_fn;

    item_ptr->menu_item_ptr = wlmtk_menu_item_create(
        wlmtk_menu_style_to_ref(
            context_menu_ptr->server_ptr->style_ptr->menu_style_ptr));
    if (NULL == item_ptr->menu_item_ptr) {
        if (NULL != free_fn) free_fn(ud_ptr);
        free(item_ptr);
        return NULL;
    }
    if (!wlmtk_menu_item_set_text(item_ptr->menu_item_ptr, text_ptr) ||
        !wlmtk_menu_item_set_indicator(
            item_ptr->menu_item_ptr, indicator, checked)) {
        wlmtk_menu_item_destroy(item_ptr->menu_item_ptr);
        if (NULL != free_fn) free_fn(ud_ptr);
        free(item_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(item_ptr->menu_item_ptr)->triggered,
        &item_ptr->triggered_listener,
        _wlmaker_context_menu_item_handle_triggered);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(item_ptr->menu_item_ptr)->destroy,
        &item_ptr->destroy_listener,
        _wlmaker_context_menu_item_handle_destroy);

    wlmtk_menu_add_item(parent_ptr, item_ptr->menu_item_ptr);
    return item_ptr;
}

/* ------------------------------------------------------------------------- */
/** Closes the menu. If it was opened: Destroys it, once back in the loop. */
void _wlmaker_context_menu_close(wlmaker_context_menu_t *context_menu_ptr)
{
    wlmtk_menu_set_open(context_menu_ptr->menu_ptr, false);

    if (!context_menu_ptr->opened ||
        NULL != context_menu_ptr->idle_destroy_event_source_ptr ||
        NULL == context_menu_ptr->server_ptr->wl_display_ptr) return;
    // Not right away: We may be called from the menu's own event handlers.
    context_menu_ptr->idle_destroy_event_source_ptr = wl_event_loop_add_idle(
        wl_display_get_event_loop(context_menu_ptr->server_ptr->wl_display_ptr),
        _wlmaker_context_menu_idle_destroy,
        context_menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_events_t::open_changed: Grabs or unmaps. */
void _wlmaker_context_menu_handle_open_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_context_menu_t, menu_open_changed_listener);
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        context_menu_ptr->window_ptr);

    if (!wlmtk_menu_is_open(context_menu_ptr->menu_ptr)) {
        if (NULL != workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr,
                                         context_menu_ptr->window_ptr);
        }
        _wlmaker_context_menu_close(context_menu_ptr);
        return;
    }

    // Like the root menu: Grab the pointer, to see the right button released.
    wlmtk_element_t *element_ptr = wlmtk_menu_element(
        context_menu_ptr->menu_ptr);
    if (NULL != workspace_ptr && NULL != element_ptr->parent_container_ptr) {
        wlmtk_container_pointer_grab(
            element_ptr->parent_container_ptr, element_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_events_t::request_close. Closes the menu. */
void _wlmaker_context_menu_handle_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_context_menu_t, menu_request_close_listener);
    _wlmaker_context_menu_close(context_menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_window_events_t::request_close. Closes the menu. */
void _wlmaker_context_menu_handle_window_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_context_menu_t, window_request_close_listener);
    _wlmaker_context_menu_close(context_menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles activation of the menu's window: Keyboard focus to the menu. */
void _wlmaker_context_menu_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_context_menu_t, window_set_activated_listener);
    wlmtk_element_t *e = wlmtk_menu_element(context_menu_ptr->menu_ptr);
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_set_keyboard_focus_element(
            e->parent_container_ptr, e,
            wlmtk_window_is_activated(context_menu_ptr->window_ptr));
    }
}

/* ------------------------------------------------------------------------- */
/** Destroys the closed menu, from the event loop. */
void _wlmaker_context_menu_idle_destroy(void *data_ptr)
{
    wlmaker_context_menu_t *context_menu_ptr = data_ptr;
    // Already dispatched: Must not be removed again.
    context_menu_ptr->idle_destroy_event_source_ptr = NULL;
    wlmaker_context_menu_destroy(context_menu_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_item_events_t::triggered: Callback, and close. */
void _wlmaker_context_menu_item_handle_triggered(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_context_menu_item_t *item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_context_menu_item_t, triggered_listener);
    wlmaker_context_menu_t *context_menu_ptr = item_ptr->context_menu_ptr;

    // Closes first: The menu's window is then off the workspace, which the
    // callback may switch away from, or destroy.
    _wlmaker_context_menu_close(context_menu_ptr);
    if (NULL != item_ptr->callback) item_ptr->callback(item_ptr->ud_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_item_events_t::destroy: Frees the item. */
void _wlmaker_context_menu_item_handle_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_context_menu_item_t *item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_context_menu_item_t, destroy_listener);
    wlmtk_util_disconnect_listener(&item_ptr->triggered_listener);
    wlmtk_util_disconnect_listener(&item_ptr->destroy_listener);
    if (NULL != item_ptr->free_fn) item_ptr->free_fn(item_ptr->ud_ptr);
    free(item_ptr);
}

/* == Unit tests =========================================================== */

static void test_items(bs_test_t *test_ptr);
static void test_confine(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmaker_context_menu_test_cases[] = {
    { true, "items", test_items },
    { true, "confine", test_confine },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_context_menu_test_set = BS_TEST_SET(
    true, "context_menu", _wlmaker_context_menu_test_cases);

/** Test callback: Increments the counter. */
static void _test_callback(void *ud_ptr) { ++*(int*)ud_ptr; }

/* ------------------------------------------------------------------------- */
/** Adds and triggers items; cancelling only applies to the owner's menu. */
void test_items(bs_test_t *test_ptr)
{
    wlmaker_config_style_t style = {
        .window_style_ptr = wlmtk_window_style_create(),
        .menu_style_ptr = wlmtk_menu_style_create()
    };
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, style.window_style_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, style.menu_style_ptr);
    wlmaker_server_t server = { .style_ptr = &style };
    int owner, a = 0, b = 0;

    wlmaker_context_menu_t *cm_ptr = wlmaker_context_menu_create(
        &server, &owner, "Test");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, cm_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_context_menu_add_item(
                            cm_ptr, "A", _test_callback, &a));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_context_menu_add_item(
                            cm_ptr, "B", _test_callback, &b));
    wlmtk_menu_t *menu_ptr = wlmaker_context_menu_menu(cm_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_menu_items_size(menu_ptr));

    wlmtk_menu_set_open(menu_ptr, true);
    wlmtk_menu_item_trigger(wlmtk_menu_item_at(menu_ptr, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 0, a);
    BS_TEST_VERIFY_EQ(test_ptr, 1, b);
    // Triggering closes the menu.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_is_open(menu_ptr));

    // Submenu with options. The item's data is released along with the menu.
    int *c_ptr = calloc(1, sizeof(int));
    wlmtk_menu_t *sub_ptr = wlmaker_context_menu_add_submenu(
        cm_ptr, NULL, "Sub");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, sub_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 3, wlmtk_menu_items_size(menu_ptr));
    BS_TEST_VERIFY_EQ(test_ptr, sub_ptr, wlmtk_menu_item_get_submenu(
                          wlmtk_menu_item_at(menu_ptr, 2)));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_context_menu_add_option(
                            cm_ptr, sub_ptr, "C",
                            WLMTK_MENU_ITEM_INDICATOR_CHECK, true,
                            _test_callback, c_ptr, free));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmtk_menu_items_size(sub_ptr));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_item_checked(
                            wlmtk_menu_item_at(sub_ptr, 0)));
    wlmtk_menu_set_open(menu_ptr, true);
    wlmtk_menu_item_trigger(wlmtk_menu_item_at(sub_ptr, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, *c_ptr);

    // As if it were opened: The server holds it. Other owners don't close it.
    server.context_menu_ptr = cm_ptr;
    cm_ptr->opened = true;
    wlmaker_context_menu_cancel(&server, &a);
    BS_TEST_VERIFY_EQ(test_ptr, cm_ptr, server.context_menu_ptr);
    wlmaker_context_menu_cancel(&server, &owner);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, server.context_menu_ptr);

    wlmtk_window_style_ref_release(
        wlmtk_window_style_to_ref(style.window_style_ptr));
    wlmtk_menu_style_ref_release(wlmtk_menu_style_to_ref(style.menu_style_ptr));
}

/* ------------------------------------------------------------------------- */
/** Verifies the menu is kept within the usable area. */
void test_confine(bs_test_t *test_ptr)
{
    struct wlr_box area = { .x = 64, .y = 0, .width = 1152, .height = 720 };
    struct wlr_box b;

    // Opened over a dock on the left: Moved right of it.
    b = wlmaker_context_menu_confine(&area, 30, 100, 150, 200);
    BS_TEST_VERIFY_EQ(test_ptr, 64, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 100, b.y);

    // Opened over a dock on the right, near the bottom: Left of it, up.
    b = wlmaker_context_menu_confine(&area, 1250, 650, 150, 200);
    BS_TEST_VERIFY_EQ(test_ptr, 1216 - 150, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 520, b.y);

    // Inside: Unchanged.
    b = wlmaker_context_menu_confine(&area, 300, 300, 150, 200);
    BS_TEST_VERIFY_EQ(test_ptr, 300, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 300, b.y);

    // Larger than the area: At the top-left.
    b = wlmaker_context_menu_confine(&area, 300, 300, 2000, 2000);
    BS_TEST_VERIFY_EQ(test_ptr, 64, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 0, b.y);
}

/* == End of context_menu.c ================================================ */
