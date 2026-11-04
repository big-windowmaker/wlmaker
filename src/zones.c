/* ========================================================================= */
/**
 * @file zones.c
 *
 * Copyright (c) 2026 Big WindowMaker
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include "zones.h"

#include <libbase/libbase.h>
#include <stdlib.h>

#ifndef WLMAKER_HAVE_ZONES

wlmaker_zones_t *wlmaker_zones_create(
    __UNUSED__ struct _wlmaker_server_t *server_ptr) { return NULL; }
void wlmaker_zones_destroy(__UNUSED__ wlmaker_zones_t *zones_ptr) {}
void wlmaker_zones_reload(__UNUSED__ wlmaker_zones_t *zones_ptr) {}
void wlmaker_zones_command(__UNUSED__ wlmaker_zones_t *zones_ptr,
                           __UNUSED__ const char *cmd_ptr,
                           char *out_ptr, size_t out_len)
{
    if (NULL != out_ptr && 0 < out_len) out_ptr[0] = '\0';
}

#else  // WLMAKER_HAVE_ZONES

#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "input/manager.h"
#include "server.h"
#include "toolkit/toolkit.h"

#include "wz-config.h"
#include "wz-ipc.h"
#include "wz-layout.h"
#include "wz-theme.h"
#include "wz-tile.h"

/* == Declarations ========================================================= */

/** Maximum number of zones shown. */
#define ZONES_MAX 128
/** Windows remembered (restore geometry, tiling). */
#define WINDOWS_MAX 256

/** What the zones remember about a window. */
typedef struct {
    /** The window. NULL if the slot is free. */
    wlmtk_window_t            *window_ptr;
    /** Bounding box before it was snapped or tiled. */
    struct wlr_box            restore;
    /** Whether `restore` holds a box. */
    bool                      has_restore;
    /** Zone it was snapped to, -1 if none. */
    int                       zone;
    /** Whether the window is tiled. */
    bool                      tiled;
    /** The user took it out of the tiling. */
    bool                      user_float;
    /** Tiling order: Lower is the main window. */
    long                      order;
    /** Last box requested by the tiling. */
    struct wlr_box            expect;
} zones_window_t;

/** State of the zones. */
struct _wlmaker_zones_t {
    /** Derived from a panel: The overlay, on the overlay layer. */
    wlmtk_panel_t             super_panel;
    /** Buffer showing the zones. */
    wlmtk_buffer_t            buffer;
    /** Original vmt of the buffer's element (pointer passthrough). */
    wlmtk_element_vmt_t       orig_buffer_vmt;
    /** Back-link. */
    wlmaker_server_t          *server_ptr;

    /** Configuration, shared with wmake-zones on X11. */
    wz_config                 cfg;
    /** Path of the configuration file. */
    char                      *cfg_path_ptr;
    /** Colors of the Window Maker theme. */
    wz_theme                  theme;

    /** Output the overlay is on, NULL if hidden. */
    struct wlr_output         *overlay_output_ptr;
    /** Layout box of that output. */
    struct wlr_box            output_box;
    /** Zones shown, in layout coordinates. */
    wz_rect                   zones[ZONES_MAX];
    /** Optional names of the zones. */
    char                      *names[ZONES_MAX];
    /** Number of zones shown. */
    int                       nzones;
    /** Highlighted zones (-1 = none). */
    int                       hl_a;
    /** Second highlighted zone, when spanning two. */
    int                       hl_b;
    /** Target box for the window. */
    wz_rect                   target;
    /** Size the panel was given. */
    int                       width;
    /** Size the panel was given. */
    int                       height;

    /** Window being dragged. */
    wlmtk_window_t            *drag_window_ptr;
    /** Its bounding box when the drag started. */
    struct wlr_box            drag_start;
    /** Escape was pressed during the drag. */
    bool                      cancelled;
    /** Temporary binding of Escape while dragging. */
    wlmim_keybinding_t        *esc_binding_ptr;
    /** Combo of that binding. */
    struct wlmim_keybinding_combo esc_combo;

    /** Remembered windows. */
    zones_window_t            windows[WINDOWS_MAX];
    /** Counter for the tiling order. */
    long                      order_seq;

    /** Listening socket. */
    int                       listen_fd;
    /** Its event source. */
    struct wl_event_source    *listen_source_ptr;
    /** Path of the socket. */
    char                      *sock_path_ptr;
    /** Timer: Relayout of the tiling, coalesced. */
    struct wl_event_source    *tile_timer_ptr;
    /** Timer: Hides a preview. */
    struct wl_event_source    *preview_timer_ptr;

    /** Listener for @ref wlmtk_desktop_events_t::window_move_begin. */
    struct wl_listener        move_begin_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_move_motion. */
    struct wl_listener        move_motion_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_move_end. */
    struct wl_listener        move_end_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_unmapped. */
    struct wl_listener        window_unmapped_listener;
    /** Listener for @ref wlmtk_desktop_events_t::workspace_changed. */
    struct wl_listener        workspace_changed_listener;
};

static void _wlmaker_zones_overlay_hide(wlmaker_zones_t *zones_ptr);
static void _wlmaker_zones_redraw(wlmaker_zones_t *zones_ptr);
static void _wlmaker_zones_tile_mark(wlmaker_zones_t *zones_ptr);

/* == Helpers ============================================================== */

/** Appends to a reply buffer. */
static void _out(char *out_ptr, size_t len, const char *fmt_ptr, ...)
{
    if (NULL == out_ptr || 0 == len) return;
    size_t used = strlen(out_ptr);
    if (used + 1 >= len) return;
    va_list ap;
    va_start(ap, fmt_ptr);
    vsnprintf(out_ptr + used, len - used, fmt_ptr, ap);
    va_end(ap);
}

/** @return The remembered state of the window, creating it if asked. */
static zones_window_t *_win(wlmaker_zones_t *zones_ptr,
                            wlmtk_window_t *window_ptr,
                            bool create)
{
    zones_window_t *free_ptr = NULL;
    for (int i = 0; i < WINDOWS_MAX; ++i) {
        if (zones_ptr->windows[i].window_ptr == window_ptr) {
            return &zones_ptr->windows[i];
        }
        if (NULL == free_ptr && NULL == zones_ptr->windows[i].window_ptr) {
            free_ptr = &zones_ptr->windows[i];
        }
    }
    if (!create || NULL == free_ptr) return NULL;
    *free_ptr = (zones_window_t){
        .window_ptr = window_ptr, .zone = -1,
        .order = ++zones_ptr->order_seq };
    return free_ptr;
}

/** @return The output at the layout position, or the primary one. */
static struct wlr_output *_output_at(wlmaker_zones_t *zones_ptr,
                                     double x, double y)
{
    struct wlr_output *o = wlr_output_layout_output_at(
        zones_ptr->server_ptr->wlr_output_layout_ptr, x, y);
    return NULL != o ? o : wlmaker_server_get_output_at_cursor(
        zones_ptr->server_ptr);
}

/** @return 1-based index of the current workspace. */
static int _workspace_number(wlmaker_zones_t *zones_ptr)
{
    const char *name_ptr;
    int index = 0;
    wlmtk_workspace_get_details(
        wlmtk_desktop_get_current_workspace(zones_ptr->server_ptr->desktop_ptr),
        &name_ptr, &index);
    return index + 1;
}

/** Computes the zones of the layout effective on `output_ptr`. */
static int _zones_for(wlmaker_zones_t *zones_ptr,
                      struct wlr_output *output_ptr,
                      const char *tree_override_ptr,
                      wz_rect *out_ptr,
                      char **names_ptr)
{
    if (NULL == output_ptr) return 0;
    const char *tree_ptr = tree_override_ptr;
    if (NULL == tree_ptr) {
        const wz_layout_def *l = wz_config_layout_for(
            &zones_ptr->cfg, output_ptr->name, _workspace_number(zones_ptr));
        if (NULL == l) return 0;
        tree_ptr = l->tree;
    }
    wz_node *t = wz_parse(tree_ptr, NULL);
    if (NULL == t) return 0;
    struct wlr_box u = wlmtk_desktop_get_usable_area(
        zones_ptr->server_ptr->desktop_ptr, output_ptr);
    int n = wz_compute(t, (wz_rect){ u.x, u.y, u.width, u.height },
                       zones_ptr->cfg.gap_in, zones_ptr->cfg.gap_out,
                       out_ptr, ZONES_MAX);
    if (n > ZONES_MAX) n = ZONES_MAX;
    if (NULL != names_ptr) {
        for (int i = 0; i < n; ++i) {
            wz_node *leaf = wz_leaf_nth(t, i);
            names_ptr[i] = (NULL != leaf && NULL != leaf->name) ?
                strdup(leaf->name) : NULL;
        }
    }
    wz_free(t);
    return n;
}

/** Snaps the window to a box (layout coordinates, with decorations). */
static void _snap(wlmaker_zones_t *zones_ptr,
                  wlmtk_window_t *window_ptr,
                  wz_rect target,
                  int zone)
{
    if (wlmtk_window_is_fullscreen(window_ptr)) return;
    zones_window_t *w = _win(zones_ptr, window_ptr, true);
    if (NULL != w && !w->has_restore) {
        w->restore = wlmtk_window_get_bounding_box(window_ptr);
        w->has_restore = true;
    }
    if (NULL != w) w->zone = zone;
    struct wlr_box box = {
        .x = target.x, .y = target.y,
        .width = target.w, .height = target.h };
    wlmtk_window_request_bounding_box(window_ptr, &box);
}

/* == Overlay ============================================================== */

/** The overlay never takes the pointer: The drag continues below it. */
static bool _wlmaker_zones_buffer_accepts_motion(
    __UNUSED__ wlmtk_element_t *element_ptr,
    __UNUSED__ wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    return false;
}

/** Overlay: Spans the whole output. */
static const wlmtk_panel_positioning_t _wlmaker_zones_positioning = {
    .desired_width = 0,
    .desired_height = 0,
    .anchor = WLR_EDGE_BOTTOM | WLR_EDGE_TOP | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
};

/** Panel's request_size: Remembers the size and redraws. */
static uint32_t _wlmaker_zones_request_size(
    wlmtk_panel_t *panel_ptr, int width, int height)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        panel_ptr, wlmaker_zones_t, super_panel);
    zones_ptr->width = width;
    zones_ptr->height = height;
    _wlmaker_zones_redraw(zones_ptr);
    return 0;
}

/** Virtual method table of the overlay panel. */
static const wlmtk_panel_vmt_t _wlmaker_zones_panel_vmt = {
    .request_size = _wlmaker_zones_request_size
};

/** Sets the cairo color from 0xRRGGBB and an alpha. */
static void _rgb(cairo_t *cr, unsigned rgb, double alpha)
{
    cairo_set_source_rgba(cr, (rgb >> 16 & 0xff) / 255.0,
                          (rgb >> 8 & 0xff) / 255.0,
                          (rgb & 0xff) / 255.0, alpha);
}

/** OpenStep bevel: light top/left, dark bottom/right. */
static void _bevel(cairo_t *cr, double x, double y, double w, double h,
                   int thick, double alpha)
{
    for (int t = 0; t < thick; ++t) {
        _rgb(cr, 0xffffff, alpha);
        cairo_rectangle(cr, x + t, y + t, w - 2 * t, 1);
        cairo_rectangle(cr, x + t, y + t, 1, h - 2 * t);
        cairo_fill(cr);
        _rgb(cr, 0x000000, alpha);
        cairo_rectangle(cr, x + t, y + h - 1 - t, w - 2 * t, 1);
        cairo_rectangle(cr, x + w - 1 - t, y + t, 1, h - 2 * t);
        cairo_fill(cr);
    }
}

/** @return The highlight color: The configured one, or the title bar's. */
static unsigned _hl(wlmaker_zones_t *zones_ptr, bool second)
{
    if (WZ_COLOR_AUTO != zones_ptr->cfg.highlight) {
        return second ? wz_rgb_mix(zones_ptr->cfg.highlight, 0, 90) :
            zones_ptr->cfg.highlight;
    }
    return second ? zones_ptr->theme.ftitle2 : zones_ptr->theme.ftitle1;
}

/** Draws one zone: Panel, bevel, numbered tile, name. */
static void _draw_zone(wlmaker_zones_t *zones_ptr, cairo_t *cr,
                       int i, bool hl)
{
    wz_rect r = zones_ptr->zones[i];
    double x = r.x - zones_ptr->output_box.x, y = r.y - zones_ptr->output_box.y;
    int op = zones_ptr->cfg.opacity;
    _rgb(cr, hl ? _hl(zones_ptr, false) : zones_ptr->theme.menu_bg1,
         hl ? BS_MIN(235, 90 + op * 13 / 10) / 255.0 : (30 + op) / 255.0);
    cairo_rectangle(cr, x, y, r.w, r.h);
    cairo_fill(cr);
    _bevel(cr, x, y, r.w, r.h, hl ? 3 : 2, 0.85);

    int m = BS_MIN(r.w, r.h);
    int s = BS_MAX(28, BS_MIN(64, m / 4));
    double bx = x + r.w / 2 - s / 2, by = y + r.h / 2 - s / 2;
    cairo_pattern_t *p = cairo_pattern_create_linear(bx, by, bx + s, by + s);
    unsigned c1 = hl ? _hl(zones_ptr, false) : zones_ptr->theme.icon1;
    unsigned c2 = hl ? _hl(zones_ptr, true) : zones_ptr->theme.icon2;
    cairo_pattern_add_color_stop_rgb(p, 0, (c1 >> 16 & 0xff) / 255.0,
                                     (c1 >> 8 & 0xff) / 255.0, (c1 & 0xff) / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, (c2 >> 16 & 0xff) / 255.0,
                                     (c2 >> 8 & 0xff) / 255.0, (c2 & 0xff) / 255.0);
    cairo_rectangle(cr, bx, by, s, s);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    _bevel(cr, bx, by, s, s, 1, 0.9);

    if (zones_ptr->cfg.show_numbers) {
        char num[16];
        snprintf(num, sizeof(num), "%d", i + 1);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, s * 0.5);
        cairo_text_extents_t te;
        cairo_text_extents(cr, num, &te);
        double tx = bx + s / 2.0 - te.width / 2 - te.x_bearing;
        double ty = by + s / 2.0 - te.height / 2 - te.y_bearing;
        _rgb(cr, 0x000000, 0.8);
        cairo_move_to(cr, tx + 1, ty + 1);
        cairo_show_text(cr, num);
        _rgb(cr, 0xffffff, 1);
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, num);
    }
    const char *name_ptr = zones_ptr->names[i];
    if (NULL != name_ptr && r.h > s + 40) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t te;
        cairo_text_extents(cr, name_ptr, &te);
        _rgb(cr, hl ? zones_ptr->theme.ftitle_fg : zones_ptr->theme.menu_fg, 1);
        cairo_move_to(cr, x + r.w / 2.0 - te.width / 2 - te.x_bearing,
                      by + s + 16);
        cairo_show_text(cr, name_ptr);
    }
}

/** Redraws the overlay into a new buffer. */
void _wlmaker_zones_redraw(wlmaker_zones_t *zones_ptr)
{
    if (NULL == zones_ptr->overlay_output_ptr ||
        0 >= zones_ptr->width || 0 >= zones_ptr->height) return;
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        zones_ptr->width, zones_ptr->height);
    if (NULL == wlr_buffer_ptr) return;
    cairo_t *cr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return;
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    for (int i = 0; i < zones_ptr->nzones; ++i) {
        _draw_zone(zones_ptr, cr, i, false);
    }
    if (0 <= zones_ptr->hl_a) {
        if (0 <= zones_ptr->hl_b) {
            wz_rect t = zones_ptr->target;
            double x = t.x - zones_ptr->output_box.x;
            double y = t.y - zones_ptr->output_box.y;
            _rgb(cr, _hl(zones_ptr, false), (120 + zones_ptr->cfg.opacity) / 255.0);
            cairo_rectangle(cr, x, y, t.w, t.h);
            cairo_fill(cr);
            _bevel(cr, x, y, t.w, t.h, 3, 0.85);
        }
        _draw_zone(zones_ptr, cr, zones_ptr->hl_a, true);
        if (0 <= zones_ptr->hl_b) _draw_zone(zones_ptr, cr, zones_ptr->hl_b, true);
    }
    cairo_destroy(cr);
    wlmtk_buffer_set(&zones_ptr->buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
}

/** Shows the overlay with the given zones on `output_ptr`. */
static void _wlmaker_zones_overlay_show(
    wlmaker_zones_t *zones_ptr,
    struct wlr_output *output_ptr,
    const char *tree_override_ptr)
{
    if (zones_ptr->overlay_output_ptr != output_ptr) {
        _wlmaker_zones_overlay_hide(zones_ptr);
    }
    for (int i = 0; i < zones_ptr->nzones; ++i) {
        free(zones_ptr->names[i]);
        zones_ptr->names[i] = NULL;
    }
    zones_ptr->nzones = _zones_for(zones_ptr, output_ptr, tree_override_ptr,
                                   zones_ptr->zones, zones_ptr->names);
    zones_ptr->hl_a = zones_ptr->hl_b = -1;
    wlr_output_layout_get_box(zones_ptr->server_ptr->wlr_output_layout_ptr,
                              output_ptr, &zones_ptr->output_box);
    if (NULL == zones_ptr->overlay_output_ptr) {
        wz_theme_load(&zones_ptr->theme, NULL);
        zones_ptr->overlay_output_ptr = output_ptr;
        // Adding the panel lays it out: request_size draws it.
        wlmtk_layer_add_panel(
            wlmtk_desktop_get_layer(zones_ptr->server_ptr->desktop_ptr,
                                    WLMTK_DESKTOP_LAYER_OVERLAY),
            &zones_ptr->super_panel, output_ptr);
    } else {
        _wlmaker_zones_redraw(zones_ptr);
    }
}

/** Hides the overlay and releases its buffer. */
void _wlmaker_zones_overlay_hide(wlmaker_zones_t *zones_ptr)
{
    if (NULL == zones_ptr->overlay_output_ptr) return;
    wlmtk_layer_t *layer_ptr = wlmtk_panel_get_layer(&zones_ptr->super_panel);
    if (NULL != layer_ptr) {
        wlmtk_layer_remove_panel(layer_ptr, &zones_ptr->super_panel);
    }
    wlmtk_buffer_set(&zones_ptr->buffer, NULL);
    zones_ptr->overlay_output_ptr = NULL;
    zones_ptr->hl_a = zones_ptr->hl_b = -1;
    zones_ptr->width = zones_ptr->height = 0;
}

/* == Drag ================================================================= */

/** Escape during a drag: Cancels the zones; the window goes back on drop. */
static bool _wlmaker_zones_escape(const struct wlmim_keybinding_combo *kc)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        kc, wlmaker_zones_t, esc_combo);
    if (NULL == zones_ptr->drag_window_ptr) return false;
    zones_ptr->cancelled = true;
    _wlmaker_zones_overlay_hide(zones_ptr);
    return true;
}

/** Window move begins. */
static void _wlmaker_zones_handle_move_begin(struct wl_listener *listener_ptr,
                                             void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, move_begin_listener);
    wlmtk_window_move_event_t *ev = data_ptr;
    zones_window_t *w = _win(zones_ptr, ev->window_ptr, false);
    bool tiled = NULL != w && w->tiled;
    if (!zones_ptr->cfg.zones && !tiled) return;
    zones_ptr->drag_window_ptr = ev->window_ptr;
    zones_ptr->drag_start = wlmtk_window_get_bounding_box(ev->window_ptr);
    zones_ptr->cancelled = false;
    if (NULL == zones_ptr->esc_binding_ptr) {
        zones_ptr->esc_binding_ptr = wlmim_bind_key(
            zones_ptr->server_ptr->input_manager_ptr,
            &zones_ptr->esc_combo, _wlmaker_zones_escape);
    }
}

/** Window moves: Shows the zones of the output under the pointer. */
static void _wlmaker_zones_handle_move_motion(struct wl_listener *listener_ptr,
                                              void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, move_motion_listener);
    wlmtk_window_move_event_t *ev = data_ptr;
    if (ev->window_ptr != zones_ptr->drag_window_ptr || zones_ptr->cancelled) {
        return;
    }
    struct wlr_box now = wlmtk_window_get_bounding_box(ev->window_ptr);
    int dx = now.x - zones_ptr->drag_start.x, dy = now.y - zones_ptr->drag_start.y;
    if (dx * dx + dy * dy < zones_ptr->cfg.min_drag_px * zones_ptr->cfg.min_drag_px) {
        return;
    }
    struct wlr_output *output_ptr = _output_at(zones_ptr, ev->x, ev->y);
    if (NULL == output_ptr) return;
    bool active = true;
    if (WZ_ACTIVATE_EDGE == zones_ptr->cfg.activation) {
        struct wlr_box b;
        wlr_output_layout_get_box(zones_ptr->server_ptr->wlr_output_layout_ptr,
                                  output_ptr, &b);
        int e = zones_ptr->cfg.edge_px;
        active = ev->x < b.x + e || ev->y < b.y + e ||
            ev->x >= b.x + b.width - e || ev->y >= b.y + b.height - e;
    }
    // The modifier activation needs the keyboard state; on Wayland the
    // zones show while dragging, as with "drag".
    if (!active) {
        _wlmaker_zones_overlay_hide(zones_ptr);
        return;
    }
    if (zones_ptr->overlay_output_ptr != output_ptr) {
        _wlmaker_zones_overlay_show(zones_ptr, output_ptr, NULL);
    }
    wz_rect target;
    int b = -1;
    int a = wz_target(zones_ptr->zones, zones_ptr->nzones, (int)ev->x, (int)ev->y,
                      zones_ptr->cfg.span_px, &target, &b);
    if (a != zones_ptr->hl_a || b != zones_ptr->hl_b) {
        zones_ptr->hl_a = a;
        zones_ptr->hl_b = b;
        zones_ptr->target = target;
        _wlmaker_zones_redraw(zones_ptr);
    }
}

/** Window move ends: Snaps, puts it back (Escape), or restores its size. */
static void _wlmaker_zones_handle_move_end(struct wl_listener *listener_ptr,
                                           void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, move_end_listener);
    wlmtk_window_move_event_t *ev = data_ptr;
    if (NULL != zones_ptr->esc_binding_ptr) {
        wlmim_unbind_key(zones_ptr->server_ptr->input_manager_ptr,
                         zones_ptr->esc_binding_ptr);
        zones_ptr->esc_binding_ptr = NULL;
    }
    if (ev->window_ptr != zones_ptr->drag_window_ptr) return;
    wlmtk_window_t *window_ptr = zones_ptr->drag_window_ptr;
    zones_ptr->drag_window_ptr = NULL;
    zones_window_t *w = _win(zones_ptr, window_ptr, false);

    if (zones_ptr->cancelled) {
        _wlmaker_zones_overlay_hide(zones_ptr);
        if (NULL != w && w->tiled) {
            _wlmaker_zones_tile_mark(zones_ptr);
        } else {
            wlmtk_window_request_bounding_box(window_ptr, &zones_ptr->drag_start);
        }
        return;
    }
    if (NULL != w && w->tiled) {
        // Dropped on another tiled window: swap them.
        for (int i = 0; i < WINDOWS_MAX; ++i) {
            zones_window_t *o = &zones_ptr->windows[i];
            if (NULL == o->window_ptr || o == w || !o->tiled) continue;
            struct wlr_box ob = wlmtk_window_get_bounding_box(o->window_ptr);
            if (wlr_box_contains_point(&ob, ev->x, ev->y)) {
                long t = o->order;
                o->order = w->order;
                w->order = t;
                break;
            }
        }
        _wlmaker_zones_overlay_hide(zones_ptr);
        _wlmaker_zones_tile_mark(zones_ptr);
        return;
    }
    if (NULL != zones_ptr->overlay_output_ptr && 0 <= zones_ptr->hl_a) {
        zones_window_t *s = _win(zones_ptr, window_ptr, true);
        if (NULL != s && !s->has_restore) {
            s->restore = zones_ptr->drag_start;
            s->has_restore = true;
        }
        _snap(zones_ptr, window_ptr, zones_ptr->target, zones_ptr->hl_a);
    } else if (NULL != w && w->zone >= 0 && w->has_restore &&
               zones_ptr->cfg.restore_size_on_unsnap) {
        // Dragged out of the zones: Back to the size from before.
        struct wlr_box now = wlmtk_window_get_bounding_box(window_ptr);
        double fx = now.width > 0 ? (ev->x - now.x) / now.width : 0.5;
        struct wlr_box r = {
            .x = (int)(ev->x - fx * w->restore.width), .y = now.y,
            .width = w->restore.width, .height = w->restore.height };
        w->zone = -1;
        w->has_restore = false;
        wlmtk_window_request_bounding_box(window_ptr, &r);
    }
    _wlmaker_zones_overlay_hide(zones_ptr);
}

/* == Tiling =============================================================== */

/** @return Whether the window can be tiled. */
static bool _tileable(wlmaker_zones_t *zones_ptr, wlmtk_window_t *window_ptr)
{
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    if (NULL != w && w->user_float) return false;
    if (wlmtk_window_is_fullscreen(window_ptr)) return false;
    if (!(wlmtk_window_get_properties(window_ptr) & WLMTK_WINDOW_PROPERTY_RESIZABLE)) {
        return false;
    }
    return true;
}

/** Sorts by tiling order. */
static int _by_order(const void *a, const void *b)
{
    const zones_window_t *x = *(zones_window_t *const *)a;
    const zones_window_t *y = *(zones_window_t *const *)b;
    return x->order < y->order ? -1 : x->order > y->order;
}

/** Tiles the windows of the current workspace, per output. */
static int _wlmaker_zones_tile_run(void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
        zones_ptr->server_ptr->desktop_ptr);
    if (NULL == ws_ptr) return 0;
    int wsn = _workspace_number(zones_ptr);

    zones_window_t *members[WINDOWS_MAX];
    struct wlr_output *outs[WINDOWS_MAX];
    int n = 0;
    bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
    for (bs_dllist_node_t *d = list_ptr->head_ptr; NULL != d; d = d->next_ptr) {
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(d);
        struct wlr_output *o = wlmtk_window_get_wlr_output(window_ptr);
        if (NULL == o) continue;
        bool on = wz_config_autotile_for(&zones_ptr->cfg, o->name, wsn);
        zones_window_t *w = _win(zones_ptr, window_ptr, on);
        if (NULL == w) continue;
        if (!on || !_tileable(zones_ptr, window_ptr)) {
            if (w->tiled) {
                w->tiled = false;
                if (w->has_restore) {
                    wlmtk_window_request_bounding_box(window_ptr, &w->restore);
                    w->has_restore = false;
                    w->zone = -1;
                }
            }
            continue;
        }
        if (n < WINDOWS_MAX) {
            members[n] = w;
            outs[n] = o;
            n++;
        }
    }
    // One output at a time.
    for (int i = 0; i < n; ++i) {
        if (NULL == outs[i]) continue;
        struct wlr_output *o = outs[i];
        zones_window_t *set[WINDOWS_MAX];
        int k = 0;
        for (int j = i; j < n; ++j) {
            if (outs[j] == o) {
                set[k++] = members[j];
                outs[j] = NULL;
            }
        }
        qsort(set, (size_t)k, sizeof(set[0]), _by_order);
        wz_tile_params p = zones_ptr->cfg.tile;
        p.algo = wz_config_algo_for(&zones_ptr->cfg, o->name, wsn);
        if (WZ_TILE_BSP == p.algo) p.algo = WZ_TILE_DWINDLE;
        struct wlr_box u = wlmtk_desktop_get_usable_area(
            zones_ptr->server_ptr->desktop_ptr, o);
        wz_rect rects[WINDOWS_MAX];
        wz_tile(&p, (wz_rect){ u.x, u.y, u.width, u.height }, k, rects, WINDOWS_MAX);
        for (int j = 0; j < k; ++j) {
            zones_window_t *w = set[j];
            if (!w->tiled) {
                if (!w->has_restore) {
                    w->restore = wlmtk_window_get_bounding_box(w->window_ptr);
                    w->has_restore = true;
                }
                w->tiled = true;
            }
            struct wlr_box box = { rects[j].x, rects[j].y, rects[j].w, rects[j].h };
            if (memcmp(&box, &w->expect, sizeof(box)) != 0) {
                w->expect = box;
                wlmtk_window_request_bounding_box(w->window_ptr, &box);
            }
        }
    }
    return 0;
}

/** Schedules a relayout (coalesces bursts of events). */
void _wlmaker_zones_tile_mark(wlmaker_zones_t *zones_ptr)
{
    if (NULL != zones_ptr->tile_timer_ptr) {
        wl_event_source_timer_update(zones_ptr->tile_timer_ptr, 40);
    }
}

/** Tiling off: All tiled windows back to where they were. */
static void _wlmaker_zones_untile_all(wlmaker_zones_t *zones_ptr)
{
    for (int i = 0; i < WINDOWS_MAX; ++i) {
        zones_window_t *w = &zones_ptr->windows[i];
        if (NULL == w->window_ptr || !w->tiled) continue;
        w->tiled = false;
        memset(&w->expect, 0, sizeof(w->expect));
        if (w->has_restore) {
            wlmtk_window_request_bounding_box(w->window_ptr, &w->restore);
            w->has_restore = false;
            w->zone = -1;
        }
    }
}

/** A window was mapped. */
static void _wlmaker_zones_handle_window_mapped(
    struct wl_listener *listener_ptr, void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, window_mapped_listener);
    // Remembered from the start: The tiling order follows the mapping order.
    _win(zones_ptr, data_ptr, true);
    _wlmaker_zones_tile_mark(zones_ptr);
}

/** A window was unmapped: Forget it. */
static void _wlmaker_zones_handle_window_unmapped(
    struct wl_listener *listener_ptr, void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, window_unmapped_listener);
    zones_window_t *w = _win(zones_ptr, data_ptr, false);
    if (NULL != w) w->window_ptr = NULL;
    if (zones_ptr->drag_window_ptr == data_ptr) {
        zones_ptr->drag_window_ptr = NULL;
        _wlmaker_zones_overlay_hide(zones_ptr);
    }
    _wlmaker_zones_tile_mark(zones_ptr);
}

/** The workspace changed. */
static void _wlmaker_zones_handle_workspace_changed(
    struct wl_listener *listener_ptr, __UNUSED__ void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, workspace_changed_listener);
    _wlmaker_zones_tile_mark(zones_ptr);
}

/* == Commands ============================================================= */

/** @return The activated window of the current workspace. */
static wlmtk_window_t *_active(wlmaker_zones_t *zones_ptr)
{
    return wlmtk_workspace_get_activated_window(
        wlmtk_desktop_get_current_workspace(zones_ptr->server_ptr->desktop_ptr));
}

/** Parses on/off/toggle. */
static bool _onoff(const char *arg_ptr, bool cur)
{
    if (NULL == arg_ptr || 0 == strcmp(arg_ptr, "toggle")) return !cur;
    if (0 == strcmp(arg_ptr, "on") || 0 == strcmp(arg_ptr, "true")) return true;
    if (0 == strcmp(arg_ptr, "off") || 0 == strcmp(arg_ptr, "false")) return false;
    return cur;
}

/** Saves a key of the shared configuration file. */
static void _save(wlmaker_zones_t *zones_ptr, const char *sec_ptr,
                  const char *key_ptr, const char *value_ptr)
{
    if (0 != wz_ini_set(zones_ptr->cfg_path_ptr, sec_ptr, key_ptr, value_ptr)) {
        bs_log(BS_WARNING, "zones: Failed to write %s", zones_ptr->cfg_path_ptr);
    }
}

/** Snaps the window to zone `n` of its output. */
static void _snap_to(wlmaker_zones_t *zones_ptr, wlmtk_window_t *window_ptr,
                     struct wlr_output *output_ptr, int n)
{
    wz_rect z[ZONES_MAX];
    int k = _zones_for(zones_ptr, output_ptr, NULL, z, NULL);
    if (0 <= n && n < k) _snap(zones_ptr, window_ptr, z[n], n);
}

/** Moves the active window to the neighbouring zone (or output). */
static void _move(wlmaker_zones_t *zones_ptr, int dir)
{
    wlmtk_window_t *window_ptr = _active(zones_ptr);
    if (NULL == window_ptr) return;
    struct wlr_output *o = wlmtk_window_get_wlr_output(window_ptr);
    wz_rect z[ZONES_MAX];
    int k = _zones_for(zones_ptr, o, NULL, z, NULL);
    if (0 >= k) return;
    struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    int from = (NULL != w && 0 <= w->zone && w->zone < k) ? w->zone : -1;
    if (0 > from) {
        _snap_to(zones_ptr, window_ptr, o,
                 wz_zone_at(z, k, b.x + b.width / 2, b.y + b.height / 2));
        return;
    }
    int nb = wz_neighbor(z, k, from, dir);
    if (0 <= nb) {
        _snap_to(zones_ptr, window_ptr, o, nb);
        return;
    }
    // Next output in that direction.
    struct wlr_box ob;
    wlr_output_layout_get_box(zones_ptr->server_ptr->wlr_output_layout_ptr, o, &ob);
    double px = ob.x + ob.width / 2.0, py = ob.y + ob.height / 2.0;
    if (WZ_LEFT == dir) px = ob.x - 1;
    if (WZ_RIGHT == dir) px = ob.x + ob.width + 1;
    if (WZ_UP == dir) py = ob.y - 1;
    if (WZ_DOWN == dir) py = ob.y + ob.height + 1;
    struct wlr_output *next = wlr_output_layout_output_at(
        zones_ptr->server_ptr->wlr_output_layout_ptr, px, py);
    if (NULL == next || next == o) return;
    k = _zones_for(zones_ptr, next, NULL, z, NULL);
    if (0 < k) _snap_to(zones_ptr, window_ptr, next, wz_zone_at(z, k, (int)px, (int)py));
}

/** Cycles the general layout. */
static void _cycle_layout(wlmaker_zones_t *zones_ptr, int step)
{
    int idx = 0;
    for (int i = 0; i < zones_ptr->cfg.nlayouts; ++i) {
        if (0 == strcmp(zones_ptr->cfg.layouts[i].id, zones_ptr->cfg.layout)) idx = i;
    }
    idx = (idx + step + zones_ptr->cfg.nlayouts) % zones_ptr->cfg.nlayouts;
    free(zones_ptr->cfg.layout);
    zones_ptr->cfg.layout = strdup(zones_ptr->cfg.layouts[idx].id);
    _save(zones_ptr, "General", "Layout", zones_ptr->cfg.layout);
}

/** Hides a preview. */
static int _wlmaker_zones_preview_done(void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    if (NULL == zones_ptr->drag_window_ptr) _wlmaker_zones_overlay_hide(zones_ptr);
    return 0;
}

/** @return WZ_LEFT... from a word, -1 if unknown. */
static int _dir(const char *s)
{
    if (NULL == s) return -1;
    if (0 == strcmp(s, "left")) return WZ_LEFT;
    if (0 == strcmp(s, "right")) return WZ_RIGHT;
    if (0 == strcmp(s, "up")) return WZ_UP;
    if (0 == strcmp(s, "down")) return WZ_DOWN;
    return -1;
}

/* ------------------------------------------------------------------------- */
void wlmaker_zones_command(wlmaker_zones_t *zones_ptr,
                           const char *cmd_ptr,
                           char *out_ptr,
                           size_t out_len)
{
    if (NULL != out_ptr && 0 < out_len) out_ptr[0] = '\0';
    if (NULL == zones_ptr || NULL == cmd_ptr) return;
    char buf[WZ_IPC_MAX];
    snprintf(buf, sizeof(buf), "%s", cmd_ptr);
    char *nl = strpbrk(buf, "\r\n");
    if (NULL != nl) *nl = '\0';
    char *arg_ptr = strchr(buf, ' ');
    if (NULL != arg_ptr) *arg_ptr++ = '\0';

    if (0 == strcmp(buf, "ping")) {
        _out(out_ptr, out_len, "pong\n");
    } else if (0 == strcmp(buf, "status")) {
        _out(out_ptr, out_len, "backend=wlmaker\nzones=%s\nautotile=%s\n"
             "layout=%s\nalgorithm=%s\ndesktop=%d\n",
             zones_ptr->cfg.zones ? "on" : "off",
             zones_ptr->cfg.autotile ? "on" : "off", zones_ptr->cfg.layout,
             wz_algo_name(zones_ptr->cfg.tile.algo), _workspace_number(zones_ptr));
        struct wlr_output_layout_output *lo;
        wl_list_for_each(lo, &zones_ptr->server_ptr->wlr_output_layout_ptr->outputs, link) {
            struct wlr_box b, u;
            wlr_output_layout_get_box(zones_ptr->server_ptr->wlr_output_layout_ptr,
                                      lo->output, &b);
            u = wlmtk_desktop_get_usable_area(zones_ptr->server_ptr->desktop_ptr,
                                              lo->output);
            const wz_layout_def *l = wz_config_layout_for(
                &zones_ptr->cfg, lo->output->name, _workspace_number(zones_ptr));
            _out(out_ptr, out_len,
                 "monitor=%s %dx%d+%d+%d work=%dx%d+%d+%d layout=%s autotile=%s\n",
                 lo->output->name, b.width, b.height, b.x, b.y, u.width, u.height,
                 u.x, u.y, NULL != l ? l->id : "?",
                 wz_config_autotile_for(&zones_ptr->cfg, lo->output->name,
                                        _workspace_number(zones_ptr)) ? "on" : "off");
        }
        int tiled = 0, snapped = 0;
        for (int i = 0; i < WINDOWS_MAX; ++i) {
            if (NULL == zones_ptr->windows[i].window_ptr) continue;
            tiled += zones_ptr->windows[i].tiled;
            snapped += !zones_ptr->windows[i].tiled && 0 <= zones_ptr->windows[i].zone;
        }
        _out(out_ptr, out_len, "tiled=%d snapped=%d\n", tiled, snapped);
    } else if (0 == strcmp(buf, "zones")) {
        zones_ptr->cfg.zones = _onoff(arg_ptr, zones_ptr->cfg.zones);
        _save(zones_ptr, "General", "Zones", zones_ptr->cfg.zones ? "true" : "false");
        _out(out_ptr, out_len, "zones=%s\n", zones_ptr->cfg.zones ? "on" : "off");
    } else if (0 == strcmp(buf, "autotile")) {
        bool v = _onoff(arg_ptr, zones_ptr->cfg.autotile);
        zones_ptr->cfg.autotile = v;
        _save(zones_ptr, "General", "Autotile", v ? "true" : "false");
        if (!v) _wlmaker_zones_untile_all(zones_ptr);
        _wlmaker_zones_tile_mark(zones_ptr);
        _out(out_ptr, out_len, "autotile=%s\n", v ? "on" : "off");
    } else if (0 == strcmp(buf, "algorithm") && NULL != arg_ptr &&
               0 <= wz_algo_from_name(arg_ptr)) {
        zones_ptr->cfg.tile.algo = wz_algo_from_name(arg_ptr);
        _save(zones_ptr, "Tiling", "Algorithm", arg_ptr);
        _wlmaker_zones_tile_mark(zones_ptr);
    } else if (0 == strcmp(buf, "layout") && NULL != arg_ptr) {
        if (0 == strcmp(arg_ptr, "next")) {
            _cycle_layout(zones_ptr, 1);
        } else if (0 == strcmp(arg_ptr, "prev")) {
            _cycle_layout(zones_ptr, -1);
        } else if (NULL != wz_config_layout(&zones_ptr->cfg, arg_ptr)) {
            free(zones_ptr->cfg.layout);
            zones_ptr->cfg.layout = strdup(arg_ptr);
            _save(zones_ptr, "General", "Layout", arg_ptr);
        } else {
            _out(out_ptr, out_len, "error=layout desconhecido\n");
        }
    } else if (0 == strcmp(buf, "windows")) {
        // Class (app_id is not kept by the toolkit yet), title and box.
        wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
            zones_ptr->server_ptr->desktop_ptr);
        bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
        for (bs_dllist_node_t *d = list_ptr->head_ptr; NULL != d; d = d->next_ptr) {
            wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(d);
            struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
            const char *title_ptr = wlmtk_window_get_title(window_ptr);
            zones_window_t *w = _win(zones_ptr, window_ptr, false);
            _out(out_ptr, out_len, "\t\t%s\t%d,%d,%d,%d\tzone=%d tiled=%d\n",
                 NULL != title_ptr ? title_ptr : "", b.x, b.y, b.width, b.height,
                 NULL != w ? w->zone : -1, NULL != w ? w->tiled : 0);
        }
    } else if (0 == strcmp(buf, "layouts")) {
        for (int i = 0; i < zones_ptr->cfg.nlayouts; ++i) {
            const wz_layout_def *l = &zones_ptr->cfg.layouts[i];
            _out(out_ptr, out_len, "%s\t%s\t%s\n", l->id,
                 NULL != l->label ? l->label : wz_builtin_label(l->id), l->tree);
        }
    } else if (0 == strcmp(buf, "snap") && NULL != arg_ptr) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        if (NULL != window_ptr) {
            _snap_to(zones_ptr, window_ptr, wlmtk_window_get_wlr_output(window_ptr),
                     atoi(arg_ptr) - 1);
        }
    } else if ((0 == strcmp(buf, "move") || 0 == strcmp(buf, "swap")) &&
               0 <= _dir(arg_ptr)) {
        _move(zones_ptr, _dir(arg_ptr));
    } else if (0 == strcmp(buf, "restore")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = NULL != window_ptr ? _win(zones_ptr, window_ptr, false) : NULL;
        if (NULL != w && w->tiled) {
            w->user_float = true;
            _wlmaker_zones_tile_run(zones_ptr);
        } else if (NULL != w && w->has_restore) {
            wlmtk_window_request_bounding_box(window_ptr, &w->restore);
            w->has_restore = false;
            w->zone = -1;
        }
    } else if (0 == strcmp(buf, "float")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = NULL != window_ptr ? _win(zones_ptr, window_ptr, true) : NULL;
        if (NULL != w) {
            w->user_float = !w->user_float;
            if (!w->user_float) w->order = ++zones_ptr->order_seq;
            _wlmaker_zones_tile_run(zones_ptr);
        }
    } else if (0 == strcmp(buf, "promote")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = NULL != window_ptr ? _win(zones_ptr, window_ptr, false) : NULL;
        if (NULL != w && w->tiled) {
            long min = w->order;
            for (int i = 0; i < WINDOWS_MAX; ++i) {
                zones_window_t *o = &zones_ptr->windows[i];
                if (NULL != o->window_ptr && o->tiled && o->order < min) min = o->order;
            }
            w->order = min - 1;
            _wlmaker_zones_tile_mark(zones_ptr);
        }
    } else if (0 == strcmp(buf, "grow") || 0 == strcmp(buf, "shrink")) {
        double r = zones_ptr->cfg.tile.master_ratio + (0 == strcmp(buf, "grow") ? 0.05 : -0.05);
        zones_ptr->cfg.tile.master_ratio = BS_MAX(0.1, BS_MIN(0.9, r));
        _wlmaker_zones_tile_mark(zones_ptr);
    } else if (0 == strcmp(buf, "preview") && NULL != arg_ptr) {
        if (NULL == zones_ptr->drag_window_ptr) {
            _wlmaker_zones_overlay_show(
                zones_ptr, wlmaker_server_get_output_at_cursor(zones_ptr->server_ptr),
                arg_ptr);
            if (NULL != zones_ptr->preview_timer_ptr) {
                wl_event_source_timer_update(zones_ptr->preview_timer_ptr, 1800);
            }
        }
    } else if (0 == strcmp(buf, "reload")) {
        wlmaker_zones_reload(zones_ptr);
    } else if (0 == strcmp(buf, "editor")) {
        wlm_util_subprocess_monitor_run(zones_ptr->server_ptr->monitor_ptr,
            bs_subprocess_create_cmdline("wmake-zones-prefs --editor"));
    } else if (0 == strcmp(buf, "prefs")) {
        wlm_util_subprocess_monitor_run(zones_ptr->server_ptr->monitor_ptr,
            bs_subprocess_create_cmdline("wmake-zones-prefs"));
    } else if (0 == strcmp(buf, "quit")) {
        // The compositor stays; nothing to do.
    } else {
        _out(out_ptr, out_len, "error=comando desconhecido: %s\n", buf);
    }
}

/* == Socket =============================================================== */

/** A client connected: One command, one reply. */
static int _wlmaker_zones_handle_listen(int fd, __UNUSED__ uint32_t mask, void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (0 > c) return 0;
    if (!wz_ipc_peer_ok(c)) {
        close(c);
        return 0;
    }
    struct timeval tv = { 0, 300000 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char line[WZ_IPC_MAX];
    ssize_t n = recv(c, line, sizeof(line) - 1, 0);
    if (0 < n) {
        line[n] = '\0';
        static char out[16384];
        wlmaker_zones_command(zones_ptr, line, out, sizeof(out));
        size_t l = strlen(out);
        if (0 == l) strcpy(out, "ok\n");
        strncat(out, "\n", sizeof(out) - strlen(out) - 1);
        (void)!send(c, out, strlen(out), MSG_NOSIGNAL);
    }
    close(c);
    return 0;
}

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_zones_t *wlmaker_zones_create(wlmaker_server_t *server_ptr)
{
    wlmaker_zones_t *zones_ptr = logged_calloc(1, sizeof(wlmaker_zones_t));
    if (NULL == zones_ptr) return NULL;
    zones_ptr->server_ptr = server_ptr;
    zones_ptr->listen_fd = -1;
    zones_ptr->hl_a = zones_ptr->hl_b = -1;
    zones_ptr->cfg_path_ptr = wz_config_path();
    wz_config_defaults(&zones_ptr->cfg);
    wz_config_load(&zones_ptr->cfg, zones_ptr->cfg_path_ptr);
    wz_theme_load(&zones_ptr->theme, NULL);

    zones_ptr->esc_combo = (struct wlmim_keybinding_combo){
        .keysym = XKB_KEY_Escape, .modifiers = 0,
        .modifiers_mask = wlmim_modifiers_default_mask, .ignore_case = true };

    if (!wlmtk_panel_init(&zones_ptr->super_panel, &_wlmaker_zones_positioning)) {
        wlmaker_zones_destroy(zones_ptr);
        return NULL;
    }
    wlmtk_panel_extend(&zones_ptr->super_panel, &_wlmaker_zones_panel_vmt);
    wlmtk_element_set_visible(wlmtk_panel_element(&zones_ptr->super_panel), true);
    if (!wlmtk_buffer_init(&zones_ptr->buffer)) {
        wlmaker_zones_destroy(zones_ptr);
        return NULL;
    }
    wlmtk_element_vmt_t vmt = {
        .pointer_accepts_motion = _wlmaker_zones_buffer_accepts_motion };
    zones_ptr->orig_buffer_vmt = wlmtk_element_extend(
        wlmtk_buffer_element(&zones_ptr->buffer), &vmt);
    wlmtk_element_set_visible(wlmtk_buffer_element(&zones_ptr->buffer), true);
    wlmtk_container_add_element(&zones_ptr->super_panel.super_container,
                                wlmtk_buffer_element(&zones_ptr->buffer));

    wlmtk_desktop_events_t *e = wlmtk_desktop_events(server_ptr->desktop_ptr);
    wlmtk_util_connect_listener_signal(&e->window_move_begin,
        &zones_ptr->move_begin_listener, _wlmaker_zones_handle_move_begin);
    wlmtk_util_connect_listener_signal(&e->window_move_motion,
        &zones_ptr->move_motion_listener, _wlmaker_zones_handle_move_motion);
    wlmtk_util_connect_listener_signal(&e->window_move_end,
        &zones_ptr->move_end_listener, _wlmaker_zones_handle_move_end);
    wlmtk_util_connect_listener_signal(&e->window_mapped,
        &zones_ptr->window_mapped_listener, _wlmaker_zones_handle_window_mapped);
    wlmtk_util_connect_listener_signal(&e->window_unmapped,
        &zones_ptr->window_unmapped_listener, _wlmaker_zones_handle_window_unmapped);
    wlmtk_util_connect_listener_signal(&e->workspace_changed,
        &zones_ptr->workspace_changed_listener, _wlmaker_zones_handle_workspace_changed);

    struct wl_event_loop *loop_ptr = wl_display_get_event_loop(server_ptr->wl_display_ptr);
    zones_ptr->tile_timer_ptr = wl_event_loop_add_timer(
        loop_ptr, _wlmaker_zones_tile_run, zones_ptr);
    zones_ptr->preview_timer_ptr = wl_event_loop_add_timer(
        loop_ptr, _wlmaker_zones_preview_done, zones_ptr);

    // Control socket: $XDG_RUNTIME_DIR/wmake-zones/wayland-<display>.sock.
    // wlmaker sets WAYLAND_DISPLAY just before running; build the path
    // from the server's socket name.
    if (NULL != server_ptr->wl_socket_name_ptr) {
        setenv("WAYLAND_DISPLAY", server_ptr->wl_socket_name_ptr, true);
    }
    zones_ptr->sock_path_ptr = wz_ipc_path("wayland");
    if (NULL != zones_ptr->sock_path_ptr) {
        zones_ptr->listen_fd = wz_ipc_server(zones_ptr->sock_path_ptr);
        if (0 <= zones_ptr->listen_fd) {
            zones_ptr->listen_source_ptr = wl_event_loop_add_fd(
                loop_ptr, zones_ptr->listen_fd, WL_EVENT_READABLE,
                _wlmaker_zones_handle_listen, zones_ptr);
        } else {
            bs_log(BS_WARNING, "zones: No control socket at %s",
                   zones_ptr->sock_path_ptr);
        }
    }
    _wlmaker_zones_tile_mark(zones_ptr);
    return zones_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_zones_destroy(wlmaker_zones_t *zones_ptr)
{
    if (NULL == zones_ptr) return;
    _wlmaker_zones_overlay_hide(zones_ptr);
    if (NULL != zones_ptr->esc_binding_ptr) {
        wlmim_unbind_key(zones_ptr->server_ptr->input_manager_ptr,
                         zones_ptr->esc_binding_ptr);
    }
    if (NULL != zones_ptr->listen_source_ptr) {
        wl_event_source_remove(zones_ptr->listen_source_ptr);
    }
    if (0 <= zones_ptr->listen_fd) close(zones_ptr->listen_fd);
    if (NULL != zones_ptr->sock_path_ptr) {
        if (0 <= zones_ptr->listen_fd) unlink(zones_ptr->sock_path_ptr);
        free(zones_ptr->sock_path_ptr);
    }
    if (NULL != zones_ptr->tile_timer_ptr) wl_event_source_remove(zones_ptr->tile_timer_ptr);
    if (NULL != zones_ptr->preview_timer_ptr) {
        wl_event_source_remove(zones_ptr->preview_timer_ptr);
    }
    wlmtk_util_disconnect_listener(&zones_ptr->workspace_changed_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->window_unmapped_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->window_mapped_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->move_end_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->move_motion_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->move_begin_listener);
    if (NULL != wlmtk_buffer_element(&zones_ptr->buffer)->parent_container_ptr) {
        wlmtk_container_remove_element(&zones_ptr->super_panel.super_container,
                                       wlmtk_buffer_element(&zones_ptr->buffer));
    }
    wlmtk_buffer_fini(&zones_ptr->buffer);
    wlmtk_panel_fini(&zones_ptr->super_panel);
    for (int i = 0; i < ZONES_MAX; ++i) free(zones_ptr->names[i]);
    wz_config_free(&zones_ptr->cfg);
    free(zones_ptr->cfg_path_ptr);
    free(zones_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_zones_reload(wlmaker_zones_t *zones_ptr)
{
    if (NULL == zones_ptr) return;
    bool was = zones_ptr->cfg.autotile;
    wz_config c;
    wz_config_defaults(&c);
    wz_config_load(&c, zones_ptr->cfg_path_ptr);
    wz_config_free(&zones_ptr->cfg);
    zones_ptr->cfg = c;
    wz_theme_load(&zones_ptr->theme, NULL);
    if (was && !zones_ptr->cfg.autotile) _wlmaker_zones_untile_all(zones_ptr);
    _wlmaker_zones_tile_mark(zones_ptr);
}

#endif  // WLMAKER_HAVE_ZONES

/* == End of zones.c ======================================================= */
