/* ========================================================================= */
/**
 * @file appicons.c
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

#include "appicons.h"

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_keyboard.h>
#undef WLR_USE_UNSTABLE

#include "action.h"
#include "context_menu.h"
#include "dialog.h"
#include "dock_launchers.h"
#include "dock_menu.h"
#include "i18n.h"
#include "input/manager.h"
#include "toplevel_tracker.h"

/* == Declarations ========================================================= */

/** Argument of the menu's callbacks: The icon's application. */
typedef struct {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** The entry of the icon, referenced. */
    bspl_dict_t               *entry_ptr;
    /** Whether all its windows are hidden: "Unhide", else "Hide". */
    bool                      hidden;
} _wlmaker_appicons_arg_t;

/** A growing string. */
typedef struct {
    /** The string. */
    char                      *s;
    /** Its length. */
    size_t                    len;
    /** Allocated size. */
    size_t                    size;
    /** Whether an allocation failed. */
    bool                      failed;
} _wlmaker_appicons_buf_t;

/** Finds a window's process. */
typedef struct {
    /** The process, or 0. */
    pid_t                     pid;
} _wlmaker_appicons_pid_t;

/** The applications whose windows stay, for "hide the others". */
typedef struct {
    /** The entry's `AppIDs` (or `AppID`). */
    bspl_dict_t               *entry_ptr;
} _wlmaker_appicons_others_t;

static _wlmaker_appicons_arg_t *_wlmaker_appicons_arg_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr,
    bool hidden);
static void _wlmaker_appicons_arg_destroy(void *ud_ptr);
static bool _wlmaker_appicons_menu_add(
    wlmaker_context_menu_t *cm_ptr,
    const char *text_ptr,
    bool enabled,
    wlmaker_context_menu_callback_t callback,
    _wlmaker_appicons_arg_t *arg_ptr);
static void _wlmaker_appicons_unhide_here(void *ud_ptr);
static void _wlmaker_appicons_hide(void *ud_ptr);
static void _wlmaker_appicons_launch(void *ud_ptr);
static void _wlmaker_appicons_set_icon(void *ud_ptr);
static void _wlmaker_appicons_kill(void *ud_ptr);
static void _wlmaker_appicons_kill_confirmed(void *ud_ptr, bool confirmed);
static size_t _wlmaker_appicons_windows(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr,
    size_t *hidden_ptr);
static const char *_wlmaker_appicons_app_id_at(
    bspl_dict_t *entry_ptr,
    size_t i);
static void _wlmaker_appicons_find_pid(
    wlmtk_window_t *window_ptr,
    void *ud_ptr);
static void _wlmaker_appicons_hide_other(
    wlmtk_window_t *window_ptr,
    const char *app_id_ptr,
    void *ud_ptr);
static void _wlmaker_appicons_buf_add(
    _wlmaker_appicons_buf_t *buf_ptr,
    const char *s_ptr,
    size_t len);
static void _wlmaker_appicons_buf_add_arg(
    _wlmaker_appicons_buf_t *buf_ptr,
    const char *arg_ptr);
static char *_wlmaker_appicons_in_path(
    const char *name_ptr,
    const char *path_ptr);
static char *_wlmaker_appicons_read_link(const char *path_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bspl_array_t *wlmaker_appicons_load(const char *fname_ptr)
{
    char *p = NULL;
    if (NULL == fname_ptr) {
        const char *dir_ptr = getenv("XDG_RUNTIME_DIR");
        if (NULL == dir_ptr || '\0' == *dir_ptr) return NULL;
        p = bs_strdupf("%s/big-wm-appicons.plist", dir_ptr);
        if (NULL == p) return NULL;
        fname_ptr = p;
    }
    bspl_object_t *o = NULL;
    if (bs_file_realpath_is(fname_ptr, S_IFREG)) {
        o = bspl_create_object_from_plist_file(fname_ptr);
    }
    if (NULL != p) free(p);
    if (NULL == o) return NULL;
    bspl_array_t *a = bspl_dict_get_array(
        bspl_dict_from_object(o), "Appicons");
    if (NULL != a) bspl_array_ref(a);
    bspl_object_unref(o);
    return a;
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_appicons_at(bspl_array_t *array_ptr, int index)
{
    if (NULL == array_ptr || 0 > index ||
        (size_t)index >= bspl_array_size(array_ptr)) return NULL;
    return bspl_dict_from_object(bspl_array_at(array_ptr, index));
}

/* ------------------------------------------------------------------------- */
void wlmaker_appicons_open_menu(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr)
{
    if (NULL == entry_ptr || NULL == server_ptr->style_ptr ||
        NULL == server_ptr->style_ptr->menu_style_ptr) return;
    size_t hidden = 0;
    size_t windows = _wlmaker_appicons_windows(server_ptr, entry_ptr, &hidden);
    bool all_hidden = 0 < windows && hidden == windows;
    const char *cmd_ptr = wlmaker_dock_launcher_value(entry_ptr, "CommandLine");
    bspl_dict_t *launcher_ptr = NULL;
    if (NULL == cmd_ptr || '\0' == *cmd_ptr) {
        launcher_ptr = wlmaker_appicons_launcher(server_ptr, entry_ptr);
    }
    bool launchable = (NULL != cmd_ptr && '\0' != *cmd_ptr) ||
        NULL != launcher_ptr;
    if (NULL != launcher_ptr) bspl_dict_unref(launcher_ptr);
    const char *settings_ptr = NULL != server_ptr->config_dict_ptr ?
        bspl_dict_get_string_value(server_ptr->config_dict_ptr,
                                   "DockSettingsCommand") : NULL;

    const char *title_ptr = wlmaker_dock_launcher_value(entry_ptr, "Name");
    if (NULL == title_ptr) title_ptr = wlmaker_dock_launcher_value(
        entry_ptr, "AppID");
    wlmaker_context_menu_t *cm_ptr = wlmaker_context_menu_create(
        server_ptr, server_ptr, NULL != title_ptr ? title_ptr : "");
    if (NULL == cm_ptr) return;
    // Window Maker's createApplicationMenu, in its order.
    bool rv =
        _wlmaker_appicons_menu_add(
            cm_ptr, wlmaker_i18n("Unhide Here"), 0 < windows,
            _wlmaker_appicons_unhide_here,
            _wlmaker_appicons_arg_create(server_ptr, entry_ptr, all_hidden)) &&
        _wlmaker_appicons_menu_add(
            cm_ptr, all_hidden ? wlmaker_i18n("Unhide") : wlmaker_i18n("Hide"),
            0 < windows,
            _wlmaker_appicons_hide,
            _wlmaker_appicons_arg_create(server_ptr, entry_ptr, all_hidden)) &&
        _wlmaker_appicons_menu_add(
            cm_ptr, wlmaker_i18n("Launch"), launchable,
            _wlmaker_appicons_launch,
            _wlmaker_appicons_arg_create(server_ptr, entry_ptr, all_hidden)) &&
        _wlmaker_appicons_menu_add(
            cm_ptr, wlmaker_i18n("Set Icon..."),
            NULL != settings_ptr && '\0' != *settings_ptr,
            _wlmaker_appicons_set_icon,
            _wlmaker_appicons_arg_create(server_ptr, entry_ptr, all_hidden)) &&
        _wlmaker_appicons_menu_add(
            cm_ptr, wlmaker_i18n("Kill"), 0 < windows,
            _wlmaker_appicons_kill,
            _wlmaker_appicons_arg_create(server_ptr, entry_ptr, all_hidden));
    if (!rv) {
        wlmaker_context_menu_destroy(cm_ptr);
        return;
    }
    wlmaker_context_menu_open(cm_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_appicons_double_click(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr,
    uint32_t modifiers)
{
    if (NULL == entry_ptr) return;
    // Window Maker: Ctrl launches it again.
    if (modifiers & WLR_MODIFIER_CTRL) {
        wlmaker_appicons_launch(server_ptr, entry_ptr);
        return;
    }
    const char *last_ptr = NULL;
    for (size_t i = 0; ; ++i) {
        const char *app_id_ptr = _wlmaker_appicons_app_id_at(entry_ptr, i);
        if (NULL == app_id_ptr) break;
        if (0 == wlmaker_dock_app_windows(server_ptr, app_id_ptr, NULL)) {
            continue;
        }
        // Shift: Unhides it here. Else, where it is (its workspace).
        if (modifiers & WLR_MODIFIER_SHIFT) {
            wlmaker_dock_app_bring_here(server_ptr, app_id_ptr);
        } else {
            wlmaker_dock_app_unhide(server_ptr, app_id_ptr);
        }
        last_ptr = app_id_ptr;
    }
    if (NULL != last_ptr && 0 == (modifiers & WLR_MODIFIER_SHIFT)) {
        wlmaker_dock_app_activate(server_ptr, last_ptr);
    }

    // With the move modifier (Window Maker's MOD_MASK): Hides the others.
    uint32_t mod = 0;
    if (NULL != server_ptr->input_manager_ptr) {
        mod = wlmim_cursor_options(
            server_ptr->input_manager_ptr)->move_window_modifier;
    }
    if (0 != mod && mod == (modifiers & mod)) {
        _wlmaker_appicons_others_t others = { .entry_ptr = entry_ptr };
        wlmaker_toplevel_tracker_for_each_window(
            server_ptr->toplevel_tracker_ptr,
            _wlmaker_appicons_hide_other, &others);
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_appicons_launch(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr)
{
    bspl_dict_t *launcher_ptr = wlmaker_appicons_launcher(
        server_ptr, entry_ptr);
    if (NULL == launcher_ptr) return;
    const char *cmd_ptr = wlmaker_dock_launcher_value(
        launcher_ptr, "CommandLine");
    if (NULL != cmd_ptr && '\0' != *cmd_ptr) {
        bs_log(BS_INFO, "Appicon: Launching \"%s\"", cmd_ptr);
        char *c = logged_strdup(cmd_ptr);
        if (NULL != c) {
            wlmaker_action_execute(server_ptr, WLMAKER_ACTION_EXECUTE, c);
            free(c);
        }
    }
    bspl_dict_unref(launcher_ptr);
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_appicons_launcher(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr)
{
    if (NULL == entry_ptr) return NULL;
    const char *app_id_ptr = wlmaker_dock_launcher_value(entry_ptr, "AppID");
    const char *icon_ptr = wlmaker_dock_launcher_value(entry_ptr, "Icon");
    const char *cmd_ptr = wlmaker_dock_launcher_value(entry_ptr, "CommandLine");
    char *made_ptr = NULL;
    if (NULL == cmd_ptr || '\0' == *cmd_ptr) {
        // No .desktop file: The command line of its process.
        _wlmaker_appicons_pid_t p = {};
        for (size_t i = 0; 0 == p.pid; ++i) {
            const char *a_ptr = _wlmaker_appicons_app_id_at(entry_ptr, i);
            if (NULL == a_ptr) break;
            wlmaker_toplevel_tracker_for_each_app_window(
                server_ptr->toplevel_tracker_ptr, a_ptr,
                _wlmaker_appicons_find_pid, &p);
        }
        if (1 < p.pid) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%"PRIdMAX"/cmdline",
                     (intmax_t)p.pid);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            char buf[8192];
            ssize_t len = 0;
            if (0 <= fd) {
                len = read(fd, buf, sizeof(buf) - 1);
                close(fd);
            }
            if (0 < len) {
                buf[len] = '\0';
                snprintf(path, sizeof(path), "/proc/%"PRIdMAX"/cwd",
                         (intmax_t)p.pid);
                char *cwd_ptr = _wlmaker_appicons_read_link(path);
                snprintf(path, sizeof(path), "/proc/%"PRIdMAX"/exe",
                         (intmax_t)p.pid);
                char *exe_ptr = _wlmaker_appicons_read_link(path);
                made_ptr = wlmaker_appicons_command_from_process(
                    buf, len, cwd_ptr, exe_ptr, getenv("PATH"));
                if (NULL != cwd_ptr) free(cwd_ptr);
                if (NULL != exe_ptr) free(exe_ptr);
            }
        }
        cmd_ptr = made_ptr;
    }
    if (NULL == cmd_ptr || '\0' == *cmd_ptr) {
        if (NULL != made_ptr) free(made_ptr);
        return NULL;
    }

    bspl_dict_t *d = bspl_dict_create();
    const char *keys[3] = { "AppID", "CommandLine", "Icon" };
    const char *values[3] = { app_id_ptr, cmd_ptr, icon_ptr };
    for (int i = 0; NULL != d && i < 3; ++i) {
        if (NULL == values[i] || '\0' == *values[i]) continue;
        bspl_string_t *s = bspl_string_create(values[i]);
        bool ok = NULL != s && bspl_dict_add(d, keys[i],
                                             bspl_object_from_string(s));
        if (NULL != s) bspl_string_unref(s);
        if (!ok) {
            bspl_dict_unref(d);
            d = NULL;
        }
    }
    if (NULL != made_ptr) free(made_ptr);
    return d;
}

/* ------------------------------------------------------------------------- */
char *wlmaker_appicons_command_from_process(
    const char *cmdline_ptr,
    size_t len,
    const char *cwd_ptr,
    const char *exe_ptr,
    const char *path_ptr)
{
    _wlmaker_appicons_buf_t buf = {};
    size_t argc = 0;
    for (size_t pos = 0; pos < len; ) {
        const char *arg_ptr = cmdline_ptr + pos;
        size_t l = strnlen(arg_ptr, len - pos);
        pos += l + 1;
        // Some programs rewrite their arguments as one, with spaces: Taken
        // as it is.
        char *made_ptr = NULL;
        if (0 == argc) {
            if ('/' == arg_ptr[0]) {
                // Absolute: As it is, if it exists.
                if (0 != access(arg_ptr, X_OK) && NULL != exe_ptr) {
                    made_ptr = logged_strdup(exe_ptr);
                }
            } else if (NULL != strchr(arg_ptr, '/')) {
                // Relative to its working directory.
                if (NULL != cwd_ptr) {
                    made_ptr = bs_strdupf("%s/%s", cwd_ptr, arg_ptr);
                    if (NULL != made_ptr && 0 != access(made_ptr, X_OK) &&
                        NULL != exe_ptr) {
                        free(made_ptr);
                        made_ptr = logged_strdup(exe_ptr);
                    }
                } else if (NULL != exe_ptr) {
                    made_ptr = logged_strdup(exe_ptr);
                }
            } else {
                // A name: Found in the PATH, or its executable (eg. a
                // script that renamed itself, as "biglinux-tts").
                char *found_ptr = _wlmaker_appicons_in_path(arg_ptr, path_ptr);
                if (NULL != found_ptr) {
                    free(found_ptr);
                } else if (NULL != exe_ptr) {
                    made_ptr = logged_strdup(exe_ptr);
                }
            }
        } else if (NULL != cwd_ptr && '-' != arg_ptr[0] &&
                   '/' != arg_ptr[0] && '\0' != arg_ptr[0] &&
                   NULL == strstr(arg_ptr, "://")) {
            // A file relative to its working directory, eg. "main.py".
            char *p = bs_strdupf("%s/%s", cwd_ptr, arg_ptr);
            struct stat st;
            if (NULL != p && 0 == stat(p, &st) && S_ISREG(st.st_mode)) {
                made_ptr = p;
            } else if (NULL != p) {
                free(p);
            }
        }
        if (0 < argc) _wlmaker_appicons_buf_add(&buf, " ", 1);
        _wlmaker_appicons_buf_add_arg(
            &buf, NULL != made_ptr ? made_ptr : arg_ptr);
        if (NULL != made_ptr) free(made_ptr);
        ++argc;
    }
    if (0 == argc && NULL != exe_ptr) {
        _wlmaker_appicons_buf_add_arg(&buf, exe_ptr);
        ++argc;
    }
    if (buf.failed || 0 == argc) {
        if (NULL != buf.s) free(buf.s);
        return NULL;
    }
    return buf.s;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Creates the argument of a menu item. */
_wlmaker_appicons_arg_t *_wlmaker_appicons_arg_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr,
    bool hidden)
{
    _wlmaker_appicons_arg_t *arg_ptr = logged_calloc(1, sizeof(*arg_ptr));
    if (NULL == arg_ptr) return NULL;
    arg_ptr->server_ptr = server_ptr;
    arg_ptr->entry_ptr = bspl_dict_ref(entry_ptr);
    arg_ptr->hidden = hidden;
    return arg_ptr;
}

/* ------------------------------------------------------------------------- */
/** Destroys the argument of a menu item. */
void _wlmaker_appicons_arg_destroy(void *ud_ptr)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    if (NULL == arg_ptr) return;
    if (NULL != arg_ptr->entry_ptr) bspl_dict_unref(arg_ptr->entry_ptr);
    free(arg_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds an item, possibly disabled. Takes `arg_ptr`. */
bool _wlmaker_appicons_menu_add(
    wlmaker_context_menu_t *cm_ptr,
    const char *text_ptr,
    bool enabled,
    wlmaker_context_menu_callback_t callback,
    _wlmaker_appicons_arg_t *arg_ptr)
{
    if (NULL == arg_ptr) return false;
    if (!wlmaker_context_menu_add_option(
            cm_ptr, NULL, text_ptr, WLMTK_MENU_ITEM_INDICATOR_NONE, false,
            callback, arg_ptr, _wlmaker_appicons_arg_destroy)) return false;
    if (!enabled) {
        wlmtk_menu_t *m_ptr = wlmaker_context_menu_menu(cm_ptr);
        size_t n = wlmtk_menu_items_size(m_ptr);
        if (0 < n) wlmtk_menu_item_set_enabled(
            wlmtk_menu_item_at(m_ptr, n - 1), false);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** Callback: "Unhide Here". */
void _wlmaker_appicons_unhide_here(void *ud_ptr)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    for (size_t i = 0; ; ++i) {
        const char *a_ptr = _wlmaker_appicons_app_id_at(arg_ptr->entry_ptr, i);
        if (NULL == a_ptr) break;
        wlmaker_dock_app_bring_here(arg_ptr->server_ptr, a_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Callback: "Hide", or "Unhide". */
void _wlmaker_appicons_hide(void *ud_ptr)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    for (size_t i = 0; ; ++i) {
        const char *a_ptr = _wlmaker_appicons_app_id_at(arg_ptr->entry_ptr, i);
        if (NULL == a_ptr) break;
        if (arg_ptr->hidden) {
            wlmaker_dock_app_unhide(arg_ptr->server_ptr, a_ptr);
        } else {
            wlmaker_dock_app_hide(arg_ptr->server_ptr, a_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/** Callback: "Launch". */
void _wlmaker_appicons_launch(void *ud_ptr)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    wlmaker_appicons_launch(arg_ptr->server_ptr, arg_ptr->entry_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Callback: "Set Icon...". Runs the `DockSettingsCommand`, as
 * `<command> appicon <AppID> [<icon>]`: It sets the application's icon in
 * WMWindowAttributes, as Window Maker's icon chooser does.
 */
void _wlmaker_appicons_set_icon(void *ud_ptr)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    wlmaker_server_t *server_ptr = arg_ptr->server_ptr;
    const char *cmd_ptr = NULL != server_ptr->config_dict_ptr ?
        bspl_dict_get_string_value(server_ptr->config_dict_ptr,
                                   "DockSettingsCommand") : NULL;
    const char *app_id_ptr = wlmaker_dock_launcher_value(
        arg_ptr->entry_ptr, "AppID");
    if (NULL == cmd_ptr || '\0' == *cmd_ptr || NULL == app_id_ptr) return;
    const char *argv[5] = { cmd_ptr, "appicon", app_id_ptr,
                            wlmaker_dock_launcher_value(
                                arg_ptr->entry_ptr, "Icon"), NULL };
    wlm_util_subprocess_monitor_run(
        server_ptr->monitor_ptr,
        bs_subprocess_create(argv[0], argv, NULL));
}

/* ------------------------------------------------------------------------- */
/**
 * Callback: "Kill". Asks first, as Window Maker: "<name> will be forcibly
 * closed. Any unsaved changes will be lost. Please confirm."
 */
void _wlmaker_appicons_kill(void *ud_ptr)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    _wlmaker_appicons_arg_t *copy_ptr = _wlmaker_appicons_arg_create(
        arg_ptr->server_ptr, arg_ptr->entry_ptr, arg_ptr->hidden);
    if (NULL == copy_ptr) return;
    const char *name_ptr = wlmaker_dock_launcher_value(
        arg_ptr->entry_ptr, "Name");
    if (NULL == name_ptr) name_ptr = wlmaker_dock_launcher_value(
        arg_ptr->entry_ptr, "AppID");
    char *message_ptr = bs_strdupf(
        // TRANSLATORS: %s is the name of the application.
        wlmaker_i18n("%s will be forcibly closed.\n"
                     "Any unsaved changes will be lost.\n"
                     "Please confirm."),
        NULL != name_ptr ? name_ptr : "");
    // The application's own icon: As its appicon shows it.
    char *path_ptr = wlmaker_dock_launcher_icon_path(
        arg_ptr->server_ptr->files_ptr,
        wlmaker_dock_launcher_value(arg_ptr->entry_ptr, "Icon"));
    cairo_surface_t *icon_ptr = wlmaker_dialog_app_icon(
        arg_ptr->server_ptr, _wlmaker_appicons_app_id_at(arg_ptr->entry_ptr, 0),
        NULL, path_ptr);
    if (NULL != path_ptr) free(path_ptr);
    wlmaker_dialog_confirm_with_icon(
        arg_ptr->server_ptr, wlmaker_i18n("Kill Application"),
        NULL != message_ptr ? message_ptr : "",
        wlmaker_i18n("Yes"), wlmaker_i18n("No"), icon_ptr,
        _wlmaker_appicons_kill_confirmed, copy_ptr,
        _wlmaker_appicons_arg_destroy);
    if (NULL != icon_ptr) cairo_surface_destroy(icon_ptr);
    if (NULL != message_ptr) free(message_ptr);
}

/* ------------------------------------------------------------------------- */
/** The answer to "Kill Application": Terminates, if confirmed. */
void _wlmaker_appicons_kill_confirmed(void *ud_ptr, bool confirmed)
{
    _wlmaker_appicons_arg_t *arg_ptr = ud_ptr;
    if (!confirmed) return;
    for (size_t i = 0; ; ++i) {
        const char *a_ptr = _wlmaker_appicons_app_id_at(arg_ptr->entry_ptr, i);
        if (NULL == a_ptr) break;
        wlmaker_dock_app_kill(arg_ptr->server_ptr, a_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** @return The number of windows of the entry's applications. */
size_t _wlmaker_appicons_windows(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *entry_ptr,
    size_t *hidden_ptr)
{
    size_t windows = 0;
    *hidden_ptr = 0;
    for (size_t i = 0; ; ++i) {
        const char *a_ptr = _wlmaker_appicons_app_id_at(entry_ptr, i);
        if (NULL == a_ptr) break;
        size_t h = 0;
        windows += wlmaker_dock_app_windows(server_ptr, a_ptr, &h);
        *hidden_ptr += h;
    }
    return windows;
}

/* ------------------------------------------------------------------------- */
/**
 * @return The entry's app ID `i`: From `AppIDs` (its windows may have
 *     several), else `AppID`. NULL past the last.
 */
const char *_wlmaker_appicons_app_id_at(bspl_dict_t *entry_ptr, size_t i)
{
    bspl_array_t *a = bspl_dict_get_array(entry_ptr, "AppIDs");
    if (NULL != a) {
        for (size_t k = 0, n = 0; k < bspl_array_size(a); ++k) {
            const char *v = bspl_array_string_value_at(a, k);
            if (NULL == v || '\0' == *v) continue;
            if (n++ == i) return v;
        }
        return NULL;
    }
    return 0 == i ? wlmaker_dock_launcher_value(entry_ptr, "AppID") : NULL;
}

/* ------------------------------------------------------------------------- */
/** Keeps the process of the first window that has one. */
void _wlmaker_appicons_find_pid(wlmtk_window_t *window_ptr, void *ud_ptr)
{
    _wlmaker_appicons_pid_t *p_ptr = ud_ptr;
    if (0 != p_ptr->pid) return;
    const wlmtk_util_client_t *c_ptr = wlmtk_window_get_client_ptr(window_ptr);
    if (NULL != c_ptr && 1 < c_ptr->pid && getpid() != c_ptr->pid) {
        p_ptr->pid = c_ptr->pid;
    }
}

/* ------------------------------------------------------------------------- */
/** Hides the window, unless of the entry's applications. */
void _wlmaker_appicons_hide_other(
    wlmtk_window_t *window_ptr,
    const char *app_id_ptr,
    void *ud_ptr)
{
    _wlmaker_appicons_others_t *o_ptr = ud_ptr;
    for (size_t i = 0; NULL != app_id_ptr; ++i) {
        const char *a_ptr = _wlmaker_appicons_app_id_at(o_ptr->entry_ptr, i);
        if (NULL == a_ptr) break;
        if (0 == strcmp(a_ptr, app_id_ptr)) return;
    }
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    if (NULL == workspace_ptr || wlmtk_window_is_minimized(window_ptr)) return;
    wlmtk_workspace_minimize_window(workspace_ptr, window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Appends `len` bytes of `s_ptr`. */
void _wlmaker_appicons_buf_add(
    _wlmaker_appicons_buf_t *buf_ptr,
    const char *s_ptr,
    size_t len)
{
    if (buf_ptr->failed) return;
    if (buf_ptr->len + len + 1 > buf_ptr->size) {
        size_t size = BS_MAX(2 * buf_ptr->size, buf_ptr->len + len + 64);
        char *n = realloc(buf_ptr->s, size);
        if (NULL == n) {
            buf_ptr->failed = true;
            return;
        }
        buf_ptr->s = n;
        buf_ptr->size = size;
    }
    memcpy(buf_ptr->s + buf_ptr->len, s_ptr, len);
    buf_ptr->len += len;
    buf_ptr->s[buf_ptr->len] = '\0';
}

/* ------------------------------------------------------------------------- */
/**
 * Appends an argument, quoted for `bs_subprocess_create_cmdline` when it
 * has more than plain characters (in double quotes, `"` and `\` escaped).
 */
void _wlmaker_appicons_buf_add_arg(
    _wlmaker_appicons_buf_t *buf_ptr,
    const char *arg_ptr)
{
    static const char *plain_ptr =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "_-./:,+@%";
    size_t len = strlen(arg_ptr);
    if (0 < len && strspn(arg_ptr, plain_ptr) == len) {
        _wlmaker_appicons_buf_add(buf_ptr, arg_ptr, len);
        return;
    }
    _wlmaker_appicons_buf_add(buf_ptr, "\"", 1);
    for (const char *c = arg_ptr; '\0' != *c; ++c) {
        if ('"' == *c || '\\' == *c) _wlmaker_appicons_buf_add(buf_ptr, "\\", 1);
        _wlmaker_appicons_buf_add(buf_ptr, c, 1);
    }
    _wlmaker_appicons_buf_add(buf_ptr, "\"", 1);
}

/* ------------------------------------------------------------------------- */
/** @return The path of program `name_ptr` in `path_ptr`, or NULL. Free. */
char *_wlmaker_appicons_in_path(const char *name_ptr, const char *path_ptr)
{
    if (NULL == path_ptr || '\0' == *name_ptr) return NULL;
    const char *p = path_ptr;
    while (true) {
        const char *e = strchr(p, ':');
        size_t l = NULL != e ? (size_t)(e - p) : strlen(p);
        if (0 < l) {
            char *c = bs_strdupf("%.*s/%s", (int)l, p, name_ptr);
            struct stat st;
            if (NULL != c && 0 == stat(c, &st) && S_ISREG(st.st_mode) &&
                0 == access(c, X_OK)) return c;
            if (NULL != c) free(c);
        }
        if (NULL == e) return NULL;
        p = e + 1;
    }
}

/* ------------------------------------------------------------------------- */
/** @return The target of the symbolic link, or NULL. Free. */
char *_wlmaker_appicons_read_link(const char *path_ptr)
{
    char buf[PATH_MAX];
    ssize_t len = readlink(path_ptr, buf, sizeof(buf) - 1);
    if (0 >= len) return NULL;
    buf[len] = '\0';
    // A deleted executable (an update replaced it): Not usable.
    if (NULL != strstr(buf, " (deleted)")) return NULL;
    return logged_strdup(buf);
}

/* == Unit tests =========================================================== */

static void test_command(bs_test_t *test_ptr);
static void test_load(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmaker_appicons_test_cases[] = {
    { true, "command", test_command },
    { true, "load", test_load },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_appicons_test_set = BS_TEST_SET(
    true, "appicons", _wlmaker_appicons_test_cases);

/* ------------------------------------------------------------------------- */
/** Command lines from processes. */
void test_command(bs_test_t *test_ptr)
{
    char dir[] = "/tmp/wlmaker-appicons-XXXXXX";
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, mkdtemp(dir));
    char *script_ptr = bs_strdupf("%s/main.py", dir);
    char *prog_ptr = bs_strdupf("%s/run me", dir);
    FILE *f = fopen(script_ptr, "w");
    if (NULL != f) fclose(f);
    f = fopen(prog_ptr, "w");
    if (NULL != f) fclose(f);
    chmod(prog_ptr, 0700);

    // A program in the PATH, with options: As it is.
    static const char a[] = "sh\0-c\0echo $HOME \"x\"\0";
    char *c = wlmaker_appicons_command_from_process(
        a, sizeof(a) - 1, dir, "/usr/bin/bash", "/usr/bin:/bin");
    BS_TEST_VERIFY_STREQ(test_ptr, "sh -c \"echo $HOME \\\"x\\\"\"", c);
    free(c);

    // A script that renamed itself, with its file relative: Absolute.
    static const char b[] = "renamed-tts-xyz\0main.py\0--flag\0";
    c = wlmaker_appicons_command_from_process(
        b, sizeof(b) - 1, dir, "/usr/bin/python3.13", "/usr/bin:/bin");
    char *expected_ptr = bs_strdupf("/usr/bin/python3.13 %s --flag",
                                    script_ptr);
    BS_TEST_VERIFY_STREQ(test_ptr, expected_ptr, c);
    free(expected_ptr);
    free(c);

    // A relative program: Absolute, quoted.
    static const char d[] = "./run me\0https://x.org/a\0";
    c = wlmaker_appicons_command_from_process(
        d, sizeof(d) - 1, dir, NULL, NULL);
    expected_ptr = bs_strdupf("\"%s/./run me\" https://x.org/a", dir);
    BS_TEST_VERIFY_STREQ(test_ptr, expected_ptr, c);
    free(expected_ptr);
    free(c);

    // Nothing, but the executable.
    c = wlmaker_appicons_command_from_process("", 0, NULL, "/usr/bin/x", NULL);
    BS_TEST_VERIFY_STREQ(test_ptr, "/usr/bin/x", c);
    free(c);
    BS_TEST_VERIFY_EQ(
        test_ptr, NULL,
        wlmaker_appicons_command_from_process("", 0, NULL, NULL, NULL));

    unlink(script_ptr);
    unlink(prog_ptr);
    rmdir(dir);
    free(script_ptr);
    free(prog_ptr);
}

/* ------------------------------------------------------------------------- */
/** Loads the published row; entries by index. */
void test_load(bs_test_t *test_ptr)
{
    char fname[] = "/tmp/wlmaker-appicons-XXXXXX";
    int fd = mkstemp(fname);
    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, 0 <= fd);
    static const char t[] =
        "{ Appicons = ( { Name = \"Editor\"; AppID = \"org.a\"; "
        "AppIDs = (\"org.a\", \"org.b\"); }, x ); }";
    BS_TEST_VERIFY_EQ(test_ptr, (ssize_t)sizeof(t) - 1,
                      write(fd, t, sizeof(t) - 1));
    close(fd);
    bspl_array_t *a = wlmaker_appicons_load(fname);
    unlink(fname);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, a);
    BS_TEST_VERIFY_EQ(test_ptr, 2, bspl_array_size(a));
    bspl_dict_t *e = wlmaker_appicons_at(a, 0);
    BS_TEST_VERIFY_STREQ(test_ptr, "Editor",
                         wlmaker_dock_launcher_value(e, "Name"));
    BS_TEST_VERIFY_STREQ(test_ptr, "org.b", _wlmaker_appicons_app_id_at(e, 1));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, _wlmaker_appicons_app_id_at(e, 2));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlmaker_appicons_at(a, 1));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlmaker_appicons_at(a, 2));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlmaker_appicons_at(a, -1));
    bspl_array_unref(a);
    BS_TEST_VERIFY_EQ(test_ptr, NULL,
                      wlmaker_appicons_load("/nonexistent/appicons"));
}

/* == End of appicons.c ==================================================== */
