/* ========================================================================= */
/**
 * @file dnd.h
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
#ifndef __WLMAKER_DND_H__
#define __WLMAKER_DND_H__

#include <stdbool.h>
#include <stdint.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

/** Forward declaration: Drag and drop between clients. */
typedef struct _wlmaker_dnd_t wlmaker_dnd_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

struct wlr_scene_tree;

/**
 * Handles drag and drop of clients (wl_data_device): Starts the drags they
 * request (from a button press of theirs), and shows the dragged icon in
 * `parent_ptr`, at the pointer.
 *
 * As Window Maker's DropCommand: A drag released on a launcher of the Dock,
 * the Clip or an open drawer that has a `DropCommand` is taken by the
 * compositor (`wlmdock` takes no drops): It asks the source for its files
 * ("text/uri-list"; else its text), ends the drag as done (a copy), and
 * runs the `DropCommand` with "%d" replaced by the files' paths.
 *
 * @param server_ptr
 * @param parent_ptr          Scene tree for the dragged icons: Above all.
 *
 * @return The handler, or NULL on error.
 */
wlmaker_dnd_t *wlmaker_dnd_create(
    wlmaker_server_t *server_ptr,
    struct wlr_scene_tree *parent_ptr);

/** Destroys the handler. */
void wlmaker_dnd_destroy(wlmaker_dnd_t *dnd_ptr);

/**
 * Converts dropped data to the arguments of a `DropCommand`: The paths of
 * the "file://" URIs of a "text/uri-list" (decoded), other URIs as they
 * are; or the text, as one argument.
 *
 * @param text_ptr
 * @param uri_list            Whether `text_ptr` is a "text/uri-list".
 * @param args_ptr            Returns the number of arguments.
 *
 * @return A NULL-terminated array of strings, free with
 *     @ref wlmaker_dnd_free_args. NULL on error.
 */
char **wlmaker_dnd_args(const char *text_ptr, bool uri_list, size_t *args_ptr);

/** Frees what @ref wlmaker_dnd_args returned. */
void wlmaker_dnd_free_args(char **args_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dnd_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DND_H__ */
/* == End of dnd.h ========================================================= */
