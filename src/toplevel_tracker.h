/* ========================================================================= */
/**
 * @file toplevel_tracker.h
 *
 * Methods to maintain the set of all registered toplevels, so the compositor
 * can share the inofrmation with clients, such as a dock.
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
#ifndef __WLMAKER_TOPLEVEL_TRACKER_H__
#define __WLMAKER_TOPLEVEL_TRACKER_H__

#include "toolkit/toolkit.h"
#include <libbase/libbase.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

struct wl_display;

struct wlmaker_toplevel_tracker;
struct wlmaker_toplevel_tracker_handle;

/**
 * Creates a toplevel tracker.
 *
 * @param wl_display_ptr
 *
 * @return The toplevel tracker handle, or NULL on error. The toplevel tracker
 *     will be automatically destroyed when `wl_display_ptr` is destroyed.
 */
struct wlmaker_toplevel_tracker *wlmaker_toplevel_tracker_create(
    struct wl_display *wl_display_ptr);

/**
 * Creates a toplevel handle.
 *
 * @param toplevel_tracker_ptr
 * @param window_ptr
 *
 * @return The toplevel handle, or NULL on error.
 */
struct wlmaker_toplevel_tracker_handle *wlmaker_toplevel_tracker_handle_create(
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr,
    wlmtk_window_t *window_ptr);

/**
 * Destroys the toplevel handle.
 *
 * @param handle_ptr
 */
void wlmaker_toplevel_tracker_handle_destroy(
    struct wlmaker_toplevel_tracker_handle *handle_ptr);

/** Sets the title for this toplevel. */
bool wlmaker_toplevel_tracker_handle_set_title(
    struct wlmaker_toplevel_tracker_handle *handle_ptr,
    const char *title_ptr);

/** Sets the app ID for this toplevel. */
bool wlmaker_toplevel_tracker_handle_set_app_id(
    struct wlmaker_toplevel_tracker_handle *handle_ptr,
    const char *title_ptr);

/** Callback for @ref wlmaker_toplevel_tracker_for_each_app_window. */
typedef void (*wlmaker_toplevel_tracker_window_callback_t)(
    wlmtk_window_t *window_ptr,
    void *ud_ptr);

/**
 * Calls `callback` for each window of the application `app_id_ptr`: The
 * toplevels with that app ID. Like Window Maker's application, for the
 * docked icon's "Hide", "Bring Here" and "Kill".
 *
 * @param toplevel_tracker_ptr
 * @param app_id_ptr          Neither NULL nor empty, else nothing matches.
 * @param callback            May be NULL, to just count.
 * @param ud_ptr
 *
 * @return Number of windows.
 */
size_t wlmaker_toplevel_tracker_for_each_app_window(
    struct wlmaker_toplevel_tracker *toplevel_tracker_ptr,
    const char *app_id_ptr,
    wlmaker_toplevel_tracker_window_callback_t callback,
    void *ud_ptr);

/** Toplevel tracker test set. */
extern const bs_test_set_t wlmaker_toplevel_tracker_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // __WLMAKER_TOPLEVEL_TRACKER_H__
/* == End of toplevel_tracker.h ============================================ */
