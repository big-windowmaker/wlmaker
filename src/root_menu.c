/* ========================================================================= */
/**
 * @file root_menu.c
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

#include "root_menu.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

#include "../etc/root_menu.h"
#include "action.h"
#include "action_item.h"
#include "config.h"
#include "context_menu.h"
#include "input/manager.h"
#include "menu_scroll.h"
#include "util/subprocess_monitor.h"
#include "server.h"

struct wlm_util_subprocess;

/* == Declarations ========================================================= */

/** State of the root menu. */
struct _wlmaker_root_menu_t {
    /** Window. */
    wlmtk_window_t           *window_ptr;

    /** The root menu base instance. */
    wlmtk_menu_t              *menu_ptr;
    /** Listener for @ref wlmtk_menu_events_t::open_changed. */
    struct wl_listener        menu_open_changed_listener;
    /** Listener for @ref wlmtk_menu_events_t::request_close. */
    struct wl_listener        menu_request_close_listener;
    /** Listener for @ref wlmtk_menu_events_t::outside_press. */
    struct wl_listener        menu_outside_press_listener;

    /** Listener for @ref wlmtk_window_events_t::request_close. */
    struct wl_listener        window_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        window_set_activated_listener;

    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** Scrolls the menu at the edge of the output. */
    wlmaker_menu_scroll_t     *menu_scroll_ptr;
    /** Pinned (by dragging its title bar): Stays open after an action. */
    bool                      pinned;
    /** Style of the submenus' titles: That of the windows' title bar. */
    struct wlmtk_menu_style   *title_style_ptr;
    /** The submenus' titles (@ref _wlmaker_root_menu_title_t). */
    bs_dllist_t               titles;
    /** Torn-off submenus (@ref _wlmaker_root_menu_torn_t). */
    bs_dllist_t               torn;
    /** Listener for @ref wlmtk_menu_events_t::inside_press. */
    struct wl_listener        menu_inside_press_listener;
};

/**
 * The title of a submenu, as Window Maker draws it: Dragging it tears off a
 * copy of the submenu, pinned, with the close button.
 */
typedef struct {
    /** Node in @ref wlmaker_root_menu_t::titles. */
    bs_dllist_node_t          dlnode;
    /** Back-link. */
    wlmaker_root_menu_t       *root_menu_ptr;
    /** The title item (first of the submenu). */
    wlmtk_menu_item_t         *item_ptr;
    /** The submenu's definition, for the copy. */
    bspl_array_t              *array_ptr;
    /** Listener for the item's destroy event. */
    struct wl_listener        destroy_listener;
    /** The item element's original virtual methods. */
    wlmtk_element_vmt_t       orig_vmt;
} _wlmaker_root_menu_title_t;

/** A torn-off submenu: A pinned menu window of its own. */
typedef struct {
    /** Node in @ref wlmaker_root_menu_t::torn. */
    bs_dllist_node_t          dlnode;
    /** Back-link. */
    wlmaker_root_menu_t       *root_menu_ptr;
    /** The menu. */
    wlmtk_menu_t              *menu_ptr;
    /** Its window. */
    wlmtk_window_t            *window_ptr;
    /** Listener for the window's close button. */
    struct wl_listener        request_close_listener;
    /** Destroys it after the close button's handler returned. */
    struct wl_event_source    *idle_ptr;
} _wlmaker_root_menu_torn_t;

/** State of a menu generator, while waiting for subprocess to complete. */
typedef struct {
    /** Subprocess handle. */
    struct wlm_util_subprocess *subprocess_handle_ptr;
    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** The menu this generator is going to populate. */
    wlmtk_menu_t              *menu_ptr;
    /** Menu style reference. */
    wlmtk_menu_style_ref_t    *style_ref_ptr;
    /** Dynamic buffer to hold stdout while the process is running. */
    bs_dynbuf_t               *stdout_dynbuf_ptr;
    /** Listener for @ref wlmtk_menu_events_t::destroy. */
    struct wl_listener        menu_destroy_listener;
} wlmaker_root_menu_generator_t;

static void _wlmaker_root_menu_handle_window_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_root_menu_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmaker_root_menu_handle_menu_open_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static bool _wlmaker_root_menu_window_at_cursor(
    wlmaker_root_menu_t *root_menu_ptr);
static void _wlmaker_root_menu_update_title_style(
    wlmaker_root_menu_t *root_menu_ptr);
static bool _wlmaker_root_menu_add_title(
    wlmaker_root_menu_t *root_menu_ptr,
    wlmtk_menu_t *submenu_ptr,
    bspl_array_t *array_ptr);
static void _wlmaker_root_menu_handle_inside_press(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_root_menu_tear_off(
    _wlmaker_root_menu_title_t *title_ptr);
static void _wlmaker_root_menu_torn_destroy(_wlmaker_root_menu_torn_t *torn_ptr);

/** The root menu, for the titles' element handlers (there is one). */
static wlmaker_root_menu_t *_wlmaker_root_menu_instance = NULL;
static void _wlmaker_root_menu_handle_outside_press(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_root_menu_handle_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static bool _wlmaker_root_menu_init_menu_from_array(
    wlmtk_menu_t *menu_ptr,
    bspl_array_t *array_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr);
static bool _wlmaker_root_menu_populate_menu_items_from_array(
    wlmtk_menu_t *menu_ptr,
    bspl_array_t *array_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr);
static bool _wlmaker_root_menu_populate_menu_items_from_file(
    wlmtk_menu_t *menu_ptr,
    const char *filename_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr);
static bool _wlmaker_root_menu_populate_menu_items_from_generator(
    wlmtk_menu_t *menu_ptr,
    const char *command_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr);

static void _wlmaker_root_menu_generator_destroy(
    wlmaker_root_menu_generator_t *gen_menu_ptr);
static void _wlmaker_root_menu_generator_handle_menu_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_root_menu_generator_handle_terminated(
    void *userdata_ptr,
    struct wlm_util_subprocess *subprocess_handle_ptr,
    int state,
    int code);

static wlmtk_menu_item_t *_wlmaker_root_menu_create_item_from_array(
    bspl_array_t *item_array_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr);
static wlmtk_menu_item_t *_wlmaker_root_menu_create_disabled_item(
    wlmtk_menu_style_ref_t *style_ref_ptr,
    const char *fmt_ptr,
    ...) __ARG_PRINTF__(2, 3);

/* == Data ================================================================= */

/** Indicates to load the file specified in following argument. */
static const char *_wlmaker_root_menu_statement_include = "IncludePlistMenu";
/** How far above the menu a press counts as on its title bar, in pixels. */
static const double _wlmaker_root_menu_titlebar_reach = 32;

/**
 * Indicates to generate the menu using a shell command specified in the
 * following argument.
 */
static const char *_wlmaker_root_menu_statement_generate = "GeneratePlistMenu";

/**
 * Unit test injector: struct wl_display that will be terminated when
 * subprocess terminates. Must be NULL when not for in unit tests.
 */
static struct wl_display      *_wlmaker_root_menu_test_wl_display_ptr = NULL;
/** Number of generators, used to terminate display in unit tests. */
static size_t                 _wlmaker_root_menu_generators = 0;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_root_menu_t *wlmaker_root_menu_create(
    wlmaker_server_t *server_ptr,
    const char *arg_root_menu_file_ptr,
    wlmtk_window_style_ref_t *window_style_ref_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr)
{
    bspl_array_t *root_menu_array_ptr = bspl_array_from_object(
        wlmaker_config_object_load(
            server_ptr->files_ptr,
            "root_menu",
            arg_root_menu_file_ptr,
            "RootMenu.plist",
            embedded_binary_root_menu_data,
            embedded_binary_root_menu_size));
    if (NULL == root_menu_array_ptr) return NULL;

    if (bspl_array_size(root_menu_array_ptr) <= 1) {
        bs_log(BS_ERROR, "Needs > 1 array element for menu definition.");
        return NULL;
    }
    if (BSPL_STRING != bspl_object_type(
            bspl_array_at(root_menu_array_ptr, 0))) {
        bs_log(BS_ERROR, "Array element [0] must be a string.");
        return NULL;
    }

    wlmaker_root_menu_t *root_menu_ptr = logged_calloc(
        1, sizeof(wlmaker_root_menu_t));
    if (NULL == root_menu_ptr) return NULL;
    root_menu_ptr->server_ptr = server_ptr;
    root_menu_ptr->server_ptr->root_menu_ptr = root_menu_ptr;
    _wlmaker_root_menu_instance = root_menu_ptr;
    _wlmaker_root_menu_update_title_style(root_menu_ptr);

    root_menu_ptr->menu_ptr = wlmtk_menu_create(menu_style_ref_ptr);
    if (NULL == root_menu_ptr->menu_ptr) {
        wlmaker_root_menu_destroy(root_menu_ptr);
        bspl_array_unref(root_menu_array_ptr);
        return NULL;
    }
    if (!_wlmaker_root_menu_init_menu_from_array(
            root_menu_ptr->menu_ptr,
            root_menu_array_ptr,
            menu_style_ref_ptr,
            server_ptr)) {
        bspl_array_unref(root_menu_array_ptr);
        return NULL;
    }

    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(root_menu_ptr->menu_ptr)->open_changed,
        &root_menu_ptr->menu_open_changed_listener,
        _wlmaker_root_menu_handle_menu_open_changed);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(root_menu_ptr->menu_ptr)->request_close,
        &root_menu_ptr->menu_request_close_listener,
        _wlmaker_root_menu_handle_request_close);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(root_menu_ptr->menu_ptr)->outside_press,
        &root_menu_ptr->menu_outside_press_listener,
        _wlmaker_root_menu_handle_outside_press);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(root_menu_ptr->menu_ptr)->inside_press,
        &root_menu_ptr->menu_inside_press_listener,
        _wlmaker_root_menu_handle_inside_press);

    root_menu_ptr->window_ptr = wlmtk_window_create(
        wlmtk_menu_element(root_menu_ptr->menu_ptr),
        window_style_ref_ptr,
        menu_style_ref_ptr);
    if (NULL == root_menu_ptr->window_ptr) {
        wlmaker_root_menu_destroy(root_menu_ptr);
        bspl_array_unref(root_menu_array_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(root_menu_ptr->window_ptr)->request_close,
        &root_menu_ptr->window_request_close_listener,
        _wlmaker_root_menu_handle_window_request_close);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(root_menu_ptr->window_ptr)->set_activated,
        &root_menu_ptr->window_set_activated_listener,
        _wlmaker_root_menu_handle_window_set_activated);

    wlmtk_window_set_title(
        root_menu_ptr->window_ptr,
        bspl_array_string_value_at(root_menu_array_ptr, 0));
    wlmtk_window_set_server_side_decorated(root_menu_ptr->window_ptr, true);

    root_menu_ptr->menu_scroll_ptr = wlmaker_menu_scroll_create(
        server_ptr, root_menu_ptr->window_ptr, root_menu_ptr->menu_ptr);
    if (NULL == root_menu_ptr->menu_scroll_ptr) {
        wlmaker_root_menu_destroy(root_menu_ptr);
        bspl_array_unref(root_menu_array_ptr);
        return NULL;
    }

    bspl_array_unref(root_menu_array_ptr);
    return root_menu_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_root_menu_destroy(wlmaker_root_menu_t *root_menu_ptr)
{
    if (_wlmaker_root_menu_instance == root_menu_ptr) {
        _wlmaker_root_menu_instance = NULL;
    }
    while (NULL != root_menu_ptr->torn.head_ptr) {
        _wlmaker_root_menu_torn_destroy(BS_CONTAINER_OF(
            root_menu_ptr->torn.head_ptr, _wlmaker_root_menu_torn_t, dlnode));
    }
    if (NULL != root_menu_ptr->menu_scroll_ptr) {
        wlmaker_menu_scroll_destroy(root_menu_ptr->menu_scroll_ptr);
        root_menu_ptr->menu_scroll_ptr = NULL;
    }
    if (NULL != root_menu_ptr->server_ptr) {
        BS_ASSERT(root_menu_ptr->server_ptr->root_menu_ptr == root_menu_ptr);
        root_menu_ptr->server_ptr->root_menu_ptr = NULL;
        root_menu_ptr->server_ptr = NULL;;
    }

    if (NULL != root_menu_ptr->window_ptr) {
        // Unmap, in case it's not unmapped yet.
        wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
            root_menu_ptr->window_ptr);
        if (NULL != workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr,
                                          root_menu_ptr->window_ptr);
        }

        wlmtk_util_disconnect_listener(
            &root_menu_ptr->window_request_close_listener);
        wlmtk_util_disconnect_listener(
            &root_menu_ptr->window_set_activated_listener);

        wlmtk_window_destroy(root_menu_ptr->window_ptr);
        root_menu_ptr->window_ptr = NULL;
    }

    if (NULL != root_menu_ptr->menu_ptr) {
        wlmtk_util_disconnect_listener(
            &root_menu_ptr->menu_request_close_listener);
        wlmtk_util_disconnect_listener(
            &root_menu_ptr->menu_outside_press_listener);
        wlmtk_util_disconnect_listener(
            &root_menu_ptr->menu_inside_press_listener);
        wlmtk_util_disconnect_listener(
            &root_menu_ptr->menu_open_changed_listener);
        wlmtk_menu_destroy(root_menu_ptr->menu_ptr);
        root_menu_ptr->menu_ptr = NULL;
    }
    if (NULL != root_menu_ptr->title_style_ptr) {
        wlmtk_menu_style_ref_release(
            wlmtk_menu_style_to_ref(root_menu_ptr->title_style_ptr));
        root_menu_ptr->title_style_ptr = NULL;
    }
    free(root_menu_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_root_menu_reload(
    wlmaker_server_t *server_ptr,
    const char *arg_root_menu_file_ptr)
{
    wlmaker_root_menu_t *old_ptr = server_ptr->root_menu_ptr;
    if (NULL != old_ptr &&
        NULL != wlmtk_window_get_workspace(old_ptr->window_ptr)) {
        bs_log(BS_WARNING, "Root menu is open, not reloading it.");
        return false;
    }

    // wlmaker_root_menu_create() registers the new menu with the server.
    server_ptr->root_menu_ptr = NULL;
    wlmaker_root_menu_t *new_ptr = wlmaker_root_menu_create(
        server_ptr,
        arg_root_menu_file_ptr,
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == new_ptr) {
        bs_log(BS_WARNING, "Failed to reload the root menu, keeping it.");
        server_ptr->root_menu_ptr = old_ptr;
        return false;
    }
    wlmtk_menu_set_open(wlmaker_root_menu_menu(new_ptr), false);

    if (NULL != old_ptr) {
        server_ptr->root_menu_ptr = old_ptr;
        wlmaker_root_menu_destroy(old_ptr);
    }
    server_ptr->root_menu_ptr = new_ptr;
    return true;
}

/* ------------------------------------------------------------------------- */
wlmtk_window_t *wlmaker_root_menu_window(wlmaker_root_menu_t *root_menu_ptr)
{
    return root_menu_ptr->window_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_root_menu_pinned(wlmaker_root_menu_t *root_menu_ptr)
{
    return root_menu_ptr->pinned;
}

/* ------------------------------------------------------------------------- */
wlmtk_menu_t *wlmaker_root_menu_menu(wlmaker_root_menu_t *root_menu_ptr)
{
    return root_menu_ptr->menu_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_root_menu_open(wlmaker_root_menu_t *root_menu_ptr,
                            enum wlmtk_menu_mode mode)
{
    wlmaker_server_t *server_ptr = root_menu_ptr->server_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_desktop_get_current_workspace(
        server_ptr->desktop_ptr);
    struct wlr_cursor *wlr_cursor_ptr = wlmim_wlr_cursor(
        server_ptr->input_manager_ptr);
    int x = wlr_cursor_ptr->x, y = wlr_cursor_ptr->y;

    wlmtk_workspace_map_window(workspace_ptr, root_menu_ptr->window_ptr);
    wlmtk_workspace_set_window_position(
        workspace_ptr, root_menu_ptr->window_ptr, x, y);

    struct wlr_box area = wlmtk_desktop_get_usable_area(
        server_ptr->desktop_ptr,
        wlmaker_server_get_output_at_cursor(server_ptr));
    struct wlr_box bbox = wlmtk_window_get_bounding_box(
        root_menu_ptr->window_ptr);
    struct wlr_box pos = wlmaker_context_menu_confine(
        &area, bbox.x, bbox.y, bbox.width, bbox.height);
    wlmtk_workspace_set_window_position(
        workspace_ptr, root_menu_ptr->window_ptr,
        x + pos.x - bbox.x, y + pos.y - bbox.y);
    wlmtk_menu_set_bounds(root_menu_ptr->menu_ptr, &area);

    wlmtk_menu_set_mode(root_menu_ptr->menu_ptr, mode);
    wlmtk_menu_set_open(root_menu_ptr->menu_ptr, true);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Handles when window close button is pressed: Hides the menu. */
void _wlmaker_root_menu_handle_window_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_t, window_request_close_listener);
    wlmtk_menu_set_open(root_menu_ptr->menu_ptr, false);

}

/* ------------------------------------------------------------------------- */
/** Handles when the menu is activated: Get keyboard focus. */
void _wlmaker_root_menu_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_t, window_set_activated_listener);

    wlmtk_element_t *e = wlmtk_menu_element(root_menu_ptr->menu_ptr);
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_set_keyboard_focus_element(
            e->parent_container_ptr,
            e,
            wlmtk_window_is_activated(root_menu_ptr->window_ptr));
    }
}


/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_events_t::open_changed. Unmaps window on close. */
void _wlmaker_root_menu_handle_menu_open_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_t, menu_open_changed_listener);
    if (!wlmtk_menu_is_open(root_menu_ptr->menu_ptr)) {
        root_menu_ptr->pinned = false;
    }
    if (!wlmtk_menu_is_open(root_menu_ptr->menu_ptr) &&
        NULL != wlmtk_window_get_workspace(root_menu_ptr->window_ptr)) {
        wlmtk_workspace_unmap_window(
            wlmtk_window_get_workspace(root_menu_ptr->window_ptr),
            root_menu_ptr->window_ptr);
    } else {

        uint32_t properties = 0;
        if (WLMTK_MENU_MODE_RIGHTCLICK ==
            wlmtk_menu_get_mode(root_menu_ptr->menu_ptr)) {
            properties |= WLMTK_WINDOW_PROPERTY_RIGHTCLICK;
            wlmtk_container_pointer_grab(
                wlmtk_menu_element(
                    root_menu_ptr->menu_ptr)->parent_container_ptr,
                wlmtk_menu_element(root_menu_ptr->menu_ptr));
        } else {
            properties |= WLMTK_WINDOW_PROPERTY_CLOSABLE;
        }
        wlmtk_window_set_properties(root_menu_ptr->window_ptr, properties);

    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_root_menu_restyle(wlmaker_root_menu_t *root_menu_ptr)
{
    wlmaker_server_t *server_ptr = root_menu_ptr->server_ptr;
    if (NULL == server_ptr) return;
    for (bs_dllist_node_t *d = root_menu_ptr->torn.head_ptr; NULL != d;
         d = d->next_ptr) {
        _wlmaker_root_menu_torn_t *t = BS_CONTAINER_OF(
            d, _wlmaker_root_menu_torn_t, dlnode);
        wlmtk_menu_set_style(
            t->menu_ptr,
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
        wlmtk_window_set_style(
            t->window_ptr,
            wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    }
    // The menus took the items' style: The titles get theirs again.
    _wlmaker_root_menu_update_title_style(root_menu_ptr);
    for (bs_dllist_node_t *d = root_menu_ptr->titles.head_ptr; NULL != d;
         d = d->next_ptr) {
        _wlmaker_root_menu_title_t *t = BS_CONTAINER_OF(
            d, _wlmaker_root_menu_title_t, dlnode);
        wlmtk_menu_item_set_style(
            t->item_ptr, wlmtk_menu_style_to_ref(root_menu_ptr->title_style_ptr));
    }
}

/* ------------------------------------------------------------------------- */
/** The titles' style: The menu's, with the fill, font, colors and height of
 * the windows' title bar (Window Maker draws a submenu's title so). */
void _wlmaker_root_menu_update_title_style(wlmaker_root_menu_t *root_menu_ptr)
{
    wlmaker_server_t *server_ptr = root_menu_ptr->server_ptr;
    if (NULL == server_ptr || NULL == server_ptr->style_ptr ||
        NULL == server_ptr->style_ptr->menu_style_ptr ||
        NULL == server_ptr->style_ptr->window_style_ptr) return;
    if (NULL == root_menu_ptr->title_style_ptr) {
        root_menu_ptr->title_style_ptr = wlmtk_menu_style_create();
        if (NULL == root_menu_ptr->title_style_ptr) return;
    }
    const struct wlmtk_titlebar_style *ts_ptr =
        &server_ptr->style_ptr->window_style_ptr->titlebar;
    struct wlmtk_menu_style *s_ptr = root_menu_ptr->title_style_ptr;
    *s_ptr = *server_ptr->style_ptr->menu_style_ptr;
    s_ptr->item.fill = ts_ptr->focussed_fill;
    s_ptr->item.highlighted_fill = ts_ptr->focussed_fill;
    s_ptr->item.font = ts_ptr->font;
    s_ptr->item.height = ts_ptr->height;
    s_ptr->item.enabled_text_color = ts_ptr->focussed_text_color;
    s_ptr->item.highlighted_text_color = ts_ptr->focussed_text_color;
    s_ptr->item.disabled_text_color = ts_ptr->focussed_text_color;
}

/* ------------------------------------------------------------------------- */
/** Finds the title record of `element_ptr`. */
static _wlmaker_root_menu_title_t *_wlmaker_root_menu_title_of(
    wlmaker_root_menu_t *root_menu_ptr,
    wlmtk_element_t *element_ptr)
{
    for (bs_dllist_node_t *d = root_menu_ptr->titles.head_ptr; NULL != d;
         d = d->next_ptr) {
        _wlmaker_root_menu_title_t *t = BS_CONTAINER_OF(
            d, _wlmaker_root_menu_title_t, dlnode);
        if (wlmtk_menu_item_element(t->item_ptr) == element_ptr) return t;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** A press on a title (menu in window mode: pinned, or torn off). */
static bool _wlmaker_root_menu_title_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    _wlmaker_root_menu_title_t *t = NULL;
    if (NULL != _wlmaker_root_menu_instance) {
        t = _wlmaker_root_menu_title_of(_wlmaker_root_menu_instance, element_ptr);
    }
    if (NULL == t) return false;
    if (BTN_LEFT == button_event_ptr->button &&
        WLMTK_BUTTON_DOWN == button_event_ptr->type) {
        _wlmaker_root_menu_tear_off(t);
        return true;
    }
    return t->orig_vmt.pointer_button(element_ptr, button_event_ptr);
}

/* ------------------------------------------------------------------------- */
/** The title record goes with its item. */
static void _wlmaker_root_menu_handle_title_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    _wlmaker_root_menu_title_t *t = BS_CONTAINER_OF(
        listener_ptr, _wlmaker_root_menu_title_t, destroy_listener);
    wlmtk_util_disconnect_listener(&t->destroy_listener);
    bs_dllist_remove(&t->root_menu_ptr->titles, &t->dlnode);
    if (NULL != t->array_ptr) bspl_array_unref(t->array_ptr);
    free(t);
}

/* ------------------------------------------------------------------------- */
/** Puts the title on top of `submenu_ptr`, defined by `array_ptr`. */
bool _wlmaker_root_menu_add_title(
    wlmaker_root_menu_t *root_menu_ptr,
    wlmtk_menu_t *submenu_ptr,
    bspl_array_t *array_ptr)
{
    if (NULL == root_menu_ptr->title_style_ptr) return false;
    _wlmaker_root_menu_title_t *t = logged_calloc(
        1, sizeof(_wlmaker_root_menu_title_t));
    if (NULL == t) return false;
    t->root_menu_ptr = root_menu_ptr;
    t->item_ptr = wlmtk_menu_item_create(
        wlmtk_menu_style_to_ref(root_menu_ptr->title_style_ptr));
    if (NULL == t->item_ptr) {
        free(t);
        return false;
    }
    wlmtk_menu_item_set_text(t->item_ptr,
                             bspl_array_string_value_at(array_ptr, 0));
    wlmtk_menu_item_set_enabled(t->item_ptr, false);
    t->array_ptr = bspl_array_ref(array_ptr);
    static const wlmtk_element_vmt_t vmt = {
        .pointer_button = _wlmaker_root_menu_title_pointer_button,
    };
    t->orig_vmt = wlmtk_element_extend(wlmtk_menu_item_element(t->item_ptr), &vmt);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(t->item_ptr)->destroy,
        &t->destroy_listener,
        _wlmaker_root_menu_handle_title_destroy);
    bs_dllist_push_back(&root_menu_ptr->titles, &t->dlnode);
    wlmtk_menu_add_item(submenu_ptr, t->item_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** A press inside the root menu opened by a click: On a title, tear off. */
void _wlmaker_root_menu_handle_inside_press(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_t, menu_inside_press_listener);
    wlmtk_menu_outside_press_t *press_ptr = data_ptr;
    if (BTN_LEFT != press_ptr->button_event_ptr->button) return;
    struct wlr_cursor *c_ptr = wlmim_wlr_cursor(
        root_menu_ptr->server_ptr->input_manager_ptr);
    for (bs_dllist_node_t *d = root_menu_ptr->titles.head_ptr; NULL != d;
         d = d->next_ptr) {
        _wlmaker_root_menu_title_t *t = BS_CONTAINER_OF(
            d, _wlmaker_root_menu_title_t, dlnode);
        wlmtk_element_t *e = wlmtk_menu_item_element(t->item_ptr);
        if (!e->visible || NULL == e->wlr_scene_node_ptr ||
            !e->wlr_scene_node_ptr->enabled) continue;
        int x, y, l, tp, r, b;
        wlmtk_element_get_absolute_position(e, &x, &y);
        wlmtk_element_get_dimensions(e, &l, &tp, &r, &b);
        if (c_ptr->x >= x + l && c_ptr->x < x + r &&
            c_ptr->y >= y + tp && c_ptr->y < y + b) {
            press_ptr->claimed = true;
            _wlmaker_root_menu_tear_off(t);
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Closes (destroys) a torn-off submenu, after the close button's handler. */
static void _wlmaker_root_menu_torn_idle(void *data_ptr)
{
    _wlmaker_root_menu_torn_t *t = data_ptr;
    t->idle_ptr = NULL;  // An idle source frees itself once dispatched.
    _wlmaker_root_menu_torn_destroy(t);
}

/* ------------------------------------------------------------------------- */
/** The close button of a torn-off submenu. */
static void _wlmaker_root_menu_handle_torn_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    _wlmaker_root_menu_torn_t *t = BS_CONTAINER_OF(
        listener_ptr, _wlmaker_root_menu_torn_t, request_close_listener);
    if (NULL != t->idle_ptr) return;
    wlmtk_workspace_t *ws_ptr = wlmtk_window_get_workspace(t->window_ptr);
    if (NULL != ws_ptr) wlmtk_workspace_unmap_window(ws_ptr, t->window_ptr);
    t->idle_ptr = wl_event_loop_add_idle(
        wl_display_get_event_loop(t->root_menu_ptr->server_ptr->wl_display_ptr),
        _wlmaker_root_menu_torn_idle, t);
}

/* ------------------------------------------------------------------------- */
void _wlmaker_root_menu_torn_destroy(_wlmaker_root_menu_torn_t *t)
{
    if (NULL != t->idle_ptr) {
        wl_event_source_remove(t->idle_ptr);
        t->idle_ptr = NULL;
    }
    wlmtk_util_disconnect_listener(&t->request_close_listener);
    bs_dllist_remove(&t->root_menu_ptr->torn, &t->dlnode);
    if (NULL != t->window_ptr) {
        wlmtk_workspace_t *ws_ptr = wlmtk_window_get_workspace(t->window_ptr);
        if (NULL != ws_ptr) wlmtk_workspace_unmap_window(ws_ptr, t->window_ptr);
        wlmtk_window_destroy(t->window_ptr);
    }
    if (NULL != t->menu_ptr) wlmtk_menu_destroy(t->menu_ptr);
    free(t);
}

/* ------------------------------------------------------------------------- */
/**
 * Window Maker: Dragging a submenu's title tears it off: A copy of it, as a
 * window of its own, pinned (with the close button), that moves with the
 * pointer. The root menu closes, unless pinned.
 */
void _wlmaker_root_menu_tear_off(_wlmaker_root_menu_title_t *title_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = title_ptr->root_menu_ptr;
    wlmaker_server_t *server_ptr = root_menu_ptr->server_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_desktop_get_current_workspace(
        server_ptr->desktop_ptr);
    if (NULL == workspace_ptr) return;
    int x, y;
    wlmtk_element_get_absolute_position(
        wlmtk_menu_item_element(title_ptr->item_ptr), &x, &y);

    _wlmaker_root_menu_torn_t *t = logged_calloc(
        1, sizeof(_wlmaker_root_menu_torn_t));
    if (NULL == t) return;
    t->root_menu_ptr = root_menu_ptr;
    bs_dllist_push_back(&root_menu_ptr->torn, &t->dlnode);
    wlmtk_menu_style_ref_t *menu_style_ref_ptr = wlmtk_menu_style_to_ref(
        server_ptr->style_ptr->menu_style_ptr);
    t->menu_ptr = wlmtk_menu_create(menu_style_ref_ptr);
    if (NULL == t->menu_ptr ||
        !_wlmaker_root_menu_init_menu_from_array(
            t->menu_ptr, title_ptr->array_ptr, menu_style_ref_ptr, server_ptr)) {
        _wlmaker_root_menu_torn_destroy(t);
        return;
    }
    t->window_ptr = wlmtk_window_create(
        wlmtk_menu_element(t->menu_ptr),
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        menu_style_ref_ptr);
    if (NULL == t->window_ptr) {
        _wlmaker_root_menu_torn_destroy(t);
        return;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(t->window_ptr)->request_close,
        &t->request_close_listener,
        _wlmaker_root_menu_handle_torn_close);
    wlmtk_window_set_title(t->window_ptr,
                           bspl_array_string_value_at(title_ptr->array_ptr, 0));
    wlmtk_window_set_server_side_decorated(t->window_ptr, true);
    wlmtk_window_set_properties(t->window_ptr, WLMTK_WINDOW_PROPERTY_CLOSABLE);
    wlmtk_menu_set_mode(t->menu_ptr, WLMTK_MENU_MODE_NORMAL);
    wlmtk_menu_set_open(t->menu_ptr, true);

    if (!root_menu_ptr->pinned) {
        wlmtk_menu_set_open(root_menu_ptr->menu_ptr, false);
    }
    wlmtk_workspace_map_window(workspace_ptr, t->window_ptr);
    wlmtk_workspace_set_window_position(workspace_ptr, t->window_ptr, x, y);
    wlmtk_workspace_begin_window_move(workspace_ptr, t->window_ptr);
}

/* ------------------------------------------------------------------------- */
/** @return Whether a window (other than the root menu) is under the cursor. */
bool _wlmaker_root_menu_window_at_cursor(wlmaker_root_menu_t *root_menu_ptr)
{
    wlmaker_server_t *server_ptr = root_menu_ptr->server_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_desktop_get_current_workspace(
        server_ptr->desktop_ptr);
    if (NULL == workspace_ptr) return false;
    struct wlr_cursor *c_ptr = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
    bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(workspace_ptr);
    for (bs_dllist_node_t *d = list_ptr->head_ptr; NULL != d; d = d->next_ptr) {
        wlmtk_window_t *w = wlmtk_window_from_dlnode(d);
        if (w == root_menu_ptr->window_ptr || wlmtk_window_is_minimized(w)) {
            continue;
        }
        struct wlr_box b = wlmtk_window_get_bounding_box(w);
        if (wlr_box_contains_point(&b, c_ptr->x, c_ptr->y)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/**
 * Listens to @ref wlmtk_menu_events_t::outside_press. Window Maker: Dragging
 * the title bar of a menu opened by a click pins it. It stays open (also
 * after choosing an item), with the close button, and moves along.
 */
void _wlmaker_root_menu_handle_outside_press(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_t, menu_outside_press_listener);
    wlmtk_menu_outside_press_t *press_ptr = data_ptr;
    wlmtk_element_t *e = wlmtk_menu_element(root_menu_ptr->menu_ptr);
    int width = 0;
    wlmtk_element_get_dimensions(e, NULL, NULL, &width, NULL);
    // Window Maker: A right-click elsewhere on the desktop opens the menu
    // there at once (over a window, it just closes).
    if (BTN_RIGHT == press_ptr->button_event_ptr->button &&
        !root_menu_ptr->pinned &&
        !_wlmaker_root_menu_window_at_cursor(root_menu_ptr)) {
        press_ptr->claimed = true;
        wlmtk_menu_set_open(root_menu_ptr->menu_ptr, false);
        wlmaker_root_menu_open(root_menu_ptr, WLMTK_MENU_MODE_RIGHTCLICK);
        return;
    }
    // The title bar is right above the menu.
    if (BTN_LEFT != press_ptr->button_event_ptr->button ||
        press_ptr->y >= 0 ||
        press_ptr->y < -_wlmaker_root_menu_titlebar_reach ||
        press_ptr->x < 0 || press_ptr->x >= width) return;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        root_menu_ptr->window_ptr);
    if (NULL == workspace_ptr) return;

    press_ptr->claimed = true;
    root_menu_ptr->pinned = true;
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_pointer_grab_release(e->parent_container_ptr, e);
    }
    wlmtk_menu_set_mode(root_menu_ptr->menu_ptr, WLMTK_MENU_MODE_NORMAL);
    wlmtk_window_set_properties(root_menu_ptr->window_ptr,
                                WLMTK_WINDOW_PROPERTY_CLOSABLE);
    wlmtk_workspace_begin_window_move(workspace_ptr, root_menu_ptr->window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Listens to @ref wlmtk_menu_events_t::request_close. Closes the menu. */
void _wlmaker_root_menu_handle_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_root_menu_t *root_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_t, menu_request_close_listener);

    wlmtk_menu_set_open(root_menu_ptr->menu_ptr, false);
}

/* ------------------------------------------------------------------------- */
/**
 * Initializes the menu from the menu configuration array.
 *
 * The menu configuration is a Plist array. The first item is the menu's title,
 * while the second item defines the nature of the menu configuration:
 *
 * It can define a set of menu items, in form of Plist arrays:
 * @verbinclude tests/data/menu.plist
 *
 * Or, it is a definition to include a Plist menu:
 * @verbinclude tests/data/menu-include.plist
 *
 * Or, it is a definition to generate a Plist menu:
 * @verbinclude tests/data/menu-generate.plist
 *
 * @param menu_ptr
 * @param array_ptr
 * @param menu_style_ref_ptr
 * @param server_ptr
 *
 * @return true on success.
 */
bool _wlmaker_root_menu_init_menu_from_array(
    wlmtk_menu_t *menu_ptr,
    bspl_array_t *array_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr)
{
    // (1) object must be array, and have >= 2 elements: title and content.
    if (2 > bspl_array_size(array_ptr)) {
        bs_log(BS_ERROR, "Plist menu definition array size must be >= 2.");
        return false;
    }

    bspl_object_t *content_object_ptr = bspl_array_at(array_ptr, 1);
    switch (bspl_object_type(content_object_ptr)) {
    case BSPL_ARRAY:

        // Indicates the first element is an item with a submenu, and there
        // are optionally further elements. Populate the parent menu from
        // that.
        return _wlmaker_root_menu_populate_menu_items_from_array(
            menu_ptr,
            array_ptr,
            menu_style_ref_ptr,
            server_ptr);

    case BSPL_STRING:

        if (3 > bspl_array_size(array_ptr)) {
            bs_log(BS_ERROR, "Must have 3 objects on \"%s\"",
                   bspl_array_string_value_at(array_ptr, 1));
            return false;
        }

        if (0 == strcmp(
                bspl_string_value_from_object(content_object_ptr),
                _wlmaker_root_menu_statement_include)) {

            return _wlmaker_root_menu_populate_menu_items_from_file(
                menu_ptr,
                bspl_array_string_value_at(array_ptr, 2),
                menu_style_ref_ptr,
                server_ptr);

        } else if (0 == strcmp(
                       bspl_string_value_from_object(content_object_ptr),                               _wlmaker_root_menu_statement_generate)) {

            return _wlmaker_root_menu_populate_menu_items_from_generator(
                menu_ptr,
                bspl_array_string_value_at(array_ptr, 2),
                menu_style_ref_ptr,
                server_ptr);

        }
        bs_log(BS_ERROR, "Unknown menu definition \"%s\"",
               bspl_string_value_from_object(content_object_ptr));
        return false;

    default:
        break;
    }

    bs_log(BS_ERROR, "Unhandled object type to populate menu.");
    return false;
}

/* ------------------------------------------------------------------------- */
/**
 * Populates the menu's items from the Plist array. This handles the case of
 * a menu configuration that specifies the menu items as a Plist array.
 *
 * The first item of `array_ptr` is the menu's title, and each further item
 * is expected to be another Plist array, defining a menu item.
 *
 * @param menu_ptr
 * @param array_ptr
 * @param menu_style_ref_ptr
 * @param server_ptr
 *
 * @return true on success
 */
bool _wlmaker_root_menu_populate_menu_items_from_array(
    wlmtk_menu_t *menu_ptr,
    bspl_array_t *array_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr)
{
    if (bspl_array_size(array_ptr) <= 1) {
        bs_log(BS_ERROR, "Needs > 1 array element for menu definition.");
        return false;
    }
    const char *name_ptr = bspl_array_string_value_at(array_ptr, 0);
    if (NULL == name_ptr) {
        bs_log(BS_ERROR, "Array element [0] must be a string.");
        return false;
    }

    for (size_t i = 1; i < bspl_array_size(array_ptr); ++i) {
        bspl_array_t *item_array_ptr = bspl_array_from_object(
            bspl_array_at(array_ptr, i));
        if (NULL == item_array_ptr) {
            bs_log(BS_ERROR, "Menu %s: Element [%zu] must be an array",
                   name_ptr, i);
            return false;
        }

        if (NULL == bspl_array_string_value_at(item_array_ptr, 0)) {
            bs_log(BS_ERROR,
                   "Menu %s: First element of item [%zu] must be a string",
                   name_ptr, i);
            return false;
        }

        wlmtk_menu_item_t *menu_item_ptr =
            _wlmaker_root_menu_create_item_from_array(
                item_array_ptr,
                menu_style_ref_ptr,
                server_ptr);
        if (NULL == menu_item_ptr) return false;
        wlmtk_menu_add_item(menu_ptr, menu_item_ptr);
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Loads a Plist array from file and populates the menu's items from it.
 *
 * @param menu_ptr
 * @param filename_ptr
 * @param menu_style_ref_ptr
 * @param server_ptr
 *
 * @return true on success
 */
bool _wlmaker_root_menu_populate_menu_items_from_file(
    wlmtk_menu_t *menu_ptr,
    const char *filename_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr)
{
    char *path_ptr = bs_file_resolve_path(filename_ptr, NULL);
    if (NULL == path_ptr) {
        bs_log(BS_ERROR, "Failed bs_file_resolve_path(\"%s\", NULL)",
               filename_ptr);
        return false;
    }

    bspl_object_t *object_ptr = bspl_create_object_from_plist_file(path_ptr);
    free(path_ptr);
    if (NULL == object_ptr || BSPL_ARRAY != bspl_object_type(object_ptr)) {
        bs_log(BS_ERROR, "Failed to load Plist ARRAY from \"%s\"",
               filename_ptr);
        if (NULL != object_ptr) bspl_object_unref(object_ptr);
        return false;
    }

    bool rv = _wlmaker_root_menu_populate_menu_items_from_array(
            menu_ptr,
            bspl_array_from_object(object_ptr),
            menu_style_ref_ptr,
            server_ptr);
    if (!rv) {
        bs_log(BS_ERROR, "Failed to generate menu from Plist file \"%s\"",
               filename_ptr);
    }
    bspl_object_unref(object_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Launches a subprocess, to populate the menu's items.
 *
 * Uses a @ref wlmaker_root_menu_generator_t to track state of the subprocess
 * and to tie it with the menu's lifecycle.
 *
 * @param menu_ptr
 * @param command_ptr
 * @param menu_style_ref_ptr
 * @param server_ptr
 *
 * @return true on success
 */
bool _wlmaker_root_menu_populate_menu_items_from_generator(
    wlmtk_menu_t *menu_ptr,
    const char *command_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr)
{
    bs_subprocess_t *subprocess_ptr = NULL;
    wlmaker_root_menu_generator_t *generator_ptr = logged_calloc(
        1, sizeof(wlmaker_root_menu_generator_t));
    if (NULL == generator_ptr) return false;
    generator_ptr->server_ptr = server_ptr;
    generator_ptr->menu_ptr = menu_ptr;
    generator_ptr->style_ref_ptr = menu_style_ref_ptr;

    generator_ptr->stdout_dynbuf_ptr = bs_dynbuf_create(1024, INT32_MAX);
    if (NULL == generator_ptr->stdout_dynbuf_ptr) goto error;

    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(menu_ptr)->destroy,
        &generator_ptr->menu_destroy_listener,
        _wlmaker_root_menu_generator_handle_menu_destroy);

    const char *args[] = { "/bin/sh", "-c", command_ptr, NULL };
    subprocess_ptr =  bs_subprocess_create(args[0], args, NULL);
    if (NULL == subprocess_ptr) goto error;

    if (!bs_subprocess_start(subprocess_ptr)) goto error;
    bs_log(BS_INFO,
           "Created subprocess %p [%"PRIdMAX"] for \"/bin/sh\" \"-c\" \"%s\"",
           subprocess_ptr,
           (intmax_t)bs_subprocess_pid(subprocess_ptr),
           command_ptr);

    _wlmaker_root_menu_generators++;
    generator_ptr->subprocess_handle_ptr = wlm_util_subprocess_monitor_entrust(
        server_ptr->monitor_ptr,
        subprocess_ptr,
        _wlmaker_root_menu_generator_handle_terminated,
        generator_ptr,
        generator_ptr->stdout_dynbuf_ptr);
    if (NULL == generator_ptr->subprocess_handle_ptr) goto error;

    return true;

error:
    if (NULL != subprocess_ptr) {
        bs_subprocess_destroy(subprocess_ptr);
    }
    _wlmaker_root_menu_generator_destroy(generator_ptr);
    return false;
}

/* ------------------------------------------------------------------------- */
/** Dtor for the menu generator. */
void _wlmaker_root_menu_generator_destroy(
    wlmaker_root_menu_generator_t *generator_ptr)
{
    if (NULL != generator_ptr->subprocess_handle_ptr) {
        wlm_util_subprocess_monitor_cede(
            generator_ptr->server_ptr->monitor_ptr,
            generator_ptr->subprocess_handle_ptr);
        generator_ptr->subprocess_handle_ptr = NULL;
    }

    if (NULL != generator_ptr->stdout_dynbuf_ptr) {
        bs_dynbuf_destroy(generator_ptr->stdout_dynbuf_ptr);
        generator_ptr->stdout_dynbuf_ptr = NULL;
    }

    wlmtk_util_disconnect_listener(&generator_ptr->menu_destroy_listener);
    free(generator_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_events_t::destroy. Calls dtor. */
void _wlmaker_root_menu_generator_handle_menu_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_root_menu_generator_t *generator_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_root_menu_generator_t, menu_destroy_listener);

    generator_ptr->menu_ptr = NULL;
    _wlmaker_root_menu_generator_destroy(generator_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handler for when the subprocess is terminated. */
void _wlmaker_root_menu_generator_handle_terminated(
    void *userdata_ptr,
    struct wlm_util_subprocess *subprocess_handle_ptr,
    int state,
    int code)
{
    wlmaker_root_menu_generator_t *generator_ptr = userdata_ptr;
    wlmtk_menu_item_t *menu_item_ptr = NULL;

    if (0 != state) {
        if (INT_MIN != state) {
            menu_item_ptr = _wlmaker_root_menu_create_disabled_item(
                generator_ptr->style_ref_ptr,
                "Failed, exit code %d", state);
            bs_log(BS_ERROR, "Subprocess %p failed, exit code %d",
                   subprocess_handle_ptr, state);
        } else {
            menu_item_ptr = _wlmaker_root_menu_create_disabled_item(
                generator_ptr->style_ref_ptr,
                "Failed, signal %d", code);
            bs_log(BS_ERROR, "Subprocess %p failed, signal %d",
                   subprocess_handle_ptr, code);
        }
    } else {

        bs_log(BS_INFO, "Subprocess %p terminated", subprocess_handle_ptr);

        bspl_object_t *object_ptr = bspl_create_object_from_dynbuf(
            generator_ptr->stdout_dynbuf_ptr);
        if (NULL == object_ptr ||
            BSPL_ARRAY != bspl_object_type(object_ptr)) {
            menu_item_ptr = _wlmaker_root_menu_create_disabled_item(
                generator_ptr->style_ref_ptr,
                "Failed to parse Plist ARRAY from \"%.*s\"",
                (int)(generator_ptr->stdout_dynbuf_ptr->length),
                (char*)(generator_ptr->stdout_dynbuf_ptr->data_ptr));
            bs_log(BS_ERROR,
                   "Failed to parse Plist ARRAY from \"%.*s\"",
                   (int)(generator_ptr->stdout_dynbuf_ptr->length),
                   (char*)(generator_ptr->stdout_dynbuf_ptr->data_ptr));
        } else {

            if (!_wlmaker_root_menu_populate_menu_items_from_array(
                    generator_ptr->menu_ptr,
                    bspl_array_from_object(object_ptr),
                    generator_ptr->style_ref_ptr,
                    generator_ptr->server_ptr)) {
                menu_item_ptr = _wlmaker_root_menu_create_disabled_item(
                    generator_ptr->style_ref_ptr,
                    "Failed to populate menu from Plist ARRAY \"%.*s\"",
                    (int)(generator_ptr->stdout_dynbuf_ptr->length),
                    (char*)(generator_ptr->stdout_dynbuf_ptr->data_ptr));
                bs_log(BS_ERROR,
                       "Failed to populate menu from Plist ARRAY \"%.*s\"",
                       (int)(generator_ptr->stdout_dynbuf_ptr->length),
                       (char*)(generator_ptr->stdout_dynbuf_ptr->data_ptr));
            }
        }
        if (NULL != object_ptr) {
            bspl_object_unref(object_ptr);
            object_ptr = NULL;
        }
    }

    if (NULL!= menu_item_ptr) {
        wlmtk_menu_add_item(generator_ptr->menu_ptr, menu_item_ptr);
    }

    generator_ptr->subprocess_handle_ptr = NULL;
    _wlmaker_root_menu_generators--;

    if (NULL != _wlmaker_root_menu_test_wl_display_ptr &&
        0 >= _wlmaker_root_menu_generators) {
        wl_display_terminate(_wlmaker_root_menu_test_wl_display_ptr);
        _wlmaker_root_menu_test_wl_display_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Creates a menu item from the Plist array.
 *
 * The Plist array either defines a menu action item, where the array elements
 * are `(Title, ActionName, OptionalActionArg)`. Or, it defines a submenu, as
 * specified in @ref _wlmaker_root_menu_init_menu_from_array.
 * Or, it has only a title, then it will be shown as disabled menu item.
 *
 * For the list of permitted `ActionName` values, see @ref wlmaker_action_desc.

 * @param item_array_ptr
 * @param menu_style_ref_ptr
 * @param server_ptr
 *
 * @return The menu item, or NULL on error.
 */
wlmtk_menu_item_t *_wlmaker_root_menu_create_item_from_array(
    bspl_array_t *item_array_ptr,
    wlmtk_menu_style_ref_t *menu_style_ref_ptr,
    wlmaker_server_t *server_ptr)
{
    wlmtk_menu_item_t *menu_item_ptr = wlmtk_menu_item_create(
        menu_style_ref_ptr);
    if (NULL == menu_item_ptr) return NULL;

    if (!wlmtk_menu_item_set_text(
            menu_item_ptr,
            bspl_array_string_value_at(item_array_ptr, 0))) goto error;

    if (1 >= bspl_array_size(item_array_ptr)) {
        wlmtk_menu_item_set_enabled(menu_item_ptr, false);
    } else {
        // If second element is a string that translates to an action: Bind it.
        int action;
        if (bspl_enum_name_to_value(
                wlmaker_action_desc,
                bspl_array_string_value_at(item_array_ptr, 1),
                &action)) {
            wlmaker_menu_item_bind_action(
                menu_item_ptr,
                action,
                bspl_array_string_value_at(item_array_ptr, 2),
                server_ptr);
            return menu_item_ptr;
        }

        wlmtk_menu_t *submenu_ptr = wlmtk_menu_create(menu_style_ref_ptr);
        if (NULL == submenu_ptr) goto error;
        wlmtk_menu_item_set_submenu(menu_item_ptr, submenu_ptr);
        // Window Maker: A submenu has its title on top (tear it off there).
        if (NULL != server_ptr->root_menu_ptr) {
            _wlmaker_root_menu_add_title(
                server_ptr->root_menu_ptr, submenu_ptr, item_array_ptr);
        }

        if (!_wlmaker_root_menu_init_menu_from_array(
                submenu_ptr,
                item_array_ptr,
                menu_style_ref_ptr,
                server_ptr)) {
            goto error;
        }
    }
    return menu_item_ptr;

error:
    wlmtk_menu_item_destroy(menu_item_ptr);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/**
 * Creates a disabled menu item as a means to display generator state.
 *
 * @param style_ref_ptr
 * @param fmt_ptr
 *
 * @return the disabled menu item, or NULL on error.
 */
wlmtk_menu_item_t *_wlmaker_root_menu_create_disabled_item(
    wlmtk_menu_style_ref_t *style_ref_ptr,
    const char *fmt_ptr,
    ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt_ptr);
    vsnprintf(buf, sizeof(buf), fmt_ptr, ap);
    va_end(ap);

    wlmtk_menu_item_t *menu_item_ptr = wlmtk_menu_item_create(style_ref_ptr);
    if (NULL == menu_item_ptr) {
        bs_log(BS_ERROR, "Failed wlmtk_menu_item_create(%p) for \"%s\"",
               style_ref_ptr, buf);
        return NULL;
    }

    wlmtk_menu_item_set_text(menu_item_ptr, buf);
    wlmtk_menu_item_set_enabled(menu_item_ptr, false);
    return menu_item_ptr;
}

/* == Unit tests =========================================================== */

static void test_default_menu(bs_test_t *test_ptr);
static void test_generated_menu(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t wlmaker_root_menu_test_cases[] = {
    { true, "default_menu", test_default_menu },
    { true, "generated_menu", test_generated_menu },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_root_menu_test_set = BS_TEST_SET(
    true, "root_menu", wlmaker_root_menu_test_cases);

/* ------------------------------------------------------------------------- */
/** Verifies that the compiled-in configuration translates into a menu. */
void test_default_menu(bs_test_t *test_ptr)
{
    wlmaker_server_t server = {};

    wlmtk_window_style_ref_t *wsr = wlmtk_window_style_to_ref(wlmtk_window_style_create());
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wsr);
    wlmtk_menu_style_ref_t *msr = wlmtk_menu_style_to_ref(wlmtk_menu_style_create());
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, msr);
    wlmaker_root_menu_t *root_menu_ptr = wlmaker_root_menu_create(
        &server, NULL, wsr, msr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, root_menu_ptr);
    wlmaker_root_menu_destroy(root_menu_ptr);
    wlmtk_window_style_ref_release(wsr);
    wlmtk_menu_style_ref_release(msr);
}

/* ------------------------------------------------------------------------- */
/** Verifies that an example menu with generator is translated. */
void test_generated_menu(bs_test_t *test_ptr)
{
    wlmtk_window_style_ref_t *wsr = wlmtk_window_style_to_ref(wlmtk_window_style_create());
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wsr);
    wlmtk_menu_style_ref_t *msr = wlmtk_menu_style_to_ref(wlmtk_menu_style_create());
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, msr);

    wlmaker_server_t server = {
        .wl_display_ptr = wl_display_create(),
        .wlr_scene_ptr = wlr_scene_create()
    };
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.wlr_scene_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.wl_display_ptr);
    wl_signal_init(&server.window_created_event);
    wl_signal_init(&server.window_destroyed_event);
    server.wlr_output_layout_ptr = wlr_output_layout_create(
        server.wl_display_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.wlr_output_layout_ptr);
    struct wlmtk_desktop_style ds = {};
    server.desktop_ptr = wlmtk_desktop_create(
        server.wlr_scene_ptr, &ds, server.wlr_output_layout_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.desktop_ptr);
    server.monitor_ptr = wlm_util_subprocess_monitor_create(
        wl_display_get_event_loop(server.wl_display_ptr));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.monitor_ptr);

#ifndef WLMAKER_SOURCE_DIR
#error "Missing definition of WLMAKER_SOURCE_DIR!"
#endif
    BS_TEST_VERIFY_EQ_OR_RETURN(test_ptr, 0, chdir(WLMAKER_SOURCE_DIR));
    wlmaker_root_menu_t *root_menu_ptr;

    // Exercise & verify including a submenu from a file.
    root_menu_ptr = wlmaker_root_menu_create(
        &server,
        bs_test_data_path(test_ptr, "menu-include.plist"),
        wsr,
        msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, root_menu_ptr);
    wlmtk_menu_t *menu_ptr = wlmaker_root_menu_menu(root_menu_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, 0, wlmtk_menu_items_size(menu_ptr));
    wlmaker_root_menu_destroy(root_menu_ptr);

    // Exercise & verify generating a submenu from a shell command.
    root_menu_ptr = wlmaker_root_menu_create(
        &server,
        bs_test_data_path(test_ptr, "menu-generate.plist"),
        wsr,
        msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, root_menu_ptr);
    menu_ptr = wlmaker_root_menu_menu(root_menu_ptr);
    _wlmaker_root_menu_test_wl_display_ptr = server.wl_display_ptr;
    wl_display_run(server.wl_display_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, 0, wlmtk_menu_items_size(menu_ptr));
    wlmaker_root_menu_destroy(root_menu_ptr);

    // Exercise & verify that a menu can be generated from output of the
    // `wlmtool` command.
    // Note: RootMenuDebian.plist is generated, it in WLMAKER_BINARY_DIR.
#ifndef WLMAKER_BINARY_DIR
#error "Missing definition of WLMAKER_BINARY_DIR!"
#endif
    root_menu_ptr = wlmaker_root_menu_create(
        &server,
        WLMAKER_BINARY_DIR "/etc/RootMenuDebian.plist",
        wsr,
        msr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, root_menu_ptr);
    menu_ptr = wlmaker_root_menu_menu(root_menu_ptr);
    _wlmaker_root_menu_test_wl_display_ptr = server.wl_display_ptr;
    wl_display_run(server.wl_display_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, 0, wlmtk_menu_items_size(menu_ptr));

    wlmtk_menu_item_t *item_ptr = wlmtk_menu_item_at(menu_ptr, 0);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, item_ptr);
    menu_ptr = wlmtk_menu_item_get_submenu(item_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, menu_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, 0, wlmtk_menu_items_size(menu_ptr));
    item_ptr = wlmtk_menu_item_at(menu_ptr, 0);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, item_ptr);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLMTK_MENU_ITEM_ENABLED,
        wlmtk_menu_item_get_state(item_ptr));

    wlmaker_root_menu_destroy(root_menu_ptr);

    wlm_util_subprocess_monitor_destroy(server.monitor_ptr);
    wlmtk_desktop_destroy(server.desktop_ptr);
    wl_display_destroy(server.wl_display_ptr);
    wlr_scene_node_destroy(&server.wlr_scene_ptr->tree.node);
    wlmtk_window_style_ref_release(wsr);
    wlmtk_menu_style_ref_release(msr);
}

/* == End of root_menu.c =================================================== */
