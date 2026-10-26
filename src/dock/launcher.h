/* ========================================================================= */
/**
 * @file launcher.h
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
#ifndef __WLMAKER_DOCK_LAUNCHER_H__
#define __WLMAKER_DOCK_LAUNCHER_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>

#include "toolkit/toolkit.h"
#include "util/files.h"
#include "util/subprocess_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

struct wlmdock_toplevel_tracker;

/** Forward declaration: Launcher handle. */
typedef struct _wlmdock_launcher_t wlmdock_launcher_t;

/**
 * Creates an application launcher, configured from a plist dict.
 *
 * @param style_ptr
 * @param dict_ptr
 * @param tracker_ptr
 * @param monitor_ptr
 * @param files_ptr
 *
 * @return Pointer to the launcher handle or NULL on error.
 */
wlmdock_launcher_t *wlmdock_launcher_create_from_plist(
    const struct wlmtk_tile_style *style_ptr,
    bspl_dict_t *dict_ptr,
    struct wlmdock_toplevel_tracker *tracker_ptr,
    wlm_util_subprocess_monitor_t *monitor_ptr,
    wlm_util_files_t *files_ptr);

/**
 * Destroys the application launcher.
 *
 * @param launcher_ptr
 */
void wlmdock_launcher_destroy(wlmdock_launcher_t *launcher_ptr);

/** @return A pointer to the @ref wlmtk_tile_t superclass of `launcher_ptr`. */
wlmtk_tile_t *wlmdock_launcher_tile(wlmdock_launcher_t *launcher_ptr);

/** @return The launcher for the tile `tile_ptr`, if it is one. Else NULL. */
wlmdock_launcher_t *wlmdock_launcher_from_tile(wlmtk_tile_t *tile_ptr);

/** Handler for a right-click on the launcher, eg. to open its menu. */
typedef void (*wlmdock_launcher_menu_handler_t)(
    wlmdock_launcher_t *launcher_ptr,
    void *ud_ptr);

/** Sets the handler for a right-click on the launcher. */
void wlmdock_launcher_set_menu_handler(
    wlmdock_launcher_t *launcher_ptr,
    wlmdock_launcher_menu_handler_t handler,
    void *ud_ptr);

/** Launches (another instance of) the launcher's application. */
void wlmdock_launcher_launch(wlmdock_launcher_t *launcher_ptr);

/** @return Whether processes launched by the launcher are running. */
bool wlmdock_launcher_has_processes(wlmdock_launcher_t *launcher_ptr);

/** Terminates (SIGTERM) the processes launched by the launcher. */
void wlmdock_launcher_kill(wlmdock_launcher_t *launcher_ptr);

/** @return The plist dict the launcher was created from. */
bspl_dict_t *wlmdock_launcher_dict(wlmdock_launcher_t *launcher_ptr);

/** Unit test set. */
extern const bs_test_set_t wlmdock_launcher_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCK_LAUNCHER_H__ */
/* == End of launcher.h ==================================================== */
