/* ========================================================================= */
/**
 * @file wlmdock.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2026 Google LLC
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

// For `struct ucred`, to find the compositor's process.
#define _GNU_SOURCE

#include <basedir.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#undef WLR_USE_UNSTABLE

#include "config.h"
#include "dock_position.h"

#include "toolkit/toolkit.h"
#include "util/backtrace.h"
#include "util/files.h"
#include "util/subprocess_monitor.h"
#include "util/version.h"
#include "util/wlr_log.h"
#include "wlclient/layer_shell.h"
#include "wlclient/wlclient.h"

#include "dock/launcher.h"
#include "dock/subcompositor.h"
#include "dock/tilebox.h"
#include "dock/toplevel.h"

// TODO(kaeser@gubbe.ch): Move into a shared directory.
#include "../config.h"

/* == Declarations ========================================================= */

/** State of the nested client-backed dock. */
typedef struct {
    /** For tracking toplevels. */
    struct wlmdock_toplevel_tracker *tracker_ptr;

    /** Client wrapper connecting to parent compositor. */
    wlmcl_client_t            *client_ptr;

    /** Layer shell interface. */
    struct zwlr_layer_shell_v1 *layer_shell_ptr;
    /** Parent's display connection. */
    struct wl_display         *remote_display_ptr;

    /** Local Wayland server display. */
    struct wl_display         *local_display_ptr;
    /** The subcompositor. */
    wlmdock_subcompositor_t   *subcompositor_ptr;

    /** Local wlroots backend. */
    struct wlr_backend        *wlr_backend_ptr;
    /** Local renderer. */
    struct wlr_renderer       *wlr_renderer_ptr;
    /** Local allocator. */
    struct wlr_allocator      *wlr_allocator_ptr;
    /** Box holding the tiles. */
    wlmdock_tilebox_t         *tilebox_ptr;

    /** Monitoring for subprocesses. */
    wlm_util_subprocess_monitor_t *subprocess_monitor_ptr;

    /** Event source for monitoring signals via client's signal_fd. */
    struct wl_event_source    *client_signal_event_source_ptr;
    /** Listener for `destroy` of @ref wlmdock_t::wlr_backend_ptr. */
    struct wl_listener        backend_destroy_listener;

    /** Files handle, for (re-)loading and storing the state. */
    wlm_util_files_t          *files_ptr;
    /** The `--state_file` argument, or NULL. */
    const char                *state_fname_ptr;
    /** Whether showing the Clip's launchers (`--clip`). */
    bool                      clip;
    /** Section of the state: "Dock" or "Clip". */
    const char                *section_ptr;
    /** The style. */
    wlmaker_config_style_t    *style_ptr;
    /** Current position. */
    wlmtk_dock_positioning_t  positioning;

    /** What is shown on the surface: The tile box, and the menu if open. */
    wlmtk_container_t         root_container;
    /** The launcher's menu, when open. Otherwise NULL. */
    wlmtk_menu_t              *menu_ptr;
    /** The launcher the menu is for. */
    wlmdock_launcher_t        *menu_launcher_ptr;
    /** Destroys the closed menu, from the event loop. */
    struct wl_event_source    *menu_close_event_source_ptr;
    /** Listener for @ref wlmtk_menu_events_t::request_close. */
    struct wl_listener        menu_request_close_listener;
    /** Corners to flip to, once the menu is gone. 0 for none. */
    uint32_t                  pending_flip;
    /** The menu element's original virtual method table. */
    wlmtk_element_vmt_t       menu_orig_vmt;
    /** Listener for @ref wlmcl_client_events::pointer_leave. */
    struct wl_listener        pointer_leave_listener;
} wlmdock_t;

/** Actions of the launcher's menu. */
typedef enum {
    WLMDOCK_MENU_LAUNCH,
    WLMDOCK_MENU_KILL,
    WLMDOCK_MENU_REMOVE,
    WLMDOCK_MENU_FLIP_SIDE,
    WLMDOCK_MENU_FLIP_END,
} wlmdock_menu_action_t;

/** An item of the launcher's menu. */
typedef struct {
    /** The menu item. */
    wlmtk_menu_item_t         *menu_item_ptr;
    /** Back-link to the dock. */
    wlmdock_t                 *dock_ptr;
    /** What it does. */
    wlmdock_menu_action_t     action;
    /** Listener for @ref wlmtk_menu_item_events_t::triggered. */
    struct wl_listener        triggered_listener;
    /** Listener for @ref wlmtk_menu_item_events_t::destroy. */
    struct wl_listener        destroy_listener;
} wlmdock_menu_item_t;

/** TODO: Replace this. */
struct wlmdock_state {
    /** Positioning data. */
    wlmtk_dock_positioning_t  positioning;
    /** Launchers. */
    bspl_array_t            *launchers_array_ptr;
    /** Distance from the anchor, in pixels. */
    int64_t                   margin;
    /** Whether `Margin` was given. */
    bool                      has_margin;
};

static wlmdock_t *_wlmdock_create(
    const wlmtk_dock_positioning_t positioning,
    wlmaker_config_style_t *style_ptr);
static void _wlmdock_destroy(wlmdock_t *dock_ptr);
static int handle_client_signal(int fd, uint32_t mask, void *data_ptr);
static void _wlmdock_handle_backend_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static bool _wlmdock_decode_launchers(
    bspl_object_t *object_ptr,
    const union bspl_desc_value *desc_value_ptr,
    void *value_ptr);
static bool _wlmdock_load_positioning(
    wlmdock_t *dock_ptr,
    bspl_dict_t *state_dict_ptr,
    wlmtk_dock_positioning_t *positioning_ptr);
static void _wlmdock_set_positioning(
    wlmdock_t *dock_ptr,
    const wlmtk_dock_positioning_t *positioning_ptr);
static void _wlmdock_open_menu(
    wlmdock_launcher_t *launcher_ptr,
    void *ud_ptr);
static void _wlmdock_destroy_menu(wlmdock_t *dock_ptr);
static void _wlmdock_close_menu(wlmdock_t *dock_ptr);
static void _wlmdock_idle_destroy_menu(void *data_ptr);
static void _wlmdock_flip(wlmdock_t *dock_ptr, uint32_t flip);
static void _wlmdock_layout(wlmdock_t *dock_ptr);
static void _wlmdock_handle_menu_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmdock_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static bool _wlmdock_menu_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);

/* == Data ================================================================= */

/** Will hold the value of --config_file. */
static char *wlmdock_arg_config_file_ptr = NULL;
/** Will hold the value of --state_file. */
static char *wlmdock_arg_state_file_ptr = NULL;
/** Will hold the value of --theme_file. */
static char *wlmdock_arg_theme_file_ptr = NULL;
/** Will hold the value of --clip. */
static bool wlmdock_arg_clip = false;

/** Definition of commandline arguments. */
static const bs_arg_t wlmdock_args[] = {
    BS_ARG_STRING(
        "config_file",
        "Optional: Path to a configuration file. If not provided, wlmaker "
        "will scan default paths for a configuration file, or fall back to "
        "a built-in configuration.",
        NULL,
        &wlmdock_arg_config_file_ptr),
    BS_ARG_STRING(
        "state_file",
        "Optional: Path to a state file, with state of workspaces, dock and "
        "clips configured. If not provided, wlmaker will scan default paths "
        "for a state file, or fall back to a built-in default.",
        NULL,
        &wlmdock_arg_state_file_ptr),
    BS_ARG_STRING(
        "theme_file",
        "Optional: Path to a \"theme\" file, configuring the visual style for "
        "elements. If not provided, wlmaker will use a built-in default theme.",
        NULL,
        &wlmdock_arg_theme_file_ptr),
    BS_ARG_BOOL(
        "clip",
        "Optional: Shows the `Launchers` of the state file's `Clip` section "
        "(instead of the `Dock` section), placed next to the Clip. Disabled "
        "by default.",
        false,
        &wlmdock_arg_clip),
    bs_arg_log_level,
    bs_arg_log_to_stderr,
    BS_ARG_SENTINEL()
};

/** Enum descriptor for `enum wlr_edges`. */
static const bspl_enum_desc_t _wlmdock_edges[] = {
    BSPL_ENUM("TOP", WLR_EDGE_TOP),
    BSPL_ENUM("BOTTOM", WLR_EDGE_BOTTOM),
    BSPL_ENUM("LEFT", WLR_EDGE_LEFT),
    BSPL_ENUM("RIGHT", WLR_EDGE_RIGHT),
    BSPL_ENUM_SENTINEL(),
};

/** Descriptor for the dock's plist. */
const bspl_desc_t _wlmdock_plist_desc[] = {
    BSPL_DESC_ENUM("Edge", true, struct wlmdock_state,
                   positioning.edge, positioning.edge,
                   WLR_EDGE_NONE, _wlmdock_edges),
    BSPL_DESC_ENUM("Anchor", true, struct wlmdock_state,
                   positioning.anchor, positioning.anchor,
                   WLR_EDGE_NONE, _wlmdock_edges),
    BSPL_DESC_CUSTOM("Launchers", true, struct wlmdock_state,
                     launchers_array_ptr, launchers_array_ptr,
                     _wlmdock_decode_launchers,
                     NULL,
                     NULL,
                     NULL),
    BSPL_DESC_INT64("Margin", false, struct wlmdock_state,
                    margin, has_margin, 0),
    BSPL_DESC_SENTINEL(),
};

/* == Main program ========================================================= */
/** The dock's main program. */
int main(int argc, const char **argv)
{
    if (!wlm_util_backtrace_setup(argv[0]) ||
        // Ideally: xdgStateHome. But: libxdg-basedir is using FreeDesktop
        // specification v0.7, and XDG_STATE_HOME was added with v0.8 only.
        !bs_log_init(xdgCacheHome(NULL), "wlmdock")) {
        return EXIT_FAILURE;
    }

    if (!bs_arg_parse(wlmdock_args, BS_ARG_MODE_EXTRA_ARGS, &argc, argv)) {
        fprintf(stderr, "Failed to parse commandline arguments.\n");
        bs_arg_print_usage(stderr, wlmdock_args);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--help")) {
            bs_arg_print_usage(stdout, wlmdock_args);
            return EXIT_SUCCESS;
        } else if (0 == strcmp(argv[i], "--version")) {
            fprintf(stdout, "wlmdock version %s (%s)\n",
                    wlm_util_version, wlm_util_version_full);
            return EXIT_SUCCESS;
        } else {
            bs_log(BS_ERROR, "Unhandled extra argument \"%s\"", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (!wlm_util_wlr_log_init(WLR_DEBUG)) return EXIT_FAILURE;

    bs_log(BS_INFO, "Starting wlmdock %s (%s)",
           wlm_util_version, wlm_util_version_full);

    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    if (NULL == files_ptr) {
        bs_log(BS_ERROR, "Failed wlm_util_files_create(\"wlmaker\")");
        return EXIT_FAILURE;
    }

    bspl_dict_t *config_dict_ptr = wlmaker_config_load(
        files_ptr, wlmdock_arg_config_file_ptr);
    if (NULL != wlmdock_arg_config_file_ptr) free(wlmdock_arg_config_file_ptr);
    if (NULL == config_dict_ptr) {
        fprintf(stderr, "Failed to load & initialize configuration.\n");
        return EXIT_FAILURE;
    }

    const char *theme_file_ptr = wlmdock_arg_theme_file_ptr;
    if (NULL == theme_file_ptr) {
        theme_file_ptr = bspl_dict_get_string_value(
            config_dict_ptr, "ThemeFile");
    }
    wlmaker_config_style_t style = {};
    bool theme_loaded = wlmaker_theme_load(files_ptr, theme_file_ptr, &style);
    bspl_dict_unref(config_dict_ptr);
    if (!theme_loaded) {
        fprintf(stderr, "Failed to load & initialize theme.\n");
        return EXIT_FAILURE;
    }

    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        files_ptr, wlmdock_arg_state_file_ptr);
    if (NULL == state_dict_ptr) {
        fprintf(stderr, "Failed to load & initialize state.\n");
        return EXIT_FAILURE;
    }
    const char *section_ptr = wlmdock_arg_clip ? "Clip" : "Dock";
    bspl_dict_t *dock_dict_ptr = bspl_dict_get_dict(state_dict_ptr, section_ptr);
    struct wlmdock_state state = {};
    if (!bspl_decode_dict(dock_dict_ptr, _wlmdock_plist_desc, &state)) {
        bs_log(BS_ERROR, "Failed to parse the State's '%s' section.",
               section_ptr);
        return EXIT_FAILURE;
    }
    state.positioning.margin = state.margin;
    if (wlmdock_arg_clip) {
        // Placed over the room the Clip leaves for `Launchers`: Relative to
        // the full output, and by default one Clip tile from the anchor.
        state.positioning.overlay = true;
        if (!state.has_margin) {
            state.positioning.margin =
                style.tile.size + style.dock.margin.width;
        }
    }

    wlmdock_t *dock_ptr = _wlmdock_create(state.positioning, &style);
    if (NULL == dock_ptr) {
        bs_log(BS_ERROR, "Failed to create wlmdock.");
        return EXIT_FAILURE;
    }
    dock_ptr->files_ptr = files_ptr;
    dock_ptr->state_fname_ptr = wlmdock_arg_state_file_ptr;
    dock_ptr->clip = wlmdock_arg_clip;
    dock_ptr->section_ptr = section_ptr;

    for (size_t i = 0; i < bspl_array_size(state.launchers_array_ptr); ++i) {
        bspl_dict_t *dict_ptr = bspl_dict_from_object(
            bspl_array_at(state.launchers_array_ptr, i));
        if (NULL == dict_ptr) {
            bs_log(BS_ERROR, "Elements of 'Launchers' must be dicts.");
            return EXIT_FAILURE;
        }

        wlmdock_launcher_t *launcher_ptr = wlmdock_launcher_create_from_plist(
            &style.tile,
            dict_ptr,
            dock_ptr->tracker_ptr,
            dock_ptr->subprocess_monitor_ptr,
            files_ptr);
        if (NULL == launcher_ptr) return EXIT_FAILURE;
        wlmdock_launcher_set_menu_handler(
            launcher_ptr, _wlmdock_open_menu, dock_ptr);
        wlmdock_tilebox_add_tile(
            dock_ptr->tilebox_ptr,
            wlmdock_launcher_tile(launcher_ptr));
    }
    if (state.launchers_array_ptr) bspl_array_unref(state.launchers_array_ptr);

    if (0 > wl_display_roundtrip(dock_ptr->remote_display_ptr)) {
        bs_log(BS_ERROR, "Failed parent wl_display_roundtrip.");
        _wlmdock_destroy(dock_ptr);
        return EXIT_FAILURE;
    }

    bs_log(BS_INFO, "wlmdock: entering event loop.");
    wl_display_run(dock_ptr->local_display_ptr);
    bs_log(BS_INFO, "wlmdock: event loop exited.");

    _wlmdock_destroy(dock_ptr);
    bspl_decoded_destroy(wlmaker_config_style_desc, &style);
    if (NULL != wlmdock_arg_state_file_ptr) free(wlmdock_arg_state_file_ptr);

    if (NULL != files_ptr) wlm_util_files_destroy(files_ptr);

    bspl_dict_unref(state_dict_ptr);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/** Creates and initializes wlmdock_t. */
wlmdock_t *_wlmdock_create(
    const wlmtk_dock_positioning_t positioning,
    wlmaker_config_style_t *style_ptr)
{
    wlmdock_t *dock_ptr = logged_calloc(1, sizeof(wlmdock_t));
    if (NULL == dock_ptr) return NULL;
    dock_ptr->style_ptr = style_ptr;
    dock_ptr->positioning = positioning;
    if (!wlmtk_container_init(&dock_ptr->root_container)) {
        free(dock_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(&dock_ptr->root_container.super_element, true);

    // 1. Initialize client wrapper and connect to parent compositor.
    dock_ptr->client_ptr = wlmcl_client_create("wlmaker.wlmdock");
    if (NULL == dock_ptr->client_ptr) {
        bs_log(BS_ERROR, "Failed to connect to parent compositor.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    const struct wlmcl_client_attributes *attrs =
        wlmcl_client_attributes(dock_ptr->client_ptr);
    dock_ptr->remote_display_ptr = attrs->wl_display_ptr;
    // SIGUSR1: The compositor moved the Dock (or Clip). Re-read the state.
    if (!wlmcl_client_add_signal(dock_ptr->client_ptr, SIGUSR1)) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmcl_client_events(dock_ptr->client_ptr)->pointer_leave,
        &dock_ptr->pointer_leave_listener,
        _wlmdock_handle_pointer_leave);

    // Register extra Wayland interfaces.
    if (!wlmcl_layer_shell_register(
            dock_ptr->client_ptr,
            &dock_ptr->layer_shell_ptr)) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    dock_ptr->tracker_ptr = wlmdock_toplevel_tracker_create();
    if (NULL == dock_ptr->tracker_ptr) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    if (!wlmdock_toplevel_register_ext_foreign_toplevel_list(
            dock_ptr->client_ptr,
            dock_ptr->tracker_ptr)) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    if (!wlmcl_client_initialize(dock_ptr->client_ptr)) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    // 3. Setup local Wayland server display and event loops.
    dock_ptr->local_display_ptr = wl_display_create();
    if (NULL == dock_ptr->local_display_ptr) {
        bs_log(BS_ERROR, "Failed to create local server wl_display.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    // 4. Initialize wlroots backend, renderer, allocator and nested output.
    dock_ptr->wlr_backend_ptr = wlr_wl_backend_create(
        wl_display_get_event_loop(dock_ptr->local_display_ptr),
        dock_ptr->remote_display_ptr);
    if (NULL == dock_ptr->wlr_backend_ptr) {
        bs_log(BS_ERROR, "Failed to create nested Wayland backend.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &dock_ptr->wlr_backend_ptr->events.destroy,
        &dock_ptr->backend_destroy_listener,
        _wlmdock_handle_backend_destroy);

    // 5. Setup dock contents.
    wlmtk_box_orientation_t orientation = WLMTK_BOX_VERTICAL;
    if (positioning.edge == WLR_EDGE_TOP ||
        positioning.edge == WLR_EDGE_BOTTOM) {
        orientation = WLMTK_BOX_HORIZONTAL;
    }
    dock_ptr->tilebox_ptr = wlmdock_tilebox_create(
        orientation,
        (positioning.anchor == WLR_EDGE_BOTTOM ||
         positioning.anchor == WLR_EDGE_RIGHT),
        &style_ptr->dock);
    if (NULL == dock_ptr->tilebox_ptr) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_container_add_element(
        &dock_ptr->root_container,
        wlmdock_tilebox_element(dock_ptr->tilebox_ptr));

    // 6. Prepare renderer and allocator. The dock is small and drawn in
    // software anyway: Default to the pixman renderer, which makes the
    // allocator use shared memory. An automatically chosen GPU renderer
    // and allocator may pick the DRM primary node, where a client cannot
    // allocate buffers (eg. "DRM_IOCTL_MODE_CREATE_DUMB failed: Permission
    // denied"), leaving the dock invisible. WLR_RENDERER still overrides.
    if (NULL != getenv("WLR_RENDERER")) {
        dock_ptr->wlr_renderer_ptr = wlr_renderer_autocreate(
            dock_ptr->wlr_backend_ptr);
    } else {
        dock_ptr->wlr_renderer_ptr = wlr_pixman_renderer_create();
    }
    if (NULL == dock_ptr->wlr_renderer_ptr) {
        bs_log(BS_ERROR, "Failed to create renderer.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    wlr_renderer_init_wl_display(
        dock_ptr->wlr_renderer_ptr,
        dock_ptr->local_display_ptr);

    dock_ptr->wlr_allocator_ptr = wlr_allocator_autocreate(
        dock_ptr->wlr_backend_ptr, dock_ptr->wlr_renderer_ptr);
    if (NULL == dock_ptr->wlr_allocator_ptr) {
        bs_log(BS_ERROR, "Failed to create allocator.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    // 8. Start it all.
    if (!wlr_backend_start(dock_ptr->wlr_backend_ptr)) {
        bs_log(BS_ERROR, "Failed to start wlroots backend.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    dock_ptr->subcompositor_ptr = wlmdock_subcompositor_create(
        dock_ptr->local_display_ptr,
        dock_ptr->wlr_backend_ptr,
        dock_ptr->client_ptr,
        dock_ptr->layer_shell_ptr,
        // TODO(kaeser@gubbe.ch): Find a way to not provide a cursor style,
        // when using the cursor shape extension.
        &style_ptr->cursor,
        &dock_ptr->root_container,
        &positioning,
        positioning.overlay ? "wlmdock-clip" : "wlmdock");
    if (NULL == dock_ptr->subcompositor_ptr) {
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }
    wlmdock_subcompositor_prepare(
        dock_ptr->subcompositor_ptr,
        dock_ptr->wlr_backend_ptr,
        dock_ptr->wlr_allocator_ptr,
        dock_ptr->wlr_renderer_ptr);

    // 9. Monitor the client's signal_fd for signals (like SIGINT).
    struct wl_event_loop *event_loop_ptr = wl_display_get_event_loop(
        dock_ptr->local_display_ptr);
    dock_ptr->client_signal_event_source_ptr = wl_event_loop_add_fd(
        event_loop_ptr,
        wlmcl_client_signal_fd(dock_ptr->client_ptr),
        WL_EVENT_READABLE,
        handle_client_signal,
        dock_ptr);
    if (NULL == dock_ptr->client_signal_event_source_ptr) {
        bs_log(BS_ERROR, "Failed to register client signal fd to event loop.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    dock_ptr->subprocess_monitor_ptr = wlm_util_subprocess_monitor_create(
        event_loop_ptr);
    if (NULL == dock_ptr->subprocess_monitor_ptr) {
        bs_log(BS_ERROR, "Failed wlm_util_subprocess_monitor_create(%p)",
               event_loop_ptr);
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    struct wlr_compositor *wlr_compositor_ptr = wlr_compositor_create(
        dock_ptr->local_display_ptr, 5, dock_ptr->wlr_renderer_ptr);
    if (NULL == wlr_compositor_ptr) {
        bs_log(BS_ERROR, "Failed to create compositor.");
        _wlmdock_destroy(dock_ptr);
        return NULL;
    }

    return dock_ptr;
}

/* ------------------------------------------------------------------------- */
/** Destroys wlmdock_t and frees all resources. */
void _wlmdock_destroy(wlmdock_t *dock_ptr)
{
    if (NULL == dock_ptr) return;
    wlmtk_util_disconnect_listener(&dock_ptr->backend_destroy_listener);
    wlmtk_util_disconnect_listener(&dock_ptr->pointer_leave_listener);
    _wlmdock_destroy_menu(dock_ptr);

    if (NULL != dock_ptr->subprocess_monitor_ptr) {
        wlm_util_subprocess_monitor_destroy(dock_ptr->subprocess_monitor_ptr);
        dock_ptr->subprocess_monitor_ptr = NULL;
    }

    if (NULL != dock_ptr->client_signal_event_source_ptr) {
        wl_event_source_remove(dock_ptr->client_signal_event_source_ptr);
        dock_ptr->client_signal_event_source_ptr = NULL;
    }

    if (NULL != dock_ptr->subcompositor_ptr) {
        wlmdock_subcompositor_destroy(dock_ptr->subcompositor_ptr);
        dock_ptr->subcompositor_ptr = NULL;
    }
    if (NULL != dock_ptr->wlr_renderer_ptr) {
        wlr_renderer_destroy(dock_ptr->wlr_renderer_ptr);
        dock_ptr->wlr_renderer_ptr = NULL;
    }
    if (NULL != dock_ptr->wlr_allocator_ptr) {
        wlr_allocator_destroy(dock_ptr->wlr_allocator_ptr);
        dock_ptr->wlr_allocator_ptr = NULL;
    }
    if (NULL != dock_ptr->tilebox_ptr) {
        wlmtk_element_t *e = wlmdock_tilebox_element(dock_ptr->tilebox_ptr);
        if (e->parent_container_ptr == &dock_ptr->root_container) {
            wlmtk_container_remove_element(&dock_ptr->root_container, e);
        }
        wlmdock_tilebox_destroy(dock_ptr->tilebox_ptr);
        dock_ptr->tilebox_ptr = NULL;
    }
    wlmtk_container_fini(&dock_ptr->root_container);

    if (NULL != dock_ptr->local_display_ptr) {
        wl_display_destroy(dock_ptr->local_display_ptr);
        dock_ptr->local_display_ptr = NULL;
    }

    if (NULL != dock_ptr->tracker_ptr) {
        wlmdock_toplevel_tracker_destroy(dock_ptr->tracker_ptr);
        dock_ptr->tracker_ptr = NULL;
    }

    if (NULL != dock_ptr->client_ptr) {
        wlmcl_client_destroy(dock_ptr->client_ptr);
        dock_ptr->client_ptr = NULL;
    }

    free(dock_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Decoder for the "Launchers" array. Currently just stores a reference. */
bool _wlmdock_decode_launchers(
    bspl_object_t *object_ptr,
    __UNUSED__ const union bspl_desc_value *desc_value_ptr,
    void *value_ptr)
{
    bspl_array_t **array_ptr_ptr = value_ptr;

    *array_ptr_ptr = bspl_array_from_object(object_ptr);
    if (NULL == *array_ptr_ptr) return false;

    bspl_object_ref(bspl_object_from_array(*array_ptr_ptr));
    return true;
}

/* ------------------------------------------------------------------------- */
/** Handles client's signal fd events. */
int handle_client_signal(int fd, uint32_t mask, void *data_ptr)
{
    wlmdock_t *dock_ptr = data_ptr;
    struct signalfd_siginfo siginfo;

    if (mask & WL_EVENT_READABLE) {
        ssize_t rd = read(fd, &siginfo, sizeof(siginfo));
        if (rd == sizeof(siginfo) && SIGUSR1 == siginfo.ssi_signo) {
            // The compositor moved us: Re-read the position.
            bspl_dict_t *state_dict_ptr = wlmaker_state_load(
                dock_ptr->files_ptr, dock_ptr->state_fname_ptr);
            wlmtk_dock_positioning_t positioning;
            if (_wlmdock_load_positioning(
                    dock_ptr, state_dict_ptr, &positioning)) {
                _wlmdock_set_positioning(dock_ptr, &positioning);
            }
            bspl_dict_unref(state_dict_ptr);
        } else if (rd == sizeof(siginfo)) {
            bs_log(BS_INFO, "wlmdock: Signal %d caught, exiting loop.",
                   siginfo.ssi_signo);
            wl_display_terminate(dock_ptr->local_display_ptr);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Handles destruction of the nested Wayland backend: It happens when the
 * connection to the parent compositor is lost (eg. at the session's end).
 * Exits the event loop, for a clean shutdown.
 */
void _wlmdock_handle_backend_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmdock_t *dock_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmdock_t, backend_destroy_listener);
    bs_log(BS_INFO, "wlmdock: Backend destroyed, exiting loop.");
    wlmtk_util_disconnect_listener(&dock_ptr->backend_destroy_listener);
    dock_ptr->wlr_backend_ptr = NULL;
    wl_display_terminate(dock_ptr->local_display_ptr);
}

/* ------------------------------------------------------------------------- */
/** Reads the position from the dock's section of the state. */
bool _wlmdock_load_positioning(
    wlmdock_t *dock_ptr,
    bspl_dict_t *state_dict_ptr,
    wlmtk_dock_positioning_t *positioning_ptr)
{
    bspl_dict_t *dict_ptr = bspl_dict_get_dict(
        state_dict_ptr, dock_ptr->section_ptr);
    struct wlmdock_state state = {};
    if (!bspl_decode_dict(dict_ptr, _wlmdock_plist_desc, &state)) {
        bs_log(BS_WARNING, "Failed to parse the State's '%s' section.",
               dock_ptr->section_ptr);
        return false;
    }
    if (NULL != state.launchers_array_ptr) {
        bspl_array_unref(state.launchers_array_ptr);
    }
    *positioning_ptr = state.positioning;
    positioning_ptr->margin = state.margin;
    positioning_ptr->overlay = dock_ptr->positioning.overlay;
    if (dock_ptr->clip && !state.has_margin) {
        positioning_ptr->margin =
            dock_ptr->style_ptr->tile.size +
            dock_ptr->style_ptr->dock.margin.width;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** Moves the dock: Eg. to another corner. Keeps the orientation. */
void _wlmdock_set_positioning(
    wlmdock_t *dock_ptr,
    const wlmtk_dock_positioning_t *positioning_ptr)
{
    wlmaker_dock_place_t from = {
        dock_ptr->positioning.edge, dock_ptr->positioning.anchor };
    wlmaker_dock_place_t to = {
        positioning_ptr->edge, positioning_ptr->anchor };
    if (wlmaker_dock_place_vertical(from) != wlmaker_dock_place_vertical(to)) {
        bs_log(BS_WARNING, "wlmdock: Cannot change orientation, ignoring "
               "edge 0x%"PRIx32, to.edge);
        return;
    }
    _wlmdock_destroy_menu(dock_ptr);

    dock_ptr->positioning = *positioning_ptr;
    wlmdock_tilebox_set_reverse(
        dock_ptr->tilebox_ptr,
        positioning_ptr->anchor == WLR_EDGE_BOTTOM ||
        positioning_ptr->anchor == WLR_EDGE_RIGHT);
    if (NULL != dock_ptr->subcompositor_ptr) {
        wlmdock_subcompositor_set_positioning(
            dock_ptr->subcompositor_ptr, positioning_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Stores the launchers, in their order from the anchor, in the state. */
static void _wlmdock_store_launchers(wlmdock_t *dock_ptr)
{
    size_t n = wlmdock_tilebox_tiles(dock_ptr->tilebox_ptr, NULL, 0);
    wlmtk_tile_t **tiles_ptr = NULL;
    if (0 < n) {
        tiles_ptr = logged_calloc(n, sizeof(wlmtk_tile_t*));
        if (NULL == tiles_ptr) return;
        wlmdock_tilebox_tiles(dock_ptr->tilebox_ptr, tiles_ptr, n);
    }
    bspl_array_t *array_ptr = bspl_array_create();
    bspl_dict_t *values_ptr = bspl_dict_create();
    bool rv = NULL != array_ptr && NULL != values_ptr;
    for (size_t i = 0; rv && i < n; ++i) {
        wlmdock_launcher_t *l_ptr = wlmdock_launcher_from_tile(tiles_ptr[i]);
        if (NULL == l_ptr) continue;
        rv = bspl_array_push_back(
            array_ptr, bspl_object_from_dict(wlmdock_launcher_dict(l_ptr)));
    }
    rv = rv && bspl_dict_add(values_ptr, "Launchers",
                             bspl_object_from_array(array_ptr));
    rv = rv && wlmaker_state_store_section(
        dock_ptr->files_ptr, dock_ptr->state_fname_ptr,
        dock_ptr->section_ptr, values_ptr);
    if (!rv) bs_log(BS_WARNING, "wlmdock: Failed to store the launchers.");
    if (NULL != array_ptr) bspl_array_unref(array_ptr);
    if (NULL != values_ptr) bspl_dict_unref(values_ptr);
    if (NULL != tiles_ptr) free(tiles_ptr);

    // The Clip leaves room for its launchers: Tell the compositor (signals
    // SIGUSR1) to update it.
    struct ucred ucred;
    socklen_t len = sizeof(ucred);
    if (rv && dock_ptr->clip &&
        0 == getsockopt(wl_display_get_fd(dock_ptr->remote_display_ptr),
                        SOL_SOCKET, SO_PEERCRED, &ucred, &len) &&
        1 < ucred.pid && getpid() != ucred.pid) {
        if (0 != kill(ucred.pid, SIGUSR1)) {
            bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%d, SIGUSR1)",
                   (int)ucred.pid);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Corner reached by flipping `flip`, or 0 if the Clip is there. */
static uint32_t _wlmdock_flipped_corner(wlmdock_t *dock_ptr, uint32_t flip)
{
    wlmaker_dock_place_t place = {
        dock_ptr->positioning.edge, dock_ptr->positioning.anchor };
    uint32_t corner = wlmaker_dock_place_corner(place) ^ flip;

    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        dock_ptr->files_ptr, dock_ptr->state_fname_ptr);
    wlmaker_dock_place_t clip;
    if (wlmaker_dock_place_decode(
            bspl_dict_get_dict(state_dict_ptr, "Clip"), &clip) &&
        wlmaker_dock_place_corner(clip) == corner) corner = 0;
    bspl_dict_unref(state_dict_ptr);
    return corner;
}

/* ------------------------------------------------------------------------- */
/** Moves the dock by flipping `flip`, and stores the position. */
static void _wlmdock_flip(wlmdock_t *dock_ptr, uint32_t flip)
{
    uint32_t corner = _wlmdock_flipped_corner(dock_ptr, flip);
    if (0 == corner) return;
    wlmaker_dock_place_t place = wlmaker_dock_place_from_corner(
        corner, wlmaker_dock_place_vertical(
            (wlmaker_dock_place_t){
                dock_ptr->positioning.edge, dock_ptr->positioning.anchor }));
    bspl_dict_t *d = wlmaker_dock_place_encode(place);
    if (NULL != d) {
        wlmaker_state_store_section(dock_ptr->files_ptr,
                                    dock_ptr->state_fname_ptr,
                                    dock_ptr->section_ptr, d);
        bspl_dict_unref(d);
    }
    wlmtk_dock_positioning_t positioning = dock_ptr->positioning;
    positioning.edge = place.edge;
    positioning.anchor = place.anchor;
    _wlmdock_set_positioning(dock_ptr, &positioning);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_item_events_t::triggered: Does the action. */
static void _wlmdock_handle_menu_item_triggered(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmdock_menu_item_t *item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmdock_menu_item_t, triggered_listener);
    wlmdock_t *dock_ptr = item_ptr->dock_ptr;
    wlmdock_launcher_t *launcher_ptr = dock_ptr->menu_launcher_ptr;

    switch (item_ptr->action) {
    case WLMDOCK_MENU_LAUNCH:
        if (NULL != launcher_ptr) wlmdock_launcher_launch(launcher_ptr);
        break;
    case WLMDOCK_MENU_KILL:
        if (NULL != launcher_ptr) wlmdock_launcher_kill(launcher_ptr);
        break;
    case WLMDOCK_MENU_REMOVE:
        if (NULL != launcher_ptr) {
            dock_ptr->menu_launcher_ptr = NULL;
            wlmdock_tilebox_remove_tile(dock_ptr->tilebox_ptr,
                                        wlmdock_launcher_tile(launcher_ptr));
            wlmdock_launcher_destroy(launcher_ptr);
            _wlmdock_store_launchers(dock_ptr);
        }
        break;
    case WLMDOCK_MENU_FLIP_SIDE:
        // Moving destroys the menu: Not while it's dispatching this event.
        dock_ptr->pending_flip = WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
        break;
    case WLMDOCK_MENU_FLIP_END:
        dock_ptr->pending_flip = WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
        break;
    }
    _wlmdock_close_menu(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_item_events_t::destroy: Frees the item. */
static void _wlmdock_handle_menu_item_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmdock_menu_item_t *item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmdock_menu_item_t, destroy_listener);
    wlmtk_util_disconnect_listener(&item_ptr->triggered_listener);
    wlmtk_util_disconnect_listener(&item_ptr->destroy_listener);
    free(item_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds an item to the menu. */
static bool _wlmdock_menu_add(
    wlmdock_t *dock_ptr,
    const char *text_ptr,
    wlmdock_menu_action_t action)
{
    wlmdock_menu_item_t *item_ptr = logged_calloc(1, sizeof(*item_ptr));
    if (NULL == item_ptr) return false;
    item_ptr->dock_ptr = dock_ptr;
    item_ptr->action = action;
    item_ptr->menu_item_ptr = wlmtk_menu_item_create(
        wlmtk_menu_style_to_ref(dock_ptr->style_ptr->menu_style_ptr));
    if (NULL == item_ptr->menu_item_ptr ||
        !wlmtk_menu_item_set_text(item_ptr->menu_item_ptr, text_ptr)) {
        if (NULL != item_ptr->menu_item_ptr) {
            wlmtk_menu_item_destroy(item_ptr->menu_item_ptr);
        }
        free(item_ptr);
        return false;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(item_ptr->menu_item_ptr)->triggered,
        &item_ptr->triggered_listener,
        _wlmdock_handle_menu_item_triggered);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(item_ptr->menu_item_ptr)->destroy,
        &item_ptr->destroy_listener,
        _wlmdock_handle_menu_item_destroy);
    wlmtk_menu_add_item(dock_ptr->menu_ptr, item_ptr->menu_item_ptr);
    return true;
}

/** Extension of the menu element's virtual method table. */
static const wlmtk_element_vmt_t _wlmdock_menu_vmt = {
    .pointer_button = _wlmdock_menu_pointer_button,
};

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_element_vmt_t::pointer_button for the menu: Releasing
 * the right button over an item triggers it; elsewhere, the menu stays open
 * (it was a click). Pressing a button outside the menu closes it.
 */
bool _wlmdock_menu_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmdock_t *dock_ptr = NULL;
    // The menu element is a child of the root container.
    if (NULL != element_ptr->parent_container_ptr) {
        dock_ptr = BS_CONTAINER_OF(
            element_ptr->parent_container_ptr, wlmdock_t, root_container);
    }
    if (NULL == dock_ptr || NULL == dock_ptr->menu_ptr) return true;

    if (WLMTK_BUTTON_DOWN == button_event_ptr->type &&
        !element_ptr->pointer_inside) {
        _wlmdock_close_menu(dock_ptr);
        return true;
    }
    if (BTN_RIGHT == button_event_ptr->button) {
        if (WLMTK_BUTTON_UP != button_event_ptr->type ||
            !element_ptr->pointer_inside) return true;
        for (size_t i = 0; i < wlmtk_menu_items_size(dock_ptr->menu_ptr); ++i) {
            wlmtk_menu_item_t *item_ptr = wlmtk_menu_item_at(
                dock_ptr->menu_ptr, i);
            if (wlmtk_menu_item_element(item_ptr)->pointer_inside &&
                WLMTK_MENU_ITEM_DISABLED != wlmtk_menu_item_get_state(
                    item_ptr)) {
                wlmtk_menu_item_trigger(item_ptr);
                break;
            }
        }
        return true;
    }
    return dock_ptr->menu_orig_vmt.pointer_button(
        element_ptr, button_event_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Opens the launcher's right-click menu, like on Window Maker: Launch, Kill
 * (when it launched processes), Remove (stored in the state), and moving
 * the Dock to another corner (except the Clip's). Drawn next to the tiles,
 * on the dock's surface, which grows for it while open.
 */
void _wlmdock_open_menu(
    wlmdock_launcher_t *launcher_ptr,
    void *ud_ptr)
{
    wlmdock_t *dock_ptr = ud_ptr;
    _wlmdock_destroy_menu(dock_ptr);
    if (NULL == dock_ptr->style_ptr->menu_style_ptr) return;

    dock_ptr->menu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(dock_ptr->style_ptr->menu_style_ptr));
    if (NULL == dock_ptr->menu_ptr) return;
    dock_ptr->menu_launcher_ptr = launcher_ptr;

    bool rv = _wlmdock_menu_add(dock_ptr, "Launch", WLMDOCK_MENU_LAUNCH);
    if (wlmdock_launcher_has_processes(launcher_ptr)) {
        rv = rv && _wlmdock_menu_add(dock_ptr, "Kill", WLMDOCK_MENU_KILL);
    }
    rv = rv && _wlmdock_menu_add(dock_ptr, "Remove", WLMDOCK_MENU_REMOVE);
    // The Clip's launchers follow the Clip: No moving them on their own.
    if (!dock_ptr->clip) {
        uint32_t corner = dock_ptr->positioning.edge |
            dock_ptr->positioning.anchor;
        if (0 != _wlmdock_flipped_corner(
                dock_ptr, WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
            rv = rv && _wlmdock_menu_add(
                dock_ptr,
                (corner & WLR_EDGE_LEFT) ?
                "Move to Right Side" : "Move to Left Side",
                WLMDOCK_MENU_FLIP_SIDE);
        }
        if (0 != _wlmdock_flipped_corner(
                dock_ptr, WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
            rv = rv && _wlmdock_menu_add(
                dock_ptr,
                (corner & WLR_EDGE_TOP) ? "Move to Bottom" : "Move to Top",
                WLMDOCK_MENU_FLIP_END);
        }
    }
    if (!rv) {
        _wlmdock_destroy_menu(dock_ptr);
        return;
    }

    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(dock_ptr->menu_ptr)->request_close,
        &dock_ptr->menu_request_close_listener,
        _wlmdock_handle_menu_request_close);
    wlmtk_element_t *e = wlmtk_menu_element(dock_ptr->menu_ptr);
    wlmtk_container_add_element(&dock_ptr->root_container, e);

    // Not the toolkit's right-click mode: It tells a click from a drag by
    // the pointer's position in the menu, but the menu moves on the surface
    // when the surface grows for it at the left or top. Instead: Items are
    // triggered by a left click, or by releasing the right button over them.
    dock_ptr->menu_orig_vmt = wlmtk_element_extend(e, &_wlmdock_menu_vmt);
    wlmtk_menu_set_open(dock_ptr->menu_ptr, false);
    wlmtk_menu_set_open(dock_ptr->menu_ptr, true);
    _wlmdock_layout(dock_ptr);
    // Like the root menu: Gets all pointer events, to see the right button
    // released over an item, or a click elsewhere.
    wlmtk_container_pointer_grab(&dock_ptr->root_container, e);
}

/* ------------------------------------------------------------------------- */
/** Closes the menu: Hides it now, destroys it from the event loop. */
void _wlmdock_close_menu(wlmdock_t *dock_ptr)
{
    if (NULL == dock_ptr->menu_ptr ||
        NULL != dock_ptr->menu_close_event_source_ptr) return;
    dock_ptr->menu_close_event_source_ptr = wl_event_loop_add_idle(
        wl_display_get_event_loop(dock_ptr->local_display_ptr),
        _wlmdock_idle_destroy_menu,
        dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Destroys the closed menu, from the event loop. */
void _wlmdock_idle_destroy_menu(void *data_ptr)
{
    wlmdock_t *dock_ptr = data_ptr;
    // The idle source is gone, once dispatched.
    dock_ptr->menu_close_event_source_ptr = NULL;
    _wlmdock_destroy_menu(dock_ptr);
    if (0 != dock_ptr->pending_flip) {
        uint32_t flip = dock_ptr->pending_flip;
        dock_ptr->pending_flip = 0;
        _wlmdock_flip(dock_ptr, flip);
    }
}

/* ------------------------------------------------------------------------- */
/** Destroys the menu, if open. Shrinks the surface back to the tiles. */
void _wlmdock_destroy_menu(wlmdock_t *dock_ptr)
{
    if (NULL != dock_ptr->menu_close_event_source_ptr) {
        wl_event_source_remove(dock_ptr->menu_close_event_source_ptr);
        dock_ptr->menu_close_event_source_ptr = NULL;
    }
    if (NULL == dock_ptr->menu_ptr) return;

    wlmtk_element_t *e = wlmtk_menu_element(dock_ptr->menu_ptr);
    wlmtk_util_disconnect_listener(&dock_ptr->menu_request_close_listener);
    if (e->parent_container_ptr == &dock_ptr->root_container) {
        wlmtk_container_pointer_grab_release(&dock_ptr->root_container, e);
        wlmtk_container_remove_element(&dock_ptr->root_container, e);
    }
    wlmtk_menu_destroy(dock_ptr->menu_ptr);
    dock_ptr->menu_ptr = NULL;
    dock_ptr->menu_launcher_ptr = NULL;
    _wlmdock_layout(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Places the tiles and the menu on the surface: The menu beside the
 * launcher's tile, away from the edge; the tiles where they were on screen.
 */
void _wlmdock_layout(wlmdock_t *dock_ptr)
{
    wlmtk_element_t *tb_ptr = wlmdock_tilebox_element(dock_ptr->tilebox_ptr);
    if (NULL == dock_ptr->menu_ptr || NULL == dock_ptr->menu_launcher_ptr) {
        wlmtk_element_set_position(tb_ptr, 0, 0);
        wlmtk_container_invalidate_layout(&dock_ptr->root_container);
        return;
    }
    wlmtk_element_t *m_ptr = wlmtk_menu_element(dock_ptr->menu_ptr);

    struct wlr_box tb = wlmtk_element_get_dimensions_box(tb_ptr);
    struct wlr_box m = wlmtk_element_get_dimensions_box(m_ptr);
    struct wlr_box t = wlmdock_tilebox_tile_box(
        dock_ptr->tilebox_ptr,
        wlmdock_launcher_tile(dock_ptr->menu_launcher_ptr));
    uint32_t edge = dock_ptr->positioning.edge;
    uint32_t anchor = dock_ptr->positioning.anchor;

    int tb_x = 0, tb_y = 0, m_x = 0, m_y = 0;
    if (WLR_EDGE_LEFT == edge || WLR_EDGE_RIGHT == edge) {
        if (WLR_EDGE_RIGHT == edge) {
            tb_x = m.width;
        } else {
            m_x = tb.width;
        }
        // Along the tile; growing away from the anchor.
        m_y = (WLR_EDGE_BOTTOM == anchor) ? t.y + t.height - m.height : t.y;
        if (0 > m_y) {
            tb_y = -m_y;
            m_y = 0;
        }
    } else {
        if (WLR_EDGE_BOTTOM == edge) {
            tb_y = m.height;
        } else {
            m_y = tb.height;
        }
        m_x = (WLR_EDGE_RIGHT == anchor) ? t.x + t.width - m.width : t.x;
        if (0 > m_x) {
            tb_x = -m_x;
            m_x = 0;
        }
    }
    wlmtk_element_set_position(tb_ptr, tb_x - tb.x, tb_y - tb.y);
    wlmtk_element_set_position(m_ptr, m_x - m.x, m_y - m.y);
    wlmtk_container_invalidate_layout(&dock_ptr->root_container);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_events_t::request_close: Closes the menu. */
void _wlmdock_handle_menu_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmdock_t *dock_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmdock_t, menu_request_close_listener);
    _wlmdock_close_menu(dock_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmcl_client_events::pointer_leave: Closes the menu. */
void _wlmdock_handle_pointer_leave(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmdock_t *dock_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmdock_t, pointer_leave_listener);
    _wlmdock_close_menu(dock_ptr);
}

/* == End of wlmdock.c ===================================================== */
