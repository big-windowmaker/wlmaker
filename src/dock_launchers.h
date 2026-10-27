/* ========================================================================= */
/**
 * @file dock_launchers.h
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
#ifndef __WLMAKER_DOCK_LAUNCHERS_H__
#define __WLMAKER_DOCK_LAUNCHERS_H__

#include <stdbool.h>
#include <stddef.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

#include "dock_position.h"
#include "server.h"
#include "toolkit/toolkit.h"
#include "util/files.h"

struct wlr_buffer;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * The compositor's view of the launchers that `wlmdock` shows: Those of the
 * Dock (`wlmdock`, after its main tile) and those of the Clip
 * (`wlmdock --clip`), as stored in the state's `Launchers` of the `Dock`
 * and `Clip` sections. Used for Window Maker's menus, balloons, and for
 * dragging launchers, which the compositor handles for `wlmdock`.
 */

/**
 * Loads the `Launchers` of `section_ptr` from the state, as `wlmdock` shows
 * them: Leaves out those it can't show (not a dict, or no icon found).
 *
 * @param server_ptr
 * @param section_ptr         "Dock" or "Clip".
 *
 * @return An array of dicts, or NULL on error. Unref with bspl_array_unref.
 */
bspl_array_t *wlmaker_dock_launchers_load(
    wlmaker_server_t *server_ptr,
    const char *section_ptr);

/**
 * Stores `array_ptr` as the `Launchers` of `section_ptr`, and tells
 * `wlmdock` (and the Clip, which leaves room for its launchers).
 *
 * @return true on success.
 */
bool wlmaker_dock_launchers_store(
    wlmaker_server_t *server_ptr,
    const char *section_ptr,
    bspl_array_t *array_ptr);

/**
 * Loads the launchers of column `column` (see @ref WLMAKER_COLUMN_CLIP):
 * The Clip's, the Dock's, or a drawer's (the `Launchers` of its entry in
 * the Dock's), as shown (with their icon found).
 *
 * @return An array of dicts, or NULL on error.
 */
bspl_array_t *wlmaker_dock_launchers_load_column(
    wlmaker_server_t *server_ptr,
    int column);

/**
 * Moves launcher `from_index` of column `from_column` to `to_index` of
 * column `to_column`, in the state: Within a column, or across. Stores the
 * result at once, and tells `wlmdock` and the drawers.
 *
 * @return true on success.
 */
bool wlmaker_dock_launchers_move(
    wlmaker_server_t *server_ptr,
    int from_column,
    size_t from_index,
    int to_column,
    size_t to_index);

/**
 * Removes launcher `index` of column `column` from the state; or, with
 * `entry_ptr`, inserts that entry at `index`.
 *
 * @return true on success.
 */
bool wlmaker_dock_launchers_edit(
    wlmaker_server_t *server_ptr,
    int column,
    size_t index,
    bspl_dict_t *entry_ptr);

/**
 * Replaces the entry of drawer `id_ptr` in the Dock's launchers with
 * `entry_ptr`, and stores it. Eg. for its options.
 *
 * @return true on success.
 */
bool wlmaker_dock_launchers_replace_drawer(
    wlmaker_server_t *server_ptr,
    const char *id_ptr,
    bspl_dict_t *entry_ptr);

/**
 * Stores the boolean `key_ptr` (eg. "Collapsed") of drawer `id_ptr`'s entry
 * in the Dock's launchers, as Yes or No.
 *
 * @return true on success.
 */
bool wlmaker_dock_launchers_set_drawer_flag(
    wlmaker_server_t *server_ptr,
    const char *id_ptr,
    const char *key_ptr,
    bool value);

/** @return The string value of `key_ptr` in the launcher, or NULL. */
const char *wlmaker_dock_launcher_value(
    bspl_dict_t *dict_ptr,
    const char *key_ptr);

/** @return Whether the launcher's boolean `key_ptr` (eg. "Lock") is set. */
bool wlmaker_dock_launcher_flag(
    bspl_dict_t *dict_ptr,
    const char *key_ptr);

/**
 * Text of the launcher's balloon, as Window Maker's for docked icons: The
 * application's name (its `AppID`, else the program of its `CommandLine`),
 * and the command in parentheses on the next line.
 *
 * @return The text, to be free()-ed. NULL on error.
 */
char *wlmaker_dock_launcher_balloon_text(bspl_dict_t *dict_ptr);

/**
 * Resolves the launcher's `Icon`, as `wlmdock` does: A path starting with
 * '/' or '~', or a name in `${XDG_DATA_DIRS}/wlmaker/icons`.
 *
 * @return The path, to be free()-ed. NULL if not found.
 */
char *wlmaker_dock_launcher_icon_path(
    wlm_util_files_t *files_ptr,
    const char *icon_ptr);

/**
 * Draws a tile as the docks show it: The tile's background, and an image
 * (PNG at `image_path_ptr`, or a buffer) scaled to the content size and
 * centered.
 *
 * @param style_ptr
 * @param image_path_ptr      PNG file, or NULL.
 * @param image_ptr           Image buffer, or NULL.
 *
 * @return The tile, or NULL on error. Drop with wlr_buffer_drop.
 */
struct wlr_buffer *wlmaker_dock_tile_buffer(
    const struct wlmtk_tile_style *style_ptr,
    const char *image_path_ptr,
    struct wlr_buffer *image_ptr);

/**
 * Index of the tile at `x`, `y` within a dock of `width` x `height` at
 * `place`, counted from the dock's anchor.
 *
 * @param place
 * @param width
 * @param height
 * @param x
 * @param y
 * @param pitch               Tile size plus the dock's margin.
 *
 * @return The index, or -1 if outside.
 */
int wlmaker_dock_tile_index_at(
    wlmaker_dock_place_t place,
    int width,
    int height,
    double x,
    double y,
    int pitch);

/**
 * Number of windows of the application `app_id_ptr`, and of those
 * minimized (hidden). Window Maker's application, for a docked icon.
 */
size_t wlmaker_dock_app_windows(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr,
    size_t *hidden_ptr);

/** Hides the application: Minimizes its windows. Window Maker's "Hide". */
void wlmaker_dock_app_hide(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr);

/** Shows the application's windows again. Window Maker's "Unhide". */
void wlmaker_dock_app_unhide(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr);

/**
 * Moves the application's windows to the current workspace, restores and
 * raises them, and activates one. Window Maker's "Bring Here".
 */
void wlmaker_dock_app_bring_here(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr);

/**
 * Shows the application, as a double-click on a running application's docked
 * icon does on Window Maker: Restores its windows, switches to the workspace
 * of one of them (unless on the current one), raises and activates it.
 */
void wlmaker_dock_app_activate(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr);

/**
 * Terminates the application: Sends SIGTERM to the processes of its
 * windows. Window Maker's "Kill".
 */
void wlmaker_dock_app_kill(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr);

/**
 * Builds the shell script for a launcher's `PasteCommand` (placeholder 's',
 * Window Maker's "%s": the selection) or `DropCommand` ('d', "%d": the
 * dropped files): The placeholder becomes `"$@"`, the arguments given to
 * the script, so they are not parsed by the shell. "%%" becomes "%".
 *
 * @return The script, to be free()-ed. NULL on error.
 */
char *wlmaker_dock_launcher_command_script(
    const char *template_ptr,
    char placeholder);

/**
 * Runs a launcher's `PasteCommand` or `DropCommand` with `args_ptr`, as
 * Window Maker does: Through `/bin/sh -c`. See
 * @ref wlmaker_dock_launcher_command_script.
 *
 * @return true if started.
 */
bool wlmaker_dock_launcher_run_command(
    wlmaker_server_t *server_ptr,
    const char *template_ptr,
    char placeholder,
    const char **args_ptr,
    size_t args);

/** Called with the text read; NULL on error or time-out. */
typedef void (*wlmaker_dock_launchers_text_callback_t)(
    void *ud_ptr, const char *text_ptr);

/**
 * Reads all of `fd` (non-blocking, up to 1 MiB, within 5 seconds) from the
 * event loop, then calls `callback` and closes `fd`. For data from a
 * client: The selection, or dropped data.
 *
 * @return true if reading started (`callback` will be called).
 */
bool wlmaker_dock_launchers_read_fd(
    wlmaker_server_t *server_ptr,
    int fd,
    wlmaker_dock_launchers_text_callback_t callback,
    void *ud_ptr);

/**
 * The middle button on a launcher: As Window Maker, runs its
 * `PasteCommand` with the selection (the primary selection, else the
 * clipboard) for "%s".
 *
 * @return Whether the launcher has a `PasteCommand`.
 */
bool wlmaker_dock_launchers_paste_command(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *dict_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dock_launchers_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCK_LAUNCHERS_H__ */
/* == End of dock_launchers.h ============================================== */
