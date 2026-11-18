/* ========================================================================= */
/**
 * @file appicons.h
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
#ifndef __WLMAKER_APPICONS_H__
#define __WLMAKER_APPICONS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Window Maker's application icons (appicons), as drawn by `big-wm-appicons`
 * in a layer surface of namespace @ref WLMAKER_APPICONS_NAMESPACE: One tile
 * per running application that is not docked, in a row from the bottom-left
 * corner.
 *
 * The client resolves what the compositor can't (the icon from the icon
 * theme, the `.desktop` file) and publishes the row, in order, in
 * `$XDG_RUNTIME_DIR/big-wm-appicons.plist`:
 *
 *     { Appicons = ( { Name = "Text Editor"; AppID = "org.gnome.TextEditor";
 *                      AppIDs = ("org.gnome.TextEditor");
 *                      CommandLine = "gtk-launch org.gnome.TextEditor.desktop";
 *                      Icon = "big-wm-app-org.gnome.TextEditor.png"; } ); }
 *
 * The compositor handles the pointer on the row, as Window Maker does on
 * appicons: The menu (Unhide Here, Hide/Unhide, Launch, Set Icon..., Kill),
 * the double-click (unhide; Shift: here; Ctrl: launch; with the move
 * modifier: hide the others), the middle button (launch), and dragging the
 * icon to the Dock, the Clip or a drawer, where it becomes a launcher.
 */

/** Namespace of the layer surface of `big-wm-appicons`. */
#define WLMAKER_APPICONS_NAMESPACE "big-wm-appicons"

/**
 * Loads the row published by `big-wm-appicons`.
 *
 * @param fname_ptr           The file, or NULL for the default one in
 *                            `$XDG_RUNTIME_DIR`.
 *
 * @return The array of dicts (each one tile), or NULL. Unref.
 */
bspl_array_t *wlmaker_appicons_load(const char *fname_ptr);

/**
 * @return The tile `index` of `array_ptr`, if a dict. Not referenced.
 */
bspl_dict_t *wlmaker_appicons_at(bspl_array_t *array_ptr, int index);

/**
 * Opens the menu of the application icon `entry_ptr`, as Window Maker's
 * (createApplicationMenu): Unhide Here, Hide (or Unhide), Launch,
 * Set Icon..., Kill.
 */
void wlmaker_appicons_open_menu(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr);

/**
 * A double-click on the icon, as Window Maker's iconDblClick: Unhides the
 * application (on the workspace of its window); with Shift, here; with
 * Ctrl, launches it again; with the move modifier, also hides the others.
 *
 * @param server_ptr
 * @param entry_ptr
 * @param modifiers           WLR_MODIFIER_* held.
 */
void wlmaker_appicons_double_click(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr,
    uint32_t modifiers);

/** Launches the application again (Window Maker's "Launch"). */
void wlmaker_appicons_launch(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr);

/**
 * The launcher the icon becomes on the docks: `AppID`, `CommandLine` and
 * `Icon` of the entry. Without a `CommandLine` (no `.desktop` file), the
 * command line of the application's process, made launchable (see @ref
 * wlmaker_appicons_command_from_process).
 *
 * @return A new dict, or NULL if there is nothing to launch. Unref.
 */
bspl_dict_t *wlmaker_appicons_launcher(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr);

/**
 * Makes a command line, for `bs_subprocess_create_cmdline`, from a
 * process's `/proc/<pid>/cmdline`: The program is made absolute (relative
 * to the process's working directory, or its executable when not in the
 * `PATH`, eg. a script that renamed itself), and so are the arguments that
 * name a file relative to the working directory (eg. `python3 main.py`).
 *
 * @param cmdline_ptr         The arguments, each NUL-terminated.
 * @param len                 Length of `cmdline_ptr`.
 * @param cwd_ptr             Working directory of the process, or NULL.
 * @param exe_ptr             Its executable (`/proc/<pid>/exe`), or NULL.
 * @param path_ptr            The `PATH` to look programs up in, or NULL.
 *
 * @return The command line, or NULL. Free.
 */
char *wlmaker_appicons_command_from_process(
    const char *cmdline_ptr,
    size_t len,
    const char *cwd_ptr,
    const char *exe_ptr,
    const char *path_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_appicons_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_APPICONS_H__ */
/* == End of appicons.h ==================================================== */
