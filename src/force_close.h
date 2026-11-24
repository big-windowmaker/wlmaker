/* ========================================================================= */
/**
 * @file force_close.h
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
#ifndef __FORCE_CLOSE_H__
#define __FORCE_CLOSE_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <sys/types.h>

/** Forward declaration: Selecting a window to close by force (xkill). */
typedef struct _wlmaker_force_close_t wlmaker_force_close_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Creates the handler.
 *
 * @param server_ptr
 * @param config_dict_ptr     The 'ForceClose' dictionary. May be NULL.
 *
 * @return The handler, or NULL on error.
 */
wlmaker_force_close_t *wlmaker_force_close_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr);

/** Destroys the handler. */
void wlmaker_force_close_destroy(wlmaker_force_close_t *fc_ptr);

/** Applies the 'ForceClose' dictionary (on reload). */
bool wlmaker_force_close_reconfigure(
    wlmaker_force_close_t *fc_ptr,
    bspl_dict_t *config_dict_ptr);

/**
 * The `ForceCloseSelect` action: The pointer turns into a skull; a click
 * on a window closes its client by force. Escape, another button or the
 * action again cancel.
 */
void wlmaker_force_close_toggle(wlmaker_force_close_t *fc_ptr);

/** @return Whether a window is being selected. */
bool wlmaker_force_close_active(wlmaker_force_close_t *fc_ptr);

/**
 * Tells whether a process is part of the session, and never to be closed
 * by force: The compositor itself and its ancestors, the X server, system
 * and session services (systemd, D-Bus, PipeWire...), the Big WindowMaker
 * components, the Dock and the dock-apps.
 *
 * @param pid
 * @param comm_ptr            The process' name, as /proc/<pid>/comm. NULL
 *                            to read it.
 * @param exe_ptr             Base name of the executable, or of the script
 *                            run by an interpreter. NULL to read it.
 *
 * @return true if protected.
 */
bool wlmaker_force_close_protected(
    pid_t pid,
    const char *comm_ptr,
    const char *exe_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_force_close_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __FORCE_CLOSE_H__ */
/* == End of force_close.h ================================================= */
