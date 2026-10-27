/* ========================================================================= */
/**
 * @file toplevel_tracker.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser <kaeser@gubbe.ch>
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

#include "toplevel_tracker.h"

#include <libbase/libbase.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#undef WLR_USE_UNSTABLE

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the toplevel tracker. */
struct wlmaker_toplevel_tracker {
    /** The tracked toplevels. */
    bs_dllist_t               toplevels;

    /** Handle for the `ext-foreign-toplevel-list-v1` protocol. */
    struct wlr_ext_foreign_toplevel_list_v1 *ext_foreign_toplevel_list_ptr;
    /** Handle for `wlr-foreign-toplevel-management-unstable-v1` (docks, taskbars). */
    struct wlr_foreign_toplevel_manager_v1 *wlr_foreign_toplevel_manager_ptr;

    /** Connects to a `struct wl_display` destroy notification. */
    struct wl_listener        display_destroy_listener;
};

/** Handle of one tracked toplevel. */
struct wlmaker_toplevel_tracker_handle {
    /** Back-link to the toplevel tracker. */
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr;
    /** List node, to @ref wlmaker_toplevel_tracker::toplevels. */
    bs_dllist_node_t          dlnode;

    /** Handle for this toplevel for the ext-foreign-toplevel list. */
    struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel_handle_ptr;

    /** The toplevel's title. */
    char                      *title_ptr;
    /** The toplevel's application ID. */
    char                      *app_id_ptr;

    /** The window for this toplevel. */
    wlmtk_window_t            *window_ptr;

    /** Handle for wlr-foreign-toplevel-management. */
    struct wlr_foreign_toplevel_handle_v1 *wlr_handle_ptr;
    /** Listeners for requests from wlr-foreign-toplevel-management clients. */
    struct wl_listener        request_activate_listener;
    /** See above. */
    struct wl_listener        request_minimize_listener;
    /** See above. */
    struct wl_listener        request_close_listener;
    /** Window activation changes, mirrored as the `activated` state. */
    struct wl_listener        window_set_activated_listener;
};

static void _wlmaker_toplevel_tracker_handle_request_activate(
    struct wl_listener *listener_ptr, void *data_ptr);
static void _wlmaker_toplevel_tracker_handle_request_minimize(
    struct wl_listener *listener_ptr, void *data_ptr);
static void _wlmaker_toplevel_tracker_handle_request_close(
    struct wl_listener *listener_ptr, void *data_ptr);
static void _wlmaker_toplevel_tracker_handle_window_set_activated(
    struct wl_listener *listener_ptr, void *data_ptr);

static void _wlmaker_toplevel_tracker_destroy(
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr);
static void _wlmaker_toplevel_tracker_handle_display_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Data ================================================================= */

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
struct wlmaker_toplevel_tracker *wlmaker_toplevel_tracker_create(
    struct wl_display *wl_display_ptr)
{
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr = logged_calloc(
        1, sizeof(*toplevel_tracker_ptr));
    if (NULL == toplevel_tracker_ptr) return NULL;

    // Note: Will automatically be destroyed when wl_display_ptr destroys.
    toplevel_tracker_ptr->ext_foreign_toplevel_list_ptr =
        wlr_ext_foreign_toplevel_list_v1_create(wl_display_ptr, 1);
    toplevel_tracker_ptr->wlr_foreign_toplevel_manager_ptr =
        wlr_foreign_toplevel_manager_v1_create(wl_display_ptr);

    toplevel_tracker_ptr->display_destroy_listener.notify =
        _wlmaker_toplevel_tracker_handle_display_destroy;
    wl_display_add_destroy_listener(
        wl_display_ptr, &toplevel_tracker_ptr->display_destroy_listener);
    return toplevel_tracker_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlmaker_toplevel_tracker_handle *wlmaker_toplevel_tracker_handle_create(
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr,
    wlmtk_window_t *window_ptr)
{
    struct wlmaker_toplevel_tracker_handle *handle_ptr = logged_calloc(
        1, sizeof(*handle_ptr));
    if (NULL == handle_ptr) return NULL;
    handle_ptr->window_ptr = window_ptr;

    struct wlr_ext_foreign_toplevel_handle_v1_state state = {};
    handle_ptr->ext_foreign_toplevel_handle_ptr =
        wlr_ext_foreign_toplevel_handle_v1_create(
            toplevel_tracker_ptr->ext_foreign_toplevel_list_ptr,
            &state);

    if (NULL != toplevel_tracker_ptr->wlr_foreign_toplevel_manager_ptr) {
        handle_ptr->wlr_handle_ptr = wlr_foreign_toplevel_handle_v1_create(
            toplevel_tracker_ptr->wlr_foreign_toplevel_manager_ptr);
    }
    if (NULL != handle_ptr->wlr_handle_ptr) {
        wlmtk_util_connect_listener_signal(
            &handle_ptr->wlr_handle_ptr->events.request_activate,
            &handle_ptr->request_activate_listener,
            _wlmaker_toplevel_tracker_handle_request_activate);
        wlmtk_util_connect_listener_signal(
            &handle_ptr->wlr_handle_ptr->events.request_minimize,
            &handle_ptr->request_minimize_listener,
            _wlmaker_toplevel_tracker_handle_request_minimize);
        wlmtk_util_connect_listener_signal(
            &handle_ptr->wlr_handle_ptr->events.request_close,
            &handle_ptr->request_close_listener,
            _wlmaker_toplevel_tracker_handle_request_close);
        if (NULL != window_ptr) {
            wlmtk_util_connect_listener_signal(
                &wlmtk_window_events(window_ptr)->set_activated,
                &handle_ptr->window_set_activated_listener,
                _wlmaker_toplevel_tracker_handle_window_set_activated);
        }
    }

    bs_dllist_push_back(
        &toplevel_tracker_ptr->toplevels,
        &handle_ptr->dlnode);
    handle_ptr->toplevel_tracker_ptr = toplevel_tracker_ptr;
    return handle_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_toplevel_tracker_handle_destroy(
    struct wlmaker_toplevel_tracker_handle *handle_ptr)
{
    if (NULL != handle_ptr->wlr_handle_ptr) {
        wlmtk_util_disconnect_listener(&handle_ptr->request_activate_listener);
        wlmtk_util_disconnect_listener(&handle_ptr->request_minimize_listener);
        wlmtk_util_disconnect_listener(&handle_ptr->request_close_listener);
        if (NULL != handle_ptr->window_ptr) {
            wlmtk_util_disconnect_listener(
                &handle_ptr->window_set_activated_listener);
        }
        wlr_foreign_toplevel_handle_v1_destroy(handle_ptr->wlr_handle_ptr);
        handle_ptr->wlr_handle_ptr = NULL;
    }
    if (NULL != handle_ptr->ext_foreign_toplevel_handle_ptr) {
        wlr_ext_foreign_toplevel_handle_v1_destroy(
            handle_ptr->ext_foreign_toplevel_handle_ptr);
        handle_ptr->ext_foreign_toplevel_handle_ptr = NULL;
    }

    if (NULL != handle_ptr->toplevel_tracker_ptr) {
        bs_dllist_remove(
            &handle_ptr->toplevel_tracker_ptr->toplevels,
            &handle_ptr->dlnode);
        handle_ptr->toplevel_tracker_ptr = NULL;
    }

    if (NULL != handle_ptr->title_ptr) {
        free(handle_ptr->title_ptr);
        handle_ptr->title_ptr = NULL;
    }
    if (NULL != handle_ptr->app_id_ptr) {
        free(handle_ptr->app_id_ptr);
        handle_ptr->app_id_ptr = NULL;
    }

    free(handle_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_toplevel_tracker_handle_set_title(
    struct wlmaker_toplevel_tracker_handle *handle_ptr,
    const char *title_ptr)
{
    char *copied_title_ptr = NULL;
    if (NULL != title_ptr) {
        copied_title_ptr = logged_strdup(title_ptr);
        if (NULL == copied_title_ptr) return false;
    }

    if (NULL != handle_ptr->title_ptr) {
        free(handle_ptr->title_ptr);
    }
    handle_ptr->title_ptr = copied_title_ptr;

    if (NULL != handle_ptr->ext_foreign_toplevel_handle_ptr) {
        struct wlr_ext_foreign_toplevel_handle_v1_state state = {
            .title = handle_ptr->title_ptr,
            .app_id = handle_ptr->app_id_ptr
        };
        wlr_ext_foreign_toplevel_handle_v1_update_state(
            handle_ptr->ext_foreign_toplevel_handle_ptr, &state);
    }
    if (NULL != handle_ptr->wlr_handle_ptr && NULL != handle_ptr->title_ptr) {
        wlr_foreign_toplevel_handle_v1_set_title(
            handle_ptr->wlr_handle_ptr, handle_ptr->title_ptr);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_toplevel_tracker_handle_set_app_id(
    struct wlmaker_toplevel_tracker_handle *handle_ptr,
    const char *app_id_ptr)
{
    char *copied_app_id_ptr = NULL;
    if (NULL != app_id_ptr) {
        copied_app_id_ptr = logged_strdup(app_id_ptr);
        if (NULL == copied_app_id_ptr) return false;
    }

    if (NULL != handle_ptr->app_id_ptr) {
        free(handle_ptr->app_id_ptr);
    }
    handle_ptr->app_id_ptr = copied_app_id_ptr;

    if (NULL != handle_ptr->ext_foreign_toplevel_handle_ptr) {
        struct wlr_ext_foreign_toplevel_handle_v1_state state = {
            .title = handle_ptr->title_ptr,
            .app_id = handle_ptr->app_id_ptr
        };
        wlr_ext_foreign_toplevel_handle_v1_update_state(
            handle_ptr->ext_foreign_toplevel_handle_ptr, &state);
    }
    if (NULL != handle_ptr->wlr_handle_ptr && NULL != handle_ptr->app_id_ptr) {
        wlr_foreign_toplevel_handle_v1_set_app_id(
            handle_ptr->wlr_handle_ptr, handle_ptr->app_id_ptr);
    }
    return true;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Handles `request_activate`: raises and activates, restoring if minimized. */
void _wlmaker_toplevel_tracker_handle_request_activate(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    struct wlmaker_toplevel_tracker_handle *handle_ptr = BS_CONTAINER_OF(
        listener_ptr, struct wlmaker_toplevel_tracker_handle,
        request_activate_listener);
    if (NULL == handle_ptr->window_ptr) return;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        handle_ptr->window_ptr);
    if (NULL == workspace_ptr) return;
    wlmtk_workspace_raise_window(workspace_ptr, handle_ptr->window_ptr);
    wlmtk_workspace_activate_window(workspace_ptr, handle_ptr->window_ptr);
    wlr_foreign_toplevel_handle_v1_set_minimized(handle_ptr->wlr_handle_ptr, false);
}

/* ------------------------------------------------------------------------- */
/** Handles `request_minimize` (set_minimized / unset_minimized). */
void _wlmaker_toplevel_tracker_handle_request_minimize(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    struct wlmaker_toplevel_tracker_handle *handle_ptr = BS_CONTAINER_OF(
        listener_ptr, struct wlmaker_toplevel_tracker_handle,
        request_minimize_listener);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event_ptr = data_ptr;
    if (NULL == handle_ptr->window_ptr) return;
    if (event_ptr->minimized) {
        wlmtk_window_request_minimize(handle_ptr->window_ptr);
    } else {
        wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
            handle_ptr->window_ptr);
        if (NULL != workspace_ptr) {
            wlmtk_workspace_raise_window(workspace_ptr, handle_ptr->window_ptr);
        }
    }
    wlr_foreign_toplevel_handle_v1_set_minimized(
        handle_ptr->wlr_handle_ptr, event_ptr->minimized);
}

/* ------------------------------------------------------------------------- */
/** Handles `request_close`. */
void _wlmaker_toplevel_tracker_handle_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    struct wlmaker_toplevel_tracker_handle *handle_ptr = BS_CONTAINER_OF(
        listener_ptr, struct wlmaker_toplevel_tracker_handle,
        request_close_listener);
    if (NULL != handle_ptr->window_ptr) {
        wlmtk_window_request_close(handle_ptr->window_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Mirrors the window's activation into the `activated` state. */
void _wlmaker_toplevel_tracker_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    struct wlmaker_toplevel_tracker_handle *handle_ptr = BS_CONTAINER_OF(
        listener_ptr, struct wlmaker_toplevel_tracker_handle,
        window_set_activated_listener);
    bool activated = wlmtk_window_is_activated(handle_ptr->window_ptr);
    if (activated) {
        wlr_foreign_toplevel_handle_v1_set_minimized(
            handle_ptr->wlr_handle_ptr, false);
    }
    wlr_foreign_toplevel_handle_v1_set_activated(
        handle_ptr->wlr_handle_ptr, activated);
}

/* ------------------------------------------------------------------------- */
/** Destroys the toplevel tracker. */
void _wlmaker_toplevel_tracker_destroy(
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr)
{
    wlmtk_util_disconnect_listener(
        &toplevel_tracker_ptr->display_destroy_listener);

    free(toplevel_tracker_ptr);
}

/* ------------------------------------------------------------------------- */
/** wl_display destroy handler. Call @ref _wlmaker_toplevel_tracker_destroy. */
void _wlmaker_toplevel_tracker_handle_display_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
     struct wlmaker_toplevel_tracker *toplevel_tracker_ptr = BS_CONTAINER_OF(
        listener_ptr,
        struct wlmaker_toplevel_tracker,
        display_destroy_listener);
    _wlmaker_toplevel_tracker_destroy(toplevel_tracker_ptr);
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_toplevel_tracker_for_each_app_window(
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr,
    const char *app_id_ptr,
    wlmaker_toplevel_tracker_window_callback_t callback,
    void *ud_ptr)
{
    if (NULL == toplevel_tracker_ptr || NULL == app_id_ptr ||
        '\0' == *app_id_ptr) return 0;
    size_t windows = 0;
    bs_dllist_node_t *dlnode_ptr = toplevel_tracker_ptr->toplevels.head_ptr;
    while (NULL != dlnode_ptr) {
        struct wlmaker_toplevel_tracker_handle *handle_ptr = BS_CONTAINER_OF(
            dlnode_ptr, struct wlmaker_toplevel_tracker_handle, dlnode);
        // The callback may close the window: Step ahead first.
        dlnode_ptr = dlnode_ptr->next_ptr;
        if (NULL == handle_ptr->window_ptr ||
            NULL == handle_ptr->app_id_ptr ||
            0 != strcmp(handle_ptr->app_id_ptr, app_id_ptr)) continue;
        ++windows;
        if (NULL != callback) callback(handle_ptr->window_ptr, ud_ptr);
    }
    return windows;
}

/* == Unit Tests =========================================================== */

static void _wlmaker_toplevel_tracker_test_handle(bs_test_t *test_ptr);
static void _wlmaker_toplevel_tracker_test_app_windows(bs_test_t *test_ptr);

/** Unit test cases. */
const bs_test_case_t _wlmaker_toplevel_tracker_test_cases[] = {
    { true, "handle", _wlmaker_toplevel_tracker_test_handle },
    { true, "app_windows", _wlmaker_toplevel_tracker_test_app_windows },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_toplevel_tracker_test_set = BS_TEST_SET(
    true, "toplevel_tracker", _wlmaker_toplevel_tracker_test_cases);


/* ------------------------------------------------------------------------- */
/** Tests handle basic functionality. */
void _wlmaker_toplevel_tracker_test_handle(bs_test_t *test_ptr)
{
    struct wlmaker_toplevel_tracker_handle *h = logged_calloc(1, sizeof(*h));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, h);

    wlmaker_toplevel_tracker_handle_set_title(h, "title0");
    wlmaker_toplevel_tracker_handle_set_title(h, NULL);
    wlmaker_toplevel_tracker_handle_set_title(h, "title1");

    wlmaker_toplevel_tracker_handle_set_app_id(h, "app_id0");
    wlmaker_toplevel_tracker_handle_set_app_id(h, NULL);
    wlmaker_toplevel_tracker_handle_set_app_id(h, "app_id1");

    wlmaker_toplevel_tracker_handle_destroy(h);
}

/* ------------------------------------------------------------------------- */
/** Counts calls, for the test. */
static void _test_count_window(__UNUSED__ wlmtk_window_t *w, void *ud_ptr)
{
    ++*(int*)ud_ptr;
}

/** Tests finding the windows of an application by app ID. */
void _wlmaker_toplevel_tracker_test_app_windows(bs_test_t *test_ptr)
{
    struct wlmaker_toplevel_tracker tracker = {};
    struct wlmaker_toplevel_tracker_handle h[3] = {};
    // Not dereferenced: Just to tell them apart.
    int windows[3];
    const char *app_ids[3] = { "firefox", "thunar", "firefox" };
    for (int i = 0; i < 3; ++i) {
        h[i].window_ptr = (wlmtk_window_t*)&windows[i];
        h[i].app_id_ptr = (char*)app_ids[i];
        bs_dllist_push_back(&tracker.toplevels, &h[i].dlnode);
    }
    int calls = 0;
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmaker_toplevel_tracker_for_each_app_window(
                          &tracker, "firefox", _test_count_window, &calls));
    BS_TEST_VERIFY_EQ(test_ptr, 2, calls);
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_toplevel_tracker_for_each_app_window(
                          &tracker, "thunar", NULL, NULL));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_toplevel_tracker_for_each_app_window(
                          &tracker, "", NULL, NULL));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlmaker_toplevel_tracker_for_each_app_window(
                          &tracker, NULL, NULL, NULL));
}

/* == End of toplevel_tracker.c ============================================ */
