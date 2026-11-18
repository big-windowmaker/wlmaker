/* ========================================================================= */
/**
 * @file wlmaker.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2023 Google LLC
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

/// setenv() is a POSIX extension.
#define _POSIX_C_SOURCE 200112L

#include <basedir.h>
#include <fcntl.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <wayland-server-core.h>
/// Use non-stable features of wlroots.
#define WLR_USE_UNSTABLE
#include <wlr/util/log.h>
#undef WLR_USE_UNSTABLE

#include "../share/theme.h"  // IWYU pragma: keep
#include "action.h"
#include "backend/backend.h"
#include "clip.h"
#include "config.h"
#include "i18n.h"
#include "root_menu.h"
#include "server.h"
#include "workspace_manager.h"
#include "zones.h"
#include "task_list.h"
#include "toolkit/toolkit.h"
#include "util/backtrace.h"
#include "util/files.h"
#include "util/version.h"
#include "util/wlr_log.h"

/** Will hold the value of --config_file. */
static char *wlmaker_arg_config_file_ptr = NULL;
/** Will hold the value of --state_file. */
static char *wlmaker_arg_state_file_ptr = NULL;
/** Will hold the value of --theme_file. */
static char *wlmaker_arg_theme_file_ptr = NULL;
/** Will hold the value of --root_menu_file. */
static char *wlmaker_arg_root_menu_file_ptr = NULL;

/** Startup options for the server. */
static wlmaker_server_options_t wlmaker_server_options = {
    .start_xwayland = false,
    .lazy_xwayland = false,
    .width = 0,
    .height = 0,
    .bind_with_logo = false,
};

/** Definition of commandline arguments. */
static const bs_arg_t wlmaker_args[] = {
#if defined(WLMAKER_HAVE_XWAYLAND)
    BS_ARG_BOOL(
        "start_xwayland",
        "Optional: Whether to start XWayland. Disabled by default.",
        false,
        &wlmaker_server_options.start_xwayland),
    BS_ARG_BOOL(
        "lazy_xwayland",
        "Optional: With --start_xwayland, starts the XWayland server only "
        "when the first X11 client connects. Disabled by default.",
        false,
        &wlmaker_server_options.lazy_xwayland),
#endif  // defined(WLMAKER_HAVE_XWAYLAND)
    BS_ARG_STRING(
        "config_file",
        "Optional: Path to a configuration file. If not provided, wlmaker "
        "will scan default paths for a configuration file, or fall back to "
        "a built-in configuration.",
        NULL,
        &wlmaker_arg_config_file_ptr),
    BS_ARG_STRING(
        "state_file",
        "Optional: Path to a state file, with state of workspaces, dock and "
        "clips configured. If not provided, wlmaker will scan default paths "
        "for a state file, or fall back to a built-in default.",
        NULL,
        &wlmaker_arg_state_file_ptr),
    BS_ARG_STRING(
        "theme_file",
        "Optional: Path to a \"theme\" file, configuring the visual style for "
        "elements. If not provided, wlmaker will use a built-in default theme.",
        NULL,
        &wlmaker_arg_theme_file_ptr),
    BS_ARG_STRING(
        "root_menu_file",
        "Optional: Path to a file describing the root menu. If not provided, "
        "wlmaker will use a built-in definition for the root menu.",
        NULL,
        &wlmaker_arg_root_menu_file_ptr),
    bs_arg_log_level,
    bs_arg_log_to_stderr,
    BS_ARG_BOOL(
        "bind_with_logo",
        "Optional: Whether to add 'Logo' as modifier to each key binding. "
        "Useful to test key bindings in wlmaker when running as a Wayland "
        "or X11 client. Disabled by default.",
        false,
        &wlmaker_server_options.bind_with_logo),
    BS_ARG_UINT32(
        "height",
        "Desired output height. Applies when running in windowed mode, and "
        "only if --width is set, too. Set to 0 for using the output's "
        "preferred dimensions.",
        0, 0, UINT32_MAX,
        &wlmaker_server_options.height),
    BS_ARG_UINT32(
        "width",
        "Desired output width. Applies when running in windowed mode, and "
        "only if --height is set, too. Set to 0 for using the output's "
        "preferred dimensions.",
        0, 0, UINT32_MAX,
        &wlmaker_server_options.width),
    BS_ARG_SENTINEL()
};

/** Event source for SIGHUP, which reloads the configuration. */
static struct wl_event_source *wlmaker_sighup_event_source_ptr = NULL;

/** References auto-started subprocesses. */
static bs_ptr_stack_t         wlmaker_subprocess_stack;

/** The RLIMIT_NOFILE wlmaker started with. Restored in forked processes. */
static struct rlimit          wlmaker_nofile_rlimit;
/** Whether @ref wlmaker_nofile_rlimit was raised, and needs restoring. */
static bool                   wlmaker_nofile_rlimit_raised = false;

/* ------------------------------------------------------------------------- */
/** Launches a sub-process, and keeps it on the subprocess stack. */
bool start_subprocess(const char *cmdline_ptr)
{
    bs_subprocess_t *sp_ptr = bs_subprocess_create_cmdline(cmdline_ptr);
    if (NULL == sp_ptr) {
        bs_log(BS_ERROR, "Failed bs_subprocess_create_cmdline(\"%s\")",
               cmdline_ptr);
        return false;
    }
    if (!bs_subprocess_start(sp_ptr)) {
        bs_log(BS_ERROR, "Failed bs_subprocess_start for \"%s\"",
               cmdline_ptr);
        return false;
    }
    // Push to stack. Ignore errors: We'll keep it running untracked.
    bs_ptr_stack_push(&wlmaker_subprocess_stack, sp_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Handles SIGHUP: Reloads configuration, theme, key bindings and menu. */
static int _wlmaker_handle_sighup(__UNUSED__ int signal_number, void *data_ptr)
{
    wlmaker_server_reload(data_ptr);
    wlmaker_zones_reload(((wlmaker_server_t*)data_ptr)->zones_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Raises the soft limit of open file descriptors to the hard limit.
 *
 * Each client holds file descriptors in the compositor: Its socket, shared
 * memory pools, DMA-BUF planes, sync fences, keymaps. A session with a few
 * monitors, XWayland windows and a browser exhausts the usual soft limit of
 * 1024. Then file descriptors received from clients are dropped (the client
 * is disconnected: "invalid arguments for wl_shm.create_pool"), fences
 * cannot be duplicated, and a lazy XWayland that terminated does not start
 * again: wlroots fails to watch the X11 sockets, without telling.
 *
 * The soft limit stays at 1024 for compatibility with select(), which cannot
 * handle higher file descriptors. wlmaker does not use select(), so it can
 * use the hard limit. Forked processes get the original limit back when
 * they can, see @ref _wlmaker_atfork_child.
 */
static void _wlmaker_raise_nofile_rlimit(void)
{
    if (0 != getrlimit(RLIMIT_NOFILE, &wlmaker_nofile_rlimit)) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed getrlimit(RLIMIT_NOFILE)");
        return;
    }
    if (wlmaker_nofile_rlimit.rlim_cur >= wlmaker_nofile_rlimit.rlim_max) {
        return;
    }
    struct rlimit raised = wlmaker_nofile_rlimit;
    raised.rlim_cur = raised.rlim_max;
    if (0 != setrlimit(RLIMIT_NOFILE, &raised)) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed setrlimit(RLIMIT_NOFILE, %ju)",
               (uintmax_t)raised.rlim_cur);
        return;
    }
    wlmaker_nofile_rlimit_raised = true;
    bs_log(BS_INFO, "Raised limit of open files from %ju to %ju.",
           (uintmax_t)wlmaker_nofile_rlimit.rlim_cur,
           (uintmax_t)raised.rlim_cur);
}

/* ------------------------------------------------------------------------- */
/**
 * In forked processes: Unblocks SIGHUP and SIGCHLD (only wlmaker monitors
 * them, see @ref wlmaker_block_monitored_signals), and
 * restores the limit of open files that wlmaker started with.
 *
 * The limit is restored only if the child can still open files under it.
 * A child inherits all of wlmaker's file descriptors until it calls exec();
 * when they reach the original limit, a lower limit would make every open()
 * fail with EMFILE before the exec: XWayland, forked by wlroots, opens
 * /dev/null first and would not start. That child keeps the raised limit.
 */
static void _wlmaker_atfork_child(void)
{
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGHUP);
    sigaddset(&signal_set, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &signal_set, NULL);

    if (wlmaker_nofile_rlimit_raised) {
        // The lowest free descriptor (async-signal-safe calls only).
        int fd = open("/dev/null", O_RDONLY);
        if (0 <= fd) {
            if ((rlim_t)fd < wlmaker_nofile_rlimit.rlim_cur) {
                setrlimit(RLIMIT_NOFILE, &wlmaker_nofile_rlimit);
            }
            close(fd);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Blocks the signals wlmaker monitors, in all threads. */
static void wlmaker_block_monitored_signals(void)
{
    // The Wayland event loop reads SIGCHLD (subprocess monitor) and SIGHUP
    // (reload) through a signalfd, which only sees signals blocked in every
    // thread. wl_event_loop_add_signal() blocks them in the calling thread
    // only: Threads started before (eg. GLib's gdbus and pool threads) would
    // take them, dropping SIGCHLD (a terminated subprocess is never noticed:
    // a generated menu stays empty) or ending the process on SIGHUP. Blocked
    // here, before any thread starts, every thread inherits the mask.
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGHUP);
    sigaddset(&signal_set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
}

/* == Main program ========================================================= */
/** The main program. */
int main(__UNUSED__ int argc, __UNUSED__ const char **argv)
{
    wlmaker_clip_t            *clip_ptr = NULL;
    wlmaker_task_list_t       *task_list_ptr = NULL;
    int                       rv = EXIT_SUCCESS;

    wlmaker_block_monitored_signals();
    wlmaker_i18n_init();
    if (!wlm_util_backtrace_setup(argv[0]) ||
        !wlm_util_wlr_log_init(WLR_DEBUG) ||
        // Ideally: xdgStateHome. But: libxdg-basedir is using FreeDesktop
        // specification v0.7, and XDG_STATE_HOME was added with v0.8 only.
        !bs_log_init(xdgCacheHome(NULL), "wlmaker")) {
        return EXIT_FAILURE;
    }
    BS_ASSERT(bs_ptr_stack_init(&wlmaker_subprocess_stack));

    if (!bs_arg_parse(wlmaker_args, BS_ARG_MODE_EXTRA_ARGS, &argc, argv)) {
        fprintf(stderr, "Failed to parse commandline arguments.\n");
        bs_arg_print_usage(stderr, wlmaker_args);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--help")) {
            bs_arg_print_usage(stdout, wlmaker_args);
            return EXIT_SUCCESS;
        } else if (0 == strcmp(argv[i], "--version")) {
            fprintf(stdout, "wlmaker version %s (%s)\n",
                    wlm_util_version, wlm_util_version_full);
            return EXIT_SUCCESS;
        } else {
            bs_log(BS_ERROR, "Unhandled extra argument \"%s\"", argv[i]);
            return EXIT_FAILURE;
        }
    }
    bs_log(BS_INFO, "Starting wlmaker %s (%s)",
           wlm_util_version,
           wlm_util_version_full);
    _wlmaker_raise_nofile_rlimit();
    // Before any fork(): XWayland (when not lazy) starts with the server.
    pthread_atfork(NULL, NULL, _wlmaker_atfork_child);

    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    if (NULL == files_ptr) {
        bs_log(BS_ERROR, "Failed wlm_util_files_create(\"wlmaker\")");
        return EXIT_FAILURE;
    }

    bspl_dict_t *config_dict_ptr = wlmaker_config_load(
        files_ptr, wlmaker_arg_config_file_ptr);
    if (NULL == config_dict_ptr) {
        fprintf(stderr, "Failed to load & initialize configuration.\n");
        return EXIT_FAILURE;
    }

    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        files_ptr, wlmaker_arg_state_file_ptr);
    if (NULL == state_dict_ptr) {
        fprintf(stderr, "Failed to load & initialize state.\n");
        return EXIT_FAILURE;
    }

    const char *theme_file_ptr = wlmaker_arg_theme_file_ptr;
    if (NULL == theme_file_ptr) {
        theme_file_ptr = bspl_dict_get_string_value(
            config_dict_ptr, "ThemeFile");
    }
    wlmaker_config_style_t style = {};
    if (!wlmaker_theme_load(files_ptr, theme_file_ptr, &style)) {
        fprintf(stderr, "Failed to load & initialize theme.\n");
        return EXIT_FAILURE;
    }

    wlmaker_server_t *server_ptr = wlmaker_server_create(
        config_dict_ptr, files_ptr, &style, &wlmaker_server_options);
    if (NULL == server_ptr) return EXIT_FAILURE;
    // Kept, for storing changes (eg. a Dock moved to another corner).
    server_ptr->state_fname_ptr = wlmaker_arg_state_file_ptr;
    wlmaker_arg_state_file_ptr = NULL;
    // Kept, for reloading.
    server_ptr->config_fname_ptr = wlmaker_arg_config_file_ptr;
    server_ptr->theme_fname_ptr = wlmaker_arg_theme_file_ptr;
    server_ptr->root_menu_fname_ptr = wlmaker_arg_root_menu_file_ptr;

    // TODO(kaeser@gubbe.ch): Uh, that's ugly...
    server_ptr->root_menu_ptr = wlmaker_root_menu_create(
        server_ptr,
        wlmaker_arg_root_menu_file_ptr,
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == server_ptr->root_menu_ptr) {
        return EXIT_FAILURE;
    }
    wlmtk_menu_set_open(
        wlmaker_root_menu_menu(server_ptr->root_menu_ptr),
        false);

    server_ptr->action_handle_ptr = wlmaker_action_bind_keys(
        server_ptr,
        bspl_dict_get_dict(config_dict_ptr, wlmaker_action_config_dict_key),
        wlmaker_server_options.bind_with_logo);
    if (NULL == server_ptr->action_handle_ptr) {
        bs_log(BS_ERROR, "Failed to bind keys.");
        return EXIT_FAILURE;
    }

    // SIGHUP reloads the configuration. The signal is blocked for regular
    // delivery: Not for launched programs.
    wlmaker_sighup_event_source_ptr = wl_event_loop_add_signal(
        wl_display_get_event_loop(server_ptr->wl_display_ptr),
        SIGHUP,
        _wlmaker_handle_sighup,
        server_ptr);
    if (NULL == wlmaker_sighup_event_source_ptr) {
        bs_log(BS_WARNING, "Failed wl_event_loop_add_signal(SIGHUP): "
               "Reloading by signal is not available.");
    }

    if (!wlm_workspace_manager_apply_state(
            server_ptr->workspace_manager_ptr,
            state_dict_ptr)) {
        return EXIT_FAILURE;
    }

    rv = EXIT_SUCCESS;
    if (wlr_backend_start(wlmbe_backend_wlr(server_ptr->backend_ptr))) {

        if (0 >= wlmbe_num_outputs(server_ptr->wlr_output_layout_ptr)) {
            bs_log(BS_ERROR, "No outputs available!");
            return EXIT_FAILURE;
        }

        bs_log(BS_INFO, "Starting Wayland compositor for server %p at %s ...",
               server_ptr, server_ptr->wl_socket_name_ptr);

        setenv("WAYLAND_DISPLAY", server_ptr->wl_socket_name_ptr, true);

        bspl_array_t *autostarted_ptr = bspl_dict_get_array(
            config_dict_ptr, "Autostart");
        if (NULL != autostarted_ptr) {
            for (size_t i = 0; i < bspl_array_size(autostarted_ptr); ++i) {
                const char *cmd_ptr = bspl_array_string_value_at(
                    autostarted_ptr, i);
                if (!start_subprocess(cmd_ptr)) return EXIT_FAILURE;
            }
        }

        clip_ptr = wlmaker_clip_create(
            server_ptr, state_dict_ptr, &style);
        server_ptr->zones_ptr = wlmaker_zones_create(server_ptr);
        task_list_ptr = wlmaker_task_list_create(
            server_ptr, &style.task_list);
        if (NULL == clip_ptr || NULL == task_list_ptr) {
            bs_log(BS_ERROR, "Failed to create clip or task list.");
        } else {
            wl_display_run(server_ptr->wl_display_ptr);
        }

    } else {
        bs_log(BS_ERROR, "Failed wlmaker_server_start()");
        rv = EXIT_FAILURE;
    }

    if (NULL != server_ptr->zones_ptr) {
        wlmaker_zones_destroy(server_ptr->zones_ptr);
        server_ptr->zones_ptr = NULL;
    }
    if (NULL != task_list_ptr) wlmaker_task_list_destroy(task_list_ptr);
    if (NULL != clip_ptr) wlmaker_clip_destroy(clip_ptr);
    if (NULL != wlmaker_sighup_event_source_ptr) {
        wl_event_source_remove(wlmaker_sighup_event_source_ptr);
        wlmaker_sighup_event_source_ptr = NULL;
    }
    wlmaker_action_unbind_keys(server_ptr->action_handle_ptr);
    server_ptr->action_handle_ptr = NULL;
    bspl_array_unref(server_ptr->root_menu_array_ptr);
    wlmaker_server_destroy(server_ptr);

    bs_subprocess_t *sp_ptr;
    while (NULL != (sp_ptr = bs_ptr_stack_pop(&wlmaker_subprocess_stack))) {
        bs_subprocess_destroy(sp_ptr);
    }
    bs_ptr_stack_fini(&wlmaker_subprocess_stack);

    if (NULL != wlmaker_arg_config_file_ptr) free(wlmaker_arg_config_file_ptr);
    bspl_decoded_destroy(wlmaker_config_style_desc, &style);
    bspl_dict_unref(config_dict_ptr);
    bspl_dict_unref(state_dict_ptr);
    return rv;
}

/* == End of wlmaker.c ===================================================== */
