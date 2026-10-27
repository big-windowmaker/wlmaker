/* ========================================================================= */
/**
 * @file dock_menu.c
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

#include "dock_menu.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "action.h"
#include "clip.h"
#include "dialog.h"
#include "config.h"
#include "dock_launchers.h"
#include "dock_position.h"
#include "i18n.h"
#include "util/subprocess_monitor.h"

/* == Declarations ========================================================= */

/** Argument of the menu's callbacks. */
typedef struct {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** Whether for the Dock (else the Clip). */
    bool                      dock;
    /** Index of the launcher, -1 for the main tile or a dock-app. */
    int                       index;
    /** Application ID. May be NULL. */
    char                      *app_id_ptr;
    /** Command. May be NULL. */
    char                      *command_ptr;
    /** Dock-app's name. May be NULL. */
    char                      *dockapp_name_ptr;
    /** Dock-app's process. */
    pid_t                     pid;
    /** A level, or corners to flip to. */
    uint32_t                  value;
} _wlmaker_dock_menu_arg_t;

static _wlmaker_dock_menu_arg_t *_wlmaker_dock_menu_arg_create(
    wlmaker_server_t *server_ptr,
    const wlmaker_dock_menu_icon_t *icon_ptr,
    bool dock,
    uint32_t value);
static void _wlmaker_dock_menu_arg_destroy(void *ud_ptr);
static bool _wlmaker_dock_menu_add(
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *parent_ptr,
    const char *text_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    bool enabled,
    wlmaker_context_menu_callback_t callback,
    _wlmaker_dock_menu_arg_t *arg_ptr);
static const char *_wlmaker_dock_menu_settings_command(
    wlmaker_server_t *server_ptr);

static void _wlmaker_dock_menu_level(void *ud_ptr);
static void _wlmaker_dock_menu_clip_keep_on_top(void *ud_ptr);
static void _wlmaker_dock_menu_clip_autoraise(void *ud_ptr);
static void _wlmaker_dock_menu_launch(void *ud_ptr);
static void _wlmaker_dock_menu_bring_here(void *ud_ptr);
static void _wlmaker_dock_menu_hide(void *ud_ptr);
static void _wlmaker_dock_menu_settings(void *ud_ptr);
static void _wlmaker_dock_menu_kill(void *ud_ptr);
static void _wlmaker_dock_menu_remove(void *ud_ptr);
static void _wlmaker_dock_menu_flip(void *ud_ptr);
static void _wlmaker_dock_menu_restore(void *ud_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
void wlmaker_dock_menu_open_launcher(
    wlmaker_server_t *server_ptr,
    bool dock,
    int index)
{
    if (NULL == server_ptr->style_ptr ||
        NULL == server_ptr->style_ptr->menu_style_ptr) return;
    bspl_array_t *a = wlmaker_dock_launchers_load(
        server_ptr, dock ? "Dock" : "Clip");
    bspl_dict_t *dict_ptr = NULL;
    if (NULL != a && 0 <= index && (size_t)index < bspl_array_size(a)) {
        dict_ptr = bspl_dict_from_object(bspl_array_at(a, index));
    }
    if (0 <= index && NULL == dict_ptr) {
        if (NULL != a) bspl_array_unref(a);
        return;
    }
    wlmaker_dock_menu_icon_t icon = {
        .app_id_ptr = wlmaker_dock_launcher_value(dict_ptr, "AppID"),
        .command_ptr = wlmaker_dock_launcher_value(dict_ptr, "CommandLine"),
        .dock = dock,
        .launcher_index = index };

    const char *title_ptr = icon.app_id_ptr;
    if (NULL == title_ptr || '\0' == *title_ptr) {
        title_ptr = dock ? "Dock" : "Clip";
    }
    wlmaker_context_menu_t *cm_ptr = wlmaker_context_menu_create(
        server_ptr, server_ptr, title_ptr);
    if (NULL == cm_ptr) {
        if (NULL != a) bspl_array_unref(a);
        return;
    }

    bool rv = wlmaker_dock_menu_add_dock_items(server_ptr, cm_ptr, dock);
    // Window Maker's Clip: Its icons can be removed from the menu.
    if (rv && !dock) {
        rv = _wlmaker_dock_menu_add(
            cm_ptr, NULL, wlmaker_i18n("Remove Icon"),
            WLMTK_MENU_ITEM_INDICATOR_NONE, false,
            !wlmaker_dock_launcher_flag(dict_ptr, "Lock"),
            _wlmaker_dock_menu_remove,
            _wlmaker_dock_menu_arg_create(server_ptr, &icon, dock, 0));
    }
    rv = rv && wlmaker_dock_menu_add_icon_items(server_ptr, cm_ptr, &icon);

    // Beyond Window Maker's: Moving the Dock, restoring its launchers.
    if (rv && dock && 0 > index) {
        wlmaker_dock_column_t column, clip;
        if (wlmaker_clip_column(server_ptr, true, &column)) {
            uint32_t corner = wlmaker_dock_place_corner(column.place);
            uint32_t clip_corner = 0;
            if (wlmaker_clip_column(server_ptr, false, &clip)) {
                clip_corner = wlmaker_dock_place_corner(clip.place);
            }
            static const struct { uint32_t flip; } flips[] = {
                { WLR_EDGE_LEFT | WLR_EDGE_RIGHT },
                { WLR_EDGE_TOP | WLR_EDGE_BOTTOM } };
            for (size_t i = 0; rv && i < 2; ++i) {
                uint32_t to = corner ^ flips[i].flip;
                if (to == clip_corner) continue;
                const char *t_ptr;
                if (0 == i) {
                    t_ptr = (corner & WLR_EDGE_LEFT) ?
                        "Move to Right Side" : "Move to Left Side";
                } else {
                    t_ptr = (corner & WLR_EDGE_TOP) ?
                        "Move to Bottom" : "Move to Top";
                }
                rv = _wlmaker_dock_menu_add(
                    cm_ptr, NULL, wlmaker_i18n(t_ptr),
                    WLMTK_MENU_ITEM_INDICATOR_NONE, false, true,
                    _wlmaker_dock_menu_flip,
                    _wlmaker_dock_menu_arg_create(
                        server_ptr, &icon, true, flips[i].flip));
            }
        }
        bspl_dict_t *d = wlmaker_state_load_defaults(server_ptr->files_ptr, NULL);
        bspl_array_t *defaults_ptr = bspl_dict_get_array(
            bspl_dict_get_dict(d, "Dock"), "Launchers");
        if (rv && NULL != defaults_ptr && 0 < bspl_array_size(defaults_ptr)) {
            rv = _wlmaker_dock_menu_add(
                cm_ptr, NULL, wlmaker_i18n("Restore Default Launchers"),
                WLMTK_MENU_ITEM_INDICATOR_NONE, false, true,
                _wlmaker_dock_menu_restore,
                _wlmaker_dock_menu_arg_create(server_ptr, &icon, true, 0));
        }
        if (NULL != d) bspl_dict_unref(d);
    }
    if (!dock && rv) rv = wlmaker_clip_add_move_items(server_ptr, cm_ptr);
    if (NULL != a) bspl_array_unref(a);

    if (!rv) {
        wlmaker_context_menu_destroy(cm_ptr);
        return;
    }
    wlmaker_context_menu_open(cm_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_menu_add_dock_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    bool dock)
{
    wlmaker_dock_menu_icon_t icon = { .dock = dock, .launcher_index = -1 };
    wlmaker_dock_level_t level = wlmaker_clip_get_level(server_ptr, dock);
    if (dock) {
        // Window Maker's "Dock position": The Dock's stacking.
        wlmtk_menu_t *menu_ptr = wlmaker_context_menu_add_submenu(
            context_menu_ptr, NULL, wlmaker_i18n("Dock position"));
        if (NULL == menu_ptr) return false;
        static const struct {
            const char *text_ptr;
            wlmaker_dock_level_t level;
        } levels[] = {
            { "Normal", WLMAKER_DOCK_LEVEL_NORMAL },
            { "Auto raise & lower", WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER },
            { "Keep on Top", WLMAKER_DOCK_LEVEL_KEEP_ON_TOP },
        };
        for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
            if (!_wlmaker_dock_menu_add(
                    context_menu_ptr, menu_ptr,
                    wlmaker_i18n(levels[i].text_ptr),
                    WLMTK_MENU_ITEM_INDICATOR_RADIO,
                    level == levels[i].level, true,
                    _wlmaker_dock_menu_level,
                    _wlmaker_dock_menu_arg_create(
                        server_ptr, &icon, true, levels[i].level))) {
                return false;
            }
        }
        // Drawers are not available (yet): Shown, as on Window Maker, but
        // disabled.
        return _wlmaker_dock_menu_add(
            context_menu_ptr, NULL, wlmaker_i18n("Add a drawer"),
            WLMTK_MENU_ITEM_INDICATOR_NONE, false, false, NULL, NULL);
    }

    // Window Maker's "Clip Options": Those that apply.
    wlmtk_menu_t *menu_ptr = wlmaker_context_menu_add_submenu(
        context_menu_ptr, NULL, wlmaker_i18n("Clip Options"));
    if (NULL == menu_ptr) return false;
    return _wlmaker_dock_menu_add(
        context_menu_ptr, menu_ptr, wlmaker_i18n("Keep on Top"),
        WLMTK_MENU_ITEM_INDICATOR_CHECK,
        WLMAKER_DOCK_LEVEL_KEEP_ON_TOP == level, true,
        _wlmaker_dock_menu_clip_keep_on_top,
        _wlmaker_dock_menu_arg_create(server_ptr, &icon, false, 0)) &&
        _wlmaker_dock_menu_add(
            context_menu_ptr, menu_ptr, wlmaker_i18n("Autoraise"),
            WLMTK_MENU_ITEM_INDICATOR_CHECK,
            WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER == level, true,
            _wlmaker_dock_menu_clip_autoraise,
            _wlmaker_dock_menu_arg_create(server_ptr, &icon, false, 0));
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_menu_add_icon_items(
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    const wlmaker_dock_menu_icon_t *icon_ptr)
{
    size_t hidden = 0;
    size_t windows = wlmaker_dock_app_windows(
        server_ptr, icon_ptr->app_id_ptr, &hidden);
    bool all_hidden = 0 < windows && hidden == windows;
    bool has_command = NULL != icon_ptr->command_ptr &&
        '\0' != *icon_ptr->command_ptr;
    bool settings = NULL != _wlmaker_dock_menu_settings_command(server_ptr) &&
        (0 <= icon_ptr->launcher_index || NULL != icon_ptr->dockapp_name_ptr);
    bool killable = 0 < windows || 1 < icon_ptr->pid;

    return _wlmaker_dock_menu_add(
        context_menu_ptr, NULL, wlmaker_i18n("Launch"),
        WLMTK_MENU_ITEM_INDICATOR_NONE, false, has_command,
        _wlmaker_dock_menu_launch,
        _wlmaker_dock_menu_arg_create(server_ptr, icon_ptr, icon_ptr->dock, 0)) &&
        _wlmaker_dock_menu_add(
            context_menu_ptr, NULL,
            wlmaker_i18n(all_hidden ? "Unhide Here" : "Bring Here"),
            WLMTK_MENU_ITEM_INDICATOR_NONE, false, 0 < windows,
            _wlmaker_dock_menu_bring_here,
            _wlmaker_dock_menu_arg_create(
                server_ptr, icon_ptr, icon_ptr->dock, 0)) &&
        _wlmaker_dock_menu_add(
            context_menu_ptr, NULL,
            wlmaker_i18n(all_hidden ? "Unhide" : "Hide"),
            WLMTK_MENU_ITEM_INDICATOR_NONE, false, 0 < windows,
            _wlmaker_dock_menu_hide,
            _wlmaker_dock_menu_arg_create(
                server_ptr, icon_ptr, icon_ptr->dock, all_hidden)) &&
        (NULL != icon_ptr->prefs_ptr &&
         wlmaker_dockapp_prefs_has_preferences(icon_ptr->prefs_ptr) ?
         // A dock-app's own preferences: Its settings.
         wlmaker_dockapp_prefs_add_preferences_titled(
             icon_ptr->prefs_ptr, context_menu_ptr,
             wlmaker_i18n("Settings...")) :
         _wlmaker_dock_menu_add(
             context_menu_ptr, NULL, wlmaker_i18n("Settings..."),
             WLMTK_MENU_ITEM_INDICATOR_NONE, false, settings,
             _wlmaker_dock_menu_settings,
             _wlmaker_dock_menu_arg_create(
                 server_ptr, icon_ptr, icon_ptr->dock, 0))) &&
        _wlmaker_dock_menu_add(
            context_menu_ptr, NULL, wlmaker_i18n("Kill"),
            WLMTK_MENU_ITEM_INDICATOR_NONE, false, killable,
            _wlmaker_dock_menu_kill,
            _wlmaker_dock_menu_arg_create(
                server_ptr, icon_ptr, icon_ptr->dock, 0));
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Creates the argument for a callback. */
_wlmaker_dock_menu_arg_t *_wlmaker_dock_menu_arg_create(
    wlmaker_server_t *server_ptr,
    const wlmaker_dock_menu_icon_t *icon_ptr,
    bool dock,
    uint32_t value)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = logged_calloc(1, sizeof(*arg_ptr));
    if (NULL == arg_ptr) return NULL;
    arg_ptr->server_ptr = server_ptr;
    arg_ptr->dock = dock;
    arg_ptr->value = value;
    arg_ptr->index = icon_ptr->launcher_index;
    arg_ptr->pid = icon_ptr->pid;
    if (NULL != icon_ptr->app_id_ptr) {
        arg_ptr->app_id_ptr = logged_strdup(icon_ptr->app_id_ptr);
    }
    if (NULL != icon_ptr->command_ptr) {
        arg_ptr->command_ptr = logged_strdup(icon_ptr->command_ptr);
    }
    if (NULL != icon_ptr->dockapp_name_ptr) {
        arg_ptr->dockapp_name_ptr = logged_strdup(icon_ptr->dockapp_name_ptr);
    }
    return arg_ptr;
}

/* ------------------------------------------------------------------------- */
/** Destroys the argument of a callback. */
void _wlmaker_dock_menu_arg_destroy(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    if (NULL == arg_ptr) return;
    if (NULL != arg_ptr->app_id_ptr) free(arg_ptr->app_id_ptr);
    if (NULL != arg_ptr->command_ptr) free(arg_ptr->command_ptr);
    if (NULL != arg_ptr->dockapp_name_ptr) free(arg_ptr->dockapp_name_ptr);
    free(arg_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds an item, possibly disabled. Takes ownership of `arg_ptr`. */
bool _wlmaker_dock_menu_add(
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *parent_ptr,
    const char *text_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    bool enabled,
    wlmaker_context_menu_callback_t callback,
    _wlmaker_dock_menu_arg_t *arg_ptr)
{
    if (NULL != callback && NULL == arg_ptr) return false;
    if (!wlmaker_context_menu_add_option(
            context_menu_ptr, parent_ptr, text_ptr, indicator, checked,
            callback, arg_ptr, _wlmaker_dock_menu_arg_destroy)) return false;
    if (!enabled) {
        wlmtk_menu_t *m_ptr = NULL != parent_ptr ? parent_ptr :
            wlmaker_context_menu_menu(context_menu_ptr);
        size_t n = wlmtk_menu_items_size(m_ptr);
        if (0 < n) wlmtk_menu_item_set_enabled(wlmtk_menu_item_at(m_ptr, n - 1),
                                               false);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** @return The configured `DockSettingsCommand`, or NULL. */
const char *_wlmaker_dock_menu_settings_command(wlmaker_server_t *server_ptr)
{
    if (NULL == server_ptr->config_dict_ptr) return NULL;
    const char *c = bspl_dict_get_string_value(
        server_ptr->config_dict_ptr, "DockSettingsCommand");
    if (NULL == c || '\0' == *c) return NULL;
    return c;
}

/* ------------------------------------------------------------------------- */
/** Callback: Sets the level ("Dock position"). */
void _wlmaker_dock_menu_level(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    wlmaker_clip_set_level(arg_ptr->server_ptr, arg_ptr->dock, arg_ptr->value);
}

/* ------------------------------------------------------------------------- */
/** Callback: Toggles the Clip's "Keep on Top". */
void _wlmaker_dock_menu_clip_keep_on_top(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    bool on = WLMAKER_DOCK_LEVEL_KEEP_ON_TOP ==
        wlmaker_clip_get_level(arg_ptr->server_ptr, false);
    wlmaker_clip_set_level(arg_ptr->server_ptr, false,
                           on ? WLMAKER_DOCK_LEVEL_NORMAL :
                           WLMAKER_DOCK_LEVEL_KEEP_ON_TOP);
}

/* ------------------------------------------------------------------------- */
/** Callback: Toggles the Clip's "Autoraise". */
void _wlmaker_dock_menu_clip_autoraise(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    bool on = WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER ==
        wlmaker_clip_get_level(arg_ptr->server_ptr, false);
    wlmaker_clip_set_level(arg_ptr->server_ptr, false,
                           on ? WLMAKER_DOCK_LEVEL_NORMAL :
                           WLMAKER_DOCK_LEVEL_AUTO_RAISE_LOWER);
}

/* ------------------------------------------------------------------------- */
/** Callback: Launches the command ("Launch"). */
void _wlmaker_dock_menu_launch(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    if (NULL == arg_ptr->command_ptr) return;
    wlmaker_action_execute(arg_ptr->server_ptr, WLMAKER_ACTION_EXECUTE,
                           arg_ptr->command_ptr);
}

/* ------------------------------------------------------------------------- */
/** Callback: "Bring Here", or "Unhide Here". */
void _wlmaker_dock_menu_bring_here(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    wlmaker_dock_app_bring_here(arg_ptr->server_ptr, arg_ptr->app_id_ptr);
}

/* ------------------------------------------------------------------------- */
/** Callback: "Hide", or "Unhide". */
void _wlmaker_dock_menu_hide(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    if (arg_ptr->value) {
        wlmaker_dock_app_unhide(arg_ptr->server_ptr, arg_ptr->app_id_ptr);
    } else {
        wlmaker_dock_app_hide(arg_ptr->server_ptr, arg_ptr->app_id_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Callback: "Settings...". Runs the `DockSettingsCommand`, as
 * `<command> launcher Dock|Clip <index>` or `<command> dockapp <name>`.
 */
void _wlmaker_dock_menu_settings(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    const char *cmd_ptr = _wlmaker_dock_menu_settings_command(
        arg_ptr->server_ptr);
    if (NULL == cmd_ptr) return;
    char index[16];
    snprintf(index, sizeof(index), "%d", arg_ptr->index);
    const char *argv[5] = { cmd_ptr, NULL, NULL, NULL, NULL };
    if (NULL != arg_ptr->dockapp_name_ptr) {
        argv[1] = "dockapp";
        argv[2] = arg_ptr->dockapp_name_ptr;
    } else if (0 <= arg_ptr->index) {
        argv[1] = "launcher";
        argv[2] = arg_ptr->dock ? "Dock" : "Clip";
        argv[3] = index;
    } else {
        return;
    }
    wlm_util_subprocess_monitor_run(
        arg_ptr->server_ptr->monitor_ptr,
        bs_subprocess_create(argv[0], argv, NULL));
}

/* ------------------------------------------------------------------------- */
/** The answer to "Kill Application": Terminates, if confirmed. */
static void _wlmaker_dock_menu_kill_confirmed(void *ud_ptr, bool confirmed)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    if (!confirmed) return;
    if (1 < arg_ptr->pid && getpid() != arg_ptr->pid) {
        bs_log(BS_INFO, "Terminating dock-app with PID %"PRIdMAX,
               (intmax_t)arg_ptr->pid);
        if (0 != kill(arg_ptr->pid, SIGTERM)) {
            bs_log(BS_WARNING | BS_ERRNO, "Failed kill(%"PRIdMAX", SIGTERM)",
                   (intmax_t)arg_ptr->pid);
        }
        return;
    }
    wlmaker_dock_app_kill(arg_ptr->server_ptr, arg_ptr->app_id_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Callback: "Kill". Asks first, as Window Maker: "<name> will be forcibly
 * closed. Any unsaved changes will be lost. Please confirm." Then terminates
 * the application's windows' processes, or the dock-app.
 */
void _wlmaker_dock_menu_kill(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    _wlmaker_dock_menu_arg_t *copy_ptr = logged_calloc(1, sizeof(*copy_ptr));
    if (NULL == copy_ptr) return;
    *copy_ptr = *arg_ptr;
    copy_ptr->app_id_ptr = NULL != arg_ptr->app_id_ptr ?
        logged_strdup(arg_ptr->app_id_ptr) : NULL;
    copy_ptr->command_ptr = NULL;
    copy_ptr->dockapp_name_ptr = NULL != arg_ptr->dockapp_name_ptr ?
        logged_strdup(arg_ptr->dockapp_name_ptr) : NULL;

    const char *name_ptr = NULL != arg_ptr->dockapp_name_ptr ?
        arg_ptr->dockapp_name_ptr : arg_ptr->app_id_ptr;
    char *message_ptr = bs_strdupf(
        "%s%s", NULL != name_ptr ? name_ptr : "",
        wlmaker_i18n(" will be forcibly closed.\n"
                     "Any unsaved changes will be lost.\n"
                     "Please confirm."));
    char *icon_ptr = wlmaker_dock_menu_logo_path(arg_ptr->server_ptr);
    wlmaker_dialog_confirm(
        arg_ptr->server_ptr, wlmaker_i18n("Kill Application"),
        NULL != message_ptr ? message_ptr : "",
        wlmaker_i18n("Yes"), wlmaker_i18n("No"), icon_ptr,
        _wlmaker_dock_menu_kill_confirmed, copy_ptr,
        _wlmaker_dock_menu_arg_destroy);
    if (NULL != icon_ptr) free(icon_ptr);
    if (NULL != message_ptr) free(message_ptr);
}

/* ------------------------------------------------------------------------- */
char *wlmaker_dock_menu_logo_path(wlmaker_server_t *server_ptr)
{
    // The Dock's main tile: Window Maker's dialogs show its logo.
    bspl_dict_t *d = wlmaker_state_load(server_ptr->files_ptr,
                                        server_ptr->state_fname_ptr);
    char *p = wlmaker_dock_launcher_icon_path(
        server_ptr->files_ptr,
        wlmaker_dock_launcher_value(bspl_dict_get_dict(d, "Dock"), "Icon"));
    if (NULL != d) bspl_dict_unref(d);
    if (NULL == p) {
        p = wlmaker_dock_launcher_icon_path(server_ptr->files_ptr,
                                            "dock-56x56.png");
    }
    return p;
}

/* ------------------------------------------------------------------------- */
/** Callback: "Remove Icon": Removes the launcher from the Clip. */
void _wlmaker_dock_menu_remove(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    const char *section_ptr = arg_ptr->dock ? "Dock" : "Clip";
    bspl_array_t *a = wlmaker_dock_launchers_load(
        arg_ptr->server_ptr, section_ptr);
    bspl_array_t *n = bspl_array_create();
    bool rv = NULL != a && NULL != n;
    for (size_t i = 0; rv && i < bspl_array_size(a); ++i) {
        if ((int)i == arg_ptr->index) continue;
        rv = bspl_array_push_back(n, bspl_array_at(a, i));
    }
    if (rv) wlmaker_dock_launchers_store(arg_ptr->server_ptr, section_ptr, n);
    if (NULL != n) bspl_array_unref(n);
    if (NULL != a) bspl_array_unref(a);
}

/* ------------------------------------------------------------------------- */
/** Callback: Moves the Dock to the other side or end. */
void _wlmaker_dock_menu_flip(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    wlmaker_dock_column_t column;
    if (!wlmaker_clip_column(arg_ptr->server_ptr, true, &column)) return;
    wlmaker_dock_place_t place = wlmaker_dock_place_from_corner(
        wlmaker_dock_place_corner(column.place) ^ arg_ptr->value,
        wlmaker_dock_place_vertical(column.place));
    wlmaker_clip_move_dock(arg_ptr->server_ptr, "Dock", place);
}

/* ------------------------------------------------------------------------- */
/** Callback: Restores the Dock's default launchers. */
void _wlmaker_dock_menu_restore(void *ud_ptr)
{
    _wlmaker_dock_menu_arg_t *arg_ptr = ud_ptr;
    bspl_dict_t *d = wlmaker_state_load_defaults(
        arg_ptr->server_ptr->files_ptr, NULL);
    bspl_array_t *a = bspl_dict_get_array(
        bspl_dict_get_dict(d, "Dock"), "Launchers");
    if (NULL != a) wlmaker_dock_launchers_store(arg_ptr->server_ptr, "Dock", a);
    if (NULL != d) bspl_dict_unref(d);
}

/* == Unit tests =========================================================== */

static void test_settings_command(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_dock_menu_test_cases[] = {
    { true, "settings_command", test_settings_command },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dock_menu_test_set = BS_TEST_SET(
    true, "dock_menu", _wlmaker_dock_menu_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests reading the settings command from the configuration. */
void test_settings_command(bs_test_t *test_ptr)
{
    wlmaker_server_t server = {};
    BS_TEST_VERIFY_EQ(test_ptr, NULL,
                      _wlmaker_dock_menu_settings_command(&server));
    bspl_object_t *o = bspl_create_object_from_plist_string(
        "{DockSettingsCommand = \"/usr/bin/settings\";}");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    server.config_dict_ptr = bspl_dict_from_object(o);
    BS_TEST_VERIFY_STREQ(test_ptr, "/usr/bin/settings",
                         _wlmaker_dock_menu_settings_command(&server));
    bspl_object_unref(o);

    // The argument holds copies.
    wlmaker_dock_menu_icon_t icon = {
        .app_id_ptr = "firefox", .command_ptr = "firefox", .dock = true,
        .launcher_index = 2 };
    _wlmaker_dock_menu_arg_t *arg_ptr = _wlmaker_dock_menu_arg_create(
        &server, &icon, true, 0);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, arg_ptr);
    BS_TEST_VERIFY_STREQ(test_ptr, "firefox", arg_ptr->app_id_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 2, arg_ptr->index);
    _wlmaker_dock_menu_arg_destroy(arg_ptr);
}

/* == End of dock_menu.c =================================================== */
