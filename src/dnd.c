/* ========================================================================= */
/**
 * @file dnd.c
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

#include "dnd.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "clip.h"
#include "dock_launchers.h"
#include "drawer.h"
#include "input/manager.h"
#include "layer_panel.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the drag and drop handler. */
struct _wlmaker_dnd_t {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** Parent of the dragged icon. */
    struct wlr_scene_tree     *parent_ptr;
    /** Listener for `request_start_drag` of the seat. */
    struct wl_listener        request_start_drag_listener;
    /** Listener for `start_drag` of the seat. */
    struct wl_listener        start_drag_listener;

    /** The current drag, if any. */
    struct wlr_drag           *drag_ptr;
    /** Listener for `destroy` of the drag. */
    struct wl_listener        drag_destroy_listener;
    /** The dragged icon's tree, at the pointer. Or NULL. */
    struct wlr_scene_tree     *icon_tree_ptr;
    /** Listener for `motion` of the cursor, while dragging. */
    struct wl_listener        cursor_motion_listener;
    /** Listener for `motion_absolute` of the cursor, while dragging. */
    struct wl_listener        cursor_motion_absolute_listener;
    /** Listener for `button` of the cursor, while dragging. */
    struct wl_listener        cursor_button_listener;
};

/** A drop taken by the compositor: Reading the data. */
typedef struct {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** The drag's source, until it goes away. */
    struct wlr_data_source    *source_ptr;
    /** Listener for `destroy` of the source. */
    struct wl_listener        source_destroy_listener;
    /** The `DropCommand`. */
    char                      *command_ptr;
    /** Whether the data is a "text/uri-list". */
    bool                      uri_list;
} wlmaker_dnd_drop_t;

static void _wlmaker_dnd_handle_request_start_drag(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_handle_start_drag(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_handle_drag_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_handle_cursor_motion(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_handle_cursor_motion_absolute(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_handle_cursor_button(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_update_icon(wlmaker_dnd_t *dnd_ptr);
static bspl_dict_t *_wlmaker_dnd_launcher_at(
    wlmaker_server_t *server_ptr,
    double x,
    double y);
static bool _wlmaker_dnd_drop_on_launcher(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *dict_ptr);
static void _wlmaker_dnd_drop_handle_source_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dnd_drop_read(void *ud_ptr, const char *text_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_dnd_t *wlmaker_dnd_create(
    wlmaker_server_t *server_ptr,
    struct wlr_scene_tree *parent_ptr)
{
    wlmaker_dnd_t *dnd_ptr = logged_calloc(1, sizeof(*dnd_ptr));
    if (NULL == dnd_ptr) return NULL;
    dnd_ptr->server_ptr = server_ptr;
    dnd_ptr->parent_ptr = wlr_scene_tree_create(parent_ptr);
    if (NULL == dnd_ptr->parent_ptr) {
        free(dnd_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &server_ptr->wlr_seat_ptr->events.request_start_drag,
        &dnd_ptr->request_start_drag_listener,
        _wlmaker_dnd_handle_request_start_drag);
    wlmtk_util_connect_listener_signal(
        &server_ptr->wlr_seat_ptr->events.start_drag,
        &dnd_ptr->start_drag_listener,
        _wlmaker_dnd_handle_start_drag);
    return dnd_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dnd_destroy(wlmaker_dnd_t *dnd_ptr)
{
    if (NULL != dnd_ptr->drag_ptr) {
        _wlmaker_dnd_handle_drag_destroy(&dnd_ptr->drag_destroy_listener,
                                         dnd_ptr->drag_ptr);
    }
    wlmtk_util_disconnect_listener(&dnd_ptr->start_drag_listener);
    wlmtk_util_disconnect_listener(&dnd_ptr->request_start_drag_listener);
    wlr_scene_node_destroy(&dnd_ptr->parent_ptr->node);
    free(dnd_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Takes the drop of the current drag, for the launcher `dict_ptr`.
 *
 * @return Whether it took the drop: There is a drag with a source, and the
 *     launcher has a `DropCommand`. The drag is then over.
 */
bool _wlmaker_dnd_drop_on_launcher(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *dict_ptr)
{
    struct wlr_seat *seat_ptr = server_ptr->wlr_seat_ptr;
    struct wlr_drag *drag_ptr = NULL != seat_ptr ? seat_ptr->drag : NULL;
    if (NULL == drag_ptr || NULL == drag_ptr->source) return false;
    const char *cmd_ptr = wlmaker_dock_launcher_value(dict_ptr, "DropCommand");
    if (NULL == cmd_ptr || '\0' == *cmd_ptr) return false;

    // Window Maker's dock takes files (XDND's text/uri-list). Else, text.
    struct wlr_data_source *source_ptr = drag_ptr->source;
    static const char *mimes[] = {
        "text/uri-list", "text/plain;charset=utf-8", "UTF8_STRING",
        "text/plain", NULL };
    const char *mime_ptr = NULL;
    for (const char **m = mimes; NULL == mime_ptr && NULL != *m; ++m) {
        char **t_ptr;
        wl_array_for_each(t_ptr, &source_ptr->mime_types) {
            if (0 == strcmp(*t_ptr, *m)) {
                mime_ptr = *m;
                break;
            }
        }
    }
    if (NULL == mime_ptr) {
        bs_log(BS_INFO, "DropCommand: Nothing to take from the drag.");
        return false;
    }

    int fds[2];
    if (0 != pipe(fds)) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed pipe()");
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    wlmaker_dnd_drop_t *drop_ptr = logged_calloc(1, sizeof(*drop_ptr));
    if (NULL != drop_ptr) drop_ptr->command_ptr = logged_strdup(cmd_ptr);
    if (NULL == drop_ptr || NULL == drop_ptr->command_ptr) {
        if (NULL != drop_ptr) free(drop_ptr);
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    drop_ptr->server_ptr = server_ptr;
    drop_ptr->source_ptr = source_ptr;
    drop_ptr->uri_list = 0 == strcmp(mime_ptr, "text/uri-list");
    wlmtk_util_connect_listener_signal(
        &source_ptr->events.destroy,
        &drop_ptr->source_destroy_listener,
        _wlmaker_dnd_drop_handle_source_destroy);

    bs_log(BS_INFO, "DropCommand: Taking the drop, as \"%s\".", mime_ptr);
    // As a drop on a client: The action, the data, then "drop performed".
    uint32_t action = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    if (0 != source_ptr->actions && 0 == (source_ptr->actions & action)) {
        action = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    }
    wlr_data_source_dnd_action(source_ptr, action);
    wlr_data_source_send(source_ptr, mime_ptr, fds[1]);
    wlr_data_source_dnd_drop(source_ptr);
    // Ends the drag (it keeps the source, which is finished when read).
    wlr_seat_pointer_end_grab(seat_ptr);

    if (!wlmaker_dock_launchers_read_fd(server_ptr, fds[0],
                                        _wlmaker_dnd_drop_read, drop_ptr)) {
        _wlmaker_dnd_drop_read(drop_ptr, NULL);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
char **wlmaker_dnd_args(const char *text_ptr, bool uri_list, size_t *args_ptr)
{
    *args_ptr = 0;
    size_t len = strlen(text_ptr);
    char **args = logged_calloc(len + 2, sizeof(char*));
    if (NULL == args) return NULL;
    if (!uri_list) {
        // Text: One argument, without the trailing newline.
        while (0 < len && ('\n' == text_ptr[len - 1] ||
                           '\r' == text_ptr[len - 1])) --len;
        if (0 < len) {
            args[0] = logged_calloc(len + 1, 1);
            if (NULL == args[0]) {
                free(args);
                return NULL;
            }
            memcpy(args[0], text_ptr, len);
            *args_ptr = 1;
        }
        return args;
    }

    // RFC 2483: One URI per line, CRLF; '#' starts a comment.
    const char *line_ptr = text_ptr;
    while ('\0' != *line_ptr) {
        const char *end_ptr = strchr(line_ptr, '\n');
        if (NULL == end_ptr) end_ptr = line_ptr + strlen(line_ptr);
        const char *e = end_ptr;
        while (e > line_ptr && ('\r' == e[-1] || ' ' == e[-1])) --e;
        const char *s = line_ptr;
        if (e > s && '#' != *s) {
            bool file = 0 == strncasecmp(s, "file://", 7);
            if (file) {
                s += 7;
                // file://host/path: The path. Only the local host.
                const char *slash_ptr = memchr(s, '/', e - s);
                s = NULL != slash_ptr ? slash_ptr : e;
            }
            char *a = logged_calloc(e - s + 1, 1);
            if (NULL == a) {
                wlmaker_dnd_free_args(args);
                return NULL;
            }
            size_t n = 0;
            for (const char *c = s; c < e; ++c) {
                if (file && '%' == c[0] && c + 2 < e &&
                    isxdigit((unsigned char)c[1]) &&
                    isxdigit((unsigned char)c[2])) {
                    char hex[3] = { c[1], c[2], '\0' };
                    a[n++] = (char)strtol(hex, NULL, 16);
                    c += 2;
                } else {
                    a[n++] = *c;
                }
            }
            if (0 < n) {
                args[(*args_ptr)++] = a;
            } else {
                free(a);
            }
        }
        line_ptr = '\0' == *end_ptr ? end_ptr : end_ptr + 1;
    }
    return args;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dnd_free_args(char **args_ptr)
{
    if (NULL == args_ptr) return;
    for (char **a = args_ptr; NULL != *a; ++a) free(*a);
    free(args_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * A client asks to start a drag: Granted if from a button it holds down on
 * its surface (the serial of the seat's pointer grab).
 */
void _wlmaker_dnd_handle_request_start_drag(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_dnd_t *dnd_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_t, request_start_drag_listener);
    struct wlr_seat_request_start_drag_event *event_ptr = data_ptr;
    struct wlr_seat *seat_ptr = dnd_ptr->server_ptr->wlr_seat_ptr;
    if (wlr_seat_validate_pointer_grab_serial(
            seat_ptr, event_ptr->origin, event_ptr->serial)) {
        wlr_seat_start_pointer_drag(seat_ptr, event_ptr->drag,
                                    event_ptr->serial);
        return;
    }
    bs_log(BS_INFO, "Refusing drag of %p: Invalid serial %"PRIu32,
           event_ptr->drag, event_ptr->serial);
    wlr_data_source_destroy(event_ptr->drag->source);
}

/* ------------------------------------------------------------------------- */
/** A drag started: Shows its icon at the pointer. */
void _wlmaker_dnd_handle_start_drag(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_dnd_t *dnd_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_t, start_drag_listener);
    struct wlr_drag *drag_ptr = data_ptr;
    if (NULL != dnd_ptr->drag_ptr) {
        _wlmaker_dnd_handle_drag_destroy(&dnd_ptr->drag_destroy_listener,
                                         dnd_ptr->drag_ptr);
    }
    dnd_ptr->drag_ptr = drag_ptr;
    bs_log(BS_INFO, "Drag %p started, icon %p.", drag_ptr, drag_ptr->icon);
    wlmtk_util_connect_listener_signal(
        &drag_ptr->events.destroy,
        &dnd_ptr->drag_destroy_listener,
        _wlmaker_dnd_handle_drag_destroy);

    struct wlr_cursor *c = NULL;
    if (NULL != dnd_ptr->server_ptr->input_manager_ptr) {
        c = wlmim_wlr_cursor(dnd_ptr->server_ptr->input_manager_ptr);
    }
    // The seat has no pointer focus while dragging: Surfaces don't pass the
    // button on. The release ends the drag, wherever it is.
    if (NULL != c) {
        wlmtk_util_connect_listener_signal(
            &c->events.button,
            &dnd_ptr->cursor_button_listener,
            _wlmaker_dnd_handle_cursor_button);
    }
    if (NULL == drag_ptr->icon || NULL == c) return;

    dnd_ptr->icon_tree_ptr = wlr_scene_tree_create(dnd_ptr->parent_ptr);
    if (NULL == dnd_ptr->icon_tree_ptr) return;
    wlr_scene_drag_icon_create(dnd_ptr->icon_tree_ptr, drag_ptr->icon);
    wlr_scene_node_raise_to_top(&dnd_ptr->parent_ptr->node);
    _wlmaker_dnd_update_icon(dnd_ptr);

    wlmtk_util_connect_listener_signal(
        &c->events.motion,
        &dnd_ptr->cursor_motion_listener,
        _wlmaker_dnd_handle_cursor_motion);
    wlmtk_util_connect_listener_signal(
        &c->events.motion_absolute,
        &dnd_ptr->cursor_motion_absolute_listener,
        _wlmaker_dnd_handle_cursor_motion_absolute);
}

/* ------------------------------------------------------------------------- */
/** The drag ended: Removes its icon. */
void _wlmaker_dnd_handle_drag_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_dnd_t *dnd_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_t, drag_destroy_listener);
    wlmtk_util_disconnect_listener(&dnd_ptr->drag_destroy_listener);
    wlmtk_util_disconnect_listener(&dnd_ptr->cursor_motion_listener);
    wlmtk_util_disconnect_listener(&dnd_ptr->cursor_motion_absolute_listener);
    wlmtk_util_disconnect_listener(&dnd_ptr->cursor_button_listener);
    if (NULL != dnd_ptr->icon_tree_ptr) {
        wlr_scene_node_destroy(&dnd_ptr->icon_tree_ptr->node);
        dnd_ptr->icon_tree_ptr = NULL;
    }
    dnd_ptr->drag_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** The pointer moved: The icon follows. */
void _wlmaker_dnd_handle_cursor_motion(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    _wlmaker_dnd_update_icon(BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_t, cursor_motion_listener));
}

/* ------------------------------------------------------------------------- */
/** The pointer moved: The icon follows. */
void _wlmaker_dnd_handle_cursor_motion_absolute(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    _wlmaker_dnd_update_icon(BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_t, cursor_motion_absolute_listener));
}

/* ------------------------------------------------------------------------- */
/**
 * A button of the cursor, while dragging. After the compositor handled it
 * (eg. a drop on a launcher), the seat gets the release, if the drag still
 * goes on: Drops, or cancels.
 */
void _wlmaker_dnd_handle_cursor_button(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_dnd_t *dnd_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_t, cursor_button_listener);
    struct wlr_pointer_button_event *event_ptr = data_ptr;
    struct wlr_seat *seat_ptr = dnd_ptr->server_ptr->wlr_seat_ptr;
    if (WL_POINTER_BUTTON_STATE_RELEASED != event_ptr->state ||
        NULL == seat_ptr->drag || seat_ptr->drag != dnd_ptr->drag_ptr) return;
    // On a launcher with a DropCommand: Taken by the compositor.
    struct wlr_cursor *c = wlmim_wlr_cursor(
        dnd_ptr->server_ptr->input_manager_ptr);
    bspl_dict_t *dict_ptr = _wlmaker_dnd_launcher_at(
        dnd_ptr->server_ptr, c->x, c->y);
    if (NULL != dict_ptr) {
        _wlmaker_dnd_drop_on_launcher(dnd_ptr->server_ptr, dict_ptr);
        bspl_dict_unref(dict_ptr);
    }
    // The seat gets the release: It drops (or cancels) the drag, if still
    // there; or keeps its button state.
    wlr_seat_pointer_notify_button(seat_ptr, event_ptr->time_msec,
                                   event_ptr->button, event_ptr->state);
}

/* ------------------------------------------------------------------------- */
/**
 * @return The launcher at `x`, `y`: Of the Dock, the Clip or an open
 *     drawer, unless a window is there, above. A reference, or NULL.
 */
bspl_dict_t *_wlmaker_dnd_launcher_at(
    wlmaker_server_t *server_ptr,
    double x,
    double y)
{
    // What is there: A client's surface, unless the Dock's or the Clip's
    // (`wlmdock`), is a window: Its client takes the drop. The drag's icon
    // is not considered.
    wlmaker_dnd_t *dnd_ptr = server_ptr->dnd_ptr;
    if (NULL != dnd_ptr) {
        wlr_scene_node_set_enabled(&dnd_ptr->parent_ptr->node, false);
    }
    double sx, sy;
    struct wlr_scene_node *node_ptr = wlr_scene_node_at(
        &server_ptr->wlr_scene_ptr->tree.node, x, y, &sx, &sy);
    if (NULL != dnd_ptr) {
        wlr_scene_node_set_enabled(&dnd_ptr->parent_ptr->node, true);
    }
    struct wlr_scene_surface *ss_ptr = NULL;
    if (NULL != node_ptr && WLR_SCENE_NODE_BUFFER == node_ptr->type) {
        ss_ptr = wlr_scene_surface_try_from_buffer(
            wlr_scene_buffer_from_node(node_ptr));
    }
    if (NULL != ss_ptr) {
        struct wlr_surface *s = wlr_surface_get_root_surface(ss_ptr->surface);
        bool docks =
            (NULL != server_ptr->dock_layer_panel_ptr &&
             s == wlmaker_layer_panel_wlr_surface(
                 server_ptr->dock_layer_panel_ptr)) ||
            (NULL != server_ptr->clip_layer_panel_ptr &&
             s == wlmaker_layer_panel_wlr_surface(
                 server_ptr->clip_layer_panel_ptr));
        if (!docks) return NULL;
    }
    if (NULL == server_ptr->clip_ptr) return NULL;

    for (int id = 0; id < wlmaker_clip_columns(server_ptr); ++id) {
        wlmaker_dock_column_t column;
        if (!wlmaker_clip_column_id(server_ptr, id, &column)) continue;
        wlmaker_drawer_t *drawer_ptr = wlmaker_clip_drawer(server_ptr, id);
        if (NULL != drawer_ptr && wlmaker_drawer_collapsed(drawer_ptr)) {
            continue;
        }
        for (size_t i = 0; i < column.launchers; ++i) {
            int d = (int)(1 + i) * column.pitch;
            struct wlr_box box = { .x = column.x, .y = column.y,
                                   .width = column.size,
                                   .height = column.size };
            switch (column.place.anchor) {
            case WLR_EDGE_TOP: box.y += d; break;
            case WLR_EDGE_BOTTOM: box.y -= d; break;
            case WLR_EDGE_LEFT: box.x += d; break;
            case WLR_EDGE_RIGHT: box.x -= d; break;
            default: break;
            }
            if (!wlr_box_contains_point(&box, x, y)) continue;
            bspl_array_t *a = wlmaker_dock_launchers_load_column(
                server_ptr, id);
            bspl_dict_t *dict_ptr = NULL;
            if (NULL != a && i < bspl_array_size(a)) {
                dict_ptr = bspl_dict_ref(
                    bspl_dict_from_object(bspl_array_at(a, i)));
            }
            if (NULL != a) bspl_array_unref(a);
            return dict_ptr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Places the dragged icon at the pointer. */
void _wlmaker_dnd_update_icon(wlmaker_dnd_t *dnd_ptr)
{
    if (NULL == dnd_ptr->icon_tree_ptr ||
        NULL == dnd_ptr->server_ptr->input_manager_ptr) return;
    struct wlr_cursor *c = wlmim_wlr_cursor(
        dnd_ptr->server_ptr->input_manager_ptr);
    wlr_scene_node_set_position(&dnd_ptr->icon_tree_ptr->node, c->x, c->y);
}

/* ------------------------------------------------------------------------- */
/** The source of a taken drop went away. */
void _wlmaker_dnd_drop_handle_source_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_dnd_drop_t *drop_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dnd_drop_t, source_destroy_listener);
    wlmtk_util_disconnect_listener(&drop_ptr->source_destroy_listener);
    drop_ptr->source_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** The dropped data was read: Runs the DropCommand, finishes the drop. */
void _wlmaker_dnd_drop_read(void *ud_ptr, const char *text_ptr)
{
    wlmaker_dnd_drop_t *drop_ptr = ud_ptr;
    size_t n = 0;
    char **args = NULL;
    if (NULL != text_ptr) {
        args = wlmaker_dnd_args(text_ptr, drop_ptr->uri_list, &n);
    }
    if (0 < n) {
        wlmaker_dock_launcher_run_command(
            drop_ptr->server_ptr, drop_ptr->command_ptr, 'd',
            (const char**)args, n);
    } else {
        bs_log(BS_INFO, "DropCommand: Nothing dropped.");
    }
    wlmaker_dnd_free_args(args);

    if (NULL != drop_ptr->source_ptr) {
        wlr_data_source_dnd_finish(drop_ptr->source_ptr);
        // The source may be gone now.
        if (NULL != drop_ptr->source_ptr) {
            wlmtk_util_disconnect_listener(&drop_ptr->source_destroy_listener);
        }
    }
    free(drop_ptr->command_ptr);
    free(drop_ptr);
}

/* == Unit tests =========================================================== */

static void test_args(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_dnd_test_cases[] = {
    { true, "args", test_args },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dnd_test_set = BS_TEST_SET(
    true, "dnd", _wlmaker_dnd_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests the arguments from dropped data. */
void test_args(bs_test_t *test_ptr)
{
    size_t n;
    char **a = wlmaker_dnd_args(
        "file:///home/u/a%20b.txt\r\n# comment\r\n"
        "file://localhost/tmp/x\r\nhttps://example.org/%20\r\n", true, &n);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, a);
    BS_TEST_VERIFY_EQ_OR_RETURN(test_ptr, 3, n);
    BS_TEST_VERIFY_STREQ(test_ptr, "/home/u/a b.txt", a[0]);
    BS_TEST_VERIFY_STREQ(test_ptr, "/tmp/x", a[1]);
    BS_TEST_VERIFY_STREQ(test_ptr, "https://example.org/%20", a[2]);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, a[3]);
    wlmaker_dnd_free_args(a);

    // Without the final newline; and text.
    a = wlmaker_dnd_args("file:///a", true, &n);
    BS_TEST_VERIFY_EQ(test_ptr, 1, n);
    BS_TEST_VERIFY_STREQ(test_ptr, "/a", a[0]);
    wlmaker_dnd_free_args(a);
    a = wlmaker_dnd_args("some text\n", false, &n);
    BS_TEST_VERIFY_EQ(test_ptr, 1, n);
    BS_TEST_VERIFY_STREQ(test_ptr, "some text", a[0]);
    wlmaker_dnd_free_args(a);
    a = wlmaker_dnd_args("", true, &n);
    BS_TEST_VERIFY_EQ(test_ptr, 0, n);
    wlmaker_dnd_free_args(a);
}

/* == End of dnd.c ========================================================= */
