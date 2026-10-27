/* ========================================================================= */
/**
 * @file dock_launchers.c
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

#include "dock_launchers.h"

#include <cairo.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "clip.h"
#include "config.h"
#include "toplevel_tracker.h"

/* == Declarations ========================================================= */

static void _wlmaker_dock_app_count(wlmtk_window_t *window_ptr, void *ud_ptr);
static void _wlmaker_dock_app_hide(wlmtk_window_t *window_ptr, void *ud_ptr);
static void _wlmaker_dock_app_unhide(wlmtk_window_t *window_ptr, void *ud_ptr);
static void _wlmaker_dock_app_bring_here(
    wlmtk_window_t *window_ptr,
    void *ud_ptr);
static void _wlmaker_dock_app_collect_pid(
    wlmtk_window_t *window_ptr,
    void *ud_ptr);

/** Counts windows, and minimized ones. */
typedef struct {
    /** Windows. */
    size_t                    windows;
    /** Minimized windows. */
    size_t                    hidden;
} _wlmaker_dock_app_count_t;

/** Arguments for @ref _wlmaker_dock_app_bring_here. */
typedef struct {
    /** The current workspace. */
    wlmtk_workspace_t         *workspace_ptr;
    /** The last window brought here: Activated. */
    wlmtk_window_t            *last_window_ptr;
} _wlmaker_dock_app_bring_here_t;

/** Process IDs of an application's windows. */
typedef struct {
    /** The PIDs. */
    pid_t                     pids[32];
    /** Number of PIDs. */
    size_t                    n;
} _wlmaker_dock_app_pids_t;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bspl_array_t *wlmaker_dock_launchers_load(
    wlmaker_server_t *server_ptr,
    const char *section_ptr)
{
    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        server_ptr->files_ptr, server_ptr->state_fname_ptr);
    if (NULL == state_dict_ptr) return NULL;
    bspl_array_t *launchers_ptr = bspl_dict_get_array(
        bspl_dict_get_dict(state_dict_ptr, section_ptr), "Launchers");
    bspl_array_t *array_ptr = bspl_array_create();
    for (size_t i = 0;
         NULL != array_ptr && NULL != launchers_ptr &&
             i < bspl_array_size(launchers_ptr);
         ++i) {
        bspl_object_t *o = bspl_array_at(launchers_ptr, i);
        bspl_dict_t *d = bspl_dict_from_object(o);
        if (NULL == d) continue;
        // wlmdock leaves out launchers without icon.
        char *p = wlmaker_dock_launcher_icon_path(
            server_ptr->files_ptr, wlmaker_dock_launcher_value(d, "Icon"));
        if (NULL == p) continue;
        free(p);
        if (!bspl_array_push_back(array_ptr, o)) {
            bspl_array_unref(array_ptr);
            array_ptr = NULL;
        }
    }
    bspl_dict_unref(state_dict_ptr);
    return array_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_launchers_store(
    wlmaker_server_t *server_ptr,
    const char *section_ptr,
    bspl_array_t *array_ptr)
{
    bspl_dict_t *d = bspl_dict_create();
    bool rv = NULL != d &&
        bspl_dict_add(d, "Launchers", bspl_object_from_array(array_ptr)) &&
        wlmaker_state_store_section(server_ptr->files_ptr,
                                    server_ptr->state_fname_ptr,
                                    section_ptr, d);
    if (NULL != d) bspl_dict_unref(d);
    if (!rv) {
        bs_log(BS_WARNING, "Failed to store the launchers of '%s'",
               section_ptr);
        return false;
    }
    wlmaker_clip_launchers_changed(server_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
const char *wlmaker_dock_launcher_value(
    bspl_dict_t *dict_ptr,
    const char *key_ptr)
{
    if (NULL == dict_ptr) return NULL;
    return bspl_dict_get_string_value(dict_ptr, key_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_launcher_flag(
    bspl_dict_t *dict_ptr,
    const char *key_ptr)
{
    const char *v = wlmaker_dock_launcher_value(dict_ptr, key_ptr);
    if (NULL == v) return false;
    return 0 == strcasecmp(v, "Yes") || 0 == strcasecmp(v, "True") ||
        0 == strcasecmp(v, "On") || 0 == strcmp(v, "1");
}

/* ------------------------------------------------------------------------- */
char *wlmaker_dock_launcher_balloon_text(bspl_dict_t *dict_ptr)
{
    const char *app_id_ptr = wlmaker_dock_launcher_value(dict_ptr, "AppID");
    const char *cmd_ptr = wlmaker_dock_launcher_value(dict_ptr, "CommandLine");
    if (NULL != app_id_ptr && '\0' != *app_id_ptr) {
        if (NULL == cmd_ptr || '\0' == *cmd_ptr) return logged_strdup(app_id_ptr);
        return bs_strdupf("%s\n(%s)", app_id_ptr, cmd_ptr);
    }
    if (NULL == cmd_ptr || '\0' == *cmd_ptr) return NULL;

    // The program: The first word, without its path.
    size_t len = strcspn(cmd_ptr, " \t");
    const char *name_ptr = cmd_ptr;
    for (size_t i = 0; i < len; ++i) {
        if ('/' == cmd_ptr[i]) name_ptr = &cmd_ptr[i + 1];
    }
    int name_len = (int)(cmd_ptr + len - name_ptr);
    if (0 >= name_len) return logged_strdup(cmd_ptr);
    return bs_strdupf("%.*s\n(%s)", name_len, name_ptr, cmd_ptr);
}

/* ------------------------------------------------------------------------- */
char *wlmaker_dock_launcher_icon_path(
    wlm_util_files_t *files_ptr,
    const char *icon_ptr)
{
    if (NULL == icon_ptr || '\0' == *icon_ptr) return NULL;
    char *path_ptr = NULL;
    if ('/' == icon_ptr[0] || '~' == icon_ptr[0]) {
        path_ptr = bs_file_resolve_path(icon_ptr, NULL);
    } else {
        char *p = bs_strdupf("icons/%s", icon_ptr);
        if (NULL != p && NULL != files_ptr) {
            path_ptr = wlm_util_files_xdg_data_find(files_ptr, p, S_IFREG);
        }
        if (NULL != p) free(p);
#ifdef WLMAKER_SOURCE_DIR
        if (NULL == path_ptr) {
            path_ptr = bs_strdupf(
                WLMAKER_SOURCE_DIR "/share/wlmaker/icons/%s", icon_ptr);
        }
#endif
    }
    if (NULL != path_ptr && !bs_file_realpath_is(path_ptr, S_IFREG)) {
        free(path_ptr);
        path_ptr = NULL;
    }
    return path_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_buffer *wlmaker_dock_tile_buffer(
    const struct wlmtk_tile_style *style_ptr,
    const char *image_path_ptr,
    struct wlr_buffer *image_ptr)
{
    int size = style_ptr->size;
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        size, size);
    if (NULL == wlr_buffer_ptr) return NULL;
    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }
    wlmaker_primitives_cairo_fill(cairo_ptr, &style_ptr->fill);
    wlmaker_primitives_draw_bezel(cairo_ptr, style_ptr->bezel_width, true);

    cairo_surface_t *s_ptr = NULL;
    void *data_ptr = NULL;
    if (NULL != image_path_ptr) {
        s_ptr = cairo_image_surface_create_from_png(image_path_ptr);
    } else if (NULL != image_ptr) {
        uint32_t format;
        size_t stride;
        if (wlr_buffer_begin_data_ptr_access(
                image_ptr, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                &data_ptr, &format, &stride)) {
            s_ptr = cairo_image_surface_create_for_data(
                data_ptr, CAIRO_FORMAT_ARGB32,
                image_ptr->width, image_ptr->height, stride);
        } else {
            data_ptr = NULL;
        }
    }
    if (NULL != s_ptr && CAIRO_STATUS_SUCCESS == cairo_surface_status(s_ptr)) {
        int w = cairo_image_surface_get_width(s_ptr);
        int h = cairo_image_surface_get_height(s_ptr);
        int content = style_ptr->content_size;
        if (0 < w && 0 < h) {
            double scale = BS_MIN((double)content / w, (double)content / h);
            // Images from a buffer (eg. a dock-app) are shown as they are.
            if (NULL != image_ptr && NULL == image_path_ptr) scale = 1.0;
            cairo_save(cairo_ptr);
            cairo_translate(cairo_ptr,
                            (size - w * scale) / 2.0,
                            (size - h * scale) / 2.0);
            cairo_scale(cairo_ptr, scale, scale);
            cairo_set_source_surface(cairo_ptr, s_ptr, 0, 0);
            cairo_paint(cairo_ptr);
            cairo_restore(cairo_ptr);
        }
    }
    if (NULL != s_ptr) cairo_surface_destroy(s_ptr);
    if (NULL != data_ptr) wlr_buffer_end_data_ptr_access(image_ptr);
    cairo_destroy(cairo_ptr);
    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
int wlmaker_dock_tile_index_at(
    wlmaker_dock_place_t place,
    int width,
    int height,
    double x,
    double y,
    int pitch)
{
    if (0 >= pitch || x < 0 || y < 0 || x >= width || y >= height) return -1;
    double d;
    if (wlmaker_dock_place_vertical(place)) {
        d = (WLR_EDGE_BOTTOM == place.anchor) ? height - y : y;
    } else {
        d = (WLR_EDGE_RIGHT == place.anchor) ? width - x : x;
    }
    if (0 > d) return -1;
    return (int)(d / pitch);
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_dock_app_windows(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr,
    size_t *hidden_ptr)
{
    _wlmaker_dock_app_count_t count = {};
    wlmaker_toplevel_tracker_for_each_app_window(
        server_ptr->toplevel_tracker_ptr, app_id_ptr,
        _wlmaker_dock_app_count, &count);
    if (NULL != hidden_ptr) *hidden_ptr = count.hidden;
    return count.windows;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_app_hide(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr)
{
    wlmaker_toplevel_tracker_for_each_app_window(
        server_ptr->toplevel_tracker_ptr, app_id_ptr,
        _wlmaker_dock_app_hide, NULL);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_app_unhide(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr)
{
    wlmaker_toplevel_tracker_for_each_app_window(
        server_ptr->toplevel_tracker_ptr, app_id_ptr,
        _wlmaker_dock_app_unhide, NULL);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_app_bring_here(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr)
{
    _wlmaker_dock_app_bring_here_t arg = {
        .workspace_ptr = wlmtk_desktop_get_current_workspace(
            server_ptr->desktop_ptr) };
    if (NULL == arg.workspace_ptr) return;
    wlmaker_toplevel_tracker_for_each_app_window(
        server_ptr->toplevel_tracker_ptr, app_id_ptr,
        _wlmaker_dock_app_bring_here, &arg);
    if (NULL != arg.last_window_ptr) {
        wlmtk_workspace_activate_window(arg.workspace_ptr,
                                        arg.last_window_ptr);
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_app_kill(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr)
{
    _wlmaker_dock_app_pids_t pids = {};
    wlmaker_toplevel_tracker_for_each_app_window(
        server_ptr->toplevel_tracker_ptr, app_id_ptr,
        _wlmaker_dock_app_collect_pid, &pids);
    for (size_t i = 0; i < pids.n; ++i) {
        bs_log(BS_INFO, "Terminating \"%s\", PID %"PRIdMAX,
               app_id_ptr, (intmax_t)pids.pids[i]);
        if (0 != kill(pids.pids[i], SIGTERM)) {
            bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%"PRIdMAX", SIGTERM)",
                   (intmax_t)pids.pids[i]);
        }
    }
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Counts the window, and whether it is minimized. */
void _wlmaker_dock_app_count(wlmtk_window_t *window_ptr, void *ud_ptr)
{
    _wlmaker_dock_app_count_t *count_ptr = ud_ptr;
    ++count_ptr->windows;
    if (wlmtk_window_is_minimized(window_ptr)) ++count_ptr->hidden;
}

/* ------------------------------------------------------------------------- */
/** Minimizes the window. */
void _wlmaker_dock_app_hide(wlmtk_window_t *window_ptr,
                            __UNUSED__ void *ud_ptr)
{
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    if (NULL == workspace_ptr) return;
    wlmtk_workspace_minimize_window(workspace_ptr, window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Restores the window, where it is. */
void _wlmaker_dock_app_unhide(wlmtk_window_t *window_ptr,
                              __UNUSED__ void *ud_ptr)
{
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    if (NULL == workspace_ptr || !wlmtk_window_is_minimized(window_ptr)) {
        return;
    }
    wlmtk_workspace_raise_window(workspace_ptr, window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Moves the window to the current workspace, restores and raises it. */
void _wlmaker_dock_app_bring_here(wlmtk_window_t *window_ptr, void *ud_ptr)
{
    _wlmaker_dock_app_bring_here_t *arg_ptr = ud_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    if (NULL == workspace_ptr) return;
    if (workspace_ptr != arg_ptr->workspace_ptr) {
        wlmtk_workspace_unmap_window(workspace_ptr, window_ptr);
        wlmtk_workspace_map_window(arg_ptr->workspace_ptr, window_ptr);
    }
    wlmtk_workspace_raise_window(arg_ptr->workspace_ptr, window_ptr);
    arg_ptr->last_window_ptr = window_ptr;
}

/* ------------------------------------------------------------------------- */
/** Collects the process ID of the window's client, once. */
void _wlmaker_dock_app_collect_pid(wlmtk_window_t *window_ptr, void *ud_ptr)
{
    _wlmaker_dock_app_pids_t *pids_ptr = ud_ptr;
    const wlmtk_util_client_t *client_ptr = wlmtk_window_get_client_ptr(
        window_ptr);
    if (NULL == client_ptr) return;
    pid_t pid = client_ptr->pid;
    // Never ourselves, nor a process group.
    if (pid <= 1 || pid == getpid()) return;
    for (size_t i = 0; i < pids_ptr->n; ++i) {
        if (pids_ptr->pids[i] == pid) return;
    }
    if (pids_ptr->n < sizeof(pids_ptr->pids) / sizeof(pid_t)) {
        pids_ptr->pids[pids_ptr->n++] = pid;
    }
}

/* == Unit tests =========================================================== */

static void test_balloon_text(bs_test_t *test_ptr);
static void test_index_at(bs_test_t *test_ptr);
static void test_tile_buffer(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_dock_launchers_test_cases[] = {
    { true, "balloon_text", test_balloon_text },
    { true, "index_at", test_index_at },
    { true, "tile_buffer", test_tile_buffer },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dock_launchers_test_set = BS_TEST_SET(
    true, "dock_launchers", _wlmaker_dock_launchers_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests the balloon text, as Window Maker's: Name, and (command). */
void test_balloon_text(bs_test_t *test_ptr)
{
    bspl_object_t *o = bspl_create_object_from_plist_string(
        "{AppID = firefox; CommandLine = \"/usr/bin/big-wm-open browser\";}");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    char *t = wlmaker_dock_launcher_balloon_text(bspl_dict_from_object(o));
    BS_TEST_VERIFY_STREQ(test_ptr, "firefox\n(/usr/bin/big-wm-open browser)", t);
    free(t);
    bspl_object_unref(o);

    o = bspl_create_object_from_plist_string(
        "{CommandLine = \"/usr/bin/thunar --daemon\"; Lock = Yes;}");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    t = wlmaker_dock_launcher_balloon_text(bspl_dict_from_object(o));
    BS_TEST_VERIFY_STREQ(test_ptr, "thunar\n(/usr/bin/thunar --daemon)", t);
    free(t);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_launcher_flag(
                            bspl_dict_from_object(o), "Lock"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_launcher_flag(
                             bspl_dict_from_object(o), "AutoLaunch"));
    bspl_object_unref(o);
}

/* ------------------------------------------------------------------------- */
/** Tests finding the tile at a position, counted from the anchor. */
void test_index_at(bs_test_t *test_ptr)
{
    wlmaker_dock_place_t p = { WLR_EDGE_RIGHT, WLR_EDGE_TOP };
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dock_tile_index_at(
                          p, 64, 194, 10, 10, 65));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dock_tile_index_at(
                          p, 64, 194, 10, 140, 65));
    BS_TEST_VERIFY_EQ(test_ptr, -1, wlmaker_dock_tile_index_at(
                          p, 64, 194, 10, 200, 65));
    p = (wlmaker_dock_place_t){ WLR_EDGE_LEFT, WLR_EDGE_BOTTOM };
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_dock_tile_index_at(
                          p, 64, 194, 10, 190, 65));
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_dock_tile_index_at(
                          p, 64, 194, 10, 10, 65));
    p = (wlmaker_dock_place_t){ WLR_EDGE_BOTTOM, WLR_EDGE_RIGHT };
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dock_tile_index_at(
                          p, 129, 64, 10, 10, 65));
}

/* ------------------------------------------------------------------------- */
/** Tests drawing a tile with an image. */
void test_tile_buffer(bs_test_t *test_ptr)
{
    struct wlmtk_tile_style style = {
        .fill = { .type = WLMTK_STYLE_COLOR_SOLID,
                  .param = { .solid = { .color = 0xff808080 } } },
        .bezel_width = 2, .size = 64, .content_size = 48 };
    struct wlr_buffer *image_ptr = bs_gfxbuf_create_wlr_buffer(48, 48);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, image_ptr);
    bs_gfxbuf_clear(bs_gfxbuf_from_wlr_buffer(image_ptr), 0xffff0000);
    struct wlr_buffer *t = wlmaker_dock_tile_buffer(&style, NULL, image_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, t);
    bs_gfxbuf_t *g = bs_gfxbuf_from_wlr_buffer(t);
    BS_TEST_VERIFY_EQ(test_ptr, 0xffff0000, g->data_ptr[32 * 64 + 32]);
    BS_TEST_VERIFY_EQ(test_ptr, 0xff808080, g->data_ptr[4 * 64 + 4]);
    wlr_buffer_drop(t);
    wlr_buffer_drop(image_ptr);

    // An icon file.
    t = wlmaker_dock_tile_buffer(
        &style, WLMAKER_SOURCE_DIR "/share/wlmaker/icons/chrome-56x56.png",
        NULL);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, t);
    wlr_buffer_drop(t);
}

/* == End of dock_launchers.c ============================================== */
