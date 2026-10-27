/* ========================================================================= */
/**
 * @file launcher.c
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

#include "launcher.h"

#include <cairo.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <libbase/signal.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "toplevel.h"
#include "toolkit/toolkit.h"

struct wlm_util_subprocess;

/* == Declarations ========================================================= */

/** State of a launcher. */
struct _wlmdock_launcher_t {
    /** The launcher is derived from a @ref wlmtk_tile_t. */
    wlmtk_tile_t              super_tile;
    /** Original virtual method table fo the element. */
    wlmtk_element_vmt_t       orig_element_vmt;

    /** Image element. One element of @ref wlmdock_launcher_t::super_tile. */
    wlmtk_image_t             *image_ptr;
    /** Overlay element. Atop on the tile. */
    wlmtk_buffer_t            overlay_buffer;
    /** Whether the app is considered running. */
    bool                      running;

    /** Tracker for toplevels. */
    struct wlmdock_toplevel_tracker *tracker_ptr;
    /** Subprocess monitor to register launched processes to. */
    wlm_util_subprocess_monitor_t *monitor_ptr;

    /** Application ID associated with the launched process. */
    char                      *app_id_ptr;
    /** Commandline to launch the associated application. */
    char                      *cmdline_ptr;
    /** Path to the icon. */
    char                      *icon_path_ptr;

    /** Resolved icon path. */
    char                      *resolved_icon_path_ptr;

    /** Subprocesses that were created by this launcher. */
    bs_ptr_set_t              *subprocesses_ptr;

    /** Listener for @ref wlmdock_toplevel_tracker_events::app_id_changed. */
    struct bs_listener        app_id_changed_listener;

    /** The plist dict the launcher was created from. */
    bspl_dict_t               *dict_ptr;
    /** Whether launching: Shown dimmed, until the application runs. */
    bool                      launching;
    /** Ends @ref wlmdock_launcher_t::launching, if it takes too long. */
    struct wl_event_source    *launching_timer_ptr;
    /** Time of the last click, for telling double-clicks. 0 if none. */
    uint32_t                  last_click_msec;
    /** `AutoLaunch` of the plist: Launch when starting. */
    char                      *autolaunch_ptr;
    /** `Drawer` of the plist: The tile of a drawer, opened by wlmaker. */
    char                      *drawer_ptr;
    /** Handler for a right-click. */
    wlmdock_launcher_menu_handler_t menu_handler;
    /** Argument for @ref wlmdock_launcher_t::menu_handler. */
    void                      *menu_handler_ud_ptr;
};

/** Plist descroptor for a launcher. */
static const bspl_desc_t _wlmdock_launcher_plist_desc[] = {
    // Optional: Without an AppID, the launcher only shows as running while
    // processes it launched itself are alive. Keeps State.plist files that
    // were written for the compiled-in dock (no AppID) working.
    BSPL_DESC_STRING(
        "AppID", false, wlmdock_launcher_t, app_id_ptr, app_id_ptr, ""),
    BSPL_DESC_STRING(
        "CommandLine", true, wlmdock_launcher_t, cmdline_ptr, cmdline_ptr, ""),
    BSPL_DESC_STRING(
        "Icon", true, wlmdock_launcher_t, icon_path_ptr, icon_path_ptr, ""),
    // Window Maker's "Start when Window Maker is started".
    BSPL_DESC_STRING(
        "AutoLaunch", false, wlmdock_launcher_t, autolaunch_ptr,
        autolaunch_ptr, "No"),
    // Window Maker's drawers: The compositor shows the drawer's icons.
    BSPL_DESC_STRING(
        "Drawer", false, wlmdock_launcher_t, drawer_ptr, drawer_ptr, "No"),
    BSPL_DESC_SENTINEL(),
};

/** Window Maker's `SingleClickLaunch`: No, launch on a double-click. */
static bool _wlmdock_launcher_single_click = false;
/** Maximum time between the clicks of a double-click. */
static uint32_t _wlmdock_launcher_double_click_msec = 300;
/** Whether drawers stick out to the left (the Dock at the right edge). */
static bool _wlmdock_launcher_drawer_left = true;
/** How long a launcher shows as launching, at most. */
static const int _wlmdock_launcher_launching_msec = 10000;

static void _wlmdock_launcher_update_overlay(wlmdock_launcher_t *launcher_ptr);
static struct wlr_buffer *_wlmdock_launcher_create_overlay_buffer(
    wlmdock_launcher_t *launcher_ptr);

static void _wlmdock_launcher_element_destroy(
    wlmtk_element_t *element_ptr);
static bool _wlmdock_launcher_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);

static void _wlmdock_launcher_start(wlmdock_launcher_t *launcher_ptr);
static int _wlmdock_launcher_handle_launching_timer(void *data_ptr);
static void _wlmdock_launcher_draw_dots(cairo_t *cairo_ptr, int size);
static bool _wlmdock_launcher_is_drawer(wlmdock_launcher_t *launcher_ptr);
static void _wlmdock_launcher_draw_drawer_arrow(cairo_t *cairo_ptr, int size);

static void _wlmdock_launcher_handle_terminated(
    void *userdata_ptr,
    struct wlm_util_subprocess *subprocess_handle_ptr,
    int state,
    int code);
static void _wlmdock_launcher_handle_app_id_changed(
    struct bs_listener *listener_ptr,
    void *userdata_ptr);

static bool _wlmdock_launcher_set_content_size(
    wlmtk_tile_t *tile_ptr,
    uint64_t content_size);

/* == Data ================================================================= */

/** The launcher's extension to @ref wlmtk_element_t virtual method table. */
static const wlmtk_element_vmt_t _wlmdock_launcher_element_vmt = {
    .destroy = _wlmdock_launcher_element_destroy,
    .pointer_button = _wlmdock_launcher_pointer_button,
};

/** Launcher's extension to @ref wlmtk_tile_t virtual method table. */
static const wlmtk_tile_vmt_t _wlmdock_launcher_tile_vmt = {
    .set_content_size = _wlmdock_launcher_set_content_size,
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmdock_launcher_t *wlmdock_launcher_create_from_plist(
    const struct wlmtk_tile_style *style_ptr,
    bspl_dict_t *dict_ptr,
    __UNUSED__ struct wlmdock_toplevel_tracker *tracker_ptr,
    wlm_util_subprocess_monitor_t *monitor_ptr,
    wlm_util_files_t *files_ptr)
{
    wlmdock_launcher_t *launcher_ptr = logged_calloc(
        1, sizeof(wlmdock_launcher_t));
    if (NULL == launcher_ptr) return NULL;
    launcher_ptr->monitor_ptr = monitor_ptr;

    if (!bspl_decode_dict(
            dict_ptr, _wlmdock_launcher_plist_desc, launcher_ptr)) {
        bs_log(BS_ERROR, "Failed to create launcher from plist dict.");
        wlmdock_launcher_destroy(launcher_ptr);
        return NULL;
    }
    launcher_ptr->dict_ptr = bspl_dict_ref(dict_ptr);

     if (!wlmtk_tile_init(&launcher_ptr->super_tile, style_ptr)) {
         return NULL;
    }
    launcher_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&launcher_ptr->super_tile),
        &_wlmdock_launcher_element_vmt);
    wlmtk_tile_extend(&launcher_ptr->super_tile, &_wlmdock_launcher_tile_vmt);
    wlmtk_element_set_visible(
        wlmtk_tile_element(&launcher_ptr->super_tile), true);

    launcher_ptr->tracker_ptr = tracker_ptr;
    bs_listener_connect(
        &launcher_ptr->app_id_changed_listener,
        _wlmdock_launcher_handle_app_id_changed,
        &wlmdock_toplevel_tracker_events(tracker_ptr)->app_id_changed);

    launcher_ptr->subprocesses_ptr = bs_ptr_set_create();
    if (NULL == launcher_ptr->subprocesses_ptr) {
        wlmdock_launcher_destroy(launcher_ptr);
        return NULL;
    }

    if (!wlmtk_buffer_init(&launcher_ptr->overlay_buffer)) {
        wlmdock_launcher_destroy(launcher_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&launcher_ptr->overlay_buffer), true);
    _wlmdock_launcher_update_overlay(launcher_ptr);
    wlmtk_tile_set_overlay(
        &launcher_ptr->super_tile,
        wlmtk_buffer_element(&launcher_ptr->overlay_buffer));

    // Resolves to a full path, and verifies the icon file exists.
    if ('/' == launcher_ptr->icon_path_ptr[0] ||
        '~' == launcher_ptr->icon_path_ptr[0]) {
        launcher_ptr->resolved_icon_path_ptr = bs_file_resolve_path(
            launcher_ptr->icon_path_ptr, NULL);
        if (NULL == launcher_ptr->resolved_icon_path_ptr) {
            bs_log(BS_ERROR, "Failed bs_file_resolve_path(\"%s\", NULL)",
                   launcher_ptr->icon_path_ptr);
            wlmdock_launcher_destroy(launcher_ptr);
            return NULL;
        }
    } else {
        char *p = bs_strdupf("icons/%s", launcher_ptr->icon_path_ptr);
        if (NULL == p) {
            bs_log(BS_ERROR | BS_ERRNO, "Failed bs_strdupf(\"icons/%s\")",
                   launcher_ptr->icon_path_ptr);
            wlmdock_launcher_destroy(launcher_ptr);
            return NULL;
        }
        launcher_ptr->resolved_icon_path_ptr = wlm_util_files_xdg_data_find(
            files_ptr, p, S_IFREG);
        free(p);
        if (NULL == launcher_ptr->resolved_icon_path_ptr) {
            bs_log(BS_ERROR,
                   "Failed to locate \"icons/%s\" in ${XDG_DATA_DIRS}/wlmaker",
                   launcher_ptr->icon_path_ptr);
#ifndef WLMAKER_SOURCE_DIR
            wlmdock_launcher_destroy(launcher_ptr);
            return NULL;
#else
            launcher_ptr->resolved_icon_path_ptr = bs_strdupf(
                WLMAKER_SOURCE_DIR "/share/wlmaker/icons/%s",
                launcher_ptr->icon_path_ptr);
            if (NULL == launcher_ptr->resolved_icon_path_ptr) {
                wlmdock_launcher_destroy(launcher_ptr);
                return NULL;
            }
#endif
        }
    }
    launcher_ptr->image_ptr = wlmtk_image_create_scaled(
        launcher_ptr->resolved_icon_path_ptr,
        launcher_ptr->super_tile.style.content_size,
        launcher_ptr->super_tile.style.content_size);
    if (NULL == launcher_ptr->image_ptr) {
        wlmdock_launcher_destroy(launcher_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_image_element(launcher_ptr->image_ptr), true);
    wlmtk_tile_set_content(
        &launcher_ptr->super_tile,
        wlmtk_image_element(launcher_ptr->image_ptr));

    wlmtk_element_set_visible(
        wlmtk_tile_element(&launcher_ptr->super_tile), true);

    return launcher_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_destroy(wlmdock_launcher_t *launcher_ptr)
{
    if (NULL != launcher_ptr->image_ptr) {
        wlmtk_tile_set_content(&launcher_ptr->super_tile, NULL);
        wlmtk_image_destroy(launcher_ptr->image_ptr);
        launcher_ptr->image_ptr = NULL;
    }

    wlmtk_tile_set_overlay(&launcher_ptr->super_tile, NULL);
    wlmtk_buffer_fini(&launcher_ptr->overlay_buffer);

    if (NULL != launcher_ptr->subprocesses_ptr) {
        struct wlm_util_subprocess *subprocess_handle_ptr;
        while (NULL != (subprocess_handle_ptr = bs_ptr_set_any(
                            launcher_ptr->subprocesses_ptr))) {
            wlm_util_subprocess_monitor_cede(
                launcher_ptr->monitor_ptr,
                subprocess_handle_ptr);
            bs_ptr_set_erase(launcher_ptr->subprocesses_ptr,
                             subprocess_handle_ptr);
        }
        bs_ptr_set_destroy(launcher_ptr->subprocesses_ptr);
        launcher_ptr->subprocesses_ptr = NULL;
    }

    if (NULL != launcher_ptr->launching_timer_ptr) {
        wl_event_source_remove(launcher_ptr->launching_timer_ptr);
        launcher_ptr->launching_timer_ptr = NULL;
    }
    if (NULL != launcher_ptr->autolaunch_ptr) {
        free(launcher_ptr->autolaunch_ptr);
        launcher_ptr->autolaunch_ptr = NULL;
    }
    if (NULL != launcher_ptr->drawer_ptr) {
        free(launcher_ptr->drawer_ptr);
        launcher_ptr->drawer_ptr = NULL;
    }
    if (NULL != launcher_ptr->app_id_ptr) {
        free(launcher_ptr->app_id_ptr);
        launcher_ptr->app_id_ptr = NULL;
    }

    if (NULL != launcher_ptr->resolved_icon_path_ptr) {
        free(launcher_ptr->resolved_icon_path_ptr);
        launcher_ptr->resolved_icon_path_ptr = NULL;
    }

    if (NULL != launcher_ptr->cmdline_ptr) {
        free(launcher_ptr->cmdline_ptr);
        launcher_ptr->cmdline_ptr = NULL;
    }

    if (NULL != launcher_ptr->icon_path_ptr) {
        free(launcher_ptr->icon_path_ptr);
        launcher_ptr->icon_path_ptr = NULL;
    }

    if (NULL != launcher_ptr->tracker_ptr) {
        bs_listener_disconnect(
            &launcher_ptr->app_id_changed_listener,
            &wlmdock_toplevel_tracker_events(
                launcher_ptr->tracker_ptr)->app_id_changed);
        launcher_ptr->tracker_ptr = NULL;
    }

    if (NULL != launcher_ptr->dict_ptr) {
        bspl_dict_unref(launcher_ptr->dict_ptr);
        launcher_ptr->dict_ptr = NULL;
    }

    wlmtk_tile_fini(&launcher_ptr->super_tile);
    free(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
wlmtk_tile_t *wlmdock_launcher_tile(wlmdock_launcher_t *launcher_ptr)
{
    return &launcher_ptr->super_tile;
}

/* ------------------------------------------------------------------------- */
wlmdock_launcher_t *wlmdock_launcher_from_tile(wlmtk_tile_t *tile_ptr)
{
    if (wlmtk_tile_element(tile_ptr)->vmt.destroy !=
        _wlmdock_launcher_element_destroy) return NULL;
    return BS_CONTAINER_OF(tile_ptr, wlmdock_launcher_t, super_tile);
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_set_menu_handler(
    wlmdock_launcher_t *launcher_ptr,
    wlmdock_launcher_menu_handler_t handler,
    void *ud_ptr)
{
    launcher_ptr->menu_handler = handler;
    launcher_ptr->menu_handler_ud_ptr = ud_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_set_click_mode(
    bool single_click,
    uint32_t double_click_msec)
{
    _wlmdock_launcher_single_click = single_click;
    _wlmdock_launcher_double_click_msec =
        0 < double_click_msec ? double_click_msec : 300;
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_set_drawer_side(bool left)
{
    _wlmdock_launcher_drawer_left = left;
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_autolaunch(wlmdock_launcher_t *launcher_ptr)
{
    if (_wlmdock_launcher_is_drawer(launcher_ptr)) return;
    const char *v = launcher_ptr->autolaunch_ptr;
    if (NULL == v || (0 != strcasecmp(v, "Yes") && 0 != strcasecmp(v, "True"))) {
        return;
    }
    if (launcher_ptr->running) return;
    wlmdock_launcher_launch(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_launch(wlmdock_launcher_t *launcher_ptr)
{
    _wlmdock_launcher_start(launcher_ptr);
    // Shows as launching (only with an AppID, to tell when it runs).
    if (NULL != launcher_ptr->app_id_ptr && '\0' != *launcher_ptr->app_id_ptr &&
        !launcher_ptr->running && NULL != launcher_ptr->monitor_ptr) {
        launcher_ptr->launching = true;
        if (NULL == launcher_ptr->launching_timer_ptr) {
            launcher_ptr->launching_timer_ptr = wl_event_loop_add_timer(
                wlm_util_subprocess_monitor_event_loop(
                    launcher_ptr->monitor_ptr),
                _wlmdock_launcher_handle_launching_timer,
                launcher_ptr);
        }
        if (NULL != launcher_ptr->launching_timer_ptr) {
            wl_event_source_timer_update(launcher_ptr->launching_timer_ptr,
                                         _wlmdock_launcher_launching_msec);
        }
    }
    _wlmdock_launcher_update_overlay(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
/** Timer: Launching took too long. Shows the launcher as before. */
int _wlmdock_launcher_handle_launching_timer(void *data_ptr)
{
    wlmdock_launcher_t *launcher_ptr = data_ptr;
    launcher_ptr->launching = false;
    _wlmdock_launcher_update_overlay(launcher_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
bool wlmdock_launcher_has_processes(wlmdock_launcher_t *launcher_ptr)
{
    return NULL != bs_ptr_set_any(launcher_ptr->subprocesses_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmdock_launcher_kill(wlmdock_launcher_t *launcher_ptr)
{
    // The set has no iterator: Move all over to `others`, and back.
    bs_ptr_set_t *others_ptr = bs_ptr_set_create();
    if (NULL == others_ptr) return;
    struct wlm_util_subprocess *sp_ptr;
    while (NULL != (sp_ptr = bs_ptr_set_any(launcher_ptr->subprocesses_ptr))) {
        pid_t pid = wlm_util_subprocess_pid(sp_ptr);
        if (1 < pid && 0 != kill(pid, SIGTERM)) {
            bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%d, SIGTERM)", (int)pid);
        }
        bs_ptr_set_erase(launcher_ptr->subprocesses_ptr, sp_ptr);
        bs_ptr_set_insert(others_ptr, sp_ptr);
    }
    while (NULL != (sp_ptr = bs_ptr_set_any(others_ptr))) {
        bs_ptr_set_erase(others_ptr, sp_ptr);
        bs_ptr_set_insert(launcher_ptr->subprocesses_ptr, sp_ptr);
    }
    bs_ptr_set_destroy(others_ptr);
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmdock_launcher_dict(wlmdock_launcher_t *launcher_ptr)
{
    return launcher_ptr->dict_ptr;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Redraws the overlay element. */
void _wlmdock_launcher_update_overlay(wlmdock_launcher_t *launcher_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = _wlmdock_launcher_create_overlay_buffer(
        launcher_ptr);
    if (NULL == wlr_buffer_ptr) return;
    wlmtk_buffer_set(&launcher_ptr->overlay_buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Creates an overlay wlr_buffer. */
struct wlr_buffer *_wlmdock_launcher_create_overlay_buffer(
    wlmdock_launcher_t *launcher_ptr)
{
    int s = launcher_ptr->super_tile.style.size;
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(s, s);
    if (NULL == wlr_buffer_ptr) return NULL;

    // A drawer: Window Maker's arrow, pointing to where it opens.
    if (_wlmdock_launcher_is_drawer(launcher_ptr)) {
        launcher_ptr->running = false;
        cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
        if (NULL != cairo_ptr) {
            _wlmdock_launcher_draw_drawer_arrow(cairo_ptr, s);
            cairo_destroy(cairo_ptr);
        }
        return wlr_buffer_ptr;
    }

    bs_dllist_t *toplevel_handles = NULL;
    if (NULL != launcher_ptr->app_id_ptr && '\0' != *launcher_ptr->app_id_ptr) {
        toplevel_handles = wlmdock_toplevel_tracker_app_id_handles(
            launcher_ptr->tracker_ptr, launcher_ptr->app_id_ptr);
    }
    launcher_ptr->running =
        (NULL != toplevel_handles && !bs_dllist_empty(toplevel_handles)) ||
        // Without an AppID: While the processes it launched are alive.
        ((NULL == launcher_ptr->app_id_ptr || '\0' == *launcher_ptr->app_id_ptr) &&
         !bs_ptr_set_empty(launcher_ptr->subprocesses_ptr));
    // With an AppID, launching lasts until a window shows up.
    if (launcher_ptr->running) launcher_ptr->launching = false;
    if (launcher_ptr->running && !launcher_ptr->launching) return wlr_buffer_ptr;

    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }
    // As Window Maker: Three dots while the application is not running.
    if (!launcher_ptr->running) _wlmdock_launcher_draw_dots(cairo_ptr, s);
    // And stippled while launching.
    if (launcher_ptr->launching) {
        bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(wlr_buffer_ptr);
        cairo_surface_flush(cairo_get_target(cairo_ptr));
        for (unsigned y = 0; y < g_ptr->height; ++y) {
            for (unsigned x = y % 2; x < g_ptr->width; x += 2) {
                g_ptr->data_ptr[y * g_ptr->width + x] = 0xff000000;
            }
        }
        cairo_surface_mark_dirty(cairo_get_target(cairo_ptr));
    }
    cairo_destroy(cairo_ptr);
    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
/** @return Whether the launcher is a drawer's tile. */
bool _wlmdock_launcher_is_drawer(wlmdock_launcher_t *launcher_ptr)
{
    const char *v = launcher_ptr->drawer_ptr;
    return NULL != v &&
        (0 == strcasecmp(v, "Yes") || 0 == strcasecmp(v, "True"));
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the arrow of Window Maker's drawer tile (wDrawerIconPaint): A small
 * triangle at the middle of the side the drawer opens to.
 */
void _wlmdock_launcher_draw_drawer_arrow(cairo_t *cairo_ptr, int size)
{
    double m = size / 2.0;
    double tip = _wlmdock_launcher_drawer_left ? 5 : size - 1 - 5;
    double base = _wlmdock_launcher_drawer_left ? 10 : size - 1 - 10;
    cairo_save(cairo_ptr);
    cairo_set_source_rgb(cairo_ptr, 0, 0, 0);
    cairo_move_to(cairo_ptr, base + 0.5, m - 5);
    cairo_line_to(cairo_ptr, base + 0.5, m + 5);
    cairo_line_to(cairo_ptr, tip + 0.5, m);
    cairo_close_path(cairo_ptr);
    cairo_fill(cairo_ptr);
    cairo_restore(cairo_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws Window Maker's three dots (make3Dots) at the lower left of a tile of
 * `size`: Each 3 by 2 pixels, black and white.
 */
void _wlmdock_launcher_draw_dots(cairo_t *cairo_ptr, int size)
{
    static const int xs[3] = { 4, 9, 14 };
    cairo_save(cairo_ptr);
    cairo_set_antialias(cairo_ptr, CAIRO_ANTIALIAS_NONE);
    for (int i = 0; i < 3; ++i) {
        int x = xs[i], y = size - 6;
        cairo_set_source_rgb(cairo_ptr, 0, 0, 0);
        cairo_rectangle(cairo_ptr, x, y, 2, 1);
        cairo_rectangle(cairo_ptr, x, y + 1, 1, 1);
        cairo_fill(cairo_ptr);
        cairo_set_source_rgb(cairo_ptr, 1, 1, 1);
        cairo_rectangle(cairo_ptr, x + 2, y, 1, 2);
        cairo_rectangle(cairo_ptr, x + 1, y + 1, 1, 1);
        cairo_fill(cairo_ptr);
    }
    cairo_restore(cairo_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::destroy. Calls
 * @ref wlmdock_launcher_destroy.
 *
 * @param element_ptr
 */
void _wlmdock_launcher_element_destroy(
    wlmtk_element_t *element_ptr)
{
    wlmdock_launcher_t *launcher_ptr = BS_CONTAINER_OF(
        element_ptr, wlmdock_launcher_t,
        super_tile.super_container.super_element);
    wlmdock_launcher_destroy(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_button.
 *
 * @param element_ptr
 * @param button_event_ptr
 *
 * @return true always.
 */
bool _wlmdock_launcher_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmdock_launcher_t *launcher_ptr = BS_CONTAINER_OF(
        element_ptr, wlmdock_launcher_t,
        super_tile.super_container.super_element);

    if (BTN_RIGHT == button_event_ptr->button &&
        WLMTK_BUTTON_DOWN == button_event_ptr->type &&
        NULL != launcher_ptr->menu_handler) {
        launcher_ptr->menu_handler(launcher_ptr,
                                   launcher_ptr->menu_handler_ud_ptr);
        return true;
    }

    if (BTN_LEFT != button_event_ptr->button) return true;
    if (WLMTK_BUTTON_CLICK != button_event_ptr->type) return true;
    // A drawer's tile: The compositor opens and closes it.
    if (_wlmdock_launcher_is_drawer(launcher_ptr)) return true;

    // Window Maker launches on a double-click, unless SingleClickLaunch.
    if (!_wlmdock_launcher_single_click) {
        uint32_t t = button_event_ptr->time_msec;
        bool double_click = 0 != launcher_ptr->last_click_msec &&
            t - launcher_ptr->last_click_msec <=
            _wlmdock_launcher_double_click_msec;
        launcher_ptr->last_click_msec = double_click ? 0 : (t ? t : 1);
        if (!double_click) return true;
        // Running: The compositor shows it (Window Maker: unhides it).
        if (launcher_ptr->running) return true;
    }
    wlmdock_launcher_launch(launcher_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Starts the application, called when the launcher is clicked.
 *
 * @param launcher_ptr
 */
void _wlmdock_launcher_start(wlmdock_launcher_t *launcher_ptr)
{
    bs_subprocess_t *subprocess_ptr = bs_subprocess_create_cmdline(
        launcher_ptr->cmdline_ptr);
    if (NULL == subprocess_ptr) {
        bs_log(BS_ERROR, "Failed bs_subprocess_create_cmdline(%s)",
               launcher_ptr->cmdline_ptr);
        return;
    }

    if (!bs_subprocess_start(subprocess_ptr)) {
        bs_log(BS_ERROR, "Failed bs_subprocess_start for %s",
               launcher_ptr->cmdline_ptr);
        bs_subprocess_destroy(subprocess_ptr);
        return;
    }

    struct wlm_util_subprocess *subprocess_handle_ptr;
    subprocess_handle_ptr = wlm_util_subprocess_monitor_entrust(
        launcher_ptr->monitor_ptr,
        subprocess_ptr,
        _wlmdock_launcher_handle_terminated,
        launcher_ptr,
        NULL);

    if (!bs_ptr_set_insert(launcher_ptr->subprocesses_ptr,
                           subprocess_handle_ptr)) {
        bs_log(BS_WARNING, "Launcher %p: Failed bs_ptr_set_insert(%p, %p). "
               "Will not show status of subprocess in App.",
               launcher_ptr, launcher_ptr->subprocesses_ptr,
               subprocess_handle_ptr);
        wlm_util_subprocess_monitor_cede(
            launcher_ptr->monitor_ptr,
            subprocess_handle_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Callback handler for when the registered subprocess terminates.
 *
 * @param userdata_ptr        Points to @ref wlmdock_launcher_t.
 * @param subprocess_handle_ptr
 * @param exit_status
 * @param signal_number
 */
void _wlmdock_launcher_handle_terminated(
    void *userdata_ptr,
    __UNUSED__ struct wlm_util_subprocess *subprocess_handle_ptr,
    int exit_status,
    int signal_number)
{
    wlmdock_launcher_t *launcher_ptr = userdata_ptr;
    const char *format_ptr;
    int code;

    if (0 == signal_number) {
        format_ptr = "App '%s' (%p) terminated, status code %d.";
        code = exit_status;
    } else {
        format_ptr = "App '%s' (%p) killed by signal %d.";
        code = signal_number;
    }

    bs_log(BS_INFO, format_ptr,
           launcher_ptr->cmdline_ptr,
           launcher_ptr,
           code);
    // TODO(kaeser@gubbe.ch): Keep exit status and latest output available
    // for visualization.
    wlm_util_subprocess_monitor_cede(
        launcher_ptr->monitor_ptr,
        subprocess_handle_ptr);
    bs_ptr_set_erase(launcher_ptr->subprocesses_ptr,
                     subprocess_handle_ptr);

    _wlmdock_launcher_update_overlay(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmdock_toplevel_tracker_events::app_id_changed. */
void _wlmdock_launcher_handle_app_id_changed(
    struct bs_listener *listener_ptr,
    void *userdata_ptr)
{
    wlmdock_launcher_t *launcher_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmdock_launcher_t, app_id_changed_listener);
    const char *app_id_ptr = userdata_ptr;

    if (NULL == app_id_ptr ||
        NULL == launcher_ptr->app_id_ptr ||
        '\0' == *launcher_ptr->app_id_ptr ||
        0 != strcmp(app_id_ptr, launcher_ptr->app_id_ptr)) return;

    _wlmdock_launcher_update_overlay(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_tile_vmt_t::set_content_size. Set the image size. */
bool _wlmdock_launcher_set_content_size(
    wlmtk_tile_t *tile_ptr,
    uint64_t content_size)
{
    wlmdock_launcher_t *launcher_ptr = BS_CONTAINER_OF(
        tile_ptr, wlmdock_launcher_t, super_tile);

    wlmtk_image_t *image_ptr = wlmtk_image_create_scaled(
        launcher_ptr->resolved_icon_path_ptr, content_size, content_size);
    if (NULL == image_ptr) return false;

    wlmtk_element_set_visible(wlmtk_image_element(image_ptr), true);
    wlmtk_tile_set_content(tile_ptr, wlmtk_image_element(image_ptr));
    wlmtk_image_destroy(launcher_ptr->image_ptr);
    launcher_ptr->image_ptr = image_ptr;
    return true;
}

/* == Unit tests =========================================================== */

static void test_create_from_plist(bs_test_t *test_ptr);
static void test_create_without_app_id(bs_test_t *test_ptr);
static void test_drawer(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t wlmdock_launcher_test_cases[] = {
    { true, "create_from_plist", test_create_from_plist },
    { true, "create_without_app_id", test_create_without_app_id },
    { true, "drawer", test_drawer },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmdock_launcher_test_set = BS_TEST_SET(
    true, "launcher", wlmdock_launcher_test_cases);

/* ------------------------------------------------------------------------- */
/** Exercises plist parser. */
void test_create_from_plist(bs_test_t *test_ptr)
{
    static const struct wlmtk_tile_style style = { .size = 96 };
    static const char *plist_ptr =
        "{AppID=\"id\"; CommandLine=\"a\"; Icon=\"chrome-56x56.png\";}";

    bs_test_setenv(test_ptr, "XDG_DATA_DIRS", WLMAKER_SOURCE_DIR "/share");

    bspl_dict_t *dict_ptr = bspl_dict_from_object(
        bspl_create_object_from_plist_string(plist_ptr));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dict_ptr);
    struct wlmdock_toplevel_tracker *trk = wlmdock_toplevel_tracker_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, trk);
    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, files_ptr);

    wlmdock_launcher_t *launcher_ptr = wlmdock_launcher_create_from_plist(
        &style, dict_ptr, trk, NULL, files_ptr);
    bspl_dict_unref(dict_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, launcher_ptr);

    BS_TEST_VERIFY_STREQ(test_ptr, "id", launcher_ptr->app_id_ptr);
    BS_TEST_VERIFY_STREQ(test_ptr, "a", launcher_ptr->cmdline_ptr);
    BS_TEST_VERIFY_STREQ(
        test_ptr, "chrome-56x56.png", launcher_ptr->icon_path_ptr);

    BS_TEST_VERIFY_FALSE(test_ptr, launcher_ptr->running);
    // Not running: Window Maker's dots, at the lower left.
    struct wlr_buffer *o_ptr = _wlmdock_launcher_create_overlay_buffer(
        launcher_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o_ptr);
    bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(o_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 0xff000000, g_ptr->data_ptr[90 * 96 + 4]);
    BS_TEST_VERIFY_EQ(test_ptr, 0xffffffff, g_ptr->data_ptr[90 * 96 + 6]);
    BS_TEST_VERIFY_EQ(test_ptr, 0, g_ptr->data_ptr[40 * 96 + 40]);
    wlr_buffer_drop(o_ptr);

    // Toplevel with the configured app ID. Must now show running.
    struct wlmdock_toplevel_handle *h = wlmdock_toplevel_handle_create(trk);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, h);
    wlmdock_toplevel_handle_set_app_id(h, "id");
    BS_TEST_VERIFY_TRUE(test_ptr, launcher_ptr->running);
    // Running: No dots.
    o_ptr = _wlmdock_launcher_create_overlay_buffer(launcher_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o_ptr);
    g_ptr = bs_gfxbuf_from_wlr_buffer(o_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 0, g_ptr->data_ptr[90 * 96 + 4]);
    wlr_buffer_drop(o_ptr);

    // Toplevel changes app ID. No longer running.
    wlmdock_toplevel_handle_set_app_id(h, "other-id");
    BS_TEST_VERIFY_FALSE(test_ptr, launcher_ptr->running);

    wlmdock_launcher_destroy(launcher_ptr);
    wlmdock_toplevel_tracker_destroy(trk);
    wlm_util_files_destroy(files_ptr);
}

/* ------------------------------------------------------------------------- */
/** A launcher without AppID loads, and toplevels never mark it as running. */
void test_create_without_app_id(bs_test_t *test_ptr)
{
    static const struct wlmtk_tile_style style = { .size = 96 };
    static const char *plist_ptr =
        "{CommandLine=\"a\"; Icon=\"chrome-56x56.png\";}";

    bs_test_setenv(test_ptr, "XDG_DATA_DIRS", WLMAKER_SOURCE_DIR "/share");

    bspl_dict_t *dict_ptr = bspl_dict_from_object(
        bspl_create_object_from_plist_string(plist_ptr));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dict_ptr);
    struct wlmdock_toplevel_tracker *trk = wlmdock_toplevel_tracker_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, trk);
    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, files_ptr);

    wlmdock_launcher_t *launcher_ptr = wlmdock_launcher_create_from_plist(
        &style, dict_ptr, trk, NULL, files_ptr);
    bspl_dict_unref(dict_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, launcher_ptr);
    BS_TEST_VERIFY_STREQ(test_ptr, "a", launcher_ptr->cmdline_ptr);
    BS_TEST_VERIFY_FALSE(test_ptr, launcher_ptr->running);

    // A toplevel without app ID must not match the empty AppID.
    struct wlmdock_toplevel_handle *h = wlmdock_toplevel_handle_create(trk);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, h);
    wlmdock_toplevel_handle_set_app_id(h, "");
    BS_TEST_VERIFY_FALSE(test_ptr, launcher_ptr->running);
    wlmdock_toplevel_handle_set_app_id(h, "id");
    BS_TEST_VERIFY_FALSE(test_ptr, launcher_ptr->running);

    wlmdock_launcher_destroy(launcher_ptr);
    wlmdock_toplevel_tracker_destroy(trk);
    wlm_util_files_destroy(files_ptr);
}

/* ------------------------------------------------------------------------- */
/** A drawer's tile: The arrow, no dots, never launching. */
void test_drawer(bs_test_t *test_ptr)
{
    static const struct wlmtk_tile_style style = { .size = 64 };
    bs_test_setenv(test_ptr, "XDG_DATA_DIRS", WLMAKER_SOURCE_DIR "/share");
    bspl_dict_t *dict_ptr = bspl_dict_from_object(
        bspl_create_object_from_plist_string(
            "{Drawer = Yes; CommandLine = \"\"; Icon = \"dock-56x56.png\"; "
            "Launchers = ();}"));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dict_ptr);
    struct wlmdock_toplevel_tracker *trk = wlmdock_toplevel_tracker_create();
    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    wlmdock_launcher_t *launcher_ptr = wlmdock_launcher_create_from_plist(
        &style, dict_ptr, trk, NULL, files_ptr);
    bspl_dict_unref(dict_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, launcher_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmdock_launcher_is_drawer(launcher_ptr));

    wlmdock_launcher_set_drawer_side(true);
    struct wlr_buffer *o_ptr = _wlmdock_launcher_create_overlay_buffer(
        launcher_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o_ptr);
    bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(o_ptr);
    // Arrow at the left, in the middle; no dots at the lower left.
    BS_TEST_VERIFY_EQ(test_ptr, 0xff000000, g_ptr->data_ptr[32 * 64 + 9]);
    BS_TEST_VERIFY_EQ(test_ptr, 0, g_ptr->data_ptr[58 * 64 + 4]);
    wlr_buffer_drop(o_ptr);

    wlmdock_launcher_destroy(launcher_ptr);
    wlmdock_toplevel_tracker_destroy(trk);
    wlm_util_files_destroy(files_ptr);
}

/* == End of launcher.c ==================================================== */
