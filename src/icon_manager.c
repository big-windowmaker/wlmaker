/* ========================================================================= */
/**
 * @file icon_manager.c
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

#include "icon_manager.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <drm_fourcc.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#include <wlr/types/wlr_xdg_shell.h>
#undef WLR_USE_UNSTABLE

#include "backend/backend.h"
#include "clip.h"
#include "context_menu.h"
#include "dockapp_prefs.h"
#include "dockapps.h"
#include "i18n.h"
#include "input/cursor.h"
#include "input/manager.h"
#include "toolkit/toolkit.h"
#include "wlmaker-icon-unstable-v1-server-protocol.h"
#include "server.h"
#include "dock_fx.h"
#include "dock_launchers.h"
#include "dock_menu.h"
#include "tile_drag.h"
#include "xdg_popup.h"
#include "xdg_shell.h"

struct wl_client;
struct wl_resource;

/* == Declarations ========================================================= */

/** State of the toplevel icon manager. */
struct _wlmaker_icon_manager_t {
    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** Back-link to the wayland Display. */
    struct wl_display         *wl_display_ptr;

    /** The global holding the icon manager's interface. */
    struct wl_global          *wl_global_ptr;
    /**
     * Holds the popups of adopted layer surfaces (eg. the tray's menus and
     * balloons): An empty dock in the overlay layer, above all. Created on
     * the first popup.
     */
    wlmtk_dock_t              *popup_dock_ptr;
};

/** State of a toplevel icon. */
struct _wlmaker_toplevel_icon_t {
    /** The icon is also a toolkit tile. */
    wlmtk_tile_t              super_tile;
    /** The surface element, being the content of the tile. */
    wlmtk_surface_t           *content_surface_ptr;

    /** Back-link to the client requesting the toplevel. */
    struct wl_client          *wl_client_ptr;
    /** Back-link to the icon manager. */
    wlmaker_icon_manager_t    *icon_manager_ptr;
    /**
     * Back-link to the server. The icon manager may be gone when the icon
     * is destroyed, eg. when clients get destroyed at exit.
     */
    wlmaker_server_t          *server_ptr;
    /** The provided ID. */
    uint32_t                  id;
    /** The XDG toplevel for which the icon is specified. */
    struct wlr_xdg_toplevel   *wlr_xdg_toplevel_ptr;
    /** The surface to use for the icon of this toplevel. */
    struct wlr_surface        *wlr_surface_ptr;

    /** The resource associated with this icon. */
    struct wl_resource        *wl_resource_ptr;

    /** Whether the configuration sequence was acknowledged. */
    bool                      acknowledged;
    /** Serial that needs to be acknowledged. */
    uint32_t                  pending_serial;

    /** Listener for the `commit` event of `wlr_surface_ptr`. */
    struct wl_listener        surface_commit_listener;
    /** Listener for the `destroy` event of `wlr_surface_ptr`. */
    struct wl_listener        surface_destroy_listener;

    /** The superclass' original element virtual method table. */
    wlmtk_element_vmt_t       orig_element_vmt;
    /** Whether the left button is held for dragging: May become a drag. */
    bool                      dragging;
    /** Pointer position where the button was pressed, in tile coordinates. */
    double                    drag_start_x;
    /** Pointer position where the button was pressed, in tile coordinates. */
    double                    drag_start_y;
    /** Pointer position where the button was pressed, in layout coords. */
    double                    drag_start_lx;
    /** Pointer position where the button was pressed, in layout coords. */
    double                    drag_start_ly;
    /** The drag of the tile on or off the docks, once started. */
    wlmaker_tile_drag_t       *tile_drag_ptr;
    /** Whether the tile is lifted off its slot, while dragged. */
    bool                      lifted;
    /** Listener for @ref wlmtk_element_events_t::pointer_leave. */
    struct wl_listener        pointer_leave_listener;
    /** Whether the balloon is shown, or pending, for the icon. */
    bool                      balloon_entered;

    /**
     * A layer surface adopted as dock-app (namespace "wlmaker-dockapp"),
     * instead of the icon protocol's surface. NULL otherwise.
     */
    struct wlr_layer_surface_v1 *wlr_layer_surface_v1_ptr;
    /** Listener for `destroy` of the adopted layer surface. */
    struct wl_listener        layer_destroy_listener;
    /** Listener for `new_popup` of the adopted layer surface. */
    struct wl_listener        layer_new_popup_listener;
    /**
     * The adopted surface's popups (eg. its menus): At the surface's origin,
     * in the popup container of the dock's panel holding the tile.
     */
    wlmtk_container_t         popup_anchor;
    /** Whether @ref wlmaker_toplevel_icon_t::popup_anchor is initialized. */
    bool                      popup_anchor_ready;
};

static wlmaker_icon_manager_t *icon_manager_from_resource(
    struct wl_resource *wl_resource_ptr);
static wlmaker_toplevel_icon_t *wlmaker_toplevel_icon_from_resource(
    struct wl_resource *wl_resource_ptr);

static void bind_icon_manager(
    struct wl_client *wl_client_ptr,
    void *data_ptr,
    uint32_t version,
    uint32_t id);

static void handle_resource_destroy(
    struct wl_client *wl_client_ptr,
    struct wl_resource *wl_resource_ptr);

static void handle_get_toplevel_icon(
    struct wl_client *wl_client_ptr,
    struct wl_resource *wl_icon_manager_resource_ptr,
    uint32_t id,
    struct wl_resource *wl_toplevel_resource_ptr,
    struct wl_resource *wl_surface_resource_ptr);

static wlmaker_toplevel_icon_t *wlmaker_toplevel_icon_create(
    struct wl_client *wl_client_ptr,
    wlmaker_icon_manager_t *icon_manager_ptr,
    uint32_t id,
    int version,
    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr,
    struct wlr_surface *wlr_surface_ptr);

static void wlmaker_toplevel_icon_destroy(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);
static void toplevel_icon_resource_destroy(
    struct wl_resource *wl_resource_ptr);

static void handle_icon_ack_configure(
    struct wl_client *wl_client_ptr,
    struct wl_resource *wl_resource_ptr,
    uint32_t serial);
static void handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void handle_surface_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmaker_toplevel_icon_open_menu(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);
static bool _wlmaker_toplevel_icon_element_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);
static bool _wlmaker_toplevel_icon_element_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr);
static void _wlmaker_toplevel_icon_element_pointer_grab_cancel(
    wlmtk_element_t *element_ptr);
static void _wlmaker_toplevel_icon_balloon(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr);
static char *_wlmaker_toplevel_icon_balloon_text(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);
static char *_wlmaker_toplevel_icon_cmdline(pid_t pid);
static void _wlmaker_toplevel_icon_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_toplevel_icon_remove_from_dock(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);
static void _wlmaker_toplevel_icon_element_destroy(
    wlmtk_element_t *element_ptr);
static bool _wlmaker_toplevel_icon_tile_set_content_size(
    wlmtk_tile_t *tile_ptr,
    uint64_t content_size);
static void _wlmaker_toplevel_icon_layer_commit(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);
static void _wlmaker_toplevel_icon_handle_layer_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_toplevel_icon_handle_layer_new_popup(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static bool _wlmaker_toplevel_icon_content_has_pointer(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr);

/* == Data ================================================================= */

/** Implementation of the toplevel icon manager interface. */
static const struct zwlmaker_icon_manager_v1_interface
icon_manager_v1_implementation = {
    .destroy = handle_resource_destroy,
    .get_toplevel_icon = handle_get_toplevel_icon
};

/** Implementation of the toplevel icon interface. */
static const struct zwlmaker_toplevel_icon_v1_interface
toplevel_icon_v1_implementation = {
    .destroy = handle_resource_destroy,
    .ack_configure = handle_icon_ack_configure,
};

/** The icon's extension to @ref wlmtk_element_t virtual method table. */
static const wlmtk_element_vmt_t _wlmaker_toplevel_icon_element_vmt = {
    .destroy = _wlmaker_toplevel_icon_element_destroy,
    .pointer_button = _wlmaker_toplevel_icon_element_pointer_button,
    .pointer_accepts_motion =
    _wlmaker_toplevel_icon_element_pointer_accepts_motion,
    .pointer_grab_cancel = _wlmaker_toplevel_icon_element_pointer_grab_cancel,
};

/** Icon's extension to @ref wlmtk_tile_t virtual method table. */
static const wlmtk_tile_vmt_t _wlmaker_toplevel_icon_tile_vmt = {
    .set_content_size = _wlmaker_toplevel_icon_tile_set_content_size,
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_icon_manager_t *wlmaker_icon_manager_create(
    struct wl_display *wl_display_ptr,
    wlmaker_server_t *server_ptr)
{
    wlmaker_icon_manager_t *icon_manager_ptr =
        logged_calloc(1, sizeof(wlmaker_icon_manager_t));
    if (NULL == icon_manager_ptr) return NULL;
    icon_manager_ptr->server_ptr = server_ptr;
    icon_manager_ptr->wl_display_ptr = wl_display_ptr;

    icon_manager_ptr->wl_global_ptr = wl_global_create(
        wl_display_ptr,
        &zwlmaker_icon_manager_v1_interface,
        1,
        icon_manager_ptr,
        bind_icon_manager);
    if (NULL == icon_manager_ptr->wl_global_ptr) {
        wlmaker_icon_manager_destroy(icon_manager_ptr);
        return NULL;
    }

    return icon_manager_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_icon_manager_destroy(
    wlmaker_icon_manager_t *icon_manager_ptr)
{
    if (NULL != icon_manager_ptr->wl_global_ptr) {
        wl_global_destroy(icon_manager_ptr->wl_global_ptr);
        icon_manager_ptr->wl_global_ptr = NULL;
    }
    if (NULL != icon_manager_ptr->popup_dock_ptr) {
        wlmtk_panel_t *p_ptr = wlmtk_dock_panel(icon_manager_ptr->popup_dock_ptr);
        if (NULL != wlmtk_panel_get_layer(p_ptr)) {
            wlmtk_layer_remove_panel(wlmtk_panel_get_layer(p_ptr), p_ptr);
        }
        wlmtk_dock_destroy(icon_manager_ptr->popup_dock_ptr);
        icon_manager_ptr->popup_dock_ptr = NULL;
    }

    free(icon_manager_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Returns the toplevel icon manager from the resource, with type check.
 *
 * @param wl_resource_ptr
 *
 * @return Pointer to the @ref wlmaker_icon_manager_t.
 */
wlmaker_icon_manager_t *icon_manager_from_resource(
    struct wl_resource *wl_resource_ptr)
{
    BS_ASSERT(wl_resource_instance_of(
                  wl_resource_ptr,
                  &zwlmaker_icon_manager_v1_interface,
                  &icon_manager_v1_implementation));
    return wl_resource_get_user_data(wl_resource_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Returns the toplevel icon from the resource, with type check.
 *
 * @param wl_resource_ptr
 *
 * @return Pointer to the @ref wlmaker_toplevel_icon_t.
 */
wlmaker_toplevel_icon_t *wlmaker_toplevel_icon_from_resource(
    struct wl_resource *wl_resource_ptr)
{
    BS_ASSERT(wl_resource_instance_of(
                  wl_resource_ptr,
                  &zwlmaker_toplevel_icon_v1_interface,
                  &toplevel_icon_v1_implementation));
    return wl_resource_get_user_data(wl_resource_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Binds an icon manager for the client.
 *
 * @param wl_client_ptr
 * @param data_ptr
 * @param version
 * @param id
 */
void bind_icon_manager(
    struct wl_client *wl_client_ptr,
    void *data_ptr,
    uint32_t version,
    uint32_t id)
{
    struct wl_resource *wl_resource_ptr = wl_resource_create(
        wl_client_ptr,
        &zwlmaker_icon_manager_v1_interface,
        version, id);
    if (NULL == wl_resource_ptr) {
        wl_client_post_no_memory(wl_client_ptr);
        return;
    }
   wlmaker_icon_manager_t *icon_manager_ptr = data_ptr;

    wl_resource_set_implementation(
        wl_resource_ptr,
        &icon_manager_v1_implementation,  // implementation.
        icon_manager_ptr,  // data
        NULL);  // dtor. We don't have an explicit one.
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the 'destroy' method: Destroys the resource.
 *
 * @param wl_client_ptr
 * @param wl_resource_ptr
 */
void handle_resource_destroy(
    __UNUSED__ struct wl_client *wl_client_ptr,
    struct wl_resource *wl_resource_ptr)
{
    wl_resource_destroy(wl_resource_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the 'get_toplevel_icon' method.
 *
 * @param wl_client_ptr
 * @param wl_icon_manager_resource_ptr
 * @param id
 * @param wl_toplevel_resource_ptr
 * @param wl_surface_resource_ptr
 */
void handle_get_toplevel_icon(
    struct wl_client *wl_client_ptr,
    struct wl_resource *wl_icon_manager_resource_ptr,
    uint32_t id,
    struct wl_resource *wl_toplevel_resource_ptr,
    struct wl_resource *wl_surface_resource_ptr)
{
    wlmaker_icon_manager_t *icon_manager_ptr =
        icon_manager_from_resource(
            wl_icon_manager_resource_ptr);
    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr = NULL;
    if (NULL != wl_toplevel_resource_ptr) {
        wlr_xdg_toplevel_ptr = wlr_xdg_toplevel_from_resource(
            wl_toplevel_resource_ptr);
    }
    struct wlr_surface *wlr_surface_ptr =
        wlr_surface_from_resource(wl_surface_resource_ptr);

    wlmaker_toplevel_icon_t *toplevel_icon_ptr = wlmaker_toplevel_icon_create(
        wl_client_ptr,
        icon_manager_ptr,
        id,
        wl_resource_get_version(wl_icon_manager_resource_ptr),
        wlr_xdg_toplevel_ptr,
        wlr_surface_ptr);
    if (NULL == toplevel_icon_ptr) {
        wl_client_post_no_memory(wl_client_ptr);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Creates a new toplevel icon.
 *
 * @param wl_client_ptr
 * @param icon_manager_ptr
 * @param id
 * @param version
 * @param wlr_xdg_toplevel_ptr
 * @param wlr_surface_ptr
 *
 * @return A pointer to the new toplevel icon or NULL on error. The toplevel
 *     icon's resources must be free'd via @ref wlmaker_toplevel_icon_destroy.
 */
wlmaker_toplevel_icon_t *wlmaker_toplevel_icon_create(
    struct wl_client *wl_client_ptr,
    wlmaker_icon_manager_t *icon_manager_ptr,
    uint32_t id,
    int version,
    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr,
    struct wlr_surface *wlr_surface_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = logged_calloc(
        1, sizeof(wlmaker_toplevel_icon_t));
    if (NULL == toplevel_icon_ptr) return NULL;

    toplevel_icon_ptr->wl_client_ptr = wl_client_ptr;
    toplevel_icon_ptr->icon_manager_ptr = icon_manager_ptr;
    toplevel_icon_ptr->server_ptr = icon_manager_ptr->server_ptr;
    toplevel_icon_ptr->id = id;
    toplevel_icon_ptr->wlr_xdg_toplevel_ptr = wlr_xdg_toplevel_ptr;
    toplevel_icon_ptr->wlr_surface_ptr = wlr_surface_ptr;

    toplevel_icon_ptr->wl_resource_ptr = wl_resource_create(
        wl_client_ptr,
        &zwlmaker_toplevel_icon_v1_interface,
        version,
        id);
    if (NULL == toplevel_icon_ptr->wl_resource_ptr) {
        bs_log(BS_ERROR, "Failed wl_resource_create(%p, %p, %d, %"PRIu32")",
               wl_client_ptr, &zwlmaker_toplevel_icon_v1_interface, version,
               id);
        wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
        return NULL;
    }
    wl_resource_set_implementation(
        toplevel_icon_ptr->wl_resource_ptr,
        &toplevel_icon_v1_implementation,
        toplevel_icon_ptr,
        toplevel_icon_resource_destroy);

    if (!wlmtk_tile_init(
            &toplevel_icon_ptr->super_tile,
            &icon_manager_ptr->server_ptr->style_ptr->tile)) {
        wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
        return NULL;
    }
    toplevel_icon_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&toplevel_icon_ptr->super_tile),
        &_wlmaker_toplevel_icon_element_vmt);
    wlmtk_tile_extend(
        &toplevel_icon_ptr->super_tile,
        &_wlmaker_toplevel_icon_tile_vmt);
    wlmtk_element_set_visible(
        wlmtk_tile_element(&toplevel_icon_ptr->super_tile),
        true);
    wlmtk_util_connect_listener_signal(
        &wlmtk_tile_element(&toplevel_icon_ptr->super_tile)->events.pointer_leave,
        &toplevel_icon_ptr->pointer_leave_listener,
        _wlmaker_toplevel_icon_handle_pointer_leave);
    if (NULL != icon_manager_ptr->server_ptr->dockapps_ptr) {
        char *name_ptr = wlmaker_dockapps_client_name(wl_client_ptr);
        if (NULL == name_ptr) {
            bs_log(BS_WARNING, "Toplevel icon %p: Failed to obtain the "
                   "dock-app's name, it will be placed last.",
                   toplevel_icon_ptr);
        }
        // On the Dock or the Clip, where it was attached before.
        bool added = wlmaker_clip_add_dockapp(
            icon_manager_ptr->server_ptr,
            &toplevel_icon_ptr->super_tile,
            name_ptr);
        if (NULL != name_ptr) free(name_ptr);
        if (!added) {
            wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
            return NULL;
        }
    } else if (NULL != icon_manager_ptr->server_ptr->clip_dock_ptr) {
        wlmtk_dock_add_tile(
            icon_manager_ptr->server_ptr->clip_dock_ptr,
            &toplevel_icon_ptr->super_tile);
    }

    toplevel_icon_ptr->content_surface_ptr = wlmtk_surface_create(
        wlr_surface_ptr,
        icon_manager_ptr->server_ptr->wlr_seat_ptr);
    if (NULL == toplevel_icon_ptr->content_surface_ptr) {
        wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_surface_element(toplevel_icon_ptr->content_surface_ptr),
        true);

    // Hack: Connect this listener after wlmtk_surface creation, so that the
    // surface knows it's size before added...
    wlmtk_util_connect_listener_signal(
        &toplevel_icon_ptr->wlr_surface_ptr->events.commit,
        &toplevel_icon_ptr->surface_commit_listener,
        handle_surface_commit);
    wlmtk_util_connect_listener_signal(
        &toplevel_icon_ptr->wlr_surface_ptr->events.destroy,
        &toplevel_icon_ptr->surface_destroy_listener,
        handle_surface_destroy);

    bs_log(BS_INFO,
           "created toplevel icon %p for toplevel %p, surface %p, "
           "wlmtk surface %p",
           toplevel_icon_ptr, wlr_xdg_toplevel_ptr, wlr_surface_ptr,
           toplevel_icon_ptr->content_surface_ptr);

    return toplevel_icon_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Destroys the toplevel icon.
 *
 * @param toplevel_icon_ptr
 */
void wlmaker_toplevel_icon_destroy(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    bs_log(BS_INFO, "Destroying toplevel icon %p", toplevel_icon_ptr);
    // The menu's items refer to the icon.
    if (NULL != toplevel_icon_ptr->server_ptr) {
        wlmaker_context_menu_cancel(
            toplevel_icon_ptr->server_ptr, toplevel_icon_ptr);
    }

    _wlmaker_toplevel_icon_element_destroy(
        wlmtk_tile_element(&toplevel_icon_ptr->super_tile));
    wlmtk_util_disconnect_listener(&toplevel_icon_ptr->pointer_leave_listener);
    wlmtk_util_disconnect_listener(&toplevel_icon_ptr->layer_new_popup_listener);
    wlmtk_util_disconnect_listener(&toplevel_icon_ptr->layer_destroy_listener);
    if (toplevel_icon_ptr->popup_anchor_ready) {
        wlmtk_element_t *a_ptr = &toplevel_icon_ptr->popup_anchor.super_element;
        if (NULL != a_ptr->parent_container_ptr) {
            wlmtk_container_remove_element(a_ptr->parent_container_ptr, a_ptr);
        }
        // Popups go with their surface, before it.
        if (bs_dllist_empty(&toplevel_icon_ptr->popup_anchor.elements)) {
            wlmtk_container_fini(&toplevel_icon_ptr->popup_anchor);
        }
    }
    wlmtk_tile_fini(&toplevel_icon_ptr->super_tile);

    // Note: Not destroying toplevel_icon_ptr->resource, since that causes
    // cycles...

    free(toplevel_icon_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Destructor for the toplevel icon's resource.
 *
 * @param wl_resource_ptr
 */
void toplevel_icon_resource_destroy(struct wl_resource *wl_resource_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr =
        wlmaker_toplevel_icon_from_resource(wl_resource_ptr);
    wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles the `ack_configure` request by the icon.
 *
 * @param wl_client_ptr
 * @param wl_resource_ptr
 * @param serial
 */
void handle_icon_ack_configure(
    __UNUSED__ struct wl_client *wl_client_ptr,
    struct wl_resource *wl_resource_ptr,
    uint32_t serial)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr =
        wlmaker_toplevel_icon_from_resource(wl_resource_ptr);

    if (serial == toplevel_icon_ptr->pending_serial) {
        toplevel_icon_ptr->acknowledged = true;
        toplevel_icon_ptr->pending_serial = 0;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Event handler for the `commit` signal of the icon's surface.
 *
 * The protocol expects a first `commit` with a NULL-buffer attached to the
 * surface. This will trigger a `configure` event, informing the client of the
 * suggested icon size.
 *
 * Only when the configuration was suggested and acknowledged a first time,
 * will we accept `commit` with attached buffers.
 *
 * @param listener_ptr
 * @param data_ptr            Points to the `struct wlr_surface` of the icon.
 */
void handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_toplevel_icon_t, surface_commit_listener);
    struct wlr_surface *wlr_surface_ptr = data_ptr;
    BS_ASSERT(toplevel_icon_ptr->wlr_surface_ptr == wlr_surface_ptr);

    if (NULL != toplevel_icon_ptr->wlr_layer_surface_v1_ptr) {
        _wlmaker_toplevel_icon_layer_commit(toplevel_icon_ptr);
        return;
    }
    if (NULL == wlr_surface_ptr->buffer) {
        // An initial commit is expected with a NULL buffer, so we can
        // respond with a `configure` event.
        toplevel_icon_ptr->pending_serial = wl_display_next_serial(
            toplevel_icon_ptr->icon_manager_ptr->wl_display_ptr);
        zwlmaker_toplevel_icon_v1_send_configure(
            toplevel_icon_ptr->wl_resource_ptr,
            toplevel_icon_ptr->super_tile.style.content_size,
            toplevel_icon_ptr->super_tile.style.content_size,
            toplevel_icon_ptr->pending_serial);
        return;
    }
    BS_ASSERT(NULL != wlr_surface_ptr->buffer);

    if (!toplevel_icon_ptr->acknowledged) {
        wl_resource_post_error(
            toplevel_icon_ptr->wl_resource_ptr,
            1,
            "Commit non-NULL buffer without configure sequence.");
        return;
    }

    // TODO(kaeser@gubbe.ch): Constrain image size to the content size.
    wlmtk_tile_set_content(
        &toplevel_icon_ptr->super_tile,
        wlmtk_surface_element(toplevel_icon_ptr->content_surface_ptr));
}

/* ------------------------------------------------------------------------- */
/** Handles when the surface is destroyed. */
static void handle_surface_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_toplevel_icon_t, surface_destroy_listener);
    _wlmaker_toplevel_icon_element_destroy(
        wlmtk_tile_element(&toplevel_icon_ptr->super_tile));
}

/* ------------------------------------------------------------------------- */
/**
 * Destructor of the icon's corresponding tile element.
 *
 * This is a hack: The `wlmaker_toplevel_icon_t` is owned by the wl_resource
 * and must only be freed when that dtor is caller. But, the element dtor may
 * be called on wlmaker shutdown, when eg. an icon app is still running.
 * So, for that case, we just detach the icon here.
 *
 * Leaving as is, since the icon model will need to be revamped anyway, to
 * line up with the new XDG icon protocol.
 *
 * @param element_ptr
 */
void _wlmaker_toplevel_icon_element_destroy(wlmtk_element_t *element_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_toplevel_icon_t,
        super_tile.super_container.super_element);

    if (NULL != toplevel_icon_ptr->content_surface_ptr) {

        wlmtk_util_disconnect_listener(
            &toplevel_icon_ptr->surface_destroy_listener);
        wlmtk_util_disconnect_listener(
            &toplevel_icon_ptr->surface_commit_listener);

        wlmtk_tile_set_content(&toplevel_icon_ptr->super_tile, NULL);
        wlmtk_surface_destroy(toplevel_icon_ptr->content_surface_ptr);
        toplevel_icon_ptr->content_surface_ptr = NULL;
    }

    _wlmaker_toplevel_icon_remove_from_dock(toplevel_icon_ptr);
}

/* ------------------------------------------------------------------------- */
/** Removes the icon's tile from the clip's dock, if it is in there. */
void _wlmaker_toplevel_icon_remove_from_dock(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    wlmtk_element_t *element_ptr = wlmtk_tile_element(
        &toplevel_icon_ptr->super_tile);
    // Not in the dock (anymore), eg. if the clip was destroyed earlier. Then,
    // the icon manager and server may already be gone, too: Don't use them.
    if (NULL == element_ptr->parent_container_ptr) return;
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;

    if (toplevel_icon_ptr->dragging) {
        wlmtk_container_pointer_grab_release(
            element_ptr->parent_container_ptr, element_ptr);
        toplevel_icon_ptr->dragging = false;
    }
    toplevel_icon_ptr->tile_drag_ptr = NULL;
    wlmaker_tile_drag_forget_tile(server_ptr, &toplevel_icon_ptr->super_tile);
    if (NULL != server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, toplevel_icon_ptr);
    }
    wlmaker_clip_remove_dockapp(server_ptr, &toplevel_icon_ptr->super_tile);
    if (NULL != element_ptr->parent_container_ptr &&
        NULL != server_ptr->clip_dock_ptr) {
        wlmtk_dock_remove_tile(
            server_ptr->clip_dock_ptr, &toplevel_icon_ptr->super_tile);
    }
}

/* ------------------------------------------------------------------------- */
/** Moves the icon by one place in the dock, towards `direction`. */
static void _wlmaker_toplevel_icon_move(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr, uint32_t direction)
{
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(
        server_ptr, &toplevel_icon_ptr->super_tile);
    if (NULL == d_ptr) return;
    wlmaker_dockapps_move(
        d_ptr,
        &toplevel_icon_ptr->super_tile,
        wlmaker_dockapps_delta_towards(
            d_ptr, &toplevel_icon_ptr->super_tile, direction));
}

/** Menu callback: Moves the icon up. */
static void _wlmaker_toplevel_icon_menu_up(void *ud_ptr) {
    _wlmaker_toplevel_icon_move(ud_ptr, WLR_EDGE_TOP);
}
/** Menu callback: Moves the icon down. */
static void _wlmaker_toplevel_icon_menu_down(void *ud_ptr) {
    _wlmaker_toplevel_icon_move(ud_ptr, WLR_EDGE_BOTTOM);
}
/** Menu callback: Moves the icon left. */
static void _wlmaker_toplevel_icon_menu_left(void *ud_ptr) {
    _wlmaker_toplevel_icon_move(ud_ptr, WLR_EDGE_LEFT);
}
/** Menu callback: Moves the icon right. */
static void _wlmaker_toplevel_icon_menu_right(void *ud_ptr) {
    _wlmaker_toplevel_icon_move(ud_ptr, WLR_EDGE_RIGHT);
}

/** Returns the process ID of the dock-app, or 0 if unknown. */
static pid_t _wlmaker_toplevel_icon_pid(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    if (NULL == toplevel_icon_ptr->wl_client_ptr) return 0;
    pid_t pid = 0;
    wl_client_get_credentials(toplevel_icon_ptr->wl_client_ptr,
                              &pid, NULL, NULL);
    // Never ourselves, or a process group.
    if (pid <= 1 || pid == getpid()) return 0;
    return pid;
}

/* ------------------------------------------------------------------------- */
/**
 * Opens the dock-app tile's right-click menu, as Window Maker's menu of the
 * dock it is on: "Dock position" and "Add a drawer" (Dock), or "Clip
 * Options" (Clip). The dock-app's own actions. "Launch", "Bring Here",
 * "Hide" (disabled: dock-apps have no windows), "Settings..." (the
 * dock-app's preferences, see @ref wlmaker_dockapp_prefs_t) and "Kill".
 * Then, moving it within the dock (stored, as when dragging it), and moving
 * the Clip.
 *
 * @param toplevel_icon_ptr
 */
void _wlmaker_toplevel_icon_open_menu(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;
    wlmtk_tile_t *tile_ptr = &toplevel_icon_ptr->super_tile;
    if (NULL == server_ptr->style_ptr ||
        NULL == server_ptr->style_ptr->menu_style_ptr) return;

    static const struct {
        uint32_t direction;
        const char *text_ptr;
        wlmaker_context_menu_callback_t callback;
    } moves[] = {
        { WLR_EDGE_TOP, "Move Up", _wlmaker_toplevel_icon_menu_up },
        { WLR_EDGE_BOTTOM, "Move Down", _wlmaker_toplevel_icon_menu_down },
        { WLR_EDGE_LEFT, "Move Left", _wlmaker_toplevel_icon_menu_left },
        { WLR_EDGE_RIGHT, "Move Right", _wlmaker_toplevel_icon_menu_right },
    };

    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    int column = wlmaker_clip_dockapps_column_of(server_ptr, tile_ptr);
    if (0 > column) column = WLMAKER_COLUMN_CLIP;
    char *name_ptr = NULL;
    if (NULL != toplevel_icon_ptr->wl_client_ptr) {
        name_ptr = wlmaker_dockapps_client_name(toplevel_icon_ptr->wl_client_ptr);
    }
    wlmaker_context_menu_t *cm_ptr = wlmaker_context_menu_create(
        server_ptr, toplevel_icon_ptr,
        NULL != name_ptr ? name_ptr : wlmaker_i18n("Dock-app"));
    wlmaker_dockapp_prefs_t *prefs_ptr = NULL;
    if (NULL != name_ptr) {
        prefs_ptr = wlmaker_dockapp_prefs_load(
            server_ptr->files_ptr, NULL, NULL, name_ptr);
    }
    if (NULL == cm_ptr) {
        if (NULL != prefs_ptr) wlmaker_dockapp_prefs_destroy(prefs_ptr);
        if (NULL != name_ptr) free(name_ptr);
        return;
    }

    pid_t pid = _wlmaker_toplevel_icon_pid(toplevel_icon_ptr);
    char *cmdline_ptr = _wlmaker_toplevel_icon_cmdline(pid);
    wlmaker_dock_menu_icon_t icon = {
        .command_ptr = cmdline_ptr,
        .pid = pid,
        .dockapp_name_ptr = name_ptr,
        .column = column,
        .launcher_index = -1,
        .prefs_ptr = prefs_ptr };

    bool rv = wlmaker_dock_menu_add_dock_items(server_ptr, cm_ptr, &icon);
    // The dock-app's own actions.
    if (rv && NULL != prefs_ptr) {
        rv = wlmaker_dockapp_prefs_add_actions(prefs_ptr, server_ptr, cm_ptr);
    }
    rv = rv && wlmaker_dock_menu_add_icon_items(server_ptr, cm_ptr, &icon);
    for (size_t i = 0; rv && NULL != d_ptr &&
             i < sizeof(moves) / sizeof(moves[0]); ++i) {
        if (0 == wlmaker_dockapps_delta_towards(
                d_ptr, tile_ptr, moves[i].direction)) continue;
        rv = wlmaker_context_menu_add_item(
            cm_ptr, wlmaker_i18n(moves[i].text_ptr), moves[i].callback,
            toplevel_icon_ptr);
    }
    // Moves the Clip, which the dock-app is part of.
    if (rv && WLMAKER_COLUMN_CLIP == column) {
        rv = wlmaker_clip_add_move_items(server_ptr, cm_ptr);
    }

    if (NULL != prefs_ptr) wlmaker_dockapp_prefs_destroy(prefs_ptr);
    if (NULL != cmdline_ptr) free(cmdline_ptr);
    if (NULL != name_ptr) free(name_ptr);
    if (!rv) {
        wlmaker_context_menu_destroy(cm_ptr);
        return;
    }
    wlmaker_context_menu_open(cm_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Lifts the dock-app's tile off its slot while it is dragged, like Window
 * Maker: The slot shows empty. Or puts it back.
 */
static void _wlmaker_toplevel_icon_lift(void *ud_ptr, bool lifted)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = ud_ptr;
    if (lifted == toplevel_icon_ptr->lifted) return;
    toplevel_icon_ptr->lifted = lifted;
    wlmtk_tile_t *tile_ptr = &toplevel_icon_ptr->super_tile;
    if (NULL != toplevel_icon_ptr->content_surface_ptr) {
        wlmtk_element_set_visible(
            wlmtk_surface_element(toplevel_icon_ptr->content_surface_ptr),
            !lifted);
    }
    struct wlr_buffer *wlr_buffer_ptr = lifted ?
        bs_gfxbuf_create_wlr_buffer(tile_ptr->style.size, tile_ptr->style.size) :
        wlmaker_dock_tile_buffer(&tile_ptr->style, NULL, NULL);
    if (NULL == wlr_buffer_ptr) return;
    wlmtk_tile_set_background_buffer(tile_ptr, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the dock-app's tile as it is shown: The tile, and the dock-app's
 * current image. For dragging it, and its kaboom.
 *
 * @return The image, or NULL on error.
 */
static struct wlr_buffer *_wlmaker_toplevel_icon_snapshot(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    struct wlr_buffer *content_ptr = NULL;
    struct wlr_texture *wlr_texture_ptr = NULL;
    if (NULL != toplevel_icon_ptr->wlr_surface_ptr) {
        wlr_texture_ptr = wlr_surface_get_texture(
            toplevel_icon_ptr->wlr_surface_ptr);
    }
    if (NULL != wlr_texture_ptr) {
        content_ptr = bs_gfxbuf_create_wlr_buffer(
            wlr_texture_ptr->width, wlr_texture_ptr->height);
    }
    if (NULL != content_ptr) {
        bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(content_ptr);
        struct wlr_texture_read_pixels_options options = {
            .data = g_ptr->data_ptr,
            .format = DRM_FORMAT_ARGB8888,
            .stride = g_ptr->width * sizeof(uint32_t) };
        if (!wlr_texture_read_pixels(wlr_texture_ptr, &options)) {
            bs_log(BS_WARNING, "Failed wlr_texture_read_pixels for %p",
                   toplevel_icon_ptr);
            wlr_buffer_drop(content_ptr);
            content_ptr = NULL;
        }
    }
    struct wlr_buffer *tile_ptr = wlmaker_dock_tile_buffer(
        &toplevel_icon_ptr->super_tile.style, NULL, content_ptr);
    if (NULL != content_ptr) wlr_buffer_drop(content_ptr);
    return tile_ptr;
}

/* ------------------------------------------------------------------------- */
/** Starts dragging the dock-app's tile on or off the docks. */
static void _wlmaker_toplevel_icon_start_drag(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr,
    double x,
    double y)
{
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;
    wlmtk_tile_t *tile_ptr = &toplevel_icon_ptr->super_tile;
    wlmaker_dockapps_t *d_ptr = wlmaker_clip_dockapps_of(server_ptr, tile_ptr);
    if (NULL == d_ptr) return;
    struct wlr_buffer *image_ptr = _wlmaker_toplevel_icon_snapshot(
        toplevel_icon_ptr);
    if (NULL == image_ptr) return;
    wlmaker_tile_drag_source_t source = {
        .kind = WLMAKER_TILE_DRAG_DOCKAPP,
        .column = wlmaker_clip_dockapps_column_of(server_ptr, tile_ptr),
        .index = wlmaker_dockapps_index(d_ptr, tile_ptr),
        .tile_ptr = tile_ptr,
        .pid = _wlmaker_toplevel_icon_pid(toplevel_icon_ptr),
        .name_ptr = wlmaker_dockapps_name(d_ptr, tile_ptr),
        .image_ptr = image_ptr,
        .grab_x = toplevel_icon_ptr->drag_start_x,
        .grab_y = toplevel_icon_ptr->drag_start_y,
        .running = true,
        .lift = _wlmaker_toplevel_icon_lift,
        .lift_ud_ptr = toplevel_icon_ptr };
    toplevel_icon_ptr->tile_drag_ptr = wlmaker_tile_drag_start(
        server_ptr, &source, x, y);
    wlr_buffer_drop(image_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_button.
 *
 * Dragging the icon with the left button and the window-move modifier held
 * (the same as for moving windows), or on the tile's border, moves it on or
 * off the docks, like on Window Maker. The right button opens the tile's
 * menu. Other button events go to the dock-app.
 *
 * @param element_ptr
 * @param button_event_ptr
 *
 * @return true if the event was consumed.
 */
bool _wlmaker_toplevel_icon_element_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_toplevel_icon_t,
        super_tile.super_container.super_element);
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;

    if (NULL != server_ptr->dock_fx_ptr) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, NULL);
    }
    toplevel_icon_ptr->balloon_entered = false;

    if (toplevel_icon_ptr->dragging) {
        if (BTN_LEFT == button_event_ptr->button &&
            WLMTK_BUTTON_UP == button_event_ptr->type) {
            toplevel_icon_ptr->dragging = false;
            if (NULL != element_ptr->parent_container_ptr) {
                wlmtk_container_pointer_grab_release(
                    element_ptr->parent_container_ptr, element_ptr);
            }
            wlmaker_tile_drag_t *tile_drag_ptr =
                toplevel_icon_ptr->tile_drag_ptr;
            toplevel_icon_ptr->tile_drag_ptr = NULL;
            if (NULL != tile_drag_ptr &&
                tile_drag_ptr == server_ptr->tile_drag_ptr) {
                struct wlr_cursor *c = wlmim_wlr_cursor(
                    server_ptr->input_manager_ptr);
                wlmaker_tile_drag_drop(tile_drag_ptr, c->x, c->y);
            }
        }
        return true;
    }

    uint32_t modifier = wlmim_cursor_options(
        server_ptr->input_manager_ptr)->move_window_modifier;
    // The right button opens the tile's menu, like on Window Maker. Also on
    // the dock-app itself: It does not get the right button. Dock-apps
    // offer their own actions through the menu instead.
    // An adopted layer surface (eg. the system tray) takes the right button
    // where it takes input (its icons): Their own menus. Elsewhere on the
    // tile, the tile's menu.
    if (NULL != toplevel_icon_ptr->wlr_layer_surface_v1_ptr &&
        BTN_RIGHT == button_event_ptr->button &&
        _wlmaker_toplevel_icon_content_has_pointer(toplevel_icon_ptr)) {
        return toplevel_icon_ptr->orig_element_vmt.pointer_button(
            element_ptr, button_event_ptr);
    }
    if (NULL != server_ptr->dockapps_ptr &&
        BTN_RIGHT == button_event_ptr->button) {
        if (WLMTK_BUTTON_DOWN == button_event_ptr->type) {
            _wlmaker_toplevel_icon_open_menu(toplevel_icon_ptr);
        }
        return true;
    }

    // Dragging with the move modifier held, or on the tile's border (not
    // on the dock-app itself), moves it on or off the docks.
    if (NULL != server_ptr->dockapps_ptr &&
        BTN_LEFT == button_event_ptr->button &&
        WLMTK_BUTTON_DOWN == button_event_ptr->type &&
        ((0 != modifier &&
          modifier == button_event_ptr->keyboard_modifiers) ||
         NULL == toplevel_icon_ptr->content_surface_ptr ||
         !wlmtk_surface_element(
             toplevel_icon_ptr->content_surface_ptr)->pointer_inside) &&
        NULL != element_ptr->parent_container_ptr) {
        struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
        toplevel_icon_ptr->dragging = true;
        toplevel_icon_ptr->tile_drag_ptr = NULL;
        toplevel_icon_ptr->drag_start_x = element_ptr->last_pointer_motion_event.x;
        toplevel_icon_ptr->drag_start_y = element_ptr->last_pointer_motion_event.y;
        toplevel_icon_ptr->drag_start_lx = c->x;
        toplevel_icon_ptr->drag_start_ly = c->y;
        wlmtk_container_pointer_grab(
            element_ptr->parent_container_ptr, element_ptr);
        return true;
    }

    return toplevel_icon_ptr->orig_element_vmt.pointer_button(
        element_ptr, button_event_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_accepts_motion. Drags the
 * tile, once moved beyond a threshold with the button held. Shows the
 * balloon, with the dock-app's name.
 */
bool _wlmaker_toplevel_icon_element_pointer_accepts_motion(
    wlmtk_element_t *element_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_toplevel_icon_t,
        super_tile.super_container.super_element);
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;
    if (toplevel_icon_ptr->dragging) {
        struct wlr_cursor *c = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
        if (NULL == toplevel_icon_ptr->tile_drag_ptr) {
            double dx = c->x - toplevel_icon_ptr->drag_start_lx;
            double dy = c->y - toplevel_icon_ptr->drag_start_ly;
            // Window Maker's MOVE_THRESHOLD.
            if (dx * dx + dy * dy >= 5 * 5) {
                _wlmaker_toplevel_icon_start_drag(toplevel_icon_ptr, c->x, c->y);
            }
        } else if (toplevel_icon_ptr->tile_drag_ptr == server_ptr->tile_drag_ptr) {
            wlmaker_tile_drag_motion(toplevel_icon_ptr->tile_drag_ptr,
                                     c->x, c->y);
        }
    } else {
        _wlmaker_toplevel_icon_balloon(toplevel_icon_ptr, motion_event_ptr);
        int column = wlmaker_clip_dockapps_column_of(
            server_ptr, &toplevel_icon_ptr->super_tile);
        if (0 <= column) {
            wlmaker_clip_dock_hovered(server_ptr, WLMAKER_COLUMN_CLIP != column);
        }
        // In a drawer: It stays open.
        wlmaker_drawer_t *drawer_ptr = wlmaker_clip_drawer(server_ptr, column);
        if (NULL != drawer_ptr) wlmaker_drawer_hovered(drawer_ptr);
    }
    return toplevel_icon_ptr->orig_element_vmt.pointer_accepts_motion(
        element_ptr, motion_event_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_grab_cancel. Ends the drag. */
void _wlmaker_toplevel_icon_element_pointer_grab_cancel(
    wlmtk_element_t *element_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_toplevel_icon_t,
        super_tile.super_container.super_element);
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;
    toplevel_icon_ptr->dragging = false;
    if (NULL != toplevel_icon_ptr->tile_drag_ptr &&
        toplevel_icon_ptr->tile_drag_ptr == server_ptr->tile_drag_ptr) {
        wlmaker_tile_drag_cancel(toplevel_icon_ptr->tile_drag_ptr);
    }
    toplevel_icon_ptr->tile_drag_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/**
 * Shows the dock-app's balloon, as Window Maker's for docked icons: Its name,
 * and its command in parentheses. Hides it when the pointer leaves the tile.
 */
void _wlmaker_toplevel_icon_balloon(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr,
    wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->icon_manager_ptr->server_ptr;
    wlmtk_element_t *element_ptr = wlmtk_tile_element(
        &toplevel_icon_ptr->super_tile);
    if (NULL == server_ptr->dock_fx_ptr) return;
    struct wlr_box box = wlmtk_element_get_dimensions_box(element_ptr);
    if (motion_event_ptr->x < box.x || motion_event_ptr->y < box.y ||
        motion_event_ptr->x >= box.x + box.width ||
        motion_event_ptr->y >= box.y + box.height ||
        NULL != server_ptr->tile_drag_ptr ||
        // The adopted client shows its own balloons, on its icons.
        (NULL != toplevel_icon_ptr->wlr_layer_surface_v1_ptr &&
         _wlmaker_toplevel_icon_content_has_pointer(toplevel_icon_ptr))) {
        wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, toplevel_icon_ptr);
        toplevel_icon_ptr->balloon_entered = false;
        return;
    }
    if (toplevel_icon_ptr->balloon_entered) return;
    toplevel_icon_ptr->balloon_entered = true;
    int x, y;
    wlmtk_element_get_absolute_position(element_ptr, &x, &y);
    box.x += x;
    box.y += y;
    // As for Window Maker's appicons: The pointer goes 2 pixels in.
    box.height -= 2;

    char *text_ptr = _wlmaker_toplevel_icon_balloon_text(toplevel_icon_ptr);
    struct wlr_box area = {};
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, box.x + box.width / 2,
        box.y + box.height / 2);
    if (NULL != wlr_output_ptr) {
        wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr,
                                  wlr_output_ptr, &area);
    }
    wlmaker_dock_fx_balloon_enter(server_ptr->dock_fx_ptr, toplevel_icon_ptr,
                                  text_ptr, &box, &area);
    if (NULL != text_ptr) free(text_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_element_events_t::pointer_leave: Hides the balloon. */
void _wlmaker_toplevel_icon_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_toplevel_icon_t, pointer_leave_listener);
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->server_ptr;
    toplevel_icon_ptr->balloon_entered = false;
    if (NULL == server_ptr || NULL == server_ptr->dock_fx_ptr) return;
    wlmaker_dock_fx_balloon_leave(server_ptr->dock_fx_ptr, toplevel_icon_ptr);
    int column = wlmaker_clip_dockapps_column_of(
        server_ptr, &toplevel_icon_ptr->super_tile);
    if (0 <= column) {
        wlmaker_clip_dock_left(server_ptr, WLMAKER_COLUMN_CLIP != column);
    }
    wlmaker_drawer_t *drawer_ptr = wlmaker_clip_drawer(server_ptr, column);
    if (NULL != drawer_ptr) wlmaker_drawer_left(drawer_ptr);
}

/* ------------------------------------------------------------------------- */
/** @return The balloon's text: The dock-app's name, and (its command). */
char *_wlmaker_toplevel_icon_balloon_text(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    pid_t pid = _wlmaker_toplevel_icon_pid(toplevel_icon_ptr);
    char *name_ptr = NULL;
    if (NULL != toplevel_icon_ptr->wl_client_ptr) {
        name_ptr = wlmaker_dockapps_client_name(toplevel_icon_ptr->wl_client_ptr);
    }
    if (NULL == name_ptr) return NULL;
    char *cmdline_ptr = _wlmaker_toplevel_icon_cmdline(pid);
    char *text_ptr = (NULL != cmdline_ptr) ?
        bs_strdupf("%s\n(%s)", name_ptr, cmdline_ptr) : logged_strdup(name_ptr);
    if (NULL != cmdline_ptr) free(cmdline_ptr);
    free(name_ptr);
    return text_ptr;
}

/* ------------------------------------------------------------------------- */
/** @return The command line of process `pid`, with spaces. NULL if unknown. */
char *_wlmaker_toplevel_icon_cmdline(pid_t pid)
{
    if (0 >= pid) return NULL;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%"PRIdMAX"/cmdline", (intmax_t)pid);
    FILE *f = fopen(path, "r");
    if (NULL == f) return NULL;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (0 == n) return NULL;
    buf[n] = '\0';
    // Arguments are separated by NUL; the last one ends with it.
    while (0 < n && '\0' == buf[n - 1]) --n;
    // A script (eg. "python3 -I /usr/bin/big-wm-tray"): Its own path, not
    // the interpreter's, and the interpreter's options.
    size_t start = 0;
    const char *base_ptr = strrchr(buf, '/');
    base_ptr = NULL != base_ptr ? base_ptr + 1 : buf;
    if (0 == strncmp(base_ptr, "python", 6) || 0 == strcmp(base_ptr, "sh") ||
        0 == strcmp(base_ptr, "bash") || 0 == strcmp(base_ptr, "perl")) {
        size_t i = strlen(buf) + 1;
        while (i < n && '-' == buf[i]) i += strlen(buf + i) + 1;
        if (i < n) start = i;
    }
    for (size_t i = start; i < n; ++i) if ('\0' == buf[i]) buf[i] = ' ';
    return logged_strdup(buf + start);
}

/* ------------------------------------------------------------------------- */
/** Implements wlmtk_tile_vmt_t::set_content_size. Updates the content size. */
bool _wlmaker_toplevel_icon_tile_set_content_size(
    wlmtk_tile_t *tile_ptr,
    uint64_t content_size)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        tile_ptr, wlmaker_toplevel_icon_t, super_tile);
    // An adopted layer surface keeps the size it asked for.
    if (NULL != toplevel_icon_ptr->wlr_layer_surface_v1_ptr) return true;

    toplevel_icon_ptr->pending_serial = wl_display_next_serial(
        toplevel_icon_ptr->icon_manager_ptr->wl_display_ptr);
    zwlmaker_toplevel_icon_v1_send_configure(
        toplevel_icon_ptr->wl_resource_ptr,
        content_size,
        content_size,
        toplevel_icon_ptr->pending_serial);

    // TODO(kaeser@gubbe.ch): Constrain image size to the content size.
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_icon_manager_adopt_layer_surface(
    wlmaker_icon_manager_t *icon_manager_ptr,
    struct wlr_layer_surface_v1 *wlr_layer_surface_v1_ptr)
{
    wlmaker_server_t *server_ptr = icon_manager_ptr->server_ptr;
    if (NULL == server_ptr->dockapps_ptr || NULL == server_ptr->clip_ptr) {
        return false;
    }
    struct wl_client *wl_client_ptr = wl_resource_get_client(
        wlr_layer_surface_v1_ptr->resource);
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = logged_calloc(
        1, sizeof(wlmaker_toplevel_icon_t));
    if (NULL == toplevel_icon_ptr) return false;
    toplevel_icon_ptr->wl_client_ptr = wl_client_ptr;
    toplevel_icon_ptr->icon_manager_ptr = icon_manager_ptr;
    toplevel_icon_ptr->server_ptr = server_ptr;
    toplevel_icon_ptr->wlr_surface_ptr = wlr_layer_surface_v1_ptr->surface;
    toplevel_icon_ptr->wlr_layer_surface_v1_ptr = wlr_layer_surface_v1_ptr;
    // No configure sequence of the icon protocol: The layer surface's.
    toplevel_icon_ptr->acknowledged = true;

    if (!wlmtk_tile_init(&toplevel_icon_ptr->super_tile,
                         &server_ptr->style_ptr->tile)) {
        free(toplevel_icon_ptr);
        return false;
    }
    toplevel_icon_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&toplevel_icon_ptr->super_tile),
        &_wlmaker_toplevel_icon_element_vmt);
    wlmtk_tile_extend(&toplevel_icon_ptr->super_tile,
                      &_wlmaker_toplevel_icon_tile_vmt);
    wlmtk_element_set_visible(
        wlmtk_tile_element(&toplevel_icon_ptr->super_tile), true);
    wlmtk_util_connect_listener_signal(
        &wlmtk_tile_element(&toplevel_icon_ptr->super_tile)->events.pointer_leave,
        &toplevel_icon_ptr->pointer_leave_listener,
        _wlmaker_toplevel_icon_handle_pointer_leave);

    if (!wlmtk_container_init(&toplevel_icon_ptr->popup_anchor)) {
        wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
        return false;
    }
    toplevel_icon_ptr->popup_anchor_ready = true;
    wlmtk_element_set_visible(
        &toplevel_icon_ptr->popup_anchor.super_element, true);

    toplevel_icon_ptr->content_surface_ptr = wlmtk_surface_create(
        toplevel_icon_ptr->wlr_surface_ptr, server_ptr->wlr_seat_ptr);
    if (NULL == toplevel_icon_ptr->content_surface_ptr) {
        wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
        return false;
    }
    wlmtk_element_set_visible(
        wlmtk_surface_element(toplevel_icon_ptr->content_surface_ptr), true);

    // Placed as the other dock-apps: Where it was before, by its name.
    char *name_ptr = wlmaker_dockapps_client_name(wl_client_ptr);
    bool added = wlmaker_clip_add_dockapp(
        server_ptr, &toplevel_icon_ptr->super_tile, name_ptr);
    bs_log(BS_INFO, "Adopted layer surface %p of \"%s\" as dock-app %p.",
           wlr_layer_surface_v1_ptr, NULL != name_ptr ? name_ptr : "(unknown)",
           toplevel_icon_ptr);
    if (NULL != name_ptr) free(name_ptr);
    if (!added) {
        wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
        return false;
    }

    wlmtk_util_connect_listener_signal(
        &toplevel_icon_ptr->wlr_surface_ptr->events.commit,
        &toplevel_icon_ptr->surface_commit_listener,
        handle_surface_commit);
    wlmtk_util_connect_listener_signal(
        &toplevel_icon_ptr->wlr_surface_ptr->events.destroy,
        &toplevel_icon_ptr->surface_destroy_listener,
        handle_surface_destroy);
    wlmtk_util_connect_listener_signal(
        &wlr_layer_surface_v1_ptr->events.destroy,
        &toplevel_icon_ptr->layer_destroy_listener,
        _wlmaker_toplevel_icon_handle_layer_destroy);
    wlmtk_util_connect_listener_signal(
        &wlr_layer_surface_v1_ptr->events.new_popup,
        &toplevel_icon_ptr->layer_new_popup_listener,
        _wlmaker_toplevel_icon_handle_layer_new_popup);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * A commit of the adopted layer surface: The first one gets the configure,
 * with the size it asked for (else the tile's); then its buffer is the
 * tile's content.
 */
void _wlmaker_toplevel_icon_layer_commit(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    struct wlr_layer_surface_v1 *ls_ptr =
        toplevel_icon_ptr->wlr_layer_surface_v1_ptr;
    if (ls_ptr->initial_commit) {
        uint32_t w = ls_ptr->pending.desired_width;
        uint32_t h = ls_ptr->pending.desired_height;
        uint64_t s = toplevel_icon_ptr->super_tile.style.size;
        if (0 == w || w > s) w = s;
        if (0 == h || h > s) h = s;
        wlr_layer_surface_v1_configure(ls_ptr, w, h);
        return;
    }
    if (NULL == toplevel_icon_ptr->wlr_surface_ptr->buffer) return;
    wlmtk_element_t *e = wlmtk_surface_element(
        toplevel_icon_ptr->content_surface_ptr);
    if (NULL == e->parent_container_ptr) {
        wlmtk_tile_set_content(&toplevel_icon_ptr->super_tile, e);
    }
}

/* ------------------------------------------------------------------------- */
/** The adopted layer surface goes away: So does its tile. */
void _wlmaker_toplevel_icon_handle_layer_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_toplevel_icon_t, layer_destroy_listener);
    wlmaker_toplevel_icon_destroy(toplevel_icon_ptr);
}

/* ------------------------------------------------------------------------- */
/** @return Whether the pointer is on the content, where it takes input. */
bool _wlmaker_toplevel_icon_content_has_pointer(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    return NULL != toplevel_icon_ptr->content_surface_ptr &&
        wlmtk_surface_element(
            toplevel_icon_ptr->content_surface_ptr)->pointer_inside;
}

/* ------------------------------------------------------------------------- */
/**
 * Places the popup anchor at the adopted surface's origin, in the popup
 * container of the icon manager's popup dock: Above all, also above the
 * dock's other tiles and the windows.
 */
static void _wlmaker_toplevel_icon_place_popup_anchor(
    wlmaker_toplevel_icon_t *toplevel_icon_ptr)
{
    wlmaker_server_t *server_ptr = toplevel_icon_ptr->server_ptr;
    wlmaker_icon_manager_t *im_ptr = toplevel_icon_ptr->icon_manager_ptr;
    if (NULL == im_ptr->popup_dock_ptr) {
        wlmtk_dock_positioning_t pos = {
            .edge = WLR_EDGE_LEFT, .anchor = WLR_EDGE_TOP, .overlay = true };
        im_ptr->popup_dock_ptr = wlmtk_dock_create(
            &pos, &server_ptr->style_ptr->dock);
        if (NULL == im_ptr->popup_dock_ptr) return;
        wlmtk_element_set_visible(
            wlmtk_dock_element(im_ptr->popup_dock_ptr), true);
        struct wlr_output *wlr_output_ptr = wlmbe_primary_output(
            server_ptr->wlr_output_layout_ptr);
        wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
            server_ptr->desktop_ptr, WLMTK_DESKTOP_LAYER_OVERLAY);
        if (NULL == wlr_output_ptr || NULL == layer_ptr ||
            !wlmtk_layer_add_panel(layer_ptr,
                                   wlmtk_dock_panel(im_ptr->popup_dock_ptr),
                                   wlr_output_ptr)) {
            wlmtk_dock_destroy(im_ptr->popup_dock_ptr);
            im_ptr->popup_dock_ptr = NULL;
            return;
        }
    }
    wlmtk_panel_t *panel_ptr = wlmtk_dock_panel(im_ptr->popup_dock_ptr);
    wlmtk_element_t *anchor_ptr = &toplevel_icon_ptr->popup_anchor.super_element;
    if (anchor_ptr->parent_container_ptr != &panel_ptr->popup_container) {
        if (NULL != anchor_ptr->parent_container_ptr) {
            wlmtk_container_remove_element(
                anchor_ptr->parent_container_ptr, anchor_ptr);
        }
        wlmtk_container_add_element(&panel_ptr->popup_container, anchor_ptr);
    }
    int sx, sy, px, py;
    wlmtk_element_get_absolute_position(
        wlmtk_surface_element(toplevel_icon_ptr->content_surface_ptr),
        &sx, &sy);
    wlmtk_element_get_absolute_position(
        &panel_ptr->popup_container.super_element, &px, &py);
    wlmtk_element_set_position(anchor_ptr, sx - px, sy - py);
}

/* ------------------------------------------------------------------------- */
/** A popup of the adopted layer surface, eg. a menu. */
void _wlmaker_toplevel_icon_handle_layer_new_popup(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_toplevel_icon_t *toplevel_icon_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_toplevel_icon_t, layer_new_popup_listener);
    struct wlr_xdg_popup *wlr_xdg_popup_ptr = data_ptr;
    wlmaker_xdg_popup_t *popup_ptr = wlmaker_xdg_popup_create(
        wlr_xdg_popup_ptr, toplevel_icon_ptr->server_ptr->wlr_seat_ptr);
    if (NULL == popup_ptr) {
        wl_resource_post_error(wlr_xdg_popup_ptr->resource,
                               WL_DISPLAY_ERROR_NO_MEMORY,
                               "Failed wlmaker_xdg_popup_create.");
        return;
    }
    _wlmaker_toplevel_icon_place_popup_anchor(toplevel_icon_ptr);
    wlmtk_container_add_element(&toplevel_icon_ptr->popup_anchor,
                                wlmaker_xdg_popup_element(popup_ptr));
}

/* == End of icon_manager.c ================================================ */
