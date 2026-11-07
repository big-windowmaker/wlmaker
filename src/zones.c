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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "i18n.h"
#include "input/manager.h"
#include "server.h"
#include "toolkit/toolkit.h"

#include "wz-config.h"
#include "wz-history.h"
#include "wz-ipc.h"
#include "wz-layout.h"
#include "wz-theme.h"
#include "wz-tile.h"

/* == Declarations ========================================================= */

/** Maximum number of zones shown. */
#define ZONES_MAX 128
/** Windows remembered (restore geometry, tiling). */
#define WINDOWS_MAX 256
/** Tiling sets (output and workspace), each with a BSP tree and a ratio. */
#define TSETS_MAX 32
/** Layouts in the panel shown while dragging. */
#define STRIP_MAX 12

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
    /** Per-application rules were looked up. */
    bool                      rule_done;
    /** Attempts to find the app ID before applying the rules anyway. */
    int                       rule_tries;
    /** Rule: Floats (no tiling). */
    bool                      rule_float;
    /** Rule: Left alone by zones and tiling. */
    bool                      rule_ignore;
    /** Did not fit its cell in the last tiling run: floats meanwhile. */
    bool                      fit_float;
    /** Same, in the run before (the log line comes once). */
    bool                      fit_prev;
    /** Minimum box that did not fit (for "windows -d"). */
    int                       fit_min_w;
    /** See @ref zones_window_t::fit_min_w. */
    int                       fit_min_h;
    /** The cell it did not fit. */
    int                       fit_cell_w;
    /** See @ref zones_window_t::fit_cell_w. */
    int                       fit_cell_h;
} zones_window_t;

/** Tiling set: One output on one workspace. */
typedef struct {
    /** Whether the slot is in use. */
    bool                      used;
    /** Output name. */
    char                      output[64];
    /** Workspace, from 1. */
    int                       workspace;
    /** Persistent BSP tree; leaf IDs are window pointers. */
    wz_bsp                    bsp;
    /** Ratio of the main area. */
    double                    ratio;
} zones_tset_t;

/** What the overlay shows. */
enum { OV_ZONES = 0, OV_TILE, OV_PICKER, OV_PREVIEW };

/** State of the zones. */
struct zones_conn;

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
    /** What the overlay shows, see OV_ZONES... */
    int                       ov_mode;
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

    /** Layout panel of the drag: shown. */
    bool                      strip;
    /** Number of layouts in the panel. */
    int                       strip_n;
    /** Their IDs. */
    char                      *strip_id[STRIP_MAX];
    /** Their trees. */
    char                      *strip_tree[STRIP_MAX];
    /** Their names, as shown. */
    char                      *strip_label[STRIP_MAX];
    /** Panel box, layout coordinates. */
    wz_rect                   strip_box;
    /** Cells of the panel. */
    wz_rect                   strip_cell[STRIP_MAX];
    /** Thumbnails in the cells. */
    wz_rect                   strip_thumb[STRIP_MAX];
    /** Height of the panel's title bar. */
    int                       strip_title_h;
    /** Thumbnail under the pointer, -1 if none. */
    int                       strip_hover;
    /** Zone of that thumbnail under the pointer, -1 if none. */
    int                       strip_mini;
    /** Chosen layout, -1 if none yet. */
    int                       strip_sel;
    /** The panel appeared under the pointer: counts once it left. */
    bool                      strip_wait_leave;
    /** Panel by the top edge: Stays until the drag ends. */
    bool                      strip_latched;
    /** Layout chosen in the panel (ID), NULL if none. */
    char                      *drag_layout_ptr;
    /** Its tree. */
    char                      *drag_tree_ptr;
    /** Tree of the zones shown, to tell when they change. */
    char                      *shown_tree_ptr;
    /** Timer: Picks the thumbnail under the pointer after a while. */
    struct wl_event_source    *dwell_timer_ptr;

    /** Window being dragged. */
    wlmtk_window_t            *drag_window_ptr;
    /** Its bounding box when the drag started. */
    struct wlr_box            drag_start;
    /** Last pointer position of the drag, layout coordinates. */
    double                    drag_x;
    /** Last pointer position of the drag, layout coordinates. */
    double                    drag_y;
    /** Escape was pressed during the drag. */
    bool                      cancelled;
    /** Temporary binding of Escape while dragging. */
    wlmim_keybinding_t        *esc_binding_ptr;
    /** Combo of that binding. */
    struct wlmim_keybinding_combo esc_combo;

    /** The zone picker (Super+Z) is open. */
    bool                      picker;
    /** Window the picker places. */
    wlmtk_window_t            *picker_window_ptr;
    /** Selected zone of the picker. */
    int                       picker_sel;

    /** Remembered windows. */
    zones_window_t            windows[WINDOWS_MAX];
    /** Counter for the tiling order. */
    long                      order_seq;
    /** Tiling sets. */
    zones_tset_t              tsets[TSETS_MAX];
    /** Undo and redo. */
    wz_history                history;
    /** Window whose interactive resize ended, read when the timer fires. */
    wlmtk_window_t            *resized_window_ptr;

    /** Listening socket. */
    int                       listen_fd;
    /** Its event source. */
    struct wl_event_source    *listen_source_ptr;
    /** Path of the socket. */
    char                      *sock_path_ptr;
    /** The socket file this instance created (unlink only that one). */
    struct stat               sock_stat;
    /** Clients being read, without ever blocking the compositor. */
    struct zones_conn         *conn_ptr[8];
    /** Timer: Relayout of the tiling, coalesced. */
    struct wl_event_source    *tile_timer_ptr;
    /** Timer: Hides a preview. */
    struct wl_event_source    *preview_timer_ptr;
    /** Timer: Reads the size of a window after an interactive resize. */
    struct wl_event_source    *resize_timer_ptr;

    /** Listener for @ref wlmtk_desktop_events_t::window_move_begin. */
    struct wl_listener        move_begin_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_move_motion. */
    struct wl_listener        move_motion_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_move_end. */
    struct wl_listener        move_end_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_resize_end. */
    struct wl_listener        resize_end_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
    /** Listener for @ref wlmtk_desktop_events_t::window_unmapped. */
    struct wl_listener        window_unmapped_listener;
    /** Listener for @ref wlmtk_desktop_events_t::workspace_changed. */
    struct wl_listener        workspace_changed_listener;
    /** Listener for @ref wlmtk_desktop_events_t::usable_area_changed. */
    struct wl_listener        usable_area_listener;
    /** Listener for wlr_output_layout::events.change (hotplug, mode, scale). */
    struct wl_listener        output_layout_change_listener;
};

static void _wlmaker_zones_overlay_hide(wlmaker_zones_t *zones_ptr);
static void _wlmaker_zones_redraw(wlmaker_zones_t *zones_ptr);
static void _wlmaker_zones_tile_mark(wlmaker_zones_t *zones_ptr);
static int _wlmaker_zones_tile_run(void *data_ptr);
static void _picker_close(wlmaker_zones_t *zones_ptr);
static void _drag_update(wlmaker_zones_t *zones_ptr);

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

/** Replaces `*dest_ptr_ptr` by a copy of `src_ptr` (may be NULL). */
static void _set_str(char **dest_ptr_ptr, const char *src_ptr)
{
    free(*dest_ptr_ptr);
    *dest_ptr_ptr = NULL != src_ptr ? strdup(src_ptr) : NULL;
}

/** @return The remembered state of the window, creating it if asked. */
static zones_window_t *_win(wlmaker_zones_t *zones_ptr,
                            wlmtk_window_t *window_ptr,
                            bool create)
{
    if (NULL == window_ptr) return NULL;
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

/** @return The output at the layout position, or the one of the cursor. */
static struct wlr_output *_output_at(wlmaker_zones_t *zones_ptr,
                                     double x, double y)
{
    struct wlr_output *o = wlr_output_layout_output_at(
        zones_ptr->server_ptr->wlr_output_layout_ptr, x, y);
    return NULL != o ? o : wlmaker_server_get_output_at_cursor(
        zones_ptr->server_ptr);
}

/** @return Layout box of the output. */
static struct wlr_box _output_box(wlmaker_zones_t *zones_ptr,
                                  struct wlr_output *output_ptr)
{
    struct wlr_box b = {};
    wlr_output_layout_get_box(zones_ptr->server_ptr->wlr_output_layout_ptr,
                              output_ptr, &b);
    return b;
}

/**
 * @return 1-based index of the current workspace, as in the configuration's
 * [Workspace N]. The toolkit already numbers workspaces from 1 (the index is
 * the list size once the workspace is added).
 */
static int _workspace_number(wlmaker_zones_t *zones_ptr)
{
    const char *name_ptr;
    int index = 1;
    wlmtk_workspace_get_details(
        wlmtk_desktop_get_current_workspace(zones_ptr->server_ptr->desktop_ptr),
        &name_ptr, &index);
    return index < 1 ? 1 : index;
}

/** @return The layout in effect on the output (or picked by its shape). */
static const wz_layout_def *_layout_of(wlmaker_zones_t *zones_ptr,
                                       struct wlr_output *output_ptr)
{
    if (NULL == output_ptr) return NULL;
    struct wlr_box b = _output_box(zones_ptr, output_ptr);
    return wz_config_layout_for_size(&zones_ptr->cfg, output_ptr->name,
                                     _workspace_number(zones_ptr),
                                     b.width, b.height);
}

/** @return Whether automatic tiling is on for the output. */
static bool _autotile_on(wlmaker_zones_t *zones_ptr,
                         struct wlr_output *output_ptr)
{
    return NULL != output_ptr && wz_config_autotile_for(
        &zones_ptr->cfg, output_ptr->name, _workspace_number(zones_ptr));
}

/** Computes the zones of `tree_ptr` in the usable area of `output_ptr`. */
static int _zones_of_tree(wlmaker_zones_t *zones_ptr,
                          struct wlr_output *output_ptr,
                          const char *tree_ptr,
                          wz_rect *out_ptr,
                          char **names_ptr)
{
    if (NULL == output_ptr || NULL == tree_ptr) return 0;
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

/** Computes the zones of the layout effective on `output_ptr`. */
static int _zones_for(wlmaker_zones_t *zones_ptr,
                      struct wlr_output *output_ptr,
                      wz_rect *out_ptr,
                      char **names_ptr)
{
    const wz_layout_def *l = _layout_of(zones_ptr, output_ptr);
    return NULL != l ? _zones_of_tree(zones_ptr, output_ptr, l->tree,
                                      out_ptr, names_ptr) : 0;
}

/** @return Whether zones may place the window (not a dialog, no rule). */
static bool _snappable(wlmaker_zones_t *zones_ptr, wlmtk_window_t *window_ptr)
{
    if (NULL == window_ptr || wlmtk_window_is_fullscreen(window_ptr) ||
        wlmtk_window_is_dialog(window_ptr)) return false;
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    return NULL == w || !w->rule_ignore;
}

/** Snaps the window to a box (layout coordinates, with decorations). */
static void _snap(wlmaker_zones_t *zones_ptr,
                  wlmtk_window_t *window_ptr,
                  wz_rect target,
                  int zone)
{
    if (!_snappable(zones_ptr, window_ptr)) return;
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

/** Saves a key of the shared configuration file. */
static void _save(wlmaker_zones_t *zones_ptr, const char *sec_ptr,
                  const char *key_ptr, const char *value_ptr)
{
    if (0 != wz_ini_set(zones_ptr->cfg_path_ptr, sec_ptr, key_ptr, value_ptr)) {
        bs_log(BS_WARNING, "zones: Failed to write %s", zones_ptr->cfg_path_ptr);
    }
}

/** Argument for @ref _cfg_get_cb. */
struct _cfg_get {
    /** Section looked for. */
    const char                *section_ptr;
    /** Key looked for. */
    const char                *key_ptr;
    /** Its value (malloc), NULL if absent. */
    char                      *value_ptr;
};

/** Callback of @ref _cfg_get. */
static void _cfg_get_cb(const char *section_ptr, const char *key_ptr,
                        const char *value_ptr, void *ud_ptr)
{
    struct _cfg_get *g = ud_ptr;
    if (0 == strcmp(section_ptr, g->section_ptr) &&
        0 == strcmp(key_ptr, g->key_ptr)) {
        _set_str(&g->value_ptr, value_ptr);
    }
}

/** @return The value of a key in the file (malloc), NULL if absent. */
static char *_cfg_get(wlmaker_zones_t *zones_ptr, const char *section_ptr,
                      const char *key_ptr)
{
    struct _cfg_get g = { section_ptr, key_ptr, NULL };
    wz_ini_parse_file(zones_ptr->cfg_path_ptr, _cfg_get_cb, &g);
    return g.value_ptr;
}

/* == Tiling sets ========================================================== */

/** @return The tiling set of the output on the current workspace. */
static zones_tset_t *_tset(wlmaker_zones_t *zones_ptr,
                           struct wlr_output *output_ptr,
                           bool create)
{
    if (NULL == output_ptr) return NULL;
    int ws = _workspace_number(zones_ptr);
    for (int i = 0; i < TSETS_MAX; ++i) {
        zones_tset_t *s = &zones_ptr->tsets[i];
        if (s->used && s->workspace == ws &&
            0 == strcmp(s->output, output_ptr->name)) return s;
    }
    if (!create) return NULL;
    for (int i = 0; i < TSETS_MAX; ++i) {
        zones_tset_t *s = &zones_ptr->tsets[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = true;
        snprintf(s->output, sizeof(s->output), "%s", output_ptr->name);
        s->workspace = ws;
        s->ratio = zones_ptr->cfg.tile.master_ratio;
        return s;
    }
    return NULL;
}

/** Removes the window from every BSP tree. */
static void _tsets_forget(wlmaker_zones_t *zones_ptr, wlmtk_window_t *window_ptr)
{
    for (int i = 0; i < TSETS_MAX; ++i) {
        if (zones_ptr->tsets[i].used) {
            wz_bsp_remove(&zones_ptr->tsets[i].bsp, (long)(uintptr_t)window_ptr);
        }
    }
}

/* == History ============================================================== */

/** Describes the window for the history. `box_ptr` overrides its box. */
static void _hist_item(wlmaker_zones_t *zones_ptr,
                       wlmtk_window_t *window_ptr,
                       const struct wlr_box *box_ptr,
                       wz_hist_item *it)
{
    memset(it, 0, sizeof(*it));
    struct wlr_box b = NULL != box_ptr ? *box_ptr :
        wlmtk_window_get_bounding_box(window_ptr);
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    it->win = (uintptr_t)window_ptr;
    it->r = (wz_rect){ b.x, b.y, b.width, b.height };
    it->zone = -1;
    if (NULL == w) return;
    it->zone = w->zone;
    it->order = w->order;
    it->flags = (w->tiled ? WZ_HIST_TILED : 0) |
        (w->user_float ? WZ_HIST_USER_FLOAT : 0);
    if (w->has_restore) {
        it->flags |= WZ_HIST_HAS_RESTORE;
        it->restore = (wz_rect){ w->restore.x, w->restore.y,
                                 w->restore.width, w->restore.height };
    }
}

/** Opens a history step. */
static wz_hist_entry *_hist_begin(wlmaker_zones_t *zones_ptr)
{
    wz_history_set_depth(&zones_ptr->history, zones_ptr->cfg.history_depth);
    return wz_history_begin(&zones_ptr->history);
}

/** Adds the window, as it is now (or with `box_ptr`), to the step. */
static void _hist_add(wlmaker_zones_t *zones_ptr, wz_hist_entry *e,
                      wlmtk_window_t *window_ptr,
                      const struct wlr_box *box_ptr)
{
    if (NULL == e || NULL == window_ptr) return;
    wz_hist_item it;
    _hist_item(zones_ptr, window_ptr, box_ptr, &it);
    wz_hist_add(e, &it);
}

/** Adds the current value of a configuration key to the step. */
static void _hist_add_cfg(wlmaker_zones_t *zones_ptr, wz_hist_entry *e,
                          const char *section_ptr, const char *key_ptr)
{
    char *before_ptr = _cfg_get(zones_ptr, section_ptr, key_ptr);
    wz_hist_add_cfg(e, section_ptr, key_ptr, before_ptr);
    free(before_ptr);
}

/** One step with these windows. */
static void _hist_push(wlmaker_zones_t *zones_ptr,
                       wlmtk_window_t **windows_ptr, int n)
{
    wz_hist_entry *e = _hist_begin(zones_ptr);
    for (int i = 0; i < n; ++i) _hist_add(zones_ptr, e, windows_ptr[i], NULL);
    wz_history_commit(&zones_ptr->history);
}

/** A step with every remembered window, plus up to three keys. */
static void _hist_push_all(wlmaker_zones_t *zones_ptr,
                           const char *s1, const char *k1,
                           const char *s2, const char *k2,
                           const char *s3, const char *k3)
{
    wz_hist_entry *e = _hist_begin(zones_ptr);
    if (NULL != s1) _hist_add_cfg(zones_ptr, e, s1, k1);
    if (NULL != s2) _hist_add_cfg(zones_ptr, e, s2, k2);
    if (NULL != s3) _hist_add_cfg(zones_ptr, e, s3, k3);
    for (int i = 0; i < WINDOWS_MAX; ++i) {
        if (NULL != zones_ptr->windows[i].window_ptr) {
            _hist_add(zones_ptr, e, zones_ptr->windows[i].window_ptr, NULL);
        }
    }
    wz_history_commit(&zones_ptr->history);
}

/** @return `r`, moved into an output if it lies on none (output removed). */
static struct wlr_box _reachable(wlmaker_zones_t *zones_ptr, wz_rect r)
{
    struct wlr_box b = { r.x, r.y, r.w, r.h };
    struct wlr_output_layout *l = zones_ptr->server_ptr->wlr_output_layout_ptr;
    if (NULL != wlr_output_layout_output_at(l, r.x + r.w / 2.0, r.y + r.h / 2.0)) {
        return b;
    }
    double cx, cy;
    wlr_output_layout_closest_point(l, NULL, r.x + r.w / 2.0, r.y + r.h / 2.0,
                                    &cx, &cy);
    struct wlr_output *o = wlr_output_layout_output_at(l, cx, cy);
    if (NULL == o) return b;
    struct wlr_box u = wlmtk_desktop_get_usable_area(
        zones_ptr->server_ptr->desktop_ptr, o);
    b.width = BS_MIN(b.width, u.width);
    b.height = BS_MIN(b.height, u.height);
    b.x = BS_MAX(u.x, BS_MIN(b.x, u.x + u.width - b.width));
    b.y = BS_MAX(u.y, BS_MIN(b.y, u.y + u.height - b.height));
    return b;
}

/** Undoes (redo = false) or redoes the last step. @return false if none. */
static bool _hist_step(wlmaker_zones_t *zones_ptr, bool redo)
{
    wz_hist_entry e;
    if (!wz_history_pop(&zones_ptr->history, redo, &e)) return false;
    wz_hist_entry *inv = wz_history_push_inverse(&zones_ptr->history, !redo);
    for (int i = 0; i < e.ncfg; ++i) {
        _hist_add_cfg(zones_ptr, inv, e.cfg[i].section, e.cfg[i].key);
        _save(zones_ptr, e.cfg[i].section, e.cfg[i].key, e.cfg[i].value);
    }
    if (0 < e.ncfg) wlmaker_zones_reload(zones_ptr);
    if (e.has_aux && 0 < e.n) {
        zones_window_t *w = _win(
            zones_ptr, (wlmtk_window_t*)e.items[0].win, false);
        zones_tset_t *s = NULL != w ? _tset(
            zones_ptr, wlmtk_window_get_wlr_output(w->window_ptr), true) : NULL;
        if (NULL != s) {
            wz_hist_set_aux(inv, lround(s->ratio * 10000));
            s->ratio = e.aux / 10000.0;
        }
    }
    for (int i = 0; i < e.n; ++i) {
        const wz_hist_item *it = &e.items[i];
        // Only windows still known: closed ones were forgotten.
        zones_window_t *w = _win(zones_ptr, (wlmtk_window_t*)it->win, false);
        if (NULL == w) continue;
        _hist_add(zones_ptr, inv, w->window_ptr, NULL);
        w->order = it->order;
        memset(&w->expect, 0, sizeof(w->expect));
        if (it->flags & WZ_HIST_TILED) {
            w->user_float = false;
            w->rule_float = false;
            continue;
        }
        struct wlr_box b = _reachable(zones_ptr, it->r);
        struct wlr_output *o = _output_at(zones_ptr, b.x + b.width / 2.0,
                                          b.y + b.height / 2.0);
        w->user_float = (it->flags & WZ_HIST_USER_FLOAT) ||
            (_autotile_on(zones_ptr, o) && !w->rule_float);
        if (w->tiled) {
            w->tiled = false;
            _tsets_forget(zones_ptr, w->window_ptr);
        }
        wlmtk_window_request_bounding_box(w->window_ptr, &b);
        w->zone = it->zone;
        w->has_restore = it->flags & WZ_HIST_HAS_RESTORE;
        w->restore = (struct wlr_box){ it->restore.x, it->restore.y,
                                       it->restore.w, it->restore.h };
    }
    wz_history_commit(&zones_ptr->history);
    wz_hist_entry_free(&e);
    _wlmaker_zones_tile_mark(zones_ptr);
    return true;
}

/* == Overlay ============================================================== */

/**
 * The overlay takes the pointer only while the picker is open (a click
 * picks a zone); during a drag, the drag continues below it.
 */
static bool _wlmaker_zones_buffer_accepts_motion(
    wlmtk_element_t *element_ptr,
    __UNUSED__ wlmtk_pointer_motion_event_t *motion_event_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_zones_t, buffer.super_element);
    return zones_ptr->picker;
}

static void _picker_apply(wlmaker_zones_t *zones_ptr, int idx);

/** A click on the picker: The zone under the pointer, or closes it. */
static bool _wlmaker_zones_buffer_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_zones_t, buffer.super_element);
    if (!zones_ptr->picker) return false;
    if (WLMTK_BUTTON_DOWN != button_event_ptr->type) return true;
    struct wlr_cursor *c = wlmim_wlr_cursor(zones_ptr->server_ptr->input_manager_ptr);
    for (int i = 0; i < zones_ptr->nzones; ++i) {
        if (wz_rect_contains(zones_ptr->zones[i], (int)c->x, (int)c->y)) {
            _picker_apply(zones_ptr, i);
            return true;
        }
    }
    _picker_close(zones_ptr);
    return true;
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

/** Bevel of a pressed (sunken) button. */
static void _sunken(cairo_t *cr, double x, double y, double w, double h)
{
    _rgb(cr, 0x000000, 1);
    cairo_rectangle(cr, x, y, w, 1);
    cairo_rectangle(cr, x, y, 1, h);
    cairo_fill(cr);
    _rgb(cr, 0x555555, 1);
    cairo_rectangle(cr, x + 1, y + 1, w - 2, 1);
    cairo_rectangle(cr, x + 1, y + 1, 1, h - 2);
    cairo_fill(cr);
    _rgb(cr, 0xffffff, 1);
    cairo_rectangle(cr, x, y + h - 1, w, 1);
    cairo_rectangle(cr, x + w - 1, y, 1, h);
    cairo_fill(cr);
}

/** Horizontal gradient. */
static void _hgrad(cairo_t *cr, double x, double y, double w, double h,
                   unsigned c1, unsigned c2)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x + w, y);
    cairo_pattern_add_color_stop_rgb(p, 0, (c1 >> 16 & 0xff) / 255.0,
                                     (c1 >> 8 & 0xff) / 255.0, (c1 & 0xff) / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, (c2 >> 16 & 0xff) / 255.0,
                                     (c2 >> 8 & 0xff) / 255.0, (c2 & 0xff) / 255.0);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
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

    if (zones_ptr->cfg.show_numbers || OV_PICKER == zones_ptr->ov_mode) {
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

/** Cuts the text (at UTF-8 characters) to fit `max_w`, with an ellipsis. */
static void _fit_text(cairo_t *cr, char *buf, size_t len, const char *src,
                      double max_w)
{
    snprintf(buf, len, "%s", src);
    cairo_text_extents_t te;
    cairo_text_extents(cr, buf, &te);
    while (strlen(buf) > 3 && te.x_advance > max_w) {
        size_t l = strlen(buf) - 1;
        while (l > 0 && (buf[l] & 0xc0) == 0x80) --l;
        buf[l] = '\0';
        if (l > 3) {
            size_t k = l - 1;
            while (k > 0 && (buf[k] & 0xc0) == 0x80) --k;
            snprintf(buf + k, len - k, "\xe2\x80\xa6");
        }
        cairo_text_extents(cr, buf, &te);
    }
}

/** Hint bar of the picker, as a Window Maker menu title. */
static void _draw_picker_hint(wlmaker_zones_t *zones_ptr, cairo_t *cr)
{
    // Words, not arrow glyphs: The cairo toy font has no fallback, and the
    // common "Sans" fonts may lack the arrows (seen as empty boxes).
    const char *msg_ptr = wlmaker_i18n(
        "1-9 or arrows to choose \xc2\xb7 Enter snaps \xc2\xb7 "
        "Tab changes the layout \xc2\xb7 Esc cancels");
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    cairo_text_extents_t te;
    cairo_text_extents(cr, msg_ptr, &te);
    double w = te.x_advance + 28, h = 24;
    double x = (zones_ptr->width - w) / 2, y = zones_ptr->height - h - 48;
    _hgrad(cr, x, y, w, h, zones_ptr->theme.title_bg1, zones_ptr->theme.title_bg2);
    _bevel(cr, x, y, w, h, 1, 0.9);
    _rgb(cr, zones_ptr->theme.title_fg, 1);
    cairo_move_to(cr, x + 14, y + h / 2 - te.height / 2 - te.y_bearing);
    cairo_show_text(cr, msg_ptr);
}

/** Zones of thumbnail `i` of the panel, layout coordinates. */
static int _strip_minis(wlmaker_zones_t *zones_ptr, int i, wz_rect *out_ptr,
                        int max)
{
    wz_node *t = wz_parse(zones_ptr->strip_tree[i], NULL);
    if (NULL == t) return 0;
    wz_rect b = zones_ptr->strip_thumb[i];
    int n = wz_compute(t, (wz_rect){ b.x + 3, b.y + 3, b.w - 6, b.h - 6 },
                       2, 0, out_ptr, max);
    wz_free(t);
    return BS_MIN(n, max);
}

/** Draws the layout panel of the drag. */
static void _draw_strip(wlmaker_zones_t *zones_ptr, cairo_t *cr)
{
    if (!zones_ptr->strip || 0 >= zones_ptr->strip_n) return;
    const wz_theme *th = &zones_ptr->theme;
    double ox = zones_ptr->output_box.x, oy = zones_ptr->output_box.y;
    wz_rect b = zones_ptr->strip_box;
    double bx = b.x - ox, by = b.y - oy;
    int tth = zones_ptr->strip_title_h;
    _hgrad(cr, bx, by, b.w, tth, th->title_bg1, th->title_bg2);
    _bevel(cr, bx, by, b.w, tth, 1, 1);
    char buf[160];
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    _fit_text(cr, buf, sizeof(buf),
              wlmaker_i18n("Window layout: choose a layout, then a zone"),
              b.w - 16);
    _rgb(cr, th->title_fg, 1);
    cairo_move_to(cr, bx + 8, by + tth / 2.0 + 4);
    cairo_show_text(cr, buf);
    _hgrad(cr, bx, by + tth, b.w, b.h - tth, th->menu_bg1, th->menu_bg2);
    _bevel(cr, bx, by + tth, b.w, b.h - tth, 1, 1);
    for (int i = 0; i < zones_ptr->strip_n; ++i) {
        wz_rect c = zones_ptr->strip_cell[i];
        double cx = c.x - ox, cy = c.y - oy;
        bool sel = i == zones_ptr->strip_sel, hov = i == zones_ptr->strip_hover;
        if (sel) {
            _rgb(cr, wz_rgb_mix(th->menu_bg1, 0x000000, 40), 1);
            cairo_rectangle(cr, cx + 2, cy + 2, c.w - 4, c.h - 4);
            cairo_fill(cr);
            _sunken(cr, cx + 2, cy + 2, c.w - 4, c.h - 4);
        } else if (hov) {
            _rgb(cr, th->hl_bg, 1);
            cairo_rectangle(cr, cx + 2, cy + 2, c.w - 4, c.h - 4);
            cairo_fill(cr);
            _bevel(cr, cx + 2, cy + 2, c.w - 4, c.h - 4, 1, 1);
        }
        wz_rect t = zones_ptr->strip_thumb[i];
        double tx = t.x - ox, ty = t.y - oy;
        _rgb(cr, 0x1c2024, 1);
        cairo_rectangle(cr, tx, ty, t.w, t.h);
        cairo_fill(cr);
        _sunken(cr, tx - 1, ty - 1, t.w + 2, t.h + 2);
        wz_rect m[64];
        int k = _strip_minis(zones_ptr, i, m, 64);
        for (int j = 0; j < k; ++j) {
            bool on = hov && j == zones_ptr->strip_mini;
            unsigned col = on ? _hl(zones_ptr, false) :
                sel ? wz_rgb_mix(_hl(zones_ptr, false), 0xffffff, 120) : 0xc2c0c5;
            _rgb(cr, col, 1);
            cairo_rectangle(cr, m[j].x - ox, m[j].y - oy, m[j].w, m[j].h);
            cairo_fill(cr);
            if (m[j].w >= 4 && m[j].h >= 4) {
                _bevel(cr, m[j].x - ox, m[j].y - oy, m[j].w, m[j].h, 1, 0.9);
            }
        }
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               sel ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        _fit_text(cr, buf, sizeof(buf), zones_ptr->strip_label[i], c.w - 8);
        cairo_text_extents_t te;
        cairo_text_extents(cr, buf, &te);
        _rgb(cr, hov && !sel ? th->hl_fg : th->menu_fg, 1);
        cairo_move_to(cr, cx + (c.w - te.x_advance) / 2, ty + t.h + 15);
        cairo_show_text(cr, buf);
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
    if (0 <= zones_ptr->hl_a && zones_ptr->hl_a < zones_ptr->nzones) {
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
    if (OV_PICKER == zones_ptr->ov_mode) _draw_picker_hint(zones_ptr, cr);
    _draw_strip(zones_ptr, cr);
    cairo_destroy(cr);
    wlmtk_buffer_set(&zones_ptr->buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
}

/** Forgets the layout panel. */
static void _strip_clear(wlmaker_zones_t *zones_ptr)
{
    for (int i = 0; i < zones_ptr->strip_n; ++i) {
        free(zones_ptr->strip_id[i]);
        free(zones_ptr->strip_tree[i]);
        free(zones_ptr->strip_label[i]);
        zones_ptr->strip_id[i] = NULL;
        zones_ptr->strip_tree[i] = NULL;
        zones_ptr->strip_label[i] = NULL;
    }
    zones_ptr->strip_n = 0;
    zones_ptr->strip = false;
    zones_ptr->strip_hover = zones_ptr->strip_mini = zones_ptr->strip_sel = -1;
}

/** Lays out the layout panel on the output (at the bottom if asked). */
static void _strip_build(wlmaker_zones_t *zones_ptr,
                         struct wlr_output *output_ptr,
                         bool bottom)
{
    _strip_clear(zones_ptr);
    struct wlr_box u = wlmtk_desktop_get_usable_area(
        zones_ptr->server_ptr->desktop_ptr, output_ptr);
    struct wlr_box g = _output_box(zones_ptr, output_ptr);
    const int cell_w = 100, thumb_w = 78, pad = 6, title_h = 22, label_h = 18;
    int thumb_h = g.width > 0 ? thumb_w * g.height / g.width : thumb_w * 9 / 16;
    thumb_h = BS_MAX(30, BS_MIN(66, thumb_h));
    int cell_h = pad + thumb_h + 4 + label_h + pad / 2;
    const wz_layout_def *ls[STRIP_MAX];
    int n = wz_config_drag_layouts(&zones_ptr->cfg, ls, STRIP_MAX);
    n = BS_MIN(n, (u.width - 32 - 2 * pad) / cell_w);
    if (0 >= n) return;
    int w = n * cell_w + 2 * pad, h = title_h + cell_h + 2 * pad;
    int x = u.x + (u.width - w) / 2;
    int y = bottom ? u.y + u.height - h - 16 : u.y + 16;
    zones_ptr->strip_box = (wz_rect){ x, y, w, h };
    zones_ptr->strip_title_h = title_h;
    for (int i = 0; i < n; ++i) {
        zones_ptr->strip_id[i] = strdup(ls[i]->id);
        zones_ptr->strip_tree[i] = strdup(ls[i]->tree);
        const char *lab = NULL != ls[i]->label ? ls[i]->label :
            wz_builtin_label(ls[i]->id);
        // Built-in names are translated; the user's are shown as written.
        zones_ptr->strip_label[i] = strdup(
            NULL != ls[i]->label ? ls[i]->label :
            NULL != lab ? wlmaker_i18n(lab) : ls[i]->id);
        wz_rect c = { x + pad + i * cell_w, y + title_h + pad, cell_w, cell_h };
        zones_ptr->strip_cell[i] = c;
        zones_ptr->strip_thumb[i] = (wz_rect){
            c.x + (cell_w - thumb_w) / 2, c.y + pad, thumb_w, thumb_h };
    }
    zones_ptr->strip_n = n;
    zones_ptr->strip = true;
}

/**
 * @return The thumbnail under the point: -1 outside the panel, -2 on the
 * panel but not on a thumbnail. `*mini_ptr` gets its zone under the point.
 */
static int _strip_hit(wlmaker_zones_t *zones_ptr, int x, int y, int *mini_ptr)
{
    *mini_ptr = -1;
    if (!zones_ptr->strip || !wz_rect_contains(zones_ptr->strip_box, x, y)) {
        return -1;
    }
    for (int i = 0; i < zones_ptr->strip_n; ++i) {
        if (!wz_rect_contains(zones_ptr->strip_cell[i], x, y)) continue;
        if (wz_rect_contains(zones_ptr->strip_thumb[i], x, y)) {
            wz_rect m[64];
            int k = _strip_minis(zones_ptr, i, m, 64);
            for (int j = 0; j < k; ++j) {
                if (wz_rect_contains(m[j], x, y)) *mini_ptr = j;
            }
        }
        return i;
    }
    return -2;
}

/** Shows `n` zones on the output (replacing what is shown there). */
static void _wlmaker_zones_overlay_set(
    wlmaker_zones_t *zones_ptr,
    struct wlr_output *output_ptr,
    const wz_rect *zones,
    char **names,
    int n,
    int mode)
{
    if (zones_ptr->overlay_output_ptr != output_ptr) {
        _wlmaker_zones_overlay_hide(zones_ptr);
    }
    for (int i = 0; i < zones_ptr->nzones; ++i) {
        free(zones_ptr->names[i]);
        zones_ptr->names[i] = NULL;
    }
    n = BS_MAX(0, BS_MIN(n, ZONES_MAX));
    for (int i = 0; i < n; ++i) {
        zones_ptr->zones[i] = zones[i];
        zones_ptr->names[i] = NULL != names ? names[i] : NULL;
    }
    zones_ptr->nzones = n;
    zones_ptr->ov_mode = mode;
    zones_ptr->hl_a = zones_ptr->hl_b = -1;
    zones_ptr->output_box = _output_box(zones_ptr, output_ptr);
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

/** Shows the zones of a layout tree (NULL: the effective one). */
static void _wlmaker_zones_overlay_show(
    wlmaker_zones_t *zones_ptr,
    struct wlr_output *output_ptr,
    const char *tree_override_ptr,
    int mode)
{
    wz_rect zones[ZONES_MAX];
    char *names[ZONES_MAX] = {};
    int n = NULL != tree_override_ptr ?
        _zones_of_tree(zones_ptr, output_ptr, tree_override_ptr, zones, names) :
        _zones_for(zones_ptr, output_ptr, zones, names);
    _wlmaker_zones_overlay_set(zones_ptr, output_ptr, zones, names, n, mode);
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
    _strip_clear(zones_ptr);
    free(zones_ptr->shown_tree_ptr);
    zones_ptr->shown_tree_ptr = NULL;
}

/* == Picker (Super+Z) ===================================================== */

/** Loads the zones of the picker's window output and selects one. */
static void _picker_load(wlmaker_zones_t *zones_ptr)
{
    wlmtk_window_t *window_ptr = zones_ptr->picker_window_ptr;
    struct wlr_output *o = wlmtk_window_get_wlr_output(window_ptr);
    _wlmaker_zones_overlay_show(zones_ptr, o, NULL, OV_PICKER);
    if (0 >= zones_ptr->nzones) {
        _picker_close(zones_ptr);
        return;
    }
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
    int sel = NULL != w && 0 <= w->zone && w->zone < zones_ptr->nzones ? w->zone :
        wz_zone_at(zones_ptr->zones, zones_ptr->nzones,
                   b.x + b.width / 2, b.y + b.height / 2);
    zones_ptr->picker_sel = BS_MAX(0, sel);
    zones_ptr->hl_a = zones_ptr->picker_sel;
    _wlmaker_zones_redraw(zones_ptr);
}

/** Snaps the picker's window to zone `idx` and closes the picker. */
void _picker_apply(wlmaker_zones_t *zones_ptr, int idx)
{
    wlmtk_window_t *window_ptr = zones_ptr->picker_window_ptr;
    if (0 <= idx && idx < zones_ptr->nzones && NULL != window_ptr) {
        wz_rect target = zones_ptr->zones[idx];
        _picker_close(zones_ptr);
        _hist_push(zones_ptr, &window_ptr, 1);
        _snap(zones_ptr, window_ptr, target, idx);
        return;
    }
    _picker_close(zones_ptr);
}

/** Keys of the picker: It has the keyboard while open. */
static bool _picker_key(xkb_keysym_t keysym, uint32_t modifiers, void *ud_ptr)
{
    wlmaker_zones_t *zones_ptr = ud_ptr;
    if (!zones_ptr->picker) return false;
    // Locked meanwhile: The keys belong to the lock screen (the password).
    if (wlmtk_desktop_locked(zones_ptr->server_ptr->desktop_ptr)) {
        _picker_close(zones_ptr);
        return false;
    }
    if (XKB_KEY_1 <= keysym && keysym <= XKB_KEY_9) {
        _picker_apply(zones_ptr, (int)(keysym - XKB_KEY_1));
        return true;
    }
    if (XKB_KEY_KP_1 <= keysym && keysym <= XKB_KEY_KP_9) {
        _picker_apply(zones_ptr, (int)(keysym - XKB_KEY_KP_1));
        return true;
    }
    int dir = -1;
    switch (keysym) {
    case XKB_KEY_Escape:
        _picker_close(zones_ptr);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        _picker_apply(zones_ptr, zones_ptr->picker_sel);
        return true;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab: {
        // The layout of the shown output changes, not the pointer's.
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "layout %s",
                 (modifiers & WLR_MODIFIER_SHIFT) ||
                 XKB_KEY_ISO_Left_Tab == keysym ? "prev" : "next");
        wlmaker_zones_command(zones_ptr, cmd, NULL, 0);
        _picker_load(zones_ptr);
        return true;
    }
    case XKB_KEY_Left: case XKB_KEY_h: dir = WZ_LEFT; break;
    case XKB_KEY_Right: case XKB_KEY_l: dir = WZ_RIGHT; break;
    case XKB_KEY_Up: case XKB_KEY_k: dir = WZ_UP; break;
    case XKB_KEY_Down: case XKB_KEY_j: dir = WZ_DOWN; break;
    default:
        return true;
    }
    int n = wz_neighbor(zones_ptr->zones, zones_ptr->nzones,
                        zones_ptr->picker_sel, dir);
    if (0 <= n) {
        zones_ptr->picker_sel = n;
        zones_ptr->hl_a = n;
        _wlmaker_zones_redraw(zones_ptr);
    }
    return true;
}

/** @return The activated window of the current workspace. */
static wlmtk_window_t *_active(wlmaker_zones_t *zones_ptr)
{
    return wlmtk_workspace_get_activated_window(
        wlmtk_desktop_get_current_workspace(zones_ptr->server_ptr->desktop_ptr));
}

/** Opens the zone picker for the active window. */
static void _picker_open(wlmaker_zones_t *zones_ptr)
{
    if (zones_ptr->picker || NULL != zones_ptr->drag_window_ptr) return;
    if (wlmtk_desktop_locked(zones_ptr->server_ptr->desktop_ptr)) return;
    wlmtk_window_t *window_ptr = _active(zones_ptr);
    if (!_snappable(zones_ptr, window_ptr)) return;
    zones_ptr->picker = true;
    zones_ptr->picker_window_ptr = window_ptr;
    wlmim_set_key_grab(zones_ptr->server_ptr->input_manager_ptr,
                       _picker_key, zones_ptr);
    _picker_load(zones_ptr);
}

/** Closes the picker: Keyboard back, overlay gone. */
void _picker_close(wlmaker_zones_t *zones_ptr)
{
    if (!zones_ptr->picker) return;
    zones_ptr->picker = false;
    zones_ptr->picker_window_ptr = NULL;
    wlmim_set_key_grab(zones_ptr->server_ptr->input_manager_ptr, NULL, NULL);
    _wlmaker_zones_overlay_hide(zones_ptr);
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

/** @return Whether the activation modifier is held. */
static bool _modifier_down(wlmaker_zones_t *zones_ptr)
{
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(
        zones_ptr->server_ptr->wlr_seat_ptr);
    if (NULL == kb) return false;
    uint32_t m = wlr_keyboard_get_modifiers(kb);
    switch (zones_ptr->cfg.modifier) {
    case WZ_MOD_ALT: return m & WLR_MODIFIER_ALT;
    case WZ_MOD_CTRL: return m & WLR_MODIFIER_CTRL;
    case WZ_MOD_SHIFT: return m & WLR_MODIFIER_SHIFT;
    default: return m & WLR_MODIFIER_LOGO;
    }
}

/** Forgets the layout chosen in the panel. */
static void _drag_layout_clear(wlmaker_zones_t *zones_ptr)
{
    free(zones_ptr->drag_layout_ptr);
    free(zones_ptr->drag_tree_ptr);
    zones_ptr->drag_layout_ptr = zones_ptr->drag_tree_ptr = NULL;
}

/** Window move begins. */
static void _wlmaker_zones_handle_move_begin(struct wl_listener *listener_ptr,
                                             void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, move_begin_listener);
    wlmtk_window_move_event_t *ev = data_ptr;
    _picker_close(zones_ptr);
    zones_window_t *w = _win(zones_ptr, ev->window_ptr, false);
    bool tiled = NULL != w && w->tiled;
    if (!zones_ptr->cfg.zones && !tiled) return;
    if (!tiled && !_snappable(zones_ptr, ev->window_ptr)) return;
    zones_ptr->drag_window_ptr = ev->window_ptr;
    zones_ptr->drag_start = wlmtk_window_get_bounding_box(ev->window_ptr);
    zones_ptr->drag_x = ev->x;
    zones_ptr->drag_y = ev->y;
    zones_ptr->cancelled = false;
    zones_ptr->strip_wait_leave = false;
    zones_ptr->strip_latched = false;
    _drag_layout_clear(zones_ptr);
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
    if (ev->window_ptr != zones_ptr->drag_window_ptr) return;
    zones_ptr->drag_x = ev->x;
    zones_ptr->drag_y = ev->y;
    _drag_update(zones_ptr);
}

/** Cells of the tiling on the output (the boxes the tiling asked for). */
static int _tile_cells(wlmaker_zones_t *zones_ptr,
                       struct wlr_output *output_ptr,
                       wz_rect *out_ptr)
{
    int n = 0;
    for (int i = 0; i < WINDOWS_MAX && n < ZONES_MAX; ++i) {
        zones_window_t *w = &zones_ptr->windows[i];
        if (NULL == w->window_ptr || !w->tiled || 0 >= w->expect.width) continue;
        if (wlmtk_window_get_wlr_output(w->window_ptr) != output_ptr) continue;
        out_ptr[n++] = (wz_rect){ w->expect.x, w->expect.y,
                                  w->expect.width, w->expect.height };
    }
    return n;
}

/** Follows the drag: Panel of layouts, zones, highlighted target. */
void _drag_update(wlmaker_zones_t *zones_ptr)
{
    wlmtk_window_t *window_ptr = zones_ptr->drag_window_ptr;
    if (NULL == window_ptr || zones_ptr->cancelled) return;
    double px = zones_ptr->drag_x, py = zones_ptr->drag_y;
    struct wlr_box now = wlmtk_window_get_bounding_box(window_ptr);
    int dx = now.x - zones_ptr->drag_start.x, dy = now.y - zones_ptr->drag_start.y;
    if (NULL == zones_ptr->overlay_output_ptr &&
        dx * dx + dy * dy < zones_ptr->cfg.min_drag_px * zones_ptr->cfg.min_drag_px) {
        return;
    }
    struct wlr_output *output_ptr = _output_at(zones_ptr, px, py);
    if (NULL == output_ptr) return;
    struct wlr_box g = _output_box(zones_ptr, output_ptr);
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    bool tile_mode = NULL != w && w->tiled && _autotile_on(zones_ptr, output_ptr);
    bool active = true;
    if (tile_mode) {
        active = true;
    } else if (!zones_ptr->cfg.zones) {
        active = false;
    } else if (WZ_ACTIVATE_MODIFIER == zones_ptr->cfg.activation) {
        active = _modifier_down(zones_ptr);
    } else if (WZ_ACTIVATE_EDGE == zones_ptr->cfg.activation) {
        int e = zones_ptr->cfg.edge_px;
        active = px < g.x + e || py < g.y + e ||
            px >= g.x + g.width - e || py >= g.y + g.height - e ||
            _modifier_down(zones_ptr);
    }
    if (!active) {
        _wlmaker_zones_overlay_hide(zones_ptr);
        return;
    }
    // First the layout (panel of thumbnails), then the zone.
    bool strip = false;
    if (!tile_mode) {
        switch (zones_ptr->cfg.drag_flow) {
        case WZ_FLOW_LAYOUTS:
            strip = true;
            break;
        case WZ_FLOW_LAYOUTS_MODIFIER:
            strip = _modifier_down(zones_ptr) || NULL != zones_ptr->drag_layout_ptr;
            break;
        case WZ_FLOW_LAYOUTS_EDGE:
            if (py < g.y + zones_ptr->cfg.edge_px) zones_ptr->strip_latched = true;
            strip = zones_ptr->strip_latched;
            break;
        default:
            break;
        }
    }
    // The layout chosen in the panel holds on the output it was chosen on.
    if (NULL != zones_ptr->drag_layout_ptr &&
        NULL != zones_ptr->overlay_output_ptr &&
        zones_ptr->overlay_output_ptr != output_ptr) {
        _drag_layout_clear(zones_ptr);
    }
    const char *tree_ptr = NULL;
    if (!tile_mode) {
        if (strip) {
            tree_ptr = zones_ptr->drag_tree_ptr;  // None yet: only the panel.
        } else {
            const wz_layout_def *l = _layout_of(zones_ptr, output_ptr);
            tree_ptr = NULL != l ? l->tree : NULL;
        }
    }
    int mode = tile_mode ? OV_TILE : OV_ZONES;
    bool rebuild = zones_ptr->overlay_output_ptr != output_ptr ||
        zones_ptr->ov_mode != mode || zones_ptr->strip != strip;
    bool changed = rebuild ||
        (NULL == tree_ptr) != (NULL == zones_ptr->shown_tree_ptr) ||
        (NULL != tree_ptr && 0 != strcmp(tree_ptr, zones_ptr->shown_tree_ptr));
    if (changed) {
        wz_rect zones[ZONES_MAX];
        char *names[ZONES_MAX] = {};
        int n = tile_mode ? _tile_cells(zones_ptr, output_ptr, zones) :
            NULL != tree_ptr ? _zones_of_tree(zones_ptr, output_ptr, tree_ptr,
                                              zones, names) : 0;
        if (0 >= n && !strip) return;
        if (rebuild) {
            _wlmaker_zones_overlay_hide(zones_ptr);
            if (strip) {
                // Away from the pointer: at the bottom when it is in the
                // upper third (unless the top edge asked for the panel).
                bool bottom = WZ_FLOW_LAYOUTS_EDGE != zones_ptr->cfg.drag_flow &&
                    py < g.y + g.height / 3.0;
                _strip_build(zones_ptr, output_ptr, bottom);
                zones_ptr->strip_wait_leave = wz_rect_contains(
                    zones_ptr->strip_box, (int)px, (int)py);
                for (int i = 0; i < zones_ptr->strip_n; ++i) {
                    if (NULL != zones_ptr->drag_layout_ptr &&
                        0 == strcmp(zones_ptr->strip_id[i],
                                    zones_ptr->drag_layout_ptr)) {
                        zones_ptr->strip_sel = i;
                    }
                }
            }
        }
        _wlmaker_zones_overlay_set(zones_ptr, output_ptr, zones, names, n, mode);
        _set_str(&zones_ptr->shown_tree_ptr, tree_ptr);
    }
    if (strip) {
        int mini = -1;
        int hit = _strip_hit(zones_ptr, (int)px, (int)py, &mini);
        if (zones_ptr->strip_wait_leave) {
            if (-1 != hit) {
                hit = -2;
                mini = -1;
            } else {
                zones_ptr->strip_wait_leave = false;
            }
        }
        if (-1 != hit) {
            // Over the panel: No zone highlighted below it.
            int hover = BS_MAX(-1, hit);
            if (hover != zones_ptr->strip_hover && NULL != zones_ptr->dwell_timer_ptr) {
                wl_event_source_timer_update(
                    zones_ptr->dwell_timer_ptr,
                    0 <= hover ? BS_MAX(1, zones_ptr->cfg.dwell_ms) : 0);
            }
            bool dirty = hover != zones_ptr->strip_hover ||
                mini != zones_ptr->strip_mini || 0 <= zones_ptr->hl_a;
            zones_ptr->strip_hover = hover;
            zones_ptr->strip_mini = 0 <= hover ? mini : -1;
            zones_ptr->hl_a = zones_ptr->hl_b = -1;
            if (dirty) _wlmaker_zones_redraw(zones_ptr);
            return;
        }
        if (0 <= zones_ptr->strip_hover) {
            if (NULL != zones_ptr->dwell_timer_ptr) {
                wl_event_source_timer_update(zones_ptr->dwell_timer_ptr, 0);
            }
            zones_ptr->strip_hover = zones_ptr->strip_mini = -1;
            _wlmaker_zones_redraw(zones_ptr);
        }
    }
    wz_rect target = {};
    int b = -1;
    int a = 0 < zones_ptr->nzones ?
        wz_target(zones_ptr->zones, zones_ptr->nzones, (int)px, (int)py,
                  tile_mode ? 0 : zones_ptr->cfg.span_px, &target, &b) : -1;
    if (a != zones_ptr->hl_a || b != zones_ptr->hl_b) {
        zones_ptr->hl_a = a;
        zones_ptr->hl_b = b;
        zones_ptr->target = target;
        _wlmaker_zones_redraw(zones_ptr);
    }
}

/** The pointer rested on a thumbnail: Its layout's zones are shown. */
static int _wlmaker_zones_dwell(void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    int i = zones_ptr->strip_hover;
    if (NULL == zones_ptr->drag_window_ptr || !zones_ptr->strip ||
        0 > i || i >= zones_ptr->strip_n || i == zones_ptr->strip_sel) return 0;
    _set_str(&zones_ptr->drag_layout_ptr, zones_ptr->strip_id[i]);
    _set_str(&zones_ptr->drag_tree_ptr, zones_ptr->strip_tree[i]);
    zones_ptr->strip_sel = i;
    _drag_update(zones_ptr);
    return 0;
}

/** Section holding the layout of the output ("General" or "Monitor X"). */
static void _layout_section(wlmaker_zones_t *zones_ptr,
                            struct wlr_output *output_ptr,
                            char *sec_ptr, size_t len)
{
    snprintf(sec_ptr, len, "General");
    for (int i = 0; i < zones_ptr->cfg.nscopes; ++i) {
        wz_scope *s = &zones_ptr->cfg.scopes[i];
        if (NULL != s->monitor && 0 == s->workspace && NULL != s->layout &&
            NULL != output_ptr && 0 == strcmp(s->monitor, output_ptr->name)) {
            snprintf(sec_ptr, len, "Monitor %s", s->monitor);
        }
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
    if (NULL != zones_ptr->dwell_timer_ptr) {
        wl_event_source_timer_update(zones_ptr->dwell_timer_ptr, 0);
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
        _drag_layout_clear(zones_ptr);
        return;
    }
    if (NULL != w && w->tiled) {
        // Dropped on another tiled window: swap them.
        for (int i = 0; i < WINDOWS_MAX; ++i) {
            zones_window_t *o = &zones_ptr->windows[i];
            if (NULL == o->window_ptr || o == w || !o->tiled) continue;
            struct wlr_box ob = wlmtk_window_get_bounding_box(o->window_ptr);
            if (wlr_box_contains_point(&ob, ev->x, ev->y)) {
                wlmtk_window_t *pair[2] = { w->window_ptr, o->window_ptr };
                _hist_push(zones_ptr, pair, 2);
                long t = o->order;
                o->order = w->order;
                w->order = t;
                zones_tset_t *s = _tset(zones_ptr, _output_at(zones_ptr, ev->x, ev->y), false);
                if (NULL != s) {
                    wz_bsp_swap(&s->bsp, (long)(uintptr_t)w->window_ptr,
                                (long)(uintptr_t)o->window_ptr);
                }
                break;
            }
        }
        memset(&w->expect, 0, sizeof(w->expect));
        _wlmaker_zones_overlay_hide(zones_ptr);
        _wlmaker_zones_tile_mark(zones_ptr);
        return;
    }
    struct wlr_output *output_ptr = zones_ptr->overlay_output_ptr;
    // Dropped on the panel: On a zone of a thumbnail, snaps there; on the
    // rest of the panel, the window stays where it was dropped.
    if (NULL != output_ptr && zones_ptr->strip) {
        int mini = -1;
        int i = _strip_hit(zones_ptr, (int)ev->x, (int)ev->y, &mini);
        if (0 <= i && 0 <= mini) {
            wz_rect zones[ZONES_MAX];
            int n = _zones_of_tree(zones_ptr, output_ptr, zones_ptr->strip_tree[i],
                                   zones, NULL);
            if (mini < n) {
                _set_str(&zones_ptr->drag_layout_ptr, zones_ptr->strip_id[i]);
                zones_ptr->target = zones[mini];
                zones_ptr->hl_a = mini;
                zones_ptr->hl_b = -1;
            }
        } else if (-1 != i) {
            _wlmaker_zones_overlay_hide(zones_ptr);
            _drag_layout_clear(zones_ptr);
            return;
        }
    }
    if (NULL != output_ptr && 0 <= zones_ptr->hl_a) {
        zones_window_t *s = _win(zones_ptr, window_ptr, true);
        if (NULL != s && !s->has_restore) {
            s->restore = zones_ptr->drag_start;
            s->has_restore = true;
        }
        // One undo step: The window as before the drag and, if the layout
        // came from the panel and changed, the layout from before.
        wz_hist_entry *e = _hist_begin(zones_ptr);
        _hist_add(zones_ptr, e, window_ptr, &zones_ptr->drag_start);
        const wz_layout_def *cur = _layout_of(zones_ptr, output_ptr);
        bool change = NULL != zones_ptr->drag_layout_ptr &&
            (NULL == cur || 0 != strcmp(cur->id, zones_ptr->drag_layout_ptr));
        char sec[96];
        _layout_section(zones_ptr, output_ptr, sec, sizeof(sec));
        if (change) _hist_add_cfg(zones_ptr, e, sec, "Layout");
        wz_history_commit(&zones_ptr->history);
        wz_rect target = zones_ptr->target;
        int zone = zones_ptr->hl_a;
        if (change) {
            _save(zones_ptr, sec, "Layout", zones_ptr->drag_layout_ptr);
            wlmaker_zones_reload(zones_ptr);
        }
        _snap(zones_ptr, window_ptr, target, zone);
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
    _drag_layout_clear(zones_ptr);
}

/** Interactive resize ended: Read the new size a bit later. */
static void _wlmaker_zones_handle_resize_end(struct wl_listener *listener_ptr,
                                             void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, resize_end_listener);
    wlmtk_window_move_event_t *ev = data_ptr;
    zones_window_t *w = _win(zones_ptr, ev->window_ptr, false);
    if (NULL == w || !w->tiled || NULL == zones_ptr->resize_timer_ptr) return;
    zones_ptr->resized_window_ptr = ev->window_ptr;
    // The client adopts the size with its next commit.
    wl_event_source_timer_update(zones_ptr->resize_timer_ptr, 150);
}

/* == Tiling =============================================================== */

/** @return Whether the window can be tiled. */
static bool _tileable(wlmaker_zones_t *zones_ptr, wlmtk_window_t *window_ptr)
{
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    if (NULL != w && (w->user_float || w->rule_float || w->rule_ignore)) return false;
    if (wlmtk_window_is_fullscreen(window_ptr) ||
        wlmtk_window_is_minimized(window_ptr) ||
        wlmtk_window_is_dialog(window_ptr)) return false;
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

/** Moves the window into the output, at the same relative place. */
static void _move_to_output(wlmaker_zones_t *zones_ptr,
                            wlmtk_window_t *window_ptr,
                            struct wlr_output *to_ptr)
{
    struct wlr_output *from_ptr = wlmtk_window_get_wlr_output(window_ptr);
    if (NULL == to_ptr || from_ptr == to_ptr) return;
    struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
    struct wlr_box ua = NULL != from_ptr ? wlmtk_desktop_get_usable_area(
        zones_ptr->server_ptr->desktop_ptr, from_ptr) : b;
    struct wlr_box ub = wlmtk_desktop_get_usable_area(
        zones_ptr->server_ptr->desktop_ptr, to_ptr);
    struct wlr_box n = { .width = BS_MIN(b.width, ub.width),
                         .height = BS_MIN(b.height, ub.height) };
    double fx = ua.width > n.width ? (double)(b.x - ua.x) / (ua.width - n.width) : 0.5;
    double fy = ua.height > n.height ? (double)(b.y - ua.y) / (ua.height - n.height) : 0.5;
    fx = BS_MAX(0, BS_MIN(1, fx));
    fy = BS_MAX(0, BS_MIN(1, fy));
    n.x = ub.x + (int)(fx * (ub.width - n.width));
    n.y = ub.y + (int)(fy * (ub.height - n.height));
    wlmtk_window_request_bounding_box(window_ptr, &n);
}

/**
 * Per-application rules, once per window: From the app ID (xdg-shell) or
 * WM_CLASS (XWayland), never only from the title. First the destination
 * (workspace, output), then the action. Dialogs are left alone.
 */
static void _apply_rules(wlmaker_zones_t *zones_ptr, zones_window_t *w)
{
    wlmtk_window_t *window_ptr = w->window_ptr;
    const char *app_id_ptr = wlmtk_window_get_app_id(window_ptr);
    // The app ID usually comes before the first commit; give it a few
    // relayouts before matching on what is known.
    if (NULL == app_id_ptr && ++w->rule_tries < 5) return;
    w->rule_done = true;
    const wz_rule *r = wz_config_match_rule(
        &zones_ptr->cfg, app_id_ptr, wlmtk_window_get_instance(window_ptr),
        wlmtk_window_get_title(window_ptr));
    if (NULL == r) return;
    bs_log(BS_INFO, "zones: Rule \"%s\" for %s", r->id,
           NULL != app_id_ptr ? app_id_ptr : "?");
    if (WZ_RULE_IGNORE == r->action) {
        w->rule_ignore = true;
        return;
    }
    if (wlmtk_window_is_dialog(window_ptr)) return;
    if (0 < r->workspace) {
        wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_workspace_at(
            zones_ptr->server_ptr->desktop_ptr, (size_t)(r->workspace - 1));
        wlmtk_workspace_t *cur_ptr = wlmtk_window_get_workspace(window_ptr);
        if (NULL != ws_ptr && NULL != cur_ptr && ws_ptr != cur_ptr) {
            wlmtk_workspace_unmap_window(cur_ptr, window_ptr);
            wlmtk_workspace_map_window(ws_ptr, window_ptr);
        }
    }
    struct wlr_output *o = wlmtk_window_get_wlr_output(window_ptr);
    if (NULL != r->monitor) {
        struct wlr_output_layout_output *lo;
        wl_list_for_each(lo, &zones_ptr->server_ptr->wlr_output_layout_ptr->outputs, link) {
            if (0 == strcmp(lo->output->name, r->monitor) && lo->output != o) {
                _move_to_output(zones_ptr, window_ptr, lo->output);
                o = lo->output;
            }
        }
    }
    switch (r->action) {
    case WZ_RULE_FLOAT:
        w->rule_float = true;
        break;
    case WZ_RULE_ZONE: {
        if (0 >= r->zone || _autotile_on(zones_ptr, o)) break;
        wz_rect zones[ZONES_MAX];
        const wz_layout_def *l = NULL != r->layout ?
            wz_config_layout(&zones_ptr->cfg, r->layout) : NULL;
        int n = NULL != l ? _zones_of_tree(zones_ptr, o, l->tree, zones, NULL) :
            _zones_for(zones_ptr, o, zones, NULL);
        if (r->zone <= n) _snap(zones_ptr, window_ptr, zones[r->zone - 1], r->zone - 1);
        break;
    }
    default:
        break;
    }
}

/** The window's app ID for the log, "?" when it has none. */
static const char *_app_id(wlmtk_window_t *window_ptr)
{
    const char *a = wlmtk_window_get_app_id(window_ptr);
    return NULL != a ? a : "?";
}

/** Cells for the tiling set (output and workspace): `rects` in the order
 * of `set` (reordered by the BSP leaves). @return how many. */
static int _tile_compute(zones_tset_t *ts,
                         const wz_tile_params *p_ptr,
                         wz_rect area,
                         zones_window_t **set,
                         int k,
                         wz_rect *rects,
                         wlmtk_window_t *active_ptr)
{
    if (WZ_TILE_BSP == p_ptr->algo && NULL != ts) {
        // Persistent tree: Leaves of windows gone leave; new windows
        // split the leaf of the active one.
        long ids[WINDOWS_MAX];
        wz_rect tmp[WINDOWS_MAX];
        int m = wz_bsp_compute(&ts->bsp, area, 0, 0, tmp, ids, WINDOWS_MAX);
        for (int a = 0; a < m; ++a) {
            bool found = false;
            for (int j = 0; j < k; ++j) {
                found |= (long)(uintptr_t)set[j]->window_ptr == ids[a];
            }
            if (!found) wz_bsp_remove(&ts->bsp, ids[a]);
        }
        for (int j = 0; j < k; ++j) {
            if (!wz_bsp_has(&ts->bsp, (long)(uintptr_t)set[j]->window_ptr)) {
                wz_bsp_insert(&ts->bsp, (long)(uintptr_t)set[j]->window_ptr,
                              (long)(uintptr_t)active_ptr, area);
            }
        }
        int gi = p_ptr->gap_in, go = p_ptr->gap_out;
        if (p_ptr->smart_gaps && 1 == k) gi = go = 0;
        m = wz_bsp_compute(&ts->bsp, area, gi, go, tmp, ids, WINDOWS_MAX);
        zones_window_t *ord[WINDOWS_MAX];
        int c = 0;
        for (int a = 0; a < m && c < k; ++a) {
            for (int j = 0; j < k; ++j) {
                if ((long)(uintptr_t)set[j]->window_ptr == ids[a]) {
                    ord[c] = set[j];
                    rects[c++] = tmp[a];
                }
            }
        }
        memcpy(set, ord, sizeof(set[0]) * (size_t)c);
        k = c;
    } else {
        int m = wz_tile(p_ptr, area, k, rects, WINDOWS_MAX);
        k = BS_MAX(0, BS_MIN(k, m));
    }
    return k;
}

/** Tiles the windows of the current workspace, per output. */
int _wlmaker_zones_tile_run(void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
        zones_ptr->server_ptr->desktop_ptr);
    if (NULL == ws_ptr) return 0;
    int wsn = _workspace_number(zones_ptr);

    bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
    if (zones_ptr->cfg.rules_enabled && 0 < zones_ptr->cfg.nrules) {
        for (bs_dllist_node_t *d = list_ptr->head_ptr; NULL != d; d = d->next_ptr) {
            zones_window_t *w = _win(zones_ptr, wlmtk_window_from_dlnode(d), true);
            if (NULL != w && !w->rule_done) _apply_rules(zones_ptr, w);
        }
    }

    zones_window_t *members[WINDOWS_MAX];
    struct wlr_output *outs[WINDOWS_MAX];
    int n = 0;
    for (bs_dllist_node_t *d = list_ptr->head_ptr; NULL != d; d = d->next_ptr) {
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(d);
        struct wlr_output *o = wlmtk_window_get_wlr_output(window_ptr);
        if (NULL == o) continue;
        bool on = wz_config_autotile_for(&zones_ptr->cfg, o->name, wsn);
        zones_window_t *w = _win(zones_ptr, window_ptr, on);
        if (NULL == w) continue;
        if (!on || !_tileable(zones_ptr, window_ptr)) {
            // Out of the mosaic: No longer "does not fit" (`windows -d`), and
            // back in it, it starts afresh.
            w->fit_float = false;
            w->fit_prev = false;
            if (w->tiled) {
                w->tiled = false;
                _tsets_forget(zones_ptr, window_ptr);
                memset(&w->expect, 0, sizeof(w->expect));
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
    wlmtk_window_t *active_ptr = wlmtk_workspace_get_activated_window(ws_ptr);
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
        zones_tset_t *ts = _tset(zones_ptr, o, true);
        wz_tile_params p = zones_ptr->cfg.tile;
        p.algo = wz_config_algo_for(&zones_ptr->cfg, o->name, wsn);
        if (NULL != ts) p.master_ratio = ts->ratio;
        struct wlr_box u = wlmtk_desktop_get_usable_area(
            zones_ptr->server_ptr->desktop_ptr, o);
        wz_rect area = { u.x, u.y, u.width, u.height };
        wz_rect rects[WINDOWS_MAX];
        for (int j = 0; j < k; ++j) {
            set[j]->fit_prev = set[j]->fit_float;
            set[j]->fit_float = false;
        }
        // Who does not fit its cell (GTK4/libadwaita ask for large minimum
        // sizes: 760 wide for Pamac) floats, one at a time, the one
        // farthest from fitting first; with one window less the cells grow.
        // A single one that does not fit first swaps places with another
        // window when both fit then. Same as wmake-zones on X11.
        for (int pass = 0; pass < 16; ++pass) {
            k = _tile_compute(ts, &p, area, set, k, rects, active_ptr);
            int conflicts = 0, worst = -1;
            long worst_def = 0;
            for (int j = 0; j < k; ++j) {
                int mw, mh;
                wlmtk_window_get_min_bounding_size(set[j]->window_ptr, &mw, &mh);
                long def = (long)BS_MAX(0, mw - rects[j].w) + BS_MAX(0, mh - rects[j].h);
                if (0 >= def) continue;
                conflicts++;
                if (0 > worst || def > worst_def) {
                    worst = j;
                    worst_def = def;
                }
            }
            if (0 == conflicts) break;
            bool swapped = false;
            for (int j = 0; 1 == conflicts && j < k && !swapped; ++j) {
                if (j == worst) continue;
                int aw, ah, bw, bh;
                wlmtk_window_get_min_bounding_size(set[worst]->window_ptr, &aw, &ah);
                wlmtk_window_get_min_bounding_size(set[j]->window_ptr, &bw, &bh);
                if (aw > rects[j].w || ah > rects[j].h ||
                    bw > rects[worst].w || bh > rects[worst].h) continue;
                long o = set[worst]->order;
                set[worst]->order = set[j]->order;
                set[j]->order = o;
                if (NULL != ts) {
                    wz_bsp_swap(&ts->bsp, (long)(uintptr_t)set[worst]->window_ptr,
                                (long)(uintptr_t)set[j]->window_ptr);
                }
                zones_window_t *t = set[worst];
                set[worst] = set[j];
                set[j] = t;
                bs_log(BS_INFO, "zones: Tiling: %s swaps places to fit (minimum %dx%d)",
                       _app_id(set[j]->window_ptr), aw, ah);
                swapped = true;
            }
            if (swapped) continue;
            zones_window_t *w = set[worst];
            int mw, mh;
            wlmtk_window_get_min_bounding_size(w->window_ptr, &mw, &mh);
            w->fit_float = true;
            w->fit_min_w = mw;
            w->fit_min_h = mh;
            w->fit_cell_w = rects[worst].w;
            w->fit_cell_h = rects[worst].h;
            if (!w->fit_prev) {
                bs_log(BS_INFO, "zones: Tiling: %s does not fit the cell %dx%d "
                       "(minimum %dx%d), floats", _app_id(w->window_ptr),
                       rects[worst].w, rects[worst].h, mw, mh);
            }
            if (w->tiled) {
                w->tiled = false;
                _tsets_forget(zones_ptr, w->window_ptr);
                memset(&w->expect, 0, sizeof(w->expect));
                if (w->has_restore) {
                    wlmtk_window_request_bounding_box(w->window_ptr, &w->restore);
                    w->has_restore = false;
                    w->zone = -1;
                }
            }
            memmove(&set[worst], &set[worst + 1], sizeof(set[0]) * (size_t)(k - worst - 1));
            k--;
        }
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

/** The size of a resized tiled window: New ratio of the main area. */
static int _wlmaker_zones_resize_done(void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    zones_window_t *w = _win(zones_ptr, zones_ptr->resized_window_ptr, false);
    zones_ptr->resized_window_ptr = NULL;
    if (NULL == w || !w->tiled) return 0;
    struct wlr_output *o = wlmtk_window_get_wlr_output(w->window_ptr);
    zones_tset_t *ts = _tset(zones_ptr, o, true);
    if (NULL == ts) return 0;
    struct wlr_box b = wlmtk_window_get_bounding_box(w->window_ptr);
    struct wlr_box u = wlmtk_desktop_get_usable_area(
        zones_ptr->server_ptr->desktop_ptr, o);
    struct wlr_box c = w->expect;
    wz_tile_params p = zones_ptr->cfg.tile;
    p.algo = wz_config_algo_for(&zones_ptr->cfg, o->name,
                                _workspace_number(zones_ptr));
    if (WZ_TILE_MASTER == p.algo) {
        // The main window is the first in the order.
        long min = w->order;
        int count = 0;
        for (int i = 0; i < WINDOWS_MAX; ++i) {
            zones_window_t *x = &zones_ptr->windows[i];
            if (NULL == x->window_ptr || !x->tiled ||
                wlmtk_window_get_wlr_output(x->window_ptr) != o) continue;
            min = BS_MIN(min, x->order);
            ++count;
        }
        if (2 > count) return 0;
        bool horiz = WZ_POS_LEFT == p.master_pos || WZ_POS_RIGHT == p.master_pos;
        double span = horiz ? u.width : u.height;
        double size = horiz ? b.width : b.height;
        if (0 >= span) return 0;
        double ratio = w->order == min ? size / span : 1 - size / span;
        wz_hist_entry *e = _hist_begin(zones_ptr);
        wz_hist_set_aux(e, lround(ts->ratio * 10000));
        _hist_add(zones_ptr, e, w->window_ptr, &c);
        wz_history_commit(&zones_ptr->history);
        ts->ratio = BS_MAX(0.1, BS_MIN(0.9, ratio));
    } else if (WZ_TILE_BSP == p.algo && 0 < c.width) {
        long id = (long)(uintptr_t)w->window_ptr;
        if (b.width != c.width && 0 < u.width) {
            wz_bsp_resize(&ts->bsp, id, b.x != c.x ? WZ_LEFT : WZ_RIGHT,
                          (double)(b.width - c.width) / u.width);
        }
        if (b.height != c.height && 0 < u.height) {
            wz_bsp_resize(&ts->bsp, id, b.y != c.y ? WZ_UP : WZ_DOWN,
                          (double)(b.height - c.height) / u.height);
        }
    }
    memset(&w->expect, 0, sizeof(w->expect));
    _wlmaker_zones_tile_mark(zones_ptr);
    return 0;
}

/** Tiling off: All tiled windows back to where they were. */
static void _wlmaker_zones_untile_all(wlmaker_zones_t *zones_ptr)
{
    for (int i = 0; i < WINDOWS_MAX; ++i) {
        zones_window_t *w = &zones_ptr->windows[i];
        if (NULL == w->window_ptr) continue;
        w->fit_float = false;
        w->fit_prev = false;
        if (!w->tiled) continue;
        w->tiled = false;
        memset(&w->expect, 0, sizeof(w->expect));
        if (w->has_restore) {
            wlmtk_window_request_bounding_box(w->window_ptr, &w->restore);
            w->has_restore = false;
            w->zone = -1;
        }
    }
    for (int i = 0; i < TSETS_MAX; ++i) {
        if (zones_ptr->tsets[i].used) wz_bsp_clear(&zones_ptr->tsets[i].bsp);
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
    // A window moved to another workspace is unmapped too; it is mapped
    // again right away and starts over (as a new window would).
    if (NULL != w) w->window_ptr = NULL;
    _tsets_forget(zones_ptr, data_ptr);
    wz_history_forget(&zones_ptr->history, (uintptr_t)data_ptr);
    if (zones_ptr->drag_window_ptr == data_ptr) {
        zones_ptr->drag_window_ptr = NULL;
        _wlmaker_zones_overlay_hide(zones_ptr);
    }
    if (zones_ptr->picker_window_ptr == data_ptr) _picker_close(zones_ptr);
    if (zones_ptr->resized_window_ptr == data_ptr) zones_ptr->resized_window_ptr = NULL;
    _wlmaker_zones_tile_mark(zones_ptr);
}

/** The workspace changed. */
/**
 * The usable area changed (the Dock, the Clip or a panel moved, appeared or
 * went away): the tiling follows it. Before, the cells stayed where they
 * were, under the Dock or leaving a gap, until "apply again".
 */
static void _wlmaker_zones_handle_usable_area(
    struct wl_listener *listener_ptr, __UNUSED__ void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, usable_area_listener);
    _wlmaker_zones_tile_mark(zones_ptr);
}

/** An output was added, removed, moved or changed mode or scale. */
static void _wlmaker_zones_handle_output_layout_change(
    struct wl_listener *listener_ptr, __UNUSED__ void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, output_layout_change_listener);
    _picker_close(zones_ptr);
    _wlmaker_zones_overlay_hide(zones_ptr);
    _wlmaker_zones_tile_mark(zones_ptr);
}

static void _wlmaker_zones_handle_workspace_changed(
    struct wl_listener *listener_ptr, __UNUSED__ void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_zones_t, workspace_changed_listener);
    _picker_close(zones_ptr);
    _wlmaker_zones_tile_mark(zones_ptr);
}

/* == Commands ============================================================= */

/** Parses on/off/toggle. */
static bool _onoff(const char *arg_ptr, bool cur)
{
    if (NULL == arg_ptr || 0 == strcmp(arg_ptr, "toggle")) return !cur;
    if (0 == strcmp(arg_ptr, "on") || 0 == strcmp(arg_ptr, "true")) return true;
    if (0 == strcmp(arg_ptr, "off") || 0 == strcmp(arg_ptr, "false")) return false;
    return cur;
}

/** Snaps the window to zone `n` of its output (one undo step). */
static void _snap_to(wlmaker_zones_t *zones_ptr, wlmtk_window_t *window_ptr,
                     struct wlr_output *output_ptr, int n)
{
    wz_rect z[ZONES_MAX];
    int k = _zones_for(zones_ptr, output_ptr, z, NULL);
    if (0 <= n && n < k && _snappable(zones_ptr, window_ptr)) {
        _hist_push(zones_ptr, &window_ptr, 1);
        if (wlmtk_window_get_wlr_output(window_ptr) != output_ptr) {
            // Another output: Its own restore box is from there.
            zones_window_t *w = _win(zones_ptr, window_ptr, false);
            if (NULL != w) w->has_restore = false;
        }
        _snap(zones_ptr, window_ptr, z[n], n);
    }
}

/** Swaps the tiled window with its neighbour in the direction. */
static void _swap(wlmaker_zones_t *zones_ptr, zones_window_t *w, int dir)
{
    struct wlr_output *o = wlmtk_window_get_wlr_output(w->window_ptr);
    wz_rect cells[ZONES_MAX];
    zones_window_t *ws[ZONES_MAX];
    int n = 0, from = -1;
    for (int i = 0; i < WINDOWS_MAX && n < ZONES_MAX; ++i) {
        zones_window_t *x = &zones_ptr->windows[i];
        if (NULL == x->window_ptr || !x->tiled || 0 >= x->expect.width ||
            wlmtk_window_get_wlr_output(x->window_ptr) != o) continue;
        if (x == w) from = n;
        ws[n] = x;
        cells[n++] = (wz_rect){ x->expect.x, x->expect.y,
                                x->expect.width, x->expect.height };
    }
    int nb = 0 <= from ? wz_neighbor(cells, n, from, dir) : -1;
    if (0 > nb) return;
    zones_window_t *other = ws[nb];
    wlmtk_window_t *pair[2] = { w->window_ptr, other->window_ptr };
    _hist_push(zones_ptr, pair, 2);
    long t = w->order;
    w->order = other->order;
    other->order = t;
    zones_tset_t *s = _tset(zones_ptr, o, false);
    if (NULL != s) {
        wz_bsp_swap(&s->bsp, (long)(uintptr_t)w->window_ptr,
                    (long)(uintptr_t)other->window_ptr);
    }
    _wlmaker_zones_tile_mark(zones_ptr);
}

/** Moves the active window to the neighbouring zone (or output). */
static void _move(wlmaker_zones_t *zones_ptr, int dir)
{
    wlmtk_window_t *window_ptr = _active(zones_ptr);
    if (!_snappable(zones_ptr, window_ptr)) return;
    zones_window_t *w = _win(zones_ptr, window_ptr, false);
    if (NULL != w && w->tiled) {
        _swap(zones_ptr, w, dir);
        return;
    }
    struct wlr_output *o = wlmtk_window_get_wlr_output(window_ptr);
    wz_rect z[ZONES_MAX];
    int k = _zones_for(zones_ptr, o, z, NULL);
    if (0 >= k) return;
    struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
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
    struct wlr_box ob = _output_box(zones_ptr, o);
    double px = ob.x + ob.width / 2.0, py = ob.y + ob.height / 2.0;
    if (WZ_LEFT == dir) px = ob.x - 1;
    if (WZ_RIGHT == dir) px = ob.x + ob.width + 1;
    if (WZ_UP == dir) py = ob.y - 1;
    if (WZ_DOWN == dir) py = ob.y + ob.height + 1;
    struct wlr_output *next = wlr_output_layout_output_at(
        zones_ptr->server_ptr->wlr_output_layout_ptr, px, py);
    if (NULL == next || next == o) return;
    k = _zones_for(zones_ptr, next, z, NULL);
    if (0 < k) _snap_to(zones_ptr, window_ptr, next, wz_zone_at(z, k, (int)px, (int)py));
}

/**
 * Focuses the window next to the active one in the direction: The most
 * natural one (see wz_focus_pick), on any output (FocusEdge=cross), only on
 * the active window's output (stop), or wrapping around on it (wrap).
 */
static void _focus(wlmaker_zones_t *zones_ptr, int dir)
{
    wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
        zones_ptr->server_ptr->desktop_ptr);
    wlmtk_window_t *active_ptr = _active(zones_ptr);
    if (NULL == ws_ptr || NULL == active_ptr) return;
    struct wlr_output *o = wlmtk_window_get_wlr_output(active_ptr);
    bool only_output = WZ_FOCUS_CROSS != zones_ptr->cfg.focus_edge;
    wz_rect r[WINDOWS_MAX];
    wlmtk_window_t *ws[WINDOWS_MAX];
    int n = 0;
    bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
    for (bs_dllist_node_t *d = list_ptr->head_ptr;
         NULL != d && n < WINDOWS_MAX; d = d->next_ptr) {
        wlmtk_window_t *c = wlmtk_window_from_dlnode(d);
        if (c == active_ptr || wlmtk_window_is_minimized(c)) continue;
        if (only_output && wlmtk_window_get_wlr_output(c) != o) continue;
        struct wlr_box b = wlmtk_window_get_bounding_box(c);
        ws[n] = c;
        r[n++] = (wz_rect){ b.x, b.y, b.width, b.height };
    }
    struct wlr_box ab = wlmtk_window_get_bounding_box(active_ptr);
    wz_rect from = { ab.x, ab.y, ab.width, ab.height };
    int pick = wz_focus_pick(r, n, from, dir);
    if (0 > pick && WZ_FOCUS_WRAP == zones_ptr->cfg.focus_edge && NULL != o) {
        // Starts over from the other side of the output.
        struct wlr_box g = _output_box(zones_ptr, o);
        if (WZ_RIGHT == dir) from.x = g.x - from.w;
        else if (WZ_LEFT == dir) from.x = g.x + g.width;
        else if (WZ_DOWN == dir) from.y = g.y - from.h;
        else from.y = g.y + g.height;
        pick = wz_focus_pick(r, n, from, dir);
    }
    if (0 > pick) return;
    wlmtk_workspace_raise_window(ws_ptr, ws[pick]);
    wlmtk_workspace_activate_window(ws_ptr, ws[pick]);
}

/** Sets the layout of the output (its own section, or the general one). */
static void _set_layout(wlmaker_zones_t *zones_ptr,
                        struct wlr_output *output_ptr,
                        const char *id_ptr)
{
    if (NULL == wz_config_layout(&zones_ptr->cfg, id_ptr)) return;
    char sec[96];
    _layout_section(zones_ptr, output_ptr, sec, sizeof(sec));
    char *before_ptr = _cfg_get(zones_ptr, sec, "Layout");
    if (NULL == before_ptr || 0 != strcmp(before_ptr, id_ptr)) {
        wz_hist_entry *e = _hist_begin(zones_ptr);
        wz_hist_add_cfg(e, sec, "Layout", before_ptr);
        wz_history_commit(&zones_ptr->history);
    }
    free(before_ptr);
    _save(zones_ptr, sec, "Layout", id_ptr);
    wlmaker_zones_reload(zones_ptr);
}

/** Cycles the layout of the output under the pointer. */
static void _cycle_layout(wlmaker_zones_t *zones_ptr, int step)
{
    if (0 >= zones_ptr->cfg.nlayouts) return;
    struct wlr_output *o = wlmaker_server_get_output_at_cursor(zones_ptr->server_ptr);
    if (zones_ptr->picker) {
        o = wlmtk_window_get_wlr_output(zones_ptr->picker_window_ptr);
    }
    const wz_layout_def *cur = _layout_of(zones_ptr, o);
    int idx = 0;
    for (int i = 0; i < zones_ptr->cfg.nlayouts; ++i) {
        if (&zones_ptr->cfg.layouts[i] == cur) idx = i;
    }
    idx = (idx + step + zones_ptr->cfg.nlayouts) % zones_ptr->cfg.nlayouts;
    char id[256];
    snprintf(id, sizeof(id), "%s", zones_ptr->cfg.layouts[idx].id);
    _set_layout(zones_ptr, o, id);
}

/** Arranges now: Each zone gets the floating window covering it most. */
static void _arrange(wlmaker_zones_t *zones_ptr)
{
    wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
        zones_ptr->server_ptr->desktop_ptr);
    if (NULL == ws_ptr) return;
    wlmtk_window_t *cand[WINDOWS_MAX];
    struct wlr_output *outs[WINDOWS_MAX];
    int nc = 0;
    bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
    for (bs_dllist_node_t *d = list_ptr->head_ptr;
         NULL != d && nc < WINDOWS_MAX; d = d->next_ptr) {
        wlmtk_window_t *c = wlmtk_window_from_dlnode(d);
        zones_window_t *w = _win(zones_ptr, c, false);
        if ((NULL != w && w->tiled) || wlmtk_window_is_minimized(c) ||
            !_snappable(zones_ptr, c)) continue;
        outs[nc] = wlmtk_window_get_wlr_output(c);
        cand[nc++] = c;
    }
    if (0 == nc) return;
    _hist_push(zones_ptr, cand, nc);
    struct wlr_output_layout_output *lo;
    wl_list_for_each(lo, &zones_ptr->server_ptr->wlr_output_layout_ptr->outputs, link) {
        wz_rect z[ZONES_MAX];
        int k = _zones_for(zones_ptr, lo->output, z, NULL);
        for (int zi = 0; zi < k; ++zi) {
            int best = -1;
            long ba = -1;
            for (int i = 0; i < nc; ++i) {
                if (NULL == cand[i] || outs[i] != lo->output) continue;
                struct wlr_box b = wlmtk_window_get_bounding_box(cand[i]);
                long a = wz_rect_overlap_area(
                    (wz_rect){ b.x, b.y, b.width, b.height }, z[zi]);
                if (a > ba) {
                    ba = a;
                    best = i;
                }
            }
            if (0 > best) break;
            _snap(zones_ptr, cand[best], z[zi], zi);
            cand[best] = NULL;
        }
    }
}

/** Puts the window back to its box from before (or out of the tiling). */
static void _restore(wlmaker_zones_t *zones_ptr, zones_window_t *w)
{
    if (NULL == w) return;
    if (w->tiled) {
        w->user_float = true;
        _wlmaker_zones_tile_run(zones_ptr);
    } else if (w->has_restore) {
        wlmtk_window_request_bounding_box(w->window_ptr, &w->restore);
        w->has_restore = false;
        w->zone = -1;
    }
}

/** Hides a preview. */
static int _wlmaker_zones_preview_done(void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    if (NULL == zones_ptr->drag_window_ptr && !zones_ptr->picker) {
        _wlmaker_zones_overlay_hide(zones_ptr);
    }
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

/** Copies a string for the tab-separated output, without tabs or newlines. */
static const char *_field(const char *s, char *buf, size_t len)
{
    snprintf(buf, len, "%s", NULL != s ? s : "");
    for (char *p = buf; *p; ++p) {
        if ('\t' == *p || '\n' == *p || '\r' == *p) *p = ' ';
    }
    return buf;
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
        _out(out_ptr, out_len, "backend=wlmaker\nversion=%s\nzones=%s\n"
             "autotile=%s\nmode=%s\nlayout=%s\nalgorithm=%s\ndesktop=%d\n",
             wz_version(), zones_ptr->cfg.zones ? "on" : "off",
             zones_ptr->cfg.autotile ? "on" : "off",
             wz_mode_name(wz_config_mode(&zones_ptr->cfg)), zones_ptr->cfg.layout,
             wz_algo_name(zones_ptr->cfg.tile.algo), _workspace_number(zones_ptr));
        struct wlr_output_layout_output *lo;
        wl_list_for_each(lo, &zones_ptr->server_ptr->wlr_output_layout_ptr->outputs, link) {
            struct wlr_box b = _output_box(zones_ptr, lo->output);
            struct wlr_box u = wlmtk_desktop_get_usable_area(
                zones_ptr->server_ptr->desktop_ptr, lo->output);
            const wz_layout_def *l = _layout_of(zones_ptr, lo->output);
            _out(out_ptr, out_len,
                 "monitor=%s %dx%d+%d+%d work=%dx%d+%d+%d layout=%s autotile=%s"
                 " scale=%.2f\n",
                 lo->output->name, b.width, b.height, b.x, b.y, u.width, u.height,
                 u.x, u.y, NULL != l ? l->id : "?",
                 _autotile_on(zones_ptr, lo->output) ? "on" : "off",
                 lo->output->scale);
        }
        int tiled = 0, snapped = 0, windows = 0;
        for (int i = 0; i < WINDOWS_MAX; ++i) {
            if (NULL == zones_ptr->windows[i].window_ptr) continue;
            ++windows;
            tiled += zones_ptr->windows[i].tiled;
            snapped += !zones_ptr->windows[i].tiled && 0 <= zones_ptr->windows[i].zone;
        }
        _out(out_ptr, out_len, "windows=%d tiled=%d snapped=%d\n", windows,
             tiled, snapped);
        char title[256];
        wlmtk_window_t *active_ptr = _active(zones_ptr);
        _out(out_ptr, out_len, "active=%s\n", _field(
            NULL != active_ptr ? wlmtk_window_get_title(active_ptr) : "",
            title, sizeof(title)));
    } else if (0 == strcmp(buf, "mode")) {
        if (NULL != arg_ptr) {
            wz_config tmp = zones_ptr->cfg;
            int m = wz_config_set_mode(&tmp, NULL, arg_ptr);
            if (0 > m) {
                _out(out_ptr, out_len, "error=modo desconhecido: %s\n", arg_ptr);
                return;
            }
            bool was = zones_ptr->cfg.autotile;
            if (m != (int)wz_config_mode(&zones_ptr->cfg)) {
                _hist_push_all(zones_ptr, "General", "Zones", "General", "Autotile",
                               "General", "LastMode");
            }
            wz_config_set_mode(&zones_ptr->cfg, zones_ptr->cfg_path_ptr, arg_ptr);
            if (was && !zones_ptr->cfg.autotile) _wlmaker_zones_untile_all(zones_ptr);
            if (!zones_ptr->cfg.zones) _wlmaker_zones_overlay_hide(zones_ptr);
            _wlmaker_zones_tile_mark(zones_ptr);
        }
        _out(out_ptr, out_len, "mode=%s\n",
             wz_mode_name(wz_config_mode(&zones_ptr->cfg)));
    } else if (0 == strcmp(buf, "zones")) {
        bool v = _onoff(arg_ptr, zones_ptr->cfg.zones);
        if (v != (bool)zones_ptr->cfg.zones) {
            wz_hist_entry *e = _hist_begin(zones_ptr);
            _hist_add_cfg(zones_ptr, e, "General", "Zones");
            wz_history_commit(&zones_ptr->history);
        }
        zones_ptr->cfg.zones = v;
        _save(zones_ptr, "General", "Zones", v ? "true" : "false");
        _out(out_ptr, out_len, "zones=%s\n", v ? "on" : "off");
    } else if (0 == strcmp(buf, "autotile")) {
        bool v = _onoff(arg_ptr, zones_ptr->cfg.autotile);
        _hist_push_all(zones_ptr, "General", "Autotile", NULL, NULL, NULL, NULL);
        zones_ptr->cfg.autotile = v;
        _save(zones_ptr, "General", "Autotile", v ? "true" : "false");
        if (!v) _wlmaker_zones_untile_all(zones_ptr);
        _wlmaker_zones_tile_mark(zones_ptr);
        _out(out_ptr, out_len, "autotile=%s\n", v ? "on" : "off");
    } else if (0 == strcmp(buf, "algorithm") && NULL != arg_ptr &&
               (0 <= wz_algo_from_name(arg_ptr) || 0 == strcmp(arg_ptr, "next") ||
                0 == strcmp(arg_ptr, "prev"))) {
        // next/prev: the following or the previous algorithm (dock-app
        // arrows, the wheel in Tiling mode).
        const char *name_ptr = arg_ptr;
        if (0 == strcmp(arg_ptr, "next") || 0 == strcmp(arg_ptr, "prev")) {
            int step = 'n' == arg_ptr[0] ? 1 : WZ_TILE_N - 1;
            name_ptr = wz_algo_name(
                (wz_algo)(((int)zones_ptr->cfg.tile.algo + step) % WZ_TILE_N));
        }
        wz_hist_entry *e = _hist_begin(zones_ptr);
        _hist_add_cfg(zones_ptr, e, "Tiling", "Algorithm");
        wz_history_commit(&zones_ptr->history);
        zones_ptr->cfg.tile.algo = wz_algo_from_name(name_ptr);
        _save(zones_ptr, "Tiling", "Algorithm", name_ptr);
        _wlmaker_zones_tile_mark(zones_ptr);
    } else if (0 == strcmp(buf, "layout") && NULL != arg_ptr) {
        if (0 == strcmp(arg_ptr, "next")) {
            _cycle_layout(zones_ptr, 1);
        } else if (0 == strcmp(arg_ptr, "prev")) {
            _cycle_layout(zones_ptr, -1);
        } else if (NULL != wz_config_layout(&zones_ptr->cfg, arg_ptr)) {
            _set_layout(zones_ptr,
                        wlmaker_server_get_output_at_cursor(zones_ptr->server_ptr),
                        arg_ptr);
        } else {
            _out(out_ptr, out_len, "error=layout desconhecido\n");
        }
    } else if (0 == strcmp(buf, "windows")) {
        // Same columns as on X11: class, instance, title; with "-l" the
        // output, zone, tiling, box, class and title.
        bool long_form = NULL != arg_ptr &&
            (0 == strcmp(arg_ptr, "-l") || 0 == strcmp(arg_ptr, "long"));
        // "-d": why each window is (or is not) tiled.
        bool why_form = NULL != arg_ptr &&
            (0 == strcmp(arg_ptr, "-d") || 0 == strcmp(arg_ptr, "why"));
        wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
            zones_ptr->server_ptr->desktop_ptr);
        bs_dllist_t *list_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
        for (bs_dllist_node_t *d = list_ptr->head_ptr; NULL != d; d = d->next_ptr) {
            wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(d);
            struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
            zones_window_t *w = _win(zones_ptr, window_ptr, false);
            char cls[128], ins[128], title[256];
            _field(wlmtk_window_get_app_id(window_ptr), cls, sizeof(cls));
            _field(wlmtk_window_get_instance(window_ptr), ins, sizeof(ins));
            _field(wlmtk_window_get_title(window_ptr), title, sizeof(title));
            if (why_form) {
                char why[160];
                if (NULL != w && w->tiled) {
                    snprintf(why, sizeof(why), "mosaico");
                } else if (NULL != w && w->rule_ignore) {
                    snprintf(why, sizeof(why), "regra: ignorar");
                } else if (wlmtk_window_is_dialog(window_ptr)) {
                    snprintf(why, sizeof(why), "diálogo");
                } else if (!(wlmtk_window_get_properties(window_ptr) &
                             WLMTK_WINDOW_PROPERTY_RESIZABLE)) {
                    snprintf(why, sizeof(why), "tamanho fixo");
                } else if (wlmtk_window_is_fullscreen(window_ptr)) {
                    snprintf(why, sizeof(why), "tela cheia");
                } else if (wlmtk_window_is_minimized(window_ptr)) {
                    snprintf(why, sizeof(why), "minimizada");
                } else if (NULL != w && w->user_float) {
                    snprintf(why, sizeof(why), "flutuante por escolha do usuário");
                } else if (NULL != w && w->rule_float) {
                    snprintf(why, sizeof(why), "regra: flutuante");
                } else if (NULL != w && w->fit_float) {
                    snprintf(why, sizeof(why), "não cabe: mínimo %dx%d, célula %dx%d",
                             w->fit_min_w, w->fit_min_h, w->fit_cell_w, w->fit_cell_h);
                } else {
                    snprintf(why, sizeof(why), "mosaico desligado ou aguardando");
                }
                int mw, mh;
                wlmtk_window_get_min_bounding_size(window_ptr, &mw, &mh);
                if ((0 < mw || 0 < mh) && (NULL == w || !w->fit_float)) {
                    size_t l = strlen(why);
                    snprintf(why + l, sizeof(why) - l, " (mínimo %dx%d)", mw, mh);
                }
                _out(out_ptr, out_len, "%s\ttiled=%d\t%s\t%s\n", cls,
                     NULL != w ? w->tiled : 0, why, title);
            } else if (long_form) {
                struct wlr_output *o = wlr_output_layout_output_at(
                    zones_ptr->server_ptr->wlr_output_layout_ptr,
                    b.x + b.width / 2.0, b.y + b.height / 2.0);
                _out(out_ptr, out_len, "%s\tzone=%d\ttiled=%d\t%dx%d%+d%+d\t%s\t%s\n",
                     NULL != o ? o->name : "?",
                     NULL != w ? w->zone : -1, NULL != w ? w->tiled : 0,
                     b.width, b.height, b.x, b.y, cls, title);
            } else {
                _out(out_ptr, out_len, "%s\t%s\t%s\n", cls, ins, title);
            }
        }
    } else if (0 == strcmp(buf, "active")) {
        // The activated window: Its box (decorations included) and title.
        // For screenshot tools (big-wm-kwin-screenshot: "active window").
        wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
            zones_ptr->server_ptr->desktop_ptr);
        wlmtk_window_t *window_ptr = NULL != ws_ptr ?
            wlmtk_workspace_get_activated_window(ws_ptr) : NULL;
        if (NULL == window_ptr) {
            _out(out_ptr, out_len, "error=nenhuma janela ativa\n");
        } else {
            struct wlr_box b = wlmtk_window_get_bounding_box(window_ptr);
            char title[256];
            _field(wlmtk_window_get_title(window_ptr), title, sizeof(title));
            _out(out_ptr, out_len, "%dx%d%+d%+d\t%s\n",
                 b.width, b.height, b.x, b.y, title);
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
    } else if (0 == strcmp(buf, "move") && 0 <= _dir(arg_ptr)) {
        _move(zones_ptr, _dir(arg_ptr));
    } else if (0 == strcmp(buf, "swap") && 0 <= _dir(arg_ptr)) {
        zones_window_t *w = _win(zones_ptr, _active(zones_ptr), false);
        if (NULL != w && w->tiled) {
            _swap(zones_ptr, w, _dir(arg_ptr));
        } else {
            _move(zones_ptr, _dir(arg_ptr));
        }
    } else if (0 == strcmp(buf, "focus") && 0 <= _dir(arg_ptr)) {
        _focus(zones_ptr, _dir(arg_ptr));
    } else if (0 == strcmp(buf, "picker")) {
        _picker_open(zones_ptr);
    } else if (0 == strcmp(buf, "undo") || 0 == strcmp(buf, "redo")) {
        bool redo = 0 == strcmp(buf, "redo");
        if (!_hist_step(zones_ptr, redo)) _out(out_ptr, out_len, "%s=empty\n", buf);
    } else if (0 == strcmp(buf, "restore")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = _win(zones_ptr, window_ptr, false);
        if (NULL != w && (w->tiled || w->has_restore)) {
            _hist_push(zones_ptr, &window_ptr, 1);
            _restore(zones_ptr, w);
        }
    } else if (0 == strcmp(buf, "restore-all")) {
        _hist_push_all(zones_ptr, NULL, NULL, NULL, NULL, NULL, NULL);
        for (int i = 0; i < WINDOWS_MAX; ++i) {
            zones_window_t *w = &zones_ptr->windows[i];
            if (NULL != w->window_ptr && !w->tiled) _restore(zones_ptr, w);
        }
    } else if (0 == strcmp(buf, "arrange")) {
        _arrange(zones_ptr);
    } else if (0 == strcmp(buf, "float")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = NULL != window_ptr ? _win(zones_ptr, window_ptr, true) : NULL;
        if (NULL != w) {
            _hist_push(zones_ptr, &window_ptr, 1);
            w->user_float = !w->user_float;
            w->rule_float = false;
            if (!w->user_float) w->order = ++zones_ptr->order_seq;
            _wlmaker_zones_tile_run(zones_ptr);
        }
    } else if (0 == strcmp(buf, "promote")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = _win(zones_ptr, window_ptr, false);
        if (NULL != w && w->tiled) {
            zones_window_t *first = NULL;
            for (int i = 0; i < WINDOWS_MAX; ++i) {
                zones_window_t *o = &zones_ptr->windows[i];
                if (NULL != o->window_ptr && o->tiled && o != w &&
                    (NULL == first || o->order < first->order)) first = o;
            }
            if (NULL != first && first->order < w->order) {
                wlmtk_window_t *pair[2] = { w->window_ptr, first->window_ptr };
                _hist_push(zones_ptr, pair, 2);
                long t = w->order;
                w->order = first->order;
                first->order = t;
                zones_tset_t *s = _tset(zones_ptr, wlmtk_window_get_wlr_output(window_ptr), false);
                if (NULL != s) {
                    wz_bsp_swap(&s->bsp, (long)(uintptr_t)w->window_ptr,
                                (long)(uintptr_t)first->window_ptr);
                }
                _wlmaker_zones_tile_mark(zones_ptr);
            }
        }
    } else if (0 == strcmp(buf, "grow") || 0 == strcmp(buf, "shrink")) {
        wlmtk_window_t *window_ptr = _active(zones_ptr);
        zones_window_t *w = _win(zones_ptr, window_ptr, false);
        struct wlr_output *o = NULL != window_ptr ?
            wlmtk_window_get_wlr_output(window_ptr) :
            wlmaker_server_get_output_at_cursor(zones_ptr->server_ptr);
        zones_tset_t *s = _tset(zones_ptr, o, true);
        double delta = 0 == strcmp(buf, "grow") ? 0.05 : -0.05;
        if (NULL != s) {
            int algo = wz_config_algo_for(&zones_ptr->cfg, o->name,
                                          _workspace_number(zones_ptr));
            if (WZ_TILE_BSP == algo && NULL != w && w->tiled) {
                wz_bsp_resize(&s->bsp, (long)(uintptr_t)window_ptr, WZ_RIGHT, delta);
                wz_bsp_resize(&s->bsp, (long)(uintptr_t)window_ptr, WZ_LEFT, delta);
            } else {
                wz_hist_entry *e = _hist_begin(zones_ptr);
                wz_hist_set_aux(e, lround(s->ratio * 10000));
                if (NULL != window_ptr) _hist_add(zones_ptr, e, window_ptr, NULL);
                wz_history_commit(&zones_ptr->history);
                s->ratio = BS_MAX(0.1, BS_MIN(0.9, s->ratio + delta));
            }
            for (int i = 0; i < WINDOWS_MAX; ++i) {
                memset(&zones_ptr->windows[i].expect, 0, sizeof(struct wlr_box));
            }
            _wlmaker_zones_tile_mark(zones_ptr);
        }
    } else if (0 == strcmp(buf, "preview") && NULL != arg_ptr) {
        if (NULL == zones_ptr->drag_window_ptr && !zones_ptr->picker) {
            _wlmaker_zones_overlay_show(
                zones_ptr, wlmaker_server_get_output_at_cursor(zones_ptr->server_ptr),
                arg_ptr, OV_PREVIEW);
            if (NULL != zones_ptr->preview_timer_ptr) {
                wl_event_source_timer_update(zones_ptr->preview_timer_ptr, 1800);
            }
        }
    } else if (0 == strcmp(buf, "overlay")) {
        // State of the overlay (tests and diagnostics).
        _out(out_ptr, out_len,
             "overlay=%s output=%s mode=%d zones=%d strip=%d sel=%s hover=%d"
             " picker=%d hl=%d\n",
             NULL != zones_ptr->overlay_output_ptr ? "shown" : "hidden",
             NULL != zones_ptr->overlay_output_ptr ?
             zones_ptr->overlay_output_ptr->name : "-",
             zones_ptr->ov_mode, zones_ptr->nzones, zones_ptr->strip,
             0 <= zones_ptr->strip_sel ? zones_ptr->strip_id[zones_ptr->strip_sel] : "-",
             zones_ptr->strip_hover, zones_ptr->picker, zones_ptr->hl_a);
        for (int i = 0; i < zones_ptr->strip_n; ++i) {
            wz_rect c = zones_ptr->strip_cell[i], t = zones_ptr->strip_thumb[i];
            _out(out_ptr, out_len, "cell=%s %dx%d%+d%+d thumb=%dx%d%+d%+d\n",
                 zones_ptr->strip_id[i], c.w, c.h, c.x, c.y, t.w, t.h, t.x, t.y);
        }
    } else if (0 == strcmp(buf, "reload")) {
        wlmaker_zones_reload(zones_ptr);
    } else if (0 == strcmp(buf, "editor")) {
        wlm_util_subprocess_monitor_run(zones_ptr->server_ptr->monitor_ptr,
            bs_subprocess_create_cmdline("wmake-zones-prefs --editor"));
    } else if (0 == strcmp(buf, "prefs")) {
        // Optional page: general, zones, tiling, monitors, appearance,
        // keys, rules, advanced.
        char cmd[96] = "wmake-zones-prefs";
        if (NULL != arg_ptr && strlen(arg_ptr) < 40 &&
            strspn(arg_ptr, "abcdefghijklmnopqrstuvwxyz") == strlen(arg_ptr)) {
            snprintf(cmd, sizeof(cmd), "wmake-zones-prefs --%s", arg_ptr);
        }
        wlm_util_subprocess_monitor_run(zones_ptr->server_ptr->monitor_ptr,
            bs_subprocess_create_cmdline(cmd));
    } else if (0 == strcmp(buf, "quit")) {
        // The compositor stays; nothing to do.
    } else {
        _out(out_ptr, out_len, "error=comando desconhecido: %s\n", buf);
    }
}

/* == Socket =============================================================== */

/** A client of the control socket, read as its bytes arrive. */
struct zones_conn {
    /** Back-link. */
    wlmaker_zones_t           *zones_ptr;
    /** Socket. */
    int                       fd;
    /** Its event source. */
    struct wl_event_source    *source_ptr;
    /** Bytes read. */
    size_t                    len;
    /** The command line. */
    char                      line[WZ_IPC_MAX];
};

/** Closes a client and forgets it. */
static void _zones_conn_close(struct zones_conn *conn_ptr)
{
    wlmaker_zones_t *zones_ptr = conn_ptr->zones_ptr;
    for (size_t i = 0; i < sizeof(zones_ptr->conn_ptr) / sizeof(zones_ptr->conn_ptr[0]); ++i) {
        if (zones_ptr->conn_ptr[i] == conn_ptr) zones_ptr->conn_ptr[i] = NULL;
    }
    if (NULL != conn_ptr->source_ptr) wl_event_source_remove(conn_ptr->source_ptr);
    close(conn_ptr->fd);
    free(conn_ptr);
}

/** Bytes from a client: once the line is complete, one command, one reply. */
static int _zones_conn_readable(int fd, __UNUSED__ uint32_t mask, void *data_ptr)
{
    struct zones_conn *conn_ptr = data_ptr;
    ssize_t n = recv(fd, conn_ptr->line + conn_ptr->len,
                     sizeof(conn_ptr->line) - 1 - conn_ptr->len, MSG_DONTWAIT);
    if (0 > n && (EAGAIN == errno || EINTR == errno)) return 0;
    if (0 < n) {
        conn_ptr->len += (size_t)n;
        conn_ptr->line[conn_ptr->len] = '\0';
        if (NULL == memchr(conn_ptr->line, '\n', conn_ptr->len) &&
            conn_ptr->len < sizeof(conn_ptr->line) - 1) {
            return 0;  // More to come.
        }
    }
    if (0 < conn_ptr->len) {
        static char out[16384];
        wlmaker_zones_command(conn_ptr->zones_ptr, conn_ptr->line, out, sizeof(out));
        size_t l = strlen(out);
        if (0 == l) strcpy(out, "ok\n");
        strncat(out, "\n", sizeof(out) - strlen(out) - 1);
        (void)!send(fd, out, strlen(out), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    _zones_conn_close(conn_ptr);
    return 0;
}

/** A client connected: read it from the event loop, never blocking. */
static int _wlmaker_zones_handle_listen(int fd, __UNUSED__ uint32_t mask, void *data_ptr)
{
    wlmaker_zones_t *zones_ptr = data_ptr;
    int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (0 > c) return 0;
    size_t slot = 0;
    const size_t max = sizeof(zones_ptr->conn_ptr) / sizeof(zones_ptr->conn_ptr[0]);
    while (slot < max && NULL != zones_ptr->conn_ptr[slot]) ++slot;
    if (!wz_ipc_peer_ok(c) || slot >= max) {
        close(c);
        return 0;
    }
    struct zones_conn *conn_ptr = calloc(1, sizeof(*conn_ptr));
    if (NULL == conn_ptr) {
        close(c);
        return 0;
    }
    conn_ptr->zones_ptr = zones_ptr;
    conn_ptr->fd = c;
    conn_ptr->source_ptr = wl_event_loop_add_fd(
        wl_display_get_event_loop(zones_ptr->server_ptr->wl_display_ptr),
        c, WL_EVENT_READABLE, _zones_conn_readable, conn_ptr);
    if (NULL == conn_ptr->source_ptr) {
        close(c);
        free(conn_ptr);
        return 0;
    }
    zones_ptr->conn_ptr[slot] = conn_ptr;
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
    zones_ptr->strip_hover = zones_ptr->strip_mini = zones_ptr->strip_sel = -1;
    zones_ptr->cfg_path_ptr = wz_config_path();
    wz_config_defaults(&zones_ptr->cfg);
    wz_config_load(&zones_ptr->cfg, zones_ptr->cfg_path_ptr);
    wz_theme_load(&zones_ptr->theme, NULL);
    wz_history_init(&zones_ptr->history, zones_ptr->cfg.history_depth);

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
        .pointer_accepts_motion = _wlmaker_zones_buffer_accepts_motion,
        .pointer_button = _wlmaker_zones_buffer_pointer_button };
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
    wlmtk_util_connect_listener_signal(&e->window_resize_end,
        &zones_ptr->resize_end_listener, _wlmaker_zones_handle_resize_end);
    wlmtk_util_connect_listener_signal(&e->window_mapped,
        &zones_ptr->window_mapped_listener, _wlmaker_zones_handle_window_mapped);
    wlmtk_util_connect_listener_signal(&e->window_unmapped,
        &zones_ptr->window_unmapped_listener, _wlmaker_zones_handle_window_unmapped);
    wlmtk_util_connect_listener_signal(&e->workspace_changed,
        &zones_ptr->workspace_changed_listener, _wlmaker_zones_handle_workspace_changed);
    wlmtk_util_connect_listener_signal(&e->usable_area_changed,
        &zones_ptr->usable_area_listener, _wlmaker_zones_handle_usable_area);
    wlmtk_util_connect_listener_signal(&server_ptr->wlr_output_layout_ptr->events.change,
        &zones_ptr->output_layout_change_listener, _wlmaker_zones_handle_output_layout_change);

    struct wl_event_loop *loop_ptr = wl_display_get_event_loop(server_ptr->wl_display_ptr);
    zones_ptr->tile_timer_ptr = wl_event_loop_add_timer(
        loop_ptr, _wlmaker_zones_tile_run, zones_ptr);
    zones_ptr->preview_timer_ptr = wl_event_loop_add_timer(
        loop_ptr, _wlmaker_zones_preview_done, zones_ptr);
    zones_ptr->dwell_timer_ptr = wl_event_loop_add_timer(
        loop_ptr, _wlmaker_zones_dwell, zones_ptr);
    zones_ptr->resize_timer_ptr = wl_event_loop_add_timer(
        loop_ptr, _wlmaker_zones_resize_done, zones_ptr);

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
            if (0 != stat(zones_ptr->sock_path_ptr, &zones_ptr->sock_stat)) {
                memset(&zones_ptr->sock_stat, 0, sizeof(zones_ptr->sock_stat));
            }
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
    _picker_close(zones_ptr);
    _wlmaker_zones_overlay_hide(zones_ptr);
    if (NULL != zones_ptr->esc_binding_ptr) {
        wlmim_unbind_key(zones_ptr->server_ptr->input_manager_ptr,
                         zones_ptr->esc_binding_ptr);
    }
    for (size_t i = 0; i < sizeof(zones_ptr->conn_ptr) / sizeof(zones_ptr->conn_ptr[0]); ++i) {
        if (NULL != zones_ptr->conn_ptr[i]) _zones_conn_close(zones_ptr->conn_ptr[i]);
    }
    if (NULL != zones_ptr->listen_source_ptr) {
        wl_event_source_remove(zones_ptr->listen_source_ptr);
    }
    if (0 <= zones_ptr->listen_fd) close(zones_ptr->listen_fd);
    if (NULL != zones_ptr->sock_path_ptr) {
        if (0 <= zones_ptr->listen_fd) {
            wz_ipc_unlink(zones_ptr->sock_path_ptr, &zones_ptr->sock_stat);
        }
        free(zones_ptr->sock_path_ptr);
    }
    struct wl_event_source *timers[] = {
        zones_ptr->tile_timer_ptr, zones_ptr->preview_timer_ptr,
        zones_ptr->dwell_timer_ptr, zones_ptr->resize_timer_ptr };
    for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); ++i) {
        if (NULL != timers[i]) wl_event_source_remove(timers[i]);
    }
    wlmtk_util_disconnect_listener(&zones_ptr->output_layout_change_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->usable_area_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->workspace_changed_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->window_unmapped_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->window_mapped_listener);
    wlmtk_util_disconnect_listener(&zones_ptr->resize_end_listener);
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
    for (int i = 0; i < TSETS_MAX; ++i) wz_bsp_clear(&zones_ptr->tsets[i].bsp);
    _strip_clear(zones_ptr);
    _drag_layout_clear(zones_ptr);
    free(zones_ptr->shown_tree_ptr);
    wz_history_free(&zones_ptr->history);
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
    wz_history_set_depth(&zones_ptr->history, zones_ptr->cfg.history_depth);
    // Rules may have changed: Look them up again for new windows only.
    if (was && !zones_ptr->cfg.autotile) _wlmaker_zones_untile_all(zones_ptr);
    _wlmaker_zones_tile_mark(zones_ptr);
}

#endif  // WLMAKER_HAVE_ZONES

/* == End of zones.c ======================================================= */
