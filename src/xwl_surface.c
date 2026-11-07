/* ========================================================================= */
/**
 * @file xwl_surface.c
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

#if defined(WLMAKER_HAVE_XWAYLAND)

#include "xwl_surface.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland/xwayland.h>
#undef WLR_USE_UNSTABLE

#include <xcb/shape.h>
#include <xcb/xcb.h>

#include "icon_manager.h"
#include "input/cursor.h"
#include "input/manager.h"
#include "server.h"
#include "tl_menu.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the XWayland window surface. */
struct _wlmaker_xwl_surface_t {
    /** Holds @ref wlmaker_xwl_surface_t::surface_ptr and child surfaces. */
    wlmtk_base_t              base;

    /** Corresponding wlroots XWayland surface. */
    struct wlr_xwayland_surface *wlr_xwayland_surface_ptr;

    /** Back-link to server. */
    wlmaker_server_t          *server_ptr;
    /** Back-link to the XWayland server. */
    wlmaker_xwl_t             *xwl_ptr;

    /** Link in @ref _xwl_surfaces, for finding a dock-app's icon window. */
    struct wl_list            link;
    /** An X11 dock-app's withdrawn leader window: Shown as its icon. */
    bool                      dockapp_leader;
    /** An X11 dock-app's icon window, adopted as the tile's content. */
    wlmaker_toplevel_icon_t   *dockapp_icon_ptr;
    /** Commits of the icon window left to read its shape again. */
    int                       dockapp_shape_checks;
    /** Listener for commits of the icon window: Its shape, once set. */
    struct wl_listener        dockapp_commit_listener;

    /** Listener for the `destroy` signal of `wlr_xwayland_surface`. */
    struct wl_listener        destroy_listener;
    /** Listener for `request_configure` signal of `wlr_xwayland_surface`. */
    struct wl_listener        request_configure_listener;
    /** Listener for `request_minimize` signal of `wlr_xwayland_surface`. */
    struct wl_listener        request_minimize_listener;
    /** Listener for `map_request` of `wlr_xwayland_surface`: Dock-apps. */
    struct wl_listener        map_request_listener;

    /** Listener for the `associate` signal of `wlr_xwayland_surface`. */
    struct wl_listener        associate_listener;
    /** Listener for the `dissociate` signal of `wlr_xwayland_surface`. */
    struct wl_listener        dissociate_listener;

    /** Listener for the `set_title` signal of `wlr_xwayland_surface`. */
    struct wl_listener        set_title_listener;
    /** Listener for the `set_class` signal of `wlr_xwayland_surface`. */
    struct wl_listener        set_class_listener;
    /** Listener for the `set_parent` signal of `wlr_xwayland_surface`. */
    struct wl_listener        set_parent_listener;
    /** Listener for the `set_decorations` signal of `wlr_xwayland_surface`. */
    struct wl_listener        set_decorations_listener;
    /** Listener for the `set_geometry` signal of `wlr_xwayland_surface`. */
    struct wl_listener        set_geometry_listener;
    /** Listener for the `map` signal of `wlr_xwayland_surface`. */
    struct wl_listener        surface_map_listener;
    /** Listener for the `unmap` signal of `wlr_xwayland_surface`. */
    struct wl_listener        surface_unmap_listener;
    /** Listener for the `commit` signal. */
    struct wl_listener        surface_commit_listener;

    /** Listener for @ref wlmtk_window_events_t::request_close. */
    struct wl_listener        window_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        window_set_activated_listener;
    /** Listener for @ref wlmtk_window_events_t::request_size. */
    struct wl_listener        window_request_size_listener;
    /** Listener for @ref wlmtk_window_events_t::request_fullscreen. */
    struct wl_listener        window_request_fullscreen_listener;
    /** Listener for @ref wlmtk_window_events_t::request_maximized. */
    struct wl_listener        window_request_maximized_listener;

    /** The toolkit surface. Only available once 'associated'. */
    wlmtk_surface_t           *surface_ptr;

    /** The toolkit window, in case the surface does not have a parent. */
    wlmtk_window_t           *window_ptr;
    /** The window menu, with the same items as for XDG toplevels. */
    wlmaker_tl_menu_t         *tl_menu_ptr;
    /** Whether the surface is mapped, but waits for its size to map. */
    bool                      map_pending;
    /** Or, the parent surface. In that case, window_ptr is NULL. */
    wlmaker_xwl_surface_t     *parent_surface_ptr;

    /** The XWL surface's title. May be set before window is created. */
    char                      *title_ptr;

    /**
     * A parent-less override-redirect window (eg. the menu of a dock-app, or
     * a tooltip): Not a window, but on top of everything (the docks, too),
     * where it placed itself. As on X11.
     */
    bool                      overlay;
    /** Holds @ref wlmaker_xwl_surface_t::base for an overlay. */
    wlmtk_panel_t             overlay_panel;
    /** The output the overlay is on, while in the overlay layer. */
    struct wlr_output         *overlay_output_ptr;
    /** Original virtual methods of the overlay panel's element. */
    wlmtk_element_vmt_t       overlay_orig_vmt;
    /** A menu holds the pointer, as with its grab on X11. */
    bool                      overlay_grab;
};

static void _xwl_surface_handle_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_request_configure(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_request_minimize(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_map_window(wlmaker_xwl_surface_t *xwl_surface_ptr);
static bool _xwl_surface_overlay_associate(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_dockapp_shape(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_handle_dockapp_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_overlay_update(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_overlay_unmap(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_overlay_grab(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_overlay_ungrab(wlmaker_xwl_surface_t *xs_ptr);
static uint32_t _xwl_surface_overlay_request_size(
    wlmtk_panel_t *panel_ptr, int width, int height);
static bool _xwl_surface_overlay_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);
/** All XWL surfaces, see @ref wlmaker_xwl_surface_t::link. */
static struct wl_list _xwl_surfaces = { &_xwl_surfaces, &_xwl_surfaces };
static bool _xwl_surface_handle_dockapp(wlmaker_xwl_surface_t *xs_ptr);
static bool _xwl_surface_dockapp_leader(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_handle_map_request(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_dockapp_forget(xcb_window_t window_id);

static void _wlmaker_xwl_surface_base_element_destroy(
    wlmtk_element_t *element_ptr);

static void _xwl_surface_handle_associate(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_dissociate(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _xwl_surface_handle_set_class(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_apply_identity(wlmaker_xwl_surface_t *xs_ptr);
static void _xwl_surface_handle_set_title(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_set_parent(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_set_decorations(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_set_geometry(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _xwl_surface_handle_surface_map(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_surface_unmap(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xwl_surface_handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmaker_xwl_surface_handle_window_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_xwl_surface_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_xwl_surface_handle_window_request_size(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_xwl_surface_handle_window_request_fullscreen(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_xwl_surface_handle_window_request_maximized(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _xwl_surface_apply_decorations(
    wlmaker_xwl_surface_t *xwl_surface_ptr);
static void _xwl_surface_adjust_absolute_pos(
    wlmaker_xwl_surface_t *surface_ptr,
    int *x_ptr,
    int *y_ptr);

/* == Data ================================================================= */

/** Virtual method table of the base's @ref wlmtk_element_t. */
static const wlmtk_element_vmt_t _wlmaker_xwl_surface_base_element_vmt = {
    .destroy = _wlmaker_xwl_surface_base_element_destroy
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `request_minimize` event of `struct wlr_xwayland_surface`:
 * Minimizes the window, or restores it.
 *
 * @param listener_ptr
 * @param data_ptr            Points to struct wlr_xwayland_minimize_event.
 */
void _xwl_surface_handle_request_minimize(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, request_minimize_listener);
    struct wlr_xwayland_minimize_event *event_ptr = data_ptr;

    if (NULL == xwl_surface_ptr->window_ptr) return;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        xwl_surface_ptr->window_ptr);
    if (NULL == workspace_ptr) return;

    if (event_ptr->minimize) {
        wlmtk_window_request_minimize(xwl_surface_ptr->window_ptr);
    } else {
        wlmtk_workspace_raise_window(workspace_ptr, xwl_surface_ptr->window_ptr);
    }
    wlr_xwayland_surface_set_minimized(
        xwl_surface_ptr->wlr_xwayland_surface_ptr,
        wlmtk_window_is_minimized(xwl_surface_ptr->window_ptr));
}

/* ------------------------------------------------------------------------- */
wlmaker_xwl_surface_t *wlmaker_xwl_surface_create(
    struct wlr_xwayland_surface *wlr_xwayland_surface_ptr,
    wlmaker_xwl_t *xwl_ptr,
    wlmaker_server_t *server_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = logged_calloc(
        1, sizeof(wlmaker_xwl_surface_t));
    if (NULL == xwl_surface_ptr) return NULL;
    xwl_surface_ptr->wlr_xwayland_surface_ptr = wlr_xwayland_surface_ptr;
    wlr_xwayland_surface_ptr->data = xwl_surface_ptr;
    xwl_surface_ptr->xwl_ptr = xwl_ptr;
    xwl_surface_ptr->server_ptr = server_ptr;
    wl_list_insert(&_xwl_surfaces, &xwl_surface_ptr->link);

    if (!wlmtk_base_init(&xwl_surface_ptr->base, NULL)) {
        wlmaker_xwl_surface_destroy(xwl_surface_ptr);
        return NULL;
    }
    wlmtk_element_extend(
        wlmtk_base_element(&xwl_surface_ptr->base),
        &_wlmaker_xwl_surface_base_element_vmt);

    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.destroy,
        &xwl_surface_ptr->destroy_listener,
        _xwl_surface_handle_destroy);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.request_configure,
        &xwl_surface_ptr->request_configure_listener,
        _xwl_surface_handle_request_configure);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.request_minimize,
        &xwl_surface_ptr->request_minimize_listener,
        _xwl_surface_handle_request_minimize);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.map_request,
        &xwl_surface_ptr->map_request_listener,
        _xwl_surface_handle_map_request);

    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.associate,
        &xwl_surface_ptr->associate_listener,
        _xwl_surface_handle_associate);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.dissociate,
        &xwl_surface_ptr->dissociate_listener,
        _xwl_surface_handle_dissociate);

    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.set_title,
        &xwl_surface_ptr->set_title_listener,
        _xwl_surface_handle_set_title);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.set_class,
        &xwl_surface_ptr->set_class_listener,
        _xwl_surface_handle_set_class);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.set_parent,
        &xwl_surface_ptr->set_parent_listener,
        _xwl_surface_handle_set_parent);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.set_decorations,
        &xwl_surface_ptr->set_decorations_listener,
        _xwl_surface_handle_set_decorations);
    wlmtk_util_connect_listener_signal(
        &wlr_xwayland_surface_ptr->events.set_geometry,
        &xwl_surface_ptr->set_geometry_listener,
        _xwl_surface_handle_set_geometry);

    bs_log(BS_INFO, "Created XWL surface %p for wlr_xwayland_surface %p",
           xwl_surface_ptr, wlr_xwayland_surface_ptr);

    return xwl_surface_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_xwl_surface_destroy(wlmaker_xwl_surface_t *xwl_surface_ptr)
{
    bs_log(BS_INFO, "Destroy XWL surface %p", xwl_surface_ptr);

    _xwl_surface_handle_dissociate(&xwl_surface_ptr->dissociate_listener, 0);
    _xwl_dockapp_forget(xwl_surface_ptr->wlr_xwayland_surface_ptr->window_id);
    if (NULL != xwl_surface_ptr->link.prev) {
        wl_list_remove(&xwl_surface_ptr->link);
    }

    wlmtk_util_disconnect_listener(&xwl_surface_ptr->set_geometry_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->set_decorations_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->set_parent_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->set_class_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->set_title_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->dissociate_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->associate_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->request_configure_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->request_minimize_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->map_request_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->destroy_listener);

    if (NULL != xwl_surface_ptr->title_ptr) {
        free(xwl_surface_ptr->title_ptr);
        xwl_surface_ptr->title_ptr = NULL;
    }

    wlmtk_base_fini(&xwl_surface_ptr->base);
    if (xwl_surface_ptr->overlay) {
        wlmtk_panel_fini(&xwl_surface_ptr->overlay_panel);
        xwl_surface_ptr->overlay = false;
    }
    xwl_surface_ptr->surface_ptr = NULL;
    free(xwl_surface_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * X11 dock-apps, as Window Maker has them: The program maps a leader window
 * in WithdrawnState, with an icon window in its WM_HINTS. Window Maker shows
 * the icon window on a tile of the Dock or the Clip, and never the leader.
 * Here: The leader gets no window; we map its icon window (as the window
 * manager does on X11), and its surface is adopted as a dock-app tile.
 * Original X11 dock-apps (wmcpuload, wmix, wmcliphist...) thus show and work
 * as on X11.
 */
typedef struct {
    /** The icon window to adopt. XCB_WINDOW_NONE: The slot is free. */
    xcb_window_t              icon_window;
    /** The leader window it belongs to. */
    xcb_window_t              leader_window;
    /** The dock-app's name: WM_CLASS instance of the leader. */
    char                      name[64];
    /** The dock-app's process. */
    pid_t                     pid;
} xwl_dockapp_t;

/** Dock-apps whose icon window was mapped. Few: One per dock-app. */
static xwl_dockapp_t _xwl_dockapps[64];

/** The fields of WM_HINTS a dock-app's leader is told by. */
typedef struct {
    /** Which fields are set. */
    uint32_t                  flags;
    /** Initial state: Withdrawn (0) for a dock-app. */
    uint32_t                  initial_state;
    /** The icon window. */
    xcb_window_t              icon_window;
} xwl_wm_hints_t;

/* ------------------------------------------------------------------------- */
/** Reads the window's WM_HINTS. @return false if it has none. */
static bool _xwl_read_wm_hints(
    xcb_connection_t *c_ptr,
    xcb_window_t window,
    xwl_wm_hints_t *hints_ptr)
{
    xcb_get_property_reply_t *r_ptr = xcb_get_property_reply(
        c_ptr,
        xcb_get_property(c_ptr, 0, window, XCB_ATOM_WM_HINTS,
                         XCB_ATOM_WM_HINTS, 0, 9),
        NULL);
    if (NULL == r_ptr) return false;
    bool rv = false;
    // flags, input, initial_state, icon_pixmap, icon_window, ...
    if (32 == r_ptr->format &&
        5 <= xcb_get_property_value_length(r_ptr) / 4) {
        const uint32_t *v_ptr = xcb_get_property_value(r_ptr);
        hints_ptr->flags = v_ptr[0];
        hints_ptr->initial_state = v_ptr[2];
        hints_ptr->icon_window = v_ptr[4];
        rv = true;
    }
    free(r_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Reads WM_CLASS: Instance into `buf`, class into `cls` ("" if none). */
static void _xwl_read_class(
    xcb_connection_t *c_ptr,
    xcb_window_t window,
    char *buf,
    size_t len,
    char *cls,
    size_t cls_len)
{
    buf[0] = '\0';
    cls[0] = '\0';
    xcb_get_property_reply_t *r_ptr = xcb_get_property_reply(
        c_ptr,
        xcb_get_property(c_ptr, 0, window, XCB_ATOM_WM_CLASS,
                         XCB_ATOM_STRING, 0, 64),
        NULL);
    if (NULL == r_ptr) return;
    int n = xcb_get_property_value_length(r_ptr);
    if (8 == r_ptr->format && 0 < n) {
        // "instance\0class\0".
        const char *v_ptr = xcb_get_property_value(r_ptr);
        snprintf(buf, len, "%.*s", n, v_ptr);
        int i = strnlen(v_ptr, n);
        if (i + 1 < n) snprintf(cls, cls_len, "%.*s", n - i - 1, v_ptr + i + 1);
    }
    free(r_ptr);
}

/* ------------------------------------------------------------------------- */
/** Reads _NET_WM_PID. @return The process, or 0 if unknown. */
static pid_t _xwl_read_pid(xcb_connection_t *c_ptr, xcb_window_t window)
{
    static xcb_atom_t net_wm_pid = XCB_ATOM_NONE;
    if (XCB_ATOM_NONE == net_wm_pid) {
        xcb_intern_atom_reply_t *a_ptr = xcb_intern_atom_reply(
            c_ptr, xcb_intern_atom(c_ptr, 0, 11, "_NET_WM_PID"), NULL);
        if (NULL == a_ptr) return 0;
        net_wm_pid = a_ptr->atom;
        free(a_ptr);
    }
    xcb_get_property_reply_t *r_ptr = xcb_get_property_reply(
        c_ptr,
        xcb_get_property(c_ptr, 0, window, net_wm_pid, XCB_ATOM_CARDINAL, 0, 1),
        NULL);
    if (NULL == r_ptr) return 0;
    pid_t pid = 0;
    if (32 == r_ptr->format && 4 <= xcb_get_property_value_length(r_ptr)) {
        pid = (pid_t)*(const uint32_t *)xcb_get_property_value(r_ptr);
    }
    free(r_ptr);
    return pid;
}

/* ------------------------------------------------------------------------- */
/** @return The entry for the icon window `window_id`, or NULL. */
static xwl_dockapp_t *_xwl_dockapp_of_icon(xcb_window_t window_id)
{
    if (XCB_WINDOW_NONE == window_id) return NULL;
    for (size_t i = 0; i < sizeof(_xwl_dockapps) / sizeof(_xwl_dockapps[0]); ++i) {
        if (_xwl_dockapps[i].icon_window == window_id) return &_xwl_dockapps[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Forgets the dock-app entries of the window (leader or icon window). */
void _xwl_dockapp_forget(xcb_window_t window_id)
{
    for (size_t i = 0; i < sizeof(_xwl_dockapps) / sizeof(_xwl_dockapps[0]); ++i) {
        xwl_dockapp_t *d_ptr = &_xwl_dockapps[i];
        if (XCB_WINDOW_NONE == d_ptr->icon_window) continue;
        if (d_ptr->leader_window == window_id ||
            d_ptr->icon_window == window_id) {
            *d_ptr = (xwl_dockapp_t){ .icon_window = XCB_WINDOW_NONE };
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Passes the shape of a dock-app's icon window (X Shape extension, eg. the
 * round corners of wmclock, wmcliphist, wmix) to its tile: Window Maker shows
 * the tile around the shape. Without a shape (one rectangle covering the
 * window), the whole window.
 *
 * @param xs_ptr
 */
void _xwl_surface_dockapp_shape(wlmaker_xwl_surface_t *xs_ptr)
{
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    xcb_connection_t *c_ptr = wlr_xwayland_get_xwm_connection(
        wlmaker_xwl_wlr_xwayland(xs_ptr->xwl_ptr));
    if (NULL == c_ptr) return;
    const xcb_query_extension_reply_t *ext_ptr = xcb_get_extension_data(
        c_ptr, &xcb_shape_id);
    if (NULL == ext_ptr || !ext_ptr->present) return;

    xcb_shape_get_rectangles_reply_t *r_ptr = xcb_shape_get_rectangles_reply(
        c_ptr,
        xcb_shape_get_rectangles(c_ptr, xws_ptr->window_id,
                                 XCB_SHAPE_SK_BOUNDING),
        NULL);
    if (NULL == r_ptr) return;
    int n = xcb_shape_get_rectangles_rectangles_length(r_ptr);
    xcb_rectangle_t *rects_ptr = xcb_shape_get_rectangles_rectangles(r_ptr);
    // Many rectangles (a shape of single pixels): The whole window instead.
    struct wlr_box boxes[256];
    size_t boxes_n = 0;
    struct wlr_box window = { .width = xws_ptr->width,
                              .height = xws_ptr->height };
    bool whole = (0 >= n || n > (int)(sizeof(boxes) / sizeof(boxes[0])));
    for (int i = 0; !whole && i < n; ++i) {
        struct wlr_box b = { .x = rects_ptr[i].x, .y = rects_ptr[i].y,
                             .width = rects_ptr[i].width,
                             .height = rects_ptr[i].height };
        if (!wlr_box_intersection(&b, &b, &window)) continue;
        if (wlr_box_equal(&b, &window)) whole = true;
        boxes[boxes_n++] = b;
    }
    free(r_ptr);
    if (whole || 0 == boxes_n) {
        wlmaker_icon_manager_set_xwayland_shape(xs_ptr->dockapp_icon_ptr,
                                                NULL, 0);
        return;
    }
    bs_log(BS_INFO, "X11 dock-app icon window 0x%"PRIx32": Shaped, %zu "
           "rectangles.", xws_ptr->window_id, boxes_n);
    wlmaker_icon_manager_set_xwayland_shape(xs_ptr->dockapp_icon_ptr,
                                            boxes, boxes_n);
}

/* ------------------------------------------------------------------------- */
/** A commit of a dock-app's icon window: Reads its shape, the first times. */
void _xwl_surface_handle_dockapp_commit(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xs_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, dockapp_commit_listener);
    if (NULL != xs_ptr->dockapp_icon_ptr) _xwl_surface_dockapp_shape(xs_ptr);
    if (0 >= --xs_ptr->dockapp_shape_checks) {
        wlmtk_util_disconnect_listener(&xs_ptr->dockapp_commit_listener);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handles an associated surface of an X11 dock-app.
 *
 * @return true if it is one (leader or icon window): No window for it.
 */
bool _xwl_surface_handle_dockapp(wlmaker_xwl_surface_t *xs_ptr)
{
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    wlmaker_server_t *server_ptr = xs_ptr->server_ptr;
    if (NULL == server_ptr->icon_manager_ptr ||
        NULL == server_ptr->dockapps_ptr) return false;

    // The icon window of a dock-app's leader: Its tile.
    xwl_dockapp_t *d_ptr = _xwl_dockapp_of_icon(xws_ptr->window_id);
    if (NULL != d_ptr) {
        xs_ptr->dockapp_icon_ptr = wlmaker_icon_manager_adopt_xwayland_surface(
            server_ptr->icon_manager_ptr, xws_ptr,
            '\0' != d_ptr->name[0] ? d_ptr->name : NULL, d_ptr->pid);
        if (NULL == xs_ptr->dockapp_icon_ptr) return false;
        _xwl_surface_dockapp_shape(xs_ptr);
        // GTK (wmcliphist) shapes the window once it is mapped: Again with
        // the first few commits.
        xs_ptr->dockapp_shape_checks = 5;
        wlmtk_util_connect_listener_signal(
            &xws_ptr->surface->events.commit,
            &xs_ptr->dockapp_commit_listener,
            _xwl_surface_handle_dockapp_commit);
        return true;
    }

    // A leader (its surface, if it draws in its window): No window.
    return xs_ptr->dockapp_leader || _xwl_surface_dockapp_leader(xs_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles `map_request`: A dock-app's leader maps its icon window. */
void _xwl_surface_handle_map_request(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xs_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, map_request_listener);
    if (!xs_ptr->dockapp_leader) _xwl_surface_dockapp_leader(xs_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Checks for an X11 dock-app's leader, and maps its icon window.
 *
 * The leader is a window in WithdrawnState with an icon window in its
 * WM_HINTS. Most dock-apps never draw in it: Its surface may never be
 * associated. So this is done at the map request already.
 *
 * @return true if it is a dock-app's leader.
 */
bool _xwl_surface_dockapp_leader(wlmaker_xwl_surface_t *xs_ptr)
{
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    wlmaker_server_t *server_ptr = xs_ptr->server_ptr;
    xwl_dockapp_t *d_ptr = NULL;
    if (NULL == server_ptr->icon_manager_ptr ||
        NULL == server_ptr->dockapps_ptr) return false;

    if (xws_ptr->override_redirect) return false;
    xcb_connection_t *c_ptr = wlr_xwayland_get_xwm_connection(
        wlmaker_xwl_wlr_xwayland(xs_ptr->xwl_ptr));
    if (NULL == c_ptr) return false;
    // Read here: wlroots reads the properties of a window when its surface
    // is associated, and a dock-app's leader may never get there.
    xwl_wm_hints_t hints;
    if (!_xwl_read_wm_hints(c_ptr, xws_ptr->window_id, &hints)) return false;
    char instance[64], cls[64];
    _xwl_read_class(c_ptr, xws_ptr->window_id,
                    instance, sizeof(instance), cls, sizeof(cls));
    // A leader, as Window Maker tells one: WithdrawnState, or the class
    // "DockApp" (libdockapp; eg. wmshutdown, in NormalState). With an icon
    // window in WM_HINTS, else the leader itself is the tile.
    const xwl_wm_hints_t *h_ptr = &hints;
    bool withdrawn = (h_ptr->flags & XCB_ICCCM_WM_HINT_STATE) &&
        XCB_ICCCM_WM_STATE_WITHDRAWN == h_ptr->initial_state;
    if (!withdrawn && 0 != strcmp(cls, "DockApp")) return false;
    if (!(h_ptr->flags & XCB_ICCCM_WM_HINT_ICON_WINDOW)) {
        hints.icon_window = xws_ptr->window_id;
    }
    if (XCB_WINDOW_NONE == h_ptr->icon_window) return false;

    // Which window becomes the tile: The icon window if it is a top-level
    // (libdockapp). Is it the leader itself (eg. wmcliphist), or a child of
    // it (the "wmgeneral" dock-apps: wmclock, wmix, wmcube...), the leader:
    // XWayland only gives top-level windows a surface, and those programs
    // draw the same image in both windows.
    xcb_window_t tile_window = h_ptr->icon_window;
    if (tile_window != xws_ptr->window_id) {
        xcb_query_tree_reply_t *t_ptr = xcb_query_tree_reply(
            c_ptr, xcb_query_tree(c_ptr, tile_window), NULL);
        if (NULL != t_ptr) {
            if (t_ptr->parent == xws_ptr->window_id) {
                tile_window = xws_ptr->window_id;
            }
            free(t_ptr);
        }
    }

    if (NULL == _xwl_dockapp_of_icon(tile_window)) {
        d_ptr = _xwl_dockapp_of_icon(XCB_WINDOW_NONE);
        for (size_t i = 0; NULL == d_ptr &&
                 i < sizeof(_xwl_dockapps) / sizeof(_xwl_dockapps[0]); ++i) {
            if (XCB_WINDOW_NONE == _xwl_dockapps[i].icon_window) {
                d_ptr = &_xwl_dockapps[i];
            }
        }
        if (NULL == d_ptr) {
            bs_log(BS_WARNING, "Too many X11 dock-apps, not docking 0x%"PRIx32,
                   xws_ptr->window_id);
            return false;
        }
        *d_ptr = (xwl_dockapp_t){
            .icon_window = tile_window,
            .leader_window = xws_ptr->window_id,
            .pid = _xwl_read_pid(c_ptr, xws_ptr->window_id),
        };
        snprintf(d_ptr->name, sizeof(d_ptr->name), "%s", instance);
    }
    d_ptr = _xwl_dockapp_of_icon(tile_window);
    if (tile_window == xws_ptr->window_id) {
        // The leader is the tile: Its surface comes through `associate`.
        bs_log(BS_INFO, "X11 dock-app \"%s\" (window 0x%"PRIx32"): Shown "
               "on its tile.", NULL != d_ptr ? d_ptr->name : "(unknown)",
               xws_ptr->window_id);
        return false;
    }
    xs_ptr->dockapp_leader = true;
    // A program that maps the icon window itself, before the leader (GTK
    // dock-apps, eg. wmshutdown): It already got a window. Associate it
    // again, as the dock-app's tile.
    wlmaker_xwl_surface_t *icon_xs_ptr;
    wl_list_for_each(icon_xs_ptr, &_xwl_surfaces, link) {
        struct wlr_xwayland_surface *i_ptr =
            icon_xs_ptr->wlr_xwayland_surface_ptr;
        if (i_ptr->window_id != h_ptr->icon_window ||
            NULL == i_ptr->surface ||
            NULL != icon_xs_ptr->dockapp_icon_ptr) continue;
        _xwl_surface_handle_dissociate(&icon_xs_ptr->dissociate_listener, NULL);
        _xwl_surface_handle_associate(&icon_xs_ptr->associate_listener, NULL);
        break;
    }
    // As the window manager on X11: Map the icon window. Its surface comes
    // back through `associate`, as the icon window's.
    xcb_map_window(c_ptr, h_ptr->icon_window);
    xcb_flush(c_ptr);
    bs_log(BS_INFO, "X11 dock-app \"%s\" (window 0x%"PRIx32"): Mapped its "
           "icon window 0x%"PRIx32".",
           NULL != d_ptr ? d_ptr->name : "(unknown)",
           xws_ptr->window_id, h_ptr->icon_window);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `destroy` event of `struct wlr_xwayland_surface`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, destroy_listener);
    wlmaker_xwl_surface_destroy(xwl_surface_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `request_configure` event of `struct wlr_xwayland_surface`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_request_configure(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, request_configure_listener);
    struct wlr_xwayland_surface_configure_event *cfg_event_ptr = data_ptr;

    bs_log(BS_INFO, "Request configure for %p: "
           "%"PRId16",%"PRId16" size %"PRIu16" x %"PRIu16" mask 0x%"PRIx16,
           xwl_surface_ptr,
           cfg_event_ptr->x, cfg_event_ptr->y,
           cfg_event_ptr->width, cfg_event_ptr->height,
           cfg_event_ptr->mask);

    // FIXME:
    // -> if we have surface: check what that means, with respect to
    //    the surface::commit handler.

    // It appears this needs to be ACKed with a surface_configure.
    wlr_xwayland_surface_configure(
        xwl_surface_ptr->wlr_xwayland_surface_ptr,
        cfg_event_ptr->x, cfg_event_ptr->y,
        cfg_event_ptr->width, cfg_event_ptr->height);
}

/* ------------------------------------------------------------------------- */
/**
 * Dtor for @ref wlmaker_xwl_surface_t::base.
 *
 * This may be called when the parent window is destroyed, but the child(ren)
 * have not yet. It implies that this window already had been removed from the
 * parent surface's base. To prevent double cleanup, we can set
 * @ref wlmaker_xwl_surface_t::parent_surface_ptr to NULL.
 *
 * @param element_ptr
 */
void _wlmaker_xwl_surface_base_element_destroy(
    wlmtk_element_t *element_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        element_ptr,
        wlmaker_xwl_surface_t,
        base.super_container.super_element);

    xwl_surface_ptr->parent_surface_ptr = NULL;
    wlmaker_xwl_surface_destroy(xwl_surface_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `associate` event of `struct wlr_xwayland_surface`.
 *
 * The `associate` event is triggered once an X11 window becomes associated
 * with the surface. Understanding this is a moment the surface can be mapped.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_associate(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, associate_listener);
    wlmaker_xwl_surface_t *parent_xwl_surface_ptr = NULL;
    if (NULL != xwl_surface_ptr->wlr_xwayland_surface_ptr->parent) {
        parent_xwl_surface_ptr =
            xwl_surface_ptr->wlr_xwayland_surface_ptr->parent->data;
    }
    for (size_t i = 0;
         i < xwl_surface_ptr->wlr_xwayland_surface_ptr->window_type_len;
         ++i) {
        const char *name_ptr = xwl_atom_name(
            xwl_surface_ptr->xwl_ptr,
            xwl_surface_ptr->wlr_xwayland_surface_ptr->window_type[i]);
        if (NULL != name_ptr) {
            bs_log(BS_INFO, "  XWL surface %p has window type %s",
                   xwl_surface_ptr, name_ptr);
        }
    }

    BS_ASSERT(NULL == xwl_surface_ptr->surface_ptr);

    // An X11 dock-app (its withdrawn leader, or its icon window): A tile on
    // the docks, no window.
    if (_xwl_surface_handle_dockapp(xwl_surface_ptr)) return;

    xwl_surface_ptr->surface_ptr = wlmtk_surface_create(
        xwl_surface_ptr->wlr_xwayland_surface_ptr->surface,
        xwl_surface_ptr->server_ptr->wlr_seat_ptr);
    if (NULL == xwl_surface_ptr->surface_ptr) {
        // TODO(kaeser@gubbe.ch): Relay error to client, instead of crash.
        bs_log(BS_FATAL, "Failed wlmtk_surface_create.");
        return;
    }

    wlmtk_surface_connect_map_listener_signal(
        xwl_surface_ptr->surface_ptr,
        &xwl_surface_ptr->surface_map_listener,
        _xwl_surface_handle_surface_map);
    wlmtk_surface_connect_unmap_listener_signal(
        xwl_surface_ptr->surface_ptr,
        &xwl_surface_ptr->surface_unmap_listener,
        _xwl_surface_handle_surface_unmap);

    wlmtk_util_connect_listener_signal(
        &xwl_surface_ptr->wlr_xwayland_surface_ptr->surface->events.commit,
        &xwl_surface_ptr->surface_commit_listener,
        _xwl_surface_handle_surface_commit);
    _xwl_surface_handle_surface_commit(
        &xwl_surface_ptr->surface_commit_listener,
        NULL);

    wlmtk_base_set_content_element(
        &xwl_surface_ptr->base,
        wlmtk_surface_element(xwl_surface_ptr->surface_ptr));

    if (_xwl_surface_overlay_associate(xwl_surface_ptr)) {
        _xwl_surface_overlay_update(xwl_surface_ptr);
        return;
    }

    // Currently we treat parent-less windows AND modal windows as toplevel.
    // Modal windows should actually be child wlmtk_window_t, but that isn't
    // supported yet.
    if (NULL == xwl_surface_ptr->wlr_xwayland_surface_ptr->parent ||
        xwl_surface_ptr->wlr_xwayland_surface_ptr->modal) {

        BS_ASSERT(NULL == xwl_surface_ptr->window_ptr);

        xwl_surface_ptr->window_ptr = wlmtk_window_create(
            wlmtk_base_element(&xwl_surface_ptr->base),
            wlmtk_window_style_to_ref(
                xwl_surface_ptr->server_ptr->style_ptr->window_style_ptr),
            wlmtk_menu_style_to_ref(
                xwl_surface_ptr->server_ptr->style_ptr->menu_style_ptr));
        if (NULL == xwl_surface_ptr->window_ptr) {
            // TODO(kaeser@gubbe.ch): Relay error to client, instead of crash.
            bs_log(BS_FATAL, "Failed wlmtk_window_create.");
            return;
        }
        _xwl_surface_apply_decorations(xwl_surface_ptr);
        wlmtk_window_set_options(
            xwl_surface_ptr->window_ptr,
            wlmim_cursor_options(xwl_surface_ptr->server_ptr->input_manager_ptr
                )->move_window_modifier);
        wlmtk_window_set_properties(
            xwl_surface_ptr->window_ptr,
            WLMTK_WINDOW_PROPERTY_RESIZABLE |
            WLMTK_WINDOW_PROPERTY_ICONIFIABLE |
            WLMTK_WINDOW_PROPERTY_CLOSABLE);
        wlmtk_util_client_t client = {
            .pid = xwl_surface_ptr->wlr_xwayland_surface_ptr->pid
        };
        wlmtk_window_set_client(xwl_surface_ptr->window_ptr, &client);
        wlmtk_window_set_title(xwl_surface_ptr->window_ptr,
                                xwl_surface_ptr->title_ptr);
        _xwl_surface_apply_identity(xwl_surface_ptr);

        xwl_surface_ptr->tl_menu_ptr = wlmaker_tl_menu_create(
            xwl_surface_ptr->window_ptr, xwl_surface_ptr->server_ptr);
        if (NULL == xwl_surface_ptr->tl_menu_ptr) {
            bs_log(BS_WARNING, "Failed wlmaker_tl_menu_create(%p, %p)",
                   xwl_surface_ptr->window_ptr, xwl_surface_ptr->server_ptr);
        }

        wlmtk_util_connect_listener_signal(
            &wlmtk_window_events(xwl_surface_ptr->window_ptr)->request_close,
            &xwl_surface_ptr->window_request_close_listener,
            _wlmaker_xwl_surface_handle_window_request_close);
        wlmtk_util_connect_listener_signal(
            &wlmtk_window_events(xwl_surface_ptr->window_ptr)->set_activated,
            &xwl_surface_ptr->window_set_activated_listener,
            _wlmaker_xwl_surface_handle_window_set_activated);
        wlmtk_util_connect_listener_signal(
            &wlmtk_window_events(xwl_surface_ptr->window_ptr)->request_size,
            &xwl_surface_ptr->window_request_size_listener,
            _wlmaker_xwl_surface_handle_window_request_size);
        wlmtk_util_connect_listener_signal(
            &wlmtk_window_events(xwl_surface_ptr->window_ptr)->request_fullscreen,
            &xwl_surface_ptr->window_request_fullscreen_listener,
            _wlmaker_xwl_surface_handle_window_request_fullscreen);
        wlmtk_util_connect_listener_signal(
            &wlmtk_window_events(xwl_surface_ptr->window_ptr)->request_maximized,
            &xwl_surface_ptr->window_request_maximized_listener,
            _wlmaker_xwl_surface_handle_window_request_maximized);

        wl_signal_emit(
            &xwl_surface_ptr->server_ptr->window_created_event,
            xwl_surface_ptr->window_ptr);
    }

    bs_log(BS_INFO,
           "Associated XWL surface %p with wlr_surface %p, parent %p at %d, %d",
           xwl_surface_ptr, xwl_surface_ptr->wlr_xwayland_surface_ptr->surface,
           parent_xwl_surface_ptr,
           xwl_surface_ptr->wlr_xwayland_surface_ptr->x,
           xwl_surface_ptr->wlr_xwayland_surface_ptr->y);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `dissociate` event of `struct wlr_xwayland_surface`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_dissociate(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, dissociate_listener);

    bs_log(BS_INFO, "Dissociate XWL surface %p from wlr_surface %p",
           xwl_surface_ptr, xwl_surface_ptr->wlr_xwayland_surface_ptr->surface);

    if (NULL != xwl_surface_ptr->dockapp_icon_ptr) {
        wlmtk_util_disconnect_listener(
            &xwl_surface_ptr->dockapp_commit_listener);
        wlmaker_icon_manager_release_xwayland_icon(
            xwl_surface_ptr->dockapp_icon_ptr);
        xwl_surface_ptr->dockapp_icon_ptr = NULL;
        return;
    }
    if (xwl_surface_ptr->dockapp_leader) return;

    if (NULL != xwl_surface_ptr->window_ptr) {
        wlmtk_util_disconnect_listener(
            &xwl_surface_ptr->window_request_close_listener);
        wlmtk_util_disconnect_listener(
            &xwl_surface_ptr->window_set_activated_listener);
        wlmtk_util_disconnect_listener(
            &xwl_surface_ptr->window_request_size_listener);
        wlmtk_util_disconnect_listener(
            &xwl_surface_ptr->window_request_fullscreen_listener);
        wlmtk_util_disconnect_listener(
            &xwl_surface_ptr->window_request_maximized_listener);

        wl_signal_emit(
            &xwl_surface_ptr->server_ptr->window_destroyed_event,
            xwl_surface_ptr->window_ptr);

        if (NULL != xwl_surface_ptr->tl_menu_ptr) {
            wlmaker_tl_menu_destroy(xwl_surface_ptr->tl_menu_ptr);
            xwl_surface_ptr->tl_menu_ptr = NULL;
        }
        wlmtk_window_destroy(xwl_surface_ptr->window_ptr);
        xwl_surface_ptr->window_ptr = NULL;
    }

    if (xwl_surface_ptr->overlay) {
        _xwl_surface_overlay_unmap(xwl_surface_ptr);
        wlmtk_element_t *e_ptr = wlmtk_base_element(&xwl_surface_ptr->base);
        if (NULL != e_ptr->parent_container_ptr) {
            wlmtk_container_remove_element(
                &xwl_surface_ptr->overlay_panel.super_container, e_ptr);
        }
    }

    if (NULL != xwl_surface_ptr->parent_surface_ptr) {
        wlmtk_base_pop_element(
            &xwl_surface_ptr->parent_surface_ptr->base,
            wlmtk_base_element(&xwl_surface_ptr->base));
        xwl_surface_ptr->parent_surface_ptr = NULL;
    }

    wlmtk_util_disconnect_listener(&xwl_surface_ptr->surface_commit_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->surface_map_listener);
    wlmtk_util_disconnect_listener(&xwl_surface_ptr->surface_unmap_listener);
    wlmtk_base_set_content_element(&xwl_surface_ptr->base, NULL);
    xwl_surface_ptr->surface_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** Copies WM_CLASS and the dialog state of the X11 window to the window. */
void _xwl_surface_apply_identity(wlmaker_xwl_surface_t *xs_ptr)
{
    if (NULL == xs_ptr->window_ptr) return;
    struct wlr_xwayland_surface *s = xs_ptr->wlr_xwayland_surface_ptr;
    wlmtk_window_set_app_id(xs_ptr->window_ptr, s->class, s->instance);
    wlmtk_window_set_dialog(
        xs_ptr->window_ptr,
        s->modal || NULL != s->parent ||
        wlr_xwayland_surface_has_window_type(
            s, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
        wlr_xwayland_surface_has_window_type(
            s, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
        wlr_xwayland_surface_has_window_type(
            s, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY));
}

/* ------------------------------------------------------------------------- */
/** Handler for the `set_class` event of `struct wlr_xwayland_surface`. */
void _xwl_surface_handle_set_class(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xs_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, set_class_listener);
    _xwl_surface_apply_identity(xs_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `set_title` event of `struct wlr_xwayland_surface`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_set_title(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xs_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, set_title_listener);

    if (NULL != xs_ptr->title_ptr) {
        free(xs_ptr->title_ptr);
        xs_ptr->title_ptr = NULL;
    }
    if (NULL != xs_ptr->wlr_xwayland_surface_ptr->title) {
        xs_ptr->title_ptr = logged_strdup(xs_ptr->wlr_xwayland_surface_ptr->title);
        if (NULL == xs_ptr->title_ptr) return;
    }

    if (NULL != xs_ptr->window_ptr) {
        wlmtk_window_set_title(xs_ptr->window_ptr, xs_ptr->title_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `set_parent` event of `struct wlr_xwayland_surface`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_set_parent(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, set_parent_listener);

    wlmaker_xwl_surface_t *parent_xwl_surface_ptr = NULL;
    if (NULL != xwl_surface_ptr->wlr_xwayland_surface_ptr->parent) {
        parent_xwl_surface_ptr =
            xwl_surface_ptr->wlr_xwayland_surface_ptr->parent->data;
    }

    if (xwl_surface_ptr->parent_surface_ptr == parent_xwl_surface_ptr) return;

    if (NULL != xwl_surface_ptr->parent_surface_ptr) {
        wlmtk_base_pop_element(
            &xwl_surface_ptr->parent_surface_ptr->base,
            wlmtk_base_element(&xwl_surface_ptr->base));
        xwl_surface_ptr->parent_surface_ptr = NULL;
    }

    if (NULL == parent_xwl_surface_ptr) return;

    // TODO(kaeser@gubbe.ch): We're currently treating modal windows as
    // toplevel windows. They're not popups, for sure. To support this,
    // we'll need wlmtk_window_t to support child wlmtk_window_t.
    if (xwl_surface_ptr->wlr_xwayland_surface_ptr->modal) return;

    wlmtk_base_push_element(
        &parent_xwl_surface_ptr->base,
        wlmtk_base_element(&xwl_surface_ptr->base));
    xwl_surface_ptr->parent_surface_ptr = parent_xwl_surface_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `set_decorations` event of `struct wlr_xwayland_surface`.
 *
 * Applies server-side decoration, if the X11 window is supposed to have
 * decorations.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_set_decorations(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, set_decorations_listener);
    _xwl_surface_apply_decorations(xwl_surface_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `set_geometry` event of `struct wlr_xwayland_surface`.
 *
 * Called from wlroots/xwayland/xwm.c, whenever the geometry (position or
 * dimensions) of the window (precisely: the xwayland_surface) changes.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xwl_surface_handle_set_geometry(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, set_geometry_listener);

    if (xwl_surface_ptr->overlay) {
        _xwl_surface_overlay_update(xwl_surface_ptr);
        return;
    }

    // For XWayland, the surface's position is given relative to the "root"
    // of the specified surface. For @ref wlmtk_element_t, the position is
    // just relative to the parent @ref wlmtk_container_t. So we need to
    // subtract each parent surface's position.
    int x = xwl_surface_ptr->wlr_xwayland_surface_ptr->x;
    int y = xwl_surface_ptr->wlr_xwayland_surface_ptr->y;
    _xwl_surface_adjust_absolute_pos(xwl_surface_ptr, &x, &y);

    wlmtk_element_set_position(
        wlmtk_base_element(&xwl_surface_ptr->base), x, y);
}

/* ------------------------------------------------------------------------- */
/** Handles when the surface is mapped: Map it to the workspace. */
void _xwl_surface_handle_surface_map(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, surface_map_listener);

    if (xwl_surface_ptr->overlay) {
        _xwl_surface_overlay_update(xwl_surface_ptr);
        return;
    }
    if (NULL == xwl_surface_ptr->window_ptr) return;

    // The surface is mapped while committing, before the toolkit surface
    // learned its size. Place & map the window once the size is known.
    struct wlr_box size = wlmtk_window_get_size(xwl_surface_ptr->window_ptr);
    if (0 >= size.width || 0 >= size.height) {
        xwl_surface_ptr->map_pending = true;
        return;
    }
    _xwl_surface_map_window(xwl_surface_ptr);
}

/* ------------------------------------------------------------------------- */
/** Places the window (respecting hints), and maps it to the workspace. */
void _xwl_surface_map_window(wlmaker_xwl_surface_t *xwl_surface_ptr)
{
    xwl_surface_ptr->map_pending = false;
    wlmtk_workspace_t *workspace_ptr =
        wlmtk_desktop_get_current_workspace(
            xwl_surface_ptr->server_ptr->desktop_ptr);

    // Respect positions requested by the user (eg. -geometry +X+Y) or the
    // program. A program position of (0, 0) is commonly just a default.
    // Transients (eg. modal dialogs) are centered on their parent.
    struct wlr_xwayland_surface *xws_ptr =
        xwl_surface_ptr->wlr_xwayland_surface_ptr;
    wlmtk_window_placement_hint_t hint = {};
    if (NULL != xws_ptr->size_hints) {
        uint32_t flags = xws_ptr->size_hints->flags;
        if (flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
            wlmtk_window_set_min_size(
                xwl_surface_ptr->window_ptr,
                xws_ptr->size_hints->min_width,
                xws_ptr->size_hints->min_height);
        }
        if ((flags & XCB_ICCCM_SIZE_HINT_US_POSITION) ||
            ((flags & XCB_ICCCM_SIZE_HINT_P_POSITION) &&
             (0 != xws_ptr->x || 0 != xws_ptr->y))) {
            hint.has_position = true;
            hint.x = xws_ptr->x;
            hint.y = xws_ptr->y;
        }
    }
    // An override-redirect window places itself, wherever it is.
    if (xws_ptr->override_redirect) {
        hint.has_position = true;
        hint.x = xws_ptr->x;
        hint.y = xws_ptr->y;
    }
    if (NULL != xws_ptr->parent && NULL != xws_ptr->parent->data) {
        wlmaker_xwl_surface_t *parent_ptr = xws_ptr->parent->data;
        if (NULL != parent_ptr->window_ptr &&
            NULL != wlmtk_window_get_workspace(parent_ptr->window_ptr)) {
            hint.parent_ptr = parent_ptr->window_ptr;
        }
    }
    wlmtk_workspace_map_window_with_hint(
        workspace_ptr, xwl_surface_ptr->window_ptr, &hint);
}

/* ------------------------------------------------------------------------- */
/** Unmaps the window */
void _xwl_surface_handle_surface_unmap(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, surface_unmap_listener);

    xwl_surface_ptr->map_pending = false;
    if (xwl_surface_ptr->overlay) {
        _xwl_surface_overlay_unmap(xwl_surface_ptr);
        return;
    }
    if (NULL == xwl_surface_ptr->window_ptr ||
        NULL == wlmtk_window_get_workspace(xwl_surface_ptr->window_ptr)) return;

    wlmtk_workspace_unmap_window(
        wlmtk_window_get_workspace(xwl_surface_ptr->window_ptr),
        xwl_surface_ptr->window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Surface committed. Update size. */
void _xwl_surface_handle_surface_commit(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, surface_commit_listener);

    if (xwl_surface_ptr->overlay) {
        _xwl_surface_overlay_update(xwl_surface_ptr);
        return;
    }
    if (NULL == xwl_surface_ptr->window_ptr) return;

    wlmtk_window_commit_size(
        xwl_surface_ptr->window_ptr,
        xwl_surface_ptr->wlr_xwayland_surface_ptr->surface->current.width,
        xwl_surface_ptr->wlr_xwayland_surface_ptr->surface->current.height);

    if (xwl_surface_ptr->map_pending) {
        struct wlr_box size = wlmtk_window_get_size(
            xwl_surface_ptr->window_ptr);
        if (0 < size.width && 0 < size.height) {
            _xwl_surface_map_window(xwl_surface_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Close button got clicked, forward to the XWL surface. */
void _wlmaker_xwl_surface_handle_window_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, window_request_close_listener);

    wlr_xwayland_surface_close(xwl_surface_ptr->wlr_xwayland_surface_ptr);
}

/* ------------------------------------------------------------------------- */
/** Surface became activated. Do that. */
void _wlmaker_xwl_surface_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, window_set_activated_listener);

    wlr_xwayland_surface_activate(
        xwl_surface_ptr->wlr_xwayland_surface_ptr,
        wlmtk_window_is_activated(xwl_surface_ptr->window_ptr));
    wlmtk_surface_set_activated(
        xwl_surface_ptr->surface_ptr,
        wlmtk_window_is_activated(xwl_surface_ptr->window_ptr));
}

/* ------------------------------------------------------------------------- */
/** A new size was requested. Forward to the XWL surface. */
void _wlmaker_xwl_surface_handle_window_request_size(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xwl_surface_t, window_request_size_listener);
    const struct wlr_box *box_ptr = data_ptr;

    wlr_xwayland_surface_configure(
        xwl_surface_ptr->wlr_xwayland_surface_ptr,
        0, 0, box_ptr->width, box_ptr->height);
}

/* ------------------------------------------------------------------------- */
/** The window is requested to go fullscreen. Forward and commit that. */
void _wlmaker_xwl_surface_handle_window_request_fullscreen(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr,
        wlmaker_xwl_surface_t,
        window_request_fullscreen_listener);
    bool *fullscreen_ptr = data_ptr;

    wlr_xwayland_surface_set_fullscreen(
        xwl_surface_ptr->wlr_xwayland_surface_ptr,
         *fullscreen_ptr);
    wlmtk_window_commit_fullscreen(
        xwl_surface_ptr->window_ptr,
        *fullscreen_ptr);

    // TODO(kaeser@gubbe.ch): In windowed mode, there appears something off
    // with XWL drawing fullscreen surfaces. See to report to wlroots.
}

/* ------------------------------------------------------------------------- */
/** The window is requested to go maximized. Forward and commit that. */
void _wlmaker_xwl_surface_handle_window_request_maximized(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xwl_surface_t *xwl_surface_ptr = BS_CONTAINER_OF(
        listener_ptr,
        wlmaker_xwl_surface_t,
        window_request_maximized_listener);
    bool *maximized_ptr = data_ptr;

    wlr_xwayland_surface_set_maximized(
        xwl_surface_ptr->wlr_xwayland_surface_ptr,
        *maximized_ptr,
        *maximized_ptr);
    wlmtk_window_commit_maximized(
        xwl_surface_ptr->window_ptr, *maximized_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Puts a parent-less override-redirect surface into its overlay panel.
 *
 * @param xs_ptr
 *
 * @return true if the surface is such an overlay. Then it is no window.
 */
bool _xwl_surface_overlay_associate(wlmaker_xwl_surface_t *xs_ptr)
{
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    if (!xws_ptr->override_redirect || NULL != xws_ptr->parent) return false;

    if (!xs_ptr->overlay) {
        static const wlmtk_panel_vmt_t vmt = {
            .request_size = _xwl_surface_overlay_request_size,
        };
        wlmtk_panel_positioning_t pos = {
            .anchor = WLR_EDGE_TOP | WLR_EDGE_LEFT,
            .exclusive_zone = -1,
        };
        if (!wlmtk_panel_init(&xs_ptr->overlay_panel, &pos)) {
            bs_log(BS_WARNING, "Failed wlmtk_panel_init for XWL surface %p",
                   xs_ptr);
            return false;
        }
        wlmtk_panel_extend(&xs_ptr->overlay_panel, &vmt);
        static const wlmtk_element_vmt_t element_vmt = {
            .pointer_button = _xwl_surface_overlay_pointer_button,
        };
        xs_ptr->overlay_orig_vmt = wlmtk_element_extend(
            wlmtk_panel_element(&xs_ptr->overlay_panel), &element_vmt);
        wlmtk_element_set_visible(
            wlmtk_panel_element(&xs_ptr->overlay_panel), true);
        xs_ptr->overlay = true;
    }
    wlmtk_element_t *e_ptr = wlmtk_base_element(&xs_ptr->base);
    wlmtk_element_set_position(e_ptr, 0, 0);
    wlmtk_element_set_visible(e_ptr, true);
    wlmtk_container_add_element(&xs_ptr->overlay_panel.super_container, e_ptr);
    bs_log(BS_INFO, "XWL surface %p: Override-redirect, without parent, at "
           "%d, %d: Shown on top, as an overlay.", xs_ptr, xws_ptr->x,
           xws_ptr->y);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Places the overlay where the X11 window is, with its size; and shows it
 * (in the overlay layer, of the output at that position) once it is mapped.
 *
 * @param xs_ptr
 */
void _xwl_surface_overlay_update(wlmaker_xwl_surface_t *xs_ptr)
{
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    if (NULL == xs_ptr->surface_ptr || NULL == xws_ptr->surface) return;
    int w = xws_ptr->surface->current.width;
    int h = xws_ptr->surface->current.height;
    if (!xws_ptr->surface->mapped || 0 >= w || 0 >= h) {
        _xwl_surface_overlay_unmap(xs_ptr);
        return;
    }

    struct wlr_output_layout *layout_ptr =
        xs_ptr->server_ptr->wlr_output_layout_ptr;
    struct wlr_output *output_ptr = wlr_output_layout_output_at(
        layout_ptr, xws_ptr->x, xws_ptr->y);
    if (NULL == output_ptr) {
        output_ptr = wlmaker_server_get_output_at_cursor(xs_ptr->server_ptr);
    }
    if (NULL == output_ptr) return;
    struct wlr_box box;
    wlr_output_layout_get_box(layout_ptr, output_ptr, &box);

    wlmtk_panel_positioning_t pos = {
        .desired_width = w,
        .desired_height = h,
        .anchor = WLR_EDGE_TOP | WLR_EDGE_LEFT,
        .margin_left = xws_ptr->x - box.x,
        .margin_top = xws_ptr->y - box.y,
        .exclusive_zone = -1,
    };
    if (output_ptr != xs_ptr->overlay_output_ptr) {
        _xwl_surface_overlay_unmap(xs_ptr);
    }
    wlmtk_panel_commit(&xs_ptr->overlay_panel, 0, &pos);
    if (NULL == wlmtk_panel_get_layer(&xs_ptr->overlay_panel)) {
        wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
            xs_ptr->server_ptr->desktop_ptr, WLMTK_DESKTOP_LAYER_OVERLAY);
        if (wlmtk_layer_add_panel(
                layer_ptr, &xs_ptr->overlay_panel, output_ptr)) {
            xs_ptr->overlay_output_ptr = output_ptr;
            _xwl_surface_overlay_grab(xs_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * A menu (of a program without a window, eg. a dock-app's) grabs the pointer
 * on X11: A click anywhere else closes it. Here, the X server only learns of
 * clicks on X11 surfaces. So the pointer stays with the menu, and a click
 * outside of it goes to the X11 client that holds the grab (and to no one
 * else), once: As on X11.
 *
 * @param xs_ptr
 */
void _xwl_surface_overlay_grab(wlmaker_xwl_surface_t *xs_ptr)
{
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    if (!wlr_xwayland_surface_has_window_type(
            xws_ptr, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU) &&
        !wlr_xwayland_surface_has_window_type(
            xws_ptr, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) &&
        !wlr_xwayland_surface_has_window_type(
            xws_ptr, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU) &&
        !wlr_xwayland_surface_has_window_type(
            xws_ptr, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO)) return;
    wlmtk_element_t *e_ptr = wlmtk_panel_element(&xs_ptr->overlay_panel);
    if (NULL == e_ptr->parent_container_ptr) return;
    wlmtk_container_pointer_grab(e_ptr->parent_container_ptr, e_ptr);
    xs_ptr->overlay_grab = true;
}

/* ------------------------------------------------------------------------- */
/** Ends the pointer grab of a menu. */
void _xwl_surface_overlay_ungrab(wlmaker_xwl_surface_t *xs_ptr)
{
    if (!xs_ptr->overlay_grab) return;
    xs_ptr->overlay_grab = false;
    wlmtk_element_t *e_ptr = wlmtk_panel_element(&xs_ptr->overlay_panel);
    if (NULL == e_ptr->parent_container_ptr) return;
    wlmtk_container_pointer_grab_release(e_ptr->parent_container_ptr, e_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_button for the overlay. Over
 * the X11 window: As usual. Outside, while a menu holds the pointer: The
 * button goes to that X11 client (at the position outside of its window, as
 * the X server tells a grabbing client), and the grab ends on release.
 *
 * @param element_ptr
 * @param button_event_ptr
 *
 * @return true.
 */
bool _xwl_surface_overlay_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_xwl_surface_t *xs_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_xwl_surface_t,
        overlay_panel.super_container.super_element);
    struct wlr_xwayland_surface *xws_ptr = xs_ptr->wlr_xwayland_surface_ptr;
    struct wlr_cursor *c_ptr = wlmim_wlr_cursor(
        xs_ptr->server_ptr->input_manager_ptr);
    struct wlr_surface *s_ptr = xws_ptr->surface;
    bool inside = NULL != s_ptr &&
        c_ptr->x >= xws_ptr->x && c_ptr->y >= xws_ptr->y &&
        c_ptr->x < xws_ptr->x + s_ptr->current.width &&
        c_ptr->y < xws_ptr->y + s_ptr->current.height;
    if (inside || !xs_ptr->overlay_grab) {
        return xs_ptr->overlay_orig_vmt.pointer_button(
            element_ptr, button_event_ptr);
    }

    struct wlr_seat *seat_ptr = xs_ptr->server_ptr->wlr_seat_ptr;
    if (NULL != s_ptr && (WLMTK_BUTTON_DOWN == button_event_ptr->type ||
                          WLMTK_BUTTON_UP == button_event_ptr->type)) {
        wlr_seat_pointer_notify_enter(
            seat_ptr, s_ptr, c_ptr->x - xws_ptr->x, c_ptr->y - xws_ptr->y);
        wlr_seat_pointer_notify_button(
            seat_ptr, button_event_ptr->time_msec, button_event_ptr->button,
            WLMTK_BUTTON_DOWN == button_event_ptr->type ?
            WL_POINTER_BUTTON_STATE_PRESSED :
            WL_POINTER_BUTTON_STATE_RELEASED);
    }
    if (WLMTK_BUTTON_UP == button_event_ptr->type) {
        _xwl_surface_overlay_ungrab(xs_ptr);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** Takes the overlay off its layer. */
void _xwl_surface_overlay_unmap(wlmaker_xwl_surface_t *xs_ptr)
{
    if (!xs_ptr->overlay) return;
    _xwl_surface_overlay_ungrab(xs_ptr);
    wlmtk_layer_t *layer_ptr = wlmtk_panel_get_layer(&xs_ptr->overlay_panel);
    if (NULL != layer_ptr) {
        wlmtk_layer_remove_panel(layer_ptr, &xs_ptr->overlay_panel);
    }
    xs_ptr->overlay_output_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** The overlay keeps the size the X11 client gave it. */
uint32_t _xwl_surface_overlay_request_size(
    __UNUSED__ wlmtk_panel_t *panel_ptr,
    __UNUSED__ int width,
    __UNUSED__ int height)
{
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Sets whether this window should be server-side-decorated.
 *
 * @param xwl_surface_ptr
 */
void _xwl_surface_apply_decorations(wlmaker_xwl_surface_t *xwl_surface_ptr)
{
    static const xwl_atom_identifier_t borderless_window_types[] = {
        NET_WM_WINDOW_TYPE_TOOLTIP, XWL_MAX_ATOM_ID};

    if (NULL == xwl_surface_ptr->window_ptr) return;

    // TODO(kaeser@gubbe.ch): Adapt whether NO_BORDER or NO_TITLE was set.
    // Override-redirect windows (menus, tooltips of programs without a parent
    // window, eg. the menu of a dock-app) are never decorated, as on X11.
    if (!xwl_surface_ptr->wlr_xwayland_surface_ptr->override_redirect &&
        xwl_surface_ptr->wlr_xwayland_surface_ptr->decorations ==
        WLR_XWAYLAND_SURFACE_DECORATIONS_ALL &&
        !xwl_is_window_type(
            xwl_surface_ptr->xwl_ptr,
            xwl_surface_ptr->wlr_xwayland_surface_ptr,
            borderless_window_types)) {
        wlmtk_window_set_server_side_decorated(
            xwl_surface_ptr->window_ptr,
            true);
    } else {
        wlmtk_window_set_server_side_decorated(
            xwl_surface_ptr->window_ptr,
            false);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Adjusts the absolute position by subtracting each parent's position.
 *
 * @param surface_ptr
 * @param x_ptr
 * @param y_ptr
 */
void _xwl_surface_adjust_absolute_pos(
    wlmaker_xwl_surface_t *surface_ptr,
    int *x_ptr,
    int *y_ptr)
{
    if (NULL == surface_ptr ||
        NULL == surface_ptr->parent_surface_ptr) return;

    wlmtk_element_t *element_ptr = wlmtk_base_element(&surface_ptr->base);
    *x_ptr = *x_ptr - element_ptr->x;
    *y_ptr = *y_ptr - element_ptr->y;
    _xwl_surface_adjust_absolute_pos(
        surface_ptr->parent_surface_ptr, x_ptr, y_ptr);
}
/* == Unit Tests =========================================================== */

static void test_create_destroy(bs_test_t *test_ptr);
static void test_nested(bs_test_t *test_ptr);

static void fake_init_wlr_xwayland_surface(
    struct wlr_xwayland_surface* wlr_xwayland_surface_ptr);

/** Unit test cases. */
static const bs_test_case_t wlmaker_xwl_surface_test_cases[] = {
    { true, "create_destroy", test_create_destroy },
    { true, "nested", test_nested },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_xwl_surface_test_set = BS_TEST_SET(
    true, "xwl_surface", wlmaker_xwl_surface_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests setup and teardown. */
void test_create_destroy(bs_test_t *test_ptr)
{
    wlmaker_server_t server = {};
    struct wlr_xwayland_surface wlr_xwayland_surface;
    fake_init_wlr_xwayland_surface(&wlr_xwayland_surface);

    wlmaker_xwl_surface_t *xwl_surface_ptr = wlmaker_xwl_surface_create(
        &wlr_xwayland_surface, NULL, &server);

    BS_TEST_VERIFY_NEQ(test_ptr, NULL, xwl_surface_ptr);
    wlmaker_xwl_surface_destroy(xwl_surface_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests nesting of XWayland surfaces, ie. parenting. */
void test_nested(bs_test_t *test_ptr)
{
    wlmaker_server_t server = {};

    // FIXME: Make this test something.
    struct wlr_xwayland_surface surface0;
    fake_init_wlr_xwayland_surface(&surface0);
    wlmaker_xwl_surface_t *surface0_ptr = wlmaker_xwl_surface_create(
        &surface0, NULL, &server);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, surface0_ptr);

    struct wlr_xwayland_surface surface1;
    fake_init_wlr_xwayland_surface(&surface1);
    wlmaker_xwl_surface_t *surface1_ptr = wlmaker_xwl_surface_create(
        &surface1, NULL, &server);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, surface1_ptr);

    struct wlr_xwayland_surface surface2;
    fake_init_wlr_xwayland_surface(&surface2);
    wlmaker_xwl_surface_t *surface2_ptr = wlmaker_xwl_surface_create(
        &surface2, NULL, &server);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, surface2_ptr);

    surface2.parent = &surface1;
    wl_signal_emit_mutable(&surface2.events.set_parent, NULL);

    surface2.x = 120;
    surface2.y = 12;
    wl_signal_emit_mutable(&surface2.events.set_geometry, NULL);

    wlmaker_xwl_surface_destroy(surface2_ptr);
    wlmaker_xwl_surface_destroy(surface1_ptr);
    wlmaker_xwl_surface_destroy(surface0_ptr);
}

/* ------------------------------------------------------------------------- */
/** Fake-initializes the `wlr_xwayland_surface_ptr`. */
void fake_init_wlr_xwayland_surface(
    struct wlr_xwayland_surface* wlr_xwayland_surface_ptr)
{
    *wlr_xwayland_surface_ptr = (struct wlr_xwayland_surface){};
    wl_signal_init(&wlr_xwayland_surface_ptr->events.destroy);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.request_configure);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.request_minimize);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.map_request);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.associate);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.dissociate);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.set_title);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.set_class);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.set_parent);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.set_decorations);
    wl_signal_init(&wlr_xwayland_surface_ptr->events.set_geometry);
}

#endif  // defined(WLMAKER_HAVE_XWAYLAND)
/* == End of xwl_surface.c ================================================= */
