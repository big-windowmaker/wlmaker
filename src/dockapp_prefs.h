/* ========================================================================= */
/**
 * @file dockapp_prefs.h
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
#ifndef __WLMAKER_DOCKAPP_PREFS_H__
#define __WLMAKER_DOCKAPP_PREFS_H__

#include <stdbool.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

/** Forward declaration: Preferences of a dock-app, for its menu. */
typedef struct _wlmaker_dockapp_prefs_t wlmaker_dockapp_prefs_t;

#include "context_menu.h"
#include "server.h"
#include "util/files.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Loads what the menu of dock-app `name_ptr` offers: Its actions and
 * preferences, from `Dockapps.plist` (in the XDG data directories, eg.
 * /usr/share/wlmaker/Dockapps.plist). And the current values, from the
 * dock-app's preferences file `${XDG_CONFIG_HOME}/wlmaker/<name>.plist`.
 *
 * @param files_ptr
 * @param schema_fname_ptr    Path to `Dockapps.plist`, or NULL to look it up.
 * @param prefs_fname_ptr     Path of the preferences file, or NULL for the
 *                            default.
 * @param name_ptr            The dock-app's name, eg. "wlmcalclock".
 *
 * @return The preferences, or NULL if there are none for the dock-app.
 */
wlmaker_dockapp_prefs_t *wlmaker_dockapp_prefs_load(
    wlm_util_files_t *files_ptr,
    const char *schema_fname_ptr,
    const char *prefs_fname_ptr,
    const char *name_ptr);

/** Destroys the preferences. */
void wlmaker_dockapp_prefs_destroy(wlmaker_dockapp_prefs_t *prefs_ptr);

/**
 * Adds the dock-app's actions to the context menu: Items running a command.
 *
 * @param prefs_ptr
 * @param server_ptr          Runs the commands.
 * @param context_menu_ptr
 *
 * @return true on success.
 */
bool wlmaker_dockapp_prefs_add_actions(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr);

/**
 * Adds the "Preferences" submenu to the context menu: Check boxes for the
 * boolean preferences, and submenus with radio buttons for the others.
 * Choosing one stores it in the preferences file.
 *
 * @param prefs_ptr
 * @param context_menu_ptr
 *
 * @return true on success.
 */
bool wlmaker_dockapp_prefs_add_preferences(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_context_menu_t *context_menu_ptr);

/**
 * @return The current value of preference `key_ptr`: As stored, or its
 *     default. NULL if unknown.
 */
const char *wlmaker_dockapp_prefs_value(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    const char *key_ptr);

/**
 * Expands ${Key} in `command_ptr` with the current values.
 *
 * @return The command, to be free()-ed. NULL on error.
 */
char *wlmaker_dockapp_prefs_expand(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    const char *command_ptr);

/**
 * Stores `value_ptr` for `key_ptr` in the preferences file `fname_ptr`,
 * keeping all other values in it. The file is replaced atomically.
 *
 * @return true on success.
 */
bool wlmaker_dockapp_prefs_store(
    const char *fname_ptr,
    const char *key_ptr,
    const char *value_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dockapp_prefs_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCKAPP_PREFS_H__ */
/* == End of dockapp_prefs.h =============================================== */
