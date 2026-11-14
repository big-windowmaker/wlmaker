/* ========================================================================= */
/**
 * @file task_list.c
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

/// for PATH_MAX.
#define _POSIX_C_SOURCE 1

#include "task_list.h"

#include <cairo.h>
#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wayland-server-core.h>
/// Include unstable interfaces of wlroots.
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_output.h>
#include <wlr/util/edges.h>
#include <wlr/interfaces/wlr_buffer.h>
#undef WLR_USE_UNSTABLE

#include "app_icon.h"
#include "config.h"
#include "server.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the task list. */
struct _wlmaker_task_list_t {
    /** Derived from a toolkit panel. */
    wlmtk_panel_t             super_panel;

    /** Buffer that shows the tasklist's content. */
    wlmtk_buffer_t            buffer;

    /** Backlink to the server. */
    wlmaker_server_t          *server_ptr;

    /** Listener for the `task_list_enabled` signal by `wlmaker_server_t`. */
    struct wl_listener        task_list_enabled_listener;
    /** Listener for the `task_list_disabled` signal by `wlmaker_server_t`. */
    struct wl_listener        task_list_disabled_listener;

    /** Listener for @ref wlmtk_desktop_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_unmapped. */
    struct wl_listener        window_unmapped_listener;

    /** Listener for @ref wlmaker_server_t::theme_changed_event. */
    struct wl_listener        theme_changed_listener;

    /** Whether the task list is currently enabled (mapped). */
    bool                      enabled;
    /** Size of the panel: It follows the number of windows. */
    wlmtk_panel_positioning_t positioning;
    /** Visual style. */
    struct wlmaker_task_list_style style;
};

static bool _wlmaker_task_list_refresh(
    wlmaker_task_list_t *task_list_ptr,
    const struct wlmaker_task_list_style *style_ptr);
static struct wlr_buffer *create_wlr_buffer(
    wlmaker_task_list_t *task_list_ptr,
    wlmtk_workspace_t *workspace_ptr,
    const struct wlmaker_task_list_style *style_ptr);
static void _wlmaker_task_list_draw_into_cairo(
    cairo_t *cairo_ptr,
    const struct wlmaker_task_list_style *style_ptr,
    wlmtk_workspace_t *workspace_ptr,
    int width,
    int height);
static uint32_t _wlmaker_task_list_request_size(
    wlmtk_panel_t *panel_ptr,
    int width,
    int height);

static void _wlmaker_task_list_handle_task_list_enabled(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_task_list_disabled(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_window_mapped(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_window_unmapped(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Data ================================================================= */

/* `wlmaker_task_list_style_desc` is defined in config.c: Referencing it
 * from here would pull the compositor into every user of the config
 * (eg. wlmdock links the config, and would link XCB and libinput). */

/** Task list positioning: Fixed dimensions, at center of layer. */
static const wlmtk_panel_positioning_t _wlmaker_task_list_positioning = {
    .desired_width = 400,
    .desired_height = 200,
    .anchor = WLR_EDGE_BOTTOM | WLR_EDGE_TOP | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
};

/** Virtual method table for the task list. */
static const wlmtk_panel_vmt_t _wlmaker_task_list_vmt = {
    .request_size = _wlmaker_task_list_request_size
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_task_list_t *wlmaker_task_list_create(
    wlmaker_server_t *server_ptr,
    const struct wlmaker_task_list_style *style_ptr)
{
    wlmaker_task_list_t *task_list_ptr = logged_calloc(
        1, sizeof(wlmaker_task_list_t));
    task_list_ptr->server_ptr = server_ptr;
    task_list_ptr->style = *style_ptr;
    task_list_ptr->positioning = _wlmaker_task_list_positioning;

    if (!wlmtk_panel_init(&task_list_ptr->super_panel,
                          &task_list_ptr->positioning)) {
        wlmaker_task_list_destroy(task_list_ptr);
        return NULL;
    }
    wlmtk_panel_extend(&task_list_ptr->super_panel,
                       &_wlmaker_task_list_vmt);
    wlmtk_element_set_visible(
        wlmtk_panel_element(&task_list_ptr->super_panel), true);

    if (!wlmtk_buffer_init(&task_list_ptr->buffer)) {
        wlmaker_task_list_destroy(task_list_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&task_list_ptr->buffer), true);
    wlmtk_container_add_element(
        &task_list_ptr->super_panel.super_container,
        wlmtk_buffer_element(&task_list_ptr->buffer));

    wlmtk_util_connect_listener_signal(
        &server_ptr->task_list_enabled_event,
        &task_list_ptr->task_list_enabled_listener,
        _wlmaker_task_list_handle_task_list_enabled);
    wlmtk_util_connect_listener_signal(
        &server_ptr->task_list_disabled_event,
        &task_list_ptr->task_list_disabled_listener,
        _wlmaker_task_list_handle_task_list_disabled);

    wlmtk_util_connect_listener_signal(
        &server_ptr->theme_changed_event,
        &task_list_ptr->theme_changed_listener,
        _wlmaker_task_list_handle_theme_changed);

    wlmtk_util_connect_listener_signal(
        &wlmtk_desktop_events(server_ptr->desktop_ptr)->window_mapped,
        &task_list_ptr->window_mapped_listener,
        _wlmaker_task_list_handle_window_mapped);
    wlmtk_util_connect_listener_signal(
        &wlmtk_desktop_events(server_ptr->desktop_ptr)->window_unmapped,
        &task_list_ptr->window_unmapped_listener,
        _wlmaker_task_list_handle_window_unmapped);

    return task_list_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_task_list_destroy(wlmaker_task_list_t *task_list_ptr)
{
    wlmtk_util_disconnect_listener(&task_list_ptr->window_unmapped_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->window_mapped_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->theme_changed_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->task_list_disabled_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->task_list_enabled_listener);

    if (wlmtk_buffer_element(&task_list_ptr->buffer)->parent_container_ptr) {
        wlmtk_container_remove_element(
            &task_list_ptr->super_panel.super_container,
            wlmtk_buffer_element(&task_list_ptr->buffer));
    }
    wlmtk_buffer_fini(&task_list_ptr->buffer);
    wlmtk_panel_fini(&task_list_ptr->super_panel);

    free(task_list_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Refreshes the task list. Should be done whenever a list is mapped/unmapped.
 *
 * @param task_list_ptr
 * @param style_ptr
 *
 * @return true on success.
 */
bool _wlmaker_task_list_refresh(
    wlmaker_task_list_t *task_list_ptr,
    const struct wlmaker_task_list_style *style_ptr)
{
    wlmtk_workspace_t *workspace_ptr =
        wlmtk_desktop_get_current_workspace(task_list_ptr->server_ptr->desktop_ptr);

    struct wlr_buffer *wlr_buffer_ptr = create_wlr_buffer(
        task_list_ptr, workspace_ptr, style_ptr);
    if (NULL == wlr_buffer_ptr) return false;

    wlmtk_buffer_set(&task_list_ptr->buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
    if (task_list_ptr->enabled) {
        wlmtk_panel_commit(&task_list_ptr->super_panel, 0,
                           &task_list_ptr->positioning);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/*
 * Window Maker's switch panel (switchpanel.c): The windows' icons in a row,
 * 64-pixel icons in tiles of 1.2 times that, 10 pixels of border, the
 * selected one in a darker tile with a white rounded frame, and its title in
 * a dark strip below. Same colors as its swtile/swback images.
 */
/** Size of an icon. */
static const int _sw_icon_size = 64;
/** Size of a tile: 1.2 times the icon. */
static const int _sw_tile_size = 77;
/** Border around the tiles and the title. */
static const int _sw_border = 10;
/** Height of the title strip. */
static const int _sw_label_height = 25;
/** Half a turn, in radians. */
static const double _sw_pi = 3.14159265358979323846;
/** Margin to the edges of the output. */
static const int _sw_screen_margin = 40;

/* ------------------------------------------------------------------------- */
/**
 * Creates a `struct wlr_buffer` with the windows of `workspace_ptr` drawn
 * into, and sizes the panel for them.
 *
 * @param task_list_ptr
 * @param workspace_ptr
 * @param style_ptr
 *
 * @return A pointer to the `struct wlr_buffer`, or NULL on error.
 */
struct wlr_buffer *create_wlr_buffer(
    wlmaker_task_list_t *task_list_ptr,
    wlmtk_workspace_t *workspace_ptr,
    const struct wlmaker_task_list_style *style_ptr)
{
    size_t count = 0;
    if (NULL != workspace_ptr) {
        count = bs_dllist_size(wlmtk_workspace_get_windows_dllist(
                                   workspace_ptr));
    }
    if (0 == count) count = 1;

    // As many tiles as fit on the output under the cursor.
    int max_width = 1024;
    struct wlr_output *o = wlmaker_server_get_output_at_cursor(
        task_list_ptr->server_ptr);
    if (NULL != o) {
        int ow, oh;
        wlr_output_effective_resolution(o, &ow, &oh);
        max_width = ow - 2 * _sw_screen_margin;
    }
    int max_tiles = (max_width - 2 * _sw_border) / _sw_tile_size;
    if (max_tiles < 1) max_tiles = 1;
    int tiles = (int)count < max_tiles ? (int)count : max_tiles;
    int width = tiles * _sw_tile_size + 2 * _sw_border;
    int height = _sw_tile_size + _sw_label_height + 3 * _sw_border;
    task_list_ptr->positioning.desired_width = width;
    task_list_ptr->positioning.desired_height = height;

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        width, height);
    if (NULL == wlr_buffer_ptr) return NULL;

    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }
    _wlmaker_task_list_draw_into_cairo(
        cairo_ptr, style_ptr, workspace_ptr, width, height);
    cairo_destroy(cairo_ptr);

    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
/** Path of a rectangle with rounded corners. */
static void _sw_rounded(cairo_t *cr, double x, double y, double w, double h,
                        double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -_sw_pi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, _sw_pi / 2);
    cairo_arc(cr, x + r, y + h - r, r, _sw_pi / 2, _sw_pi);
    cairo_arc(cr, x + r, y + r, r, _sw_pi, 3 * _sw_pi / 2);
    cairo_close_path(cr);
}

/* ------------------------------------------------------------------------- */
/** A window without an icon: A raised tile with its initial. */
static void _sw_draw_fallback(cairo_t *cr, const wlmtk_style_font_t *font_ptr,
                              const char *title_ptr, double x, double y,
                              double size)
{
    cairo_set_source_rgb(cr, 0x8d / 255.0, 0x8a / 255.0, 0x8e / 255.0);
    cairo_rectangle(cr, x, y, size, size);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, x + 0.5, y + size - 0.5);
    cairo_line_to(cr, x + 0.5, y + 0.5);
    cairo_line_to(cr, x + size - 0.5, y + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, x + size - 0.5, y + 0.5);
    cairo_line_to(cr, x + size - 0.5, y + size - 0.5);
    cairo_line_to(cr, x + 0.5, y + size - 0.5);
    cairo_stroke(cr);
    char initial[8] = "?";
    if (NULL != title_ptr && '\0' != *title_ptr) {
        // One UTF-8 character.
        size_t n = 1;
        while (n < 4 && 0x80 == ((unsigned char)title_ptr[n] & 0xc0)) ++n;
        memcpy(initial, title_ptr, n);
        initial[n] = '\0';
    }
    cairo_select_font_face(cr, font_ptr->face, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, size * 0.5);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, initial, &ext);
    cairo_set_source_rgb(cr, 0x20 / 255.0, 0x20 / 255.0, 0x20 / 255.0);
    cairo_move_to(cr, x + (size - ext.width) / 2 - ext.x_bearing,
                  y + (size - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, initial);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the switch panel: The windows of `workspace_ptr` (most recently
 * used first, as Window Maker cycles), the activated one selected.
 *
 * @param cairo_ptr
 * @param style_ptr
 * @param workspace_ptr
 * @param width
 * @param height
 */
void _wlmaker_task_list_draw_into_cairo(
    cairo_t *cairo_ptr,
    const struct wlmaker_task_list_style *style_ptr,
    wlmtk_workspace_t *workspace_ptr,
    int width,
    int height)
{
    cairo_t *cr = cairo_ptr;
    // The panel: Window Maker's gray with a black frame.
    cairo_set_source_rgb(cr, 0xae / 255.0, 0xaa / 255.0, 0xae / 255.0);
    cairo_paint(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 0.5, 0.5, width - 1, height - 1);
    cairo_stroke(cr);

    if (NULL == workspace_ptr) return;
    const bs_dllist_t *windows_ptr = wlmtk_workspace_get_windows_dllist(
        workspace_ptr);
    if (bs_dllist_empty(windows_ptr)) return;

    wlmtk_window_t *active_ptr = wlmtk_workspace_get_activated_window(
        workspace_ptr);
    int count = (int)bs_dllist_size(windows_ptr);
    int tiles = (width - 2 * _sw_border) / _sw_tile_size;
    int selected = 0, i = 0;
    for (bs_dllist_node_t *d = windows_ptr->head_ptr; NULL != d;
         d = d->next_ptr, ++i) {
        if (wlmtk_window_from_dlnode(d) == active_ptr) selected = i;
    }
    // More windows than tiles: The row scrolls to keep the selected one.
    int first = 0;
    if (count > tiles) {
        first = selected - tiles / 2;
        if (first < 0) first = 0;
        if (first > count - tiles) first = count - tiles;
    }

    const char *selected_title_ptr = NULL;
    i = 0;
    for (bs_dllist_node_t *d = windows_ptr->head_ptr; NULL != d;
         d = d->next_ptr, ++i) {
        if (i < first || i >= first + tiles) continue;
        wlmtk_window_t *w_ptr = wlmtk_window_from_dlnode(d);
        double x = _sw_border + (i - first) * _sw_tile_size;
        double y = _sw_border;
        if (i == selected) {
            selected_title_ptr = wlmtk_window_get_title(w_ptr);
            _sw_rounded(cr, x + 1.5, y + 1.5, _sw_tile_size - 3,
                        _sw_tile_size - 3, 7);
            cairo_set_source_rgb(cr, 0x8d / 255.0, 0x8a / 255.0, 0x8e / 255.0);
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, 3);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_stroke_preserve(cr);
            cairo_set_line_width(cr, 1.6);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_stroke(cr);
        }
        int icon = _sw_icon_size * 3 / 4;
        double ix = x + (_sw_tile_size - icon) / 2.0;
        double iy = y + (_sw_tile_size - icon) / 2.0;
        cairo_surface_t *s_ptr = wlmaker_app_icon_get(
            wlmtk_window_get_app_id(w_ptr),
            wlmtk_window_get_instance(w_ptr), icon);
        // Minimized windows are dimmed, as Window Maker's.
        double alpha = wlmtk_window_is_minimized(w_ptr) ? 0.45 : 1.0;
        cairo_save(cr);
        if (NULL != s_ptr) {
            cairo_set_source_surface(cr, s_ptr, ix, iy);
            cairo_paint_with_alpha(cr, alpha);
        } else {
            cairo_push_group(cr);
            _sw_draw_fallback(cr, &style_ptr->font,
                              wlmtk_window_get_title(w_ptr), ix, iy, icon);
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, alpha);
        }
        cairo_restore(cr);
    }

    // The title of the selected window, in the dark strip.
    double lx = _sw_border, ly = height - _sw_border - _sw_label_height;
    double lw = width - 2 * _sw_border;
    cairo_rectangle(cr, lx + 0.5, ly + 0.5, lw - 1, _sw_label_height - 1);
    cairo_set_source_rgb(cr, 0x50 / 255.0, 0x53 / 255.0, 0x50 / 255.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_stroke(cr);
    if (NULL != selected_title_ptr) {
        cairo_save(cr);
        cairo_rectangle(cr, lx + 2, ly, lw - 4, _sw_label_height);
        cairo_clip(cr);
        cairo_select_font_face(cr, style_ptr->font.face,
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, style_ptr->font.size);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, lx + 6,
                      ly + (_sw_label_height + fe.ascent - fe.descent) / 2.0);
        cairo_show_text(cr, selected_title_ptr);
        cairo_restore(cr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_panel_vmt_t::request_size.
 *
 * @param panel_ptr
 * @param width
 * @param height
 *
 * @return 0 always.
 */
uint32_t _wlmaker_task_list_request_size(
    wlmtk_panel_t *panel_ptr,
    __UNUSED__ int width,
    __UNUSED__ int height)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        panel_ptr, wlmaker_task_list_t, super_panel);
    wlmtk_panel_commit(panel_ptr, 0, &task_list_ptr->positioning);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `task_list_enabled_listener`.
 *
 * Enables the task listener: Creates the task list for the currently-active
 * workspace and enables the task list on that workspace.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_task_list_enabled(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, task_list_enabled_listener);

    _wlmaker_task_list_refresh(task_list_ptr, &task_list_ptr->style);

    if (task_list_ptr->enabled) {
        BS_ASSERT(NULL != wlmtk_panel_get_layer(&task_list_ptr->super_panel));
        return;
    }

    wlmtk_layer_t *layer_ptr = wlmtk_desktop_get_layer(
        task_list_ptr->server_ptr->desktop_ptr,
        WLMTK_DESKTOP_LAYER_OVERLAY);
    wlmtk_layer_add_panel(
        layer_ptr,
        &task_list_ptr->super_panel,
        wlmaker_server_get_output_at_cursor(task_list_ptr->server_ptr));
    task_list_ptr->enabled = true;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `task_list_disabled_listener`: Hides the list.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_task_list_disabled(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, task_list_disabled_listener);

    BS_ASSERT(NULL != wlmtk_panel_get_layer(&task_list_ptr->super_panel));
    wlmtk_layer_remove_panel(
        wlmtk_panel_get_layer(&task_list_ptr->super_panel),
        &task_list_ptr->super_panel);
    task_list_ptr->enabled = false;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `theme_changed_listener`: Updates the style
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, theme_changed_listener);
    wlmaker_config_style_t *style_ptr = data_ptr;

    wlmaker_app_icon_flush();
    if (_wlmaker_task_list_refresh(task_list_ptr, &style_ptr->task_list)) {
        task_list_ptr->style = style_ptr->task_list;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `window_mapped_listener`: Refreshes the list (if enabled).
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_window_mapped(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, window_mapped_listener);
    if (task_list_ptr->enabled) {
        _wlmaker_task_list_refresh(task_list_ptr, &task_list_ptr->style);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `window_unmapped_listener`: Refreshes the list (if enabled).
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_window_unmapped(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, window_unmapped_listener);
    if (task_list_ptr->enabled) {
        _wlmaker_task_list_refresh(task_list_ptr, &task_list_ptr->style);
    }
}

/* == End of task_list.c =================================================== */
