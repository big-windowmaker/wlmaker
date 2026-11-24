/* ========================================================================= */
/**
 * @file force_close.c
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
 *
 * Closing a window's client by force, as xkill: The `ForceCloseSelect`
 * action turns the pointer into a skull (the `pirate` image of a cursor
 * theme). A click with the left button on a window ends the client of that
 * window -- not processes found by name, so the other windows of
 * multi-process applications (browsers) are not touched unless they are
 * the same client. Escape, any other button or the action again cancel.
 *
 * Wayland clients: SIGKILL to the client's process (from the socket's
 * credentials), and the client's connection is destroyed. XWayland
 * windows: The X client is killed (XKillClient, as xkill does), and its
 * process (as the X server knows it, through XRes) gets SIGKILL. The
 * compositor, the X server, system and session services, the Big
 * WindowMaker components, the Dock and the dock-apps are refused.
 */

#include "force_close.h"

#include <ctype.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#if defined(WLMAKER_HAVE_XWAYLAND)
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#endif  // defined(WLMAKER_HAVE_XWAYLAND)
#undef WLR_USE_UNSTABLE

#include "dialog.h"
#include "i18n.h"
#include "input/manager.h"
#include "toolkit/toolkit.h"
#include "xwl.h"

/* == Declarations ========================================================= */

/** Decoded 'ForceClose' configuration. */
typedef struct {
    /** Cursor theme with the skull. Empty: The current theme. */
    char                      *cursor_theme_ptr;
    /** Name of the skull image in the theme. */
    char                      *cursor_name_ptr;
} _wlmaker_force_close_config_t;

/** Kind of client to close. */
typedef enum {
    _WLMAKER_FORCE_CLOSE_NONE,
    _WLMAKER_FORCE_CLOSE_WAYLAND,
    _WLMAKER_FORCE_CLOSE_XWAYLAND,
} _wlmaker_force_close_kind_t;

/** Maximum number of buttons whose release is swallowed. */
#define _WLMAKER_FORCE_CLOSE_MAX_BUTTONS 8

/** State of selecting a window to close by force. */
struct _wlmaker_force_close_t {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** Configuration. */
    _wlmaker_force_close_config_t config;
    /** Whether selecting a window. */
    bool                      active;

    /** Buttons pressed while selecting: Their release is swallowed, too. */
    uint32_t                  buttons[_WLMAKER_FORCE_CLOSE_MAX_BUTTONS];
    /** Number of entries in @ref _wlmaker_force_close_t::buttons. */
    size_t                    buttons_pressed;

    /** The client to close, from the idle callback. */
    _wlmaker_force_close_kind_t kind;
    /** Its process; 0 if unknown. */
    pid_t                     pid;
    /** The Wayland client. NULL once destroyed. */
    struct wl_client          *wl_client_ptr;
    /** Listener for the destruction of @ref _wlmaker_force_close_t::wl_client_ptr. */
    struct wl_listener        client_destroy_listener;
    /** The X11 window. */
    uint32_t                  x11_window;
    /** Idle callback: Closes outside of the input handlers. */
    struct wl_event_source    *idle_ptr;
};

static bool _wlmaker_force_close_configure(
    wlmaker_force_close_t *fc_ptr,
    bspl_dict_t *config_dict_ptr);
static void _wlmaker_force_close_begin(wlmaker_force_close_t *fc_ptr);
static void _wlmaker_force_close_end(wlmaker_force_close_t *fc_ptr);
static bool _wlmaker_force_close_key_grab(
    xkb_keysym_t keysym,
    uint32_t modifiers,
    void *ud_ptr);
static bool _wlmaker_force_close_button_filter(
    void *ud_ptr,
    const struct wlr_pointer_button_event *wlr_pointer_button_event_ptr,
    uint32_t modifiers);
static void _wlmaker_force_close_select(wlmaker_force_close_t *fc_ptr);
static wlmtk_window_t *_wlmaker_force_close_window_at(
    wlmaker_server_t *server_ptr,
    struct wlr_scene_node *node_ptr);
static struct wlr_surface *_wlmaker_force_close_surface_of(
    wlmtk_window_t *window_ptr,
    struct wlr_scene_node *node_ptr);
static void _wlmaker_force_close_notice(
    wlmaker_force_close_t *fc_ptr,
    wlmtk_window_t *window_ptr,
    const char *message_ptr);
static void _wlmaker_force_close_forget_client(wlmaker_force_close_t *fc_ptr);
static void _wlmaker_force_close_handle_client_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_force_close_idle(void *data_ptr);
static bool _wlmaker_force_close_protected_pid(pid_t pid);
static bool _wlmaker_force_close_protected_name(const char *name_ptr);

/* == Data ================================================================= */

/** Descriptor of the 'ForceClose' dictionary. */
static const bspl_desc_t _wlmaker_force_close_config_desc[] = {
    BSPL_DESC_STRING("CursorTheme", false, _wlmaker_force_close_config_t,
                     cursor_theme_ptr, cursor_theme_ptr, ""),
    BSPL_DESC_STRING("Cursor", false, _wlmaker_force_close_config_t,
                     cursor_name_ptr, cursor_name_ptr, "pirate"),
    BSPL_DESC_SENTINEL()
};

/** Session processes, by exact name (comm, executable or script). */
static const char *_wlmaker_force_close_protected_names[] = {
    "wlmaker", "Xwayland", "Xorg", "X", "init", "dbus-daemon",
    "dbus-broker", "dbus-broker-lau", "dbus-broker-launch", "pipewire",
    "pipewire-pulse", "wireplumber", "pulseaudio", "lightdm", "sddm", "gdm",
    "login", "agetty", "sshd", "polkitd", "gnome-keyring-d",
    "gnome-keyring-daemon", "at-spi-bus-laun", "at-spi2-registr",
    "xsettingsd", "dunst", "mako", "swaybg", "swaylock", "swayidle",
    "xss-lock", "xsecurelock", "startbigwm", "startbigwm-wayl",
    "startbigwm-wayland", NULL
};

/** Session processes, by prefix: Components, services, Dock, dock-apps. */
static const char *_wlmaker_force_close_protected_prefixes[] = {
    "big-wm",            // Big WindowMaker components.
    "wlm",               // wlmaker's Dock (wlmdock) and dock-apps (wlmclock).
    "wm",                // Window Maker dock-apps (wmclock, wmcliphist...).
    "systemd",           // systemd and its helpers.
    "dbus-",             // D-Bus.
    "pipewire",          // PipeWire.
    "xdg-desktop-por",   // Portals (comm is cut at 15 characters).
    "xdg-document-po",
    "xdg-permission-",
    NULL
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_force_close_t *wlmaker_force_close_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr)
{
    wlmaker_force_close_t *fc_ptr = logged_calloc(1, sizeof(*fc_ptr));
    if (NULL == fc_ptr) return NULL;
    fc_ptr->server_ptr = server_ptr;
    if (!_wlmaker_force_close_configure(fc_ptr, config_dict_ptr)) {
        wlmaker_force_close_destroy(fc_ptr);
        return NULL;
    }
    return fc_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_force_close_destroy(wlmaker_force_close_t *fc_ptr)
{
    if (fc_ptr->active) _wlmaker_force_close_end(fc_ptr);
    if (NULL != fc_ptr->server_ptr->input_manager_ptr) {
        wlmim_set_button_filter(fc_ptr->server_ptr->input_manager_ptr,
                                NULL, NULL);
    }
    if (NULL != fc_ptr->idle_ptr) {
        wl_event_source_remove(fc_ptr->idle_ptr);
        fc_ptr->idle_ptr = NULL;
    }
    _wlmaker_force_close_forget_client(fc_ptr);
    bspl_decoded_destroy(_wlmaker_force_close_config_desc, &fc_ptr->config);
    free(fc_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_force_close_reconfigure(
    wlmaker_force_close_t *fc_ptr,
    bspl_dict_t *config_dict_ptr)
{
    return _wlmaker_force_close_configure(fc_ptr, config_dict_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_force_close_toggle(wlmaker_force_close_t *fc_ptr)
{
    if (fc_ptr->active) {
        bs_log(BS_INFO, "Force close: Cancelled.");
        _wlmaker_force_close_end(fc_ptr);
    } else {
        _wlmaker_force_close_begin(fc_ptr);
    }
}

/* ------------------------------------------------------------------------- */
bool wlmaker_force_close_active(wlmaker_force_close_t *fc_ptr)
{
    return fc_ptr->active;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_force_close_protected(
    pid_t pid,
    const char *comm_ptr,
    const char *exe_ptr)
{
    if (NULL != comm_ptr && _wlmaker_force_close_protected_name(comm_ptr)) {
        return true;
    }
    if (NULL != exe_ptr && _wlmaker_force_close_protected_name(exe_ptr)) {
        return true;
    }
    if (0 >= pid) return false;
    return _wlmaker_force_close_protected_pid(pid);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Decodes the 'ForceClose' dictionary. */
bool _wlmaker_force_close_configure(
    wlmaker_force_close_t *fc_ptr,
    bspl_dict_t *config_dict_ptr)
{
    _wlmaker_force_close_config_t config = {};
    bspl_dict_t *empty_ptr = NULL;
    if (NULL == config_dict_ptr) {
        empty_ptr = bspl_dict_create();
        config_dict_ptr = empty_ptr;
    }
    bool ok = (NULL != config_dict_ptr &&
               bspl_decode_dict(config_dict_ptr,
                                _wlmaker_force_close_config_desc, &config));
    if (NULL != empty_ptr) bspl_dict_unref(empty_ptr);
    if (!ok) {
        bs_log(BS_WARNING, "Failed to decode the 'ForceClose' configuration.");
        bspl_decoded_destroy(_wlmaker_force_close_config_desc, &config);
        return false;
    }
    bspl_decoded_destroy(_wlmaker_force_close_config_desc, &fc_ptr->config);
    fc_ptr->config = config;
    return true;
}

/* ------------------------------------------------------------------------- */
/** Starts selecting: The skull, the keyboard grab, the button filter. */
void _wlmaker_force_close_begin(wlmaker_force_close_t *fc_ptr)
{
    wlmaker_server_t *server_ptr = fc_ptr->server_ptr;
    wlmim_t *im_ptr = server_ptr->input_manager_ptr;
    if (NULL == im_ptr) return;
    if (NULL != server_ptr->desktop_ptr &&
        wlmtk_desktop_locked(server_ptr->desktop_ptr)) return;
    if (wlmim_has_key_grab(im_ptr)) {
        bs_log(BS_INFO, "Force close: The keyboard is grabbed, not starting.");
        return;
    }

    const char *names[] = { fc_ptr->config.cursor_name_ptr, "pirate",
                            "X_cursor", "crosshair", NULL };
    if (NULL == names[0]) names[0] = "pirate";
    int32_t hx = 0, hy = 0;
    float scale = 1;
    struct wlr_buffer *b_ptr = NULL;
    if (NULL != fc_ptr->config.cursor_theme_ptr &&
        '\0' != *fc_ptr->config.cursor_theme_ptr) {
        b_ptr = wlmim_create_cursor_buffer(
            im_ptr, fc_ptr->config.cursor_theme_ptr, names, &hx, &hy, &scale);
        if (NULL == b_ptr) {
            bs_log(BS_WARNING, "Force close: No \"%s\" in cursor theme \"%s\".",
                   names[0], fc_ptr->config.cursor_theme_ptr);
        }
    }
    if (NULL == b_ptr) {
        b_ptr = wlmim_create_cursor_buffer(im_ptr, NULL, &names[1],
                                           &hx, &hy, &scale);
    }
    if (NULL != b_ptr) {
        wlmim_set_cursor_override(im_ptr, b_ptr, hx, hy, scale);
        wlr_buffer_drop(b_ptr);
    }

    wlmim_set_key_grab(im_ptr, _wlmaker_force_close_key_grab, fc_ptr);
    wlmim_set_button_filter(im_ptr, _wlmaker_force_close_button_filter, fc_ptr);
    fc_ptr->active = true;
    bs_log(BS_INFO, "Force close: Click on the window to close.");
}

/* ------------------------------------------------------------------------- */
/** Ends selecting. The button filter stays until the buttons are released. */
void _wlmaker_force_close_end(wlmaker_force_close_t *fc_ptr)
{
    wlmim_t *im_ptr = fc_ptr->server_ptr->input_manager_ptr;
    fc_ptr->active = false;
    if (NULL == im_ptr) return;
    wlmim_set_cursor_override(im_ptr, NULL, 0, 0, 1);
    wlmim_set_key_grab(im_ptr, NULL, NULL);
    if (0 == fc_ptr->buttons_pressed) {
        wlmim_set_button_filter(im_ptr, NULL, NULL);
    }
}

/* ------------------------------------------------------------------------- */
/** Keyboard grab: Escape cancels. Other keys go on as usual. */
bool _wlmaker_force_close_key_grab(
    xkb_keysym_t keysym,
    __UNUSED__ uint32_t modifiers,
    void *ud_ptr)
{
    wlmaker_force_close_t *fc_ptr = ud_ptr;
    if (XKB_KEY_Escape != keysym) return false;
    bs_log(BS_INFO, "Force close: Cancelled.");
    _wlmaker_force_close_end(fc_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Button filter: The left button selects the window; any other cancels. The
 * presses and their releases do not reach the toolkit nor clients.
 */
bool _wlmaker_force_close_button_filter(
    void *ud_ptr,
    const struct wlr_pointer_button_event *ev_ptr,
    __UNUSED__ uint32_t modifiers)
{
    wlmaker_force_close_t *fc_ptr = ud_ptr;
    wlmim_t *im_ptr = fc_ptr->server_ptr->input_manager_ptr;

    if (WL_POINTER_BUTTON_STATE_RELEASED == ev_ptr->state) {
        for (size_t i = 0; i < fc_ptr->buttons_pressed; ++i) {
            if (fc_ptr->buttons[i] != ev_ptr->button) continue;
            fc_ptr->buttons[i] = fc_ptr->buttons[--fc_ptr->buttons_pressed];
            if (!fc_ptr->active && 0 == fc_ptr->buttons_pressed) {
                wlmim_set_button_filter(im_ptr, NULL, NULL);
            }
            return true;
        }
        return false;
    }

    if (!fc_ptr->active) return false;
    if (fc_ptr->buttons_pressed < _WLMAKER_FORCE_CLOSE_MAX_BUTTONS) {
        fc_ptr->buttons[fc_ptr->buttons_pressed++] = ev_ptr->button;
    }
    if (NULL != fc_ptr->server_ptr->desktop_ptr &&
        wlmtk_desktop_locked(fc_ptr->server_ptr->desktop_ptr)) {
        _wlmaker_force_close_end(fc_ptr);
        return true;
    }
    if (BTN_LEFT == ev_ptr->button) {
        _wlmaker_force_close_select(fc_ptr);
    } else {
        bs_log(BS_INFO, "Force close: Cancelled.");
    }
    _wlmaker_force_close_end(fc_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** The click: Finds the window and its client, checks, and closes. */
void _wlmaker_force_close_select(wlmaker_force_close_t *fc_ptr)
{
    wlmaker_server_t *server_ptr = fc_ptr->server_ptr;
    struct wlr_cursor *c_ptr = wlmim_wlr_cursor(server_ptr->input_manager_ptr);
    double sx, sy;
    struct wlr_scene_node *node_ptr = wlr_scene_node_at(
        &server_ptr->wlr_scene_ptr->tree.node, c_ptr->x, c_ptr->y, &sx, &sy);
    wlmtk_window_t *window_ptr = _wlmaker_force_close_window_at(
        server_ptr, node_ptr);
    struct wlr_surface *surface_ptr = NULL;
    if (NULL != window_ptr) {
        surface_ptr = _wlmaker_force_close_surface_of(window_ptr, node_ptr);
    }
    if (NULL == surface_ptr) {
        // The desktop, the Dock, a panel, or a window of the compositor.
        bs_log(BS_INFO, "Force close: Not an application's window.");
        _wlmaker_force_close_notice(
            fc_ptr, NULL,
            wlmaker_i18n("This is not the window of an application.\n"
                         "Press Ctrl+Alt+Escape and click on the window\n"
                         "of the application to close."));
        return;
    }
    surface_ptr = wlr_surface_get_root_surface(surface_ptr);
    const char *title_ptr = wlmtk_window_get_title(window_ptr);

    _wlmaker_force_close_kind_t kind = _WLMAKER_FORCE_CLOSE_WAYLAND;
    pid_t pid = 0;
    uid_t uid = getuid();
    gid_t gid;
    struct wl_client *wl_client_ptr = NULL;
    uint32_t x11_window = 0;
#if defined(WLMAKER_HAVE_XWAYLAND)
    struct wlr_xwayland_surface *xs_ptr =
        wlr_xwayland_surface_try_from_wlr_surface(surface_ptr);
    if (NULL != xs_ptr) {
        kind = _WLMAKER_FORCE_CLOSE_XWAYLAND;
        pid = xs_ptr->pid;
        x11_window = xs_ptr->window_id;
    }
#endif  // defined(WLMAKER_HAVE_XWAYLAND)
    if (_WLMAKER_FORCE_CLOSE_WAYLAND == kind) {
        if (NULL == surface_ptr->resource) return;
        wl_client_ptr = wl_resource_get_client(surface_ptr->resource);
        wl_client_get_credentials(wl_client_ptr, &pid, &uid, &gid);
    }

    if (uid != getuid() ||
        (0 < pid && wlmaker_force_close_protected(pid, NULL, NULL)) ||
        (0 >= pid && _WLMAKER_FORCE_CLOSE_WAYLAND == kind)) {
        bs_log(BS_INFO, "Force close: Refused for \"%s\" (PID %d).",
               NULL != title_ptr ? title_ptr : "", (int)pid);
        bool titled = NULL != title_ptr && '\0' != *title_ptr;
        char *m_ptr = titled ?
            bs_strdupf(
                // TRANSLATORS: %s is the title of the window.
                wlmaker_i18n("\u201c%s\u201d is part of the session,\n"
                             "and is not closed by force."),
                title_ptr) :
            logged_strdup(
                wlmaker_i18n("This window is part of the session,\n"
                             "and is not closed by force."));
        _wlmaker_force_close_notice(fc_ptr, window_ptr,
                                    NULL != m_ptr ? m_ptr : "");
        if (NULL != m_ptr) free(m_ptr);
        return;
    }

    bs_log(BS_INFO, "Force close: Closing \"%s\" (%s, PID %d).",
           NULL != title_ptr ? title_ptr : "",
           _WLMAKER_FORCE_CLOSE_XWAYLAND == kind ? "X11" : "Wayland",
           (int)pid);
    // Not from within the input handlers: The client's windows go away.
    _wlmaker_force_close_forget_client(fc_ptr);
    fc_ptr->kind = kind;
    fc_ptr->pid = pid;
    fc_ptr->x11_window = x11_window;
    if (NULL != wl_client_ptr) {
        fc_ptr->wl_client_ptr = wl_client_ptr;
        fc_ptr->client_destroy_listener.notify =
            _wlmaker_force_close_handle_client_destroy;
        wl_client_add_destroy_listener(wl_client_ptr,
                                       &fc_ptr->client_destroy_listener);
    }
    if (NULL == fc_ptr->idle_ptr && NULL != server_ptr->wl_display_ptr) {
        fc_ptr->idle_ptr = wl_event_loop_add_idle(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            _wlmaker_force_close_idle, fc_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** @return The window of the current workspace holding `node_ptr`, or NULL. */
wlmtk_window_t *_wlmaker_force_close_window_at(
    wlmaker_server_t *server_ptr,
    struct wlr_scene_node *node_ptr)
{
    if (NULL == node_ptr || NULL == server_ptr->desktop_ptr) return NULL;
    wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
        server_ptr->desktop_ptr);
    if (NULL == ws_ptr) return NULL;
    bs_dllist_t *windows_ptr = wlmtk_workspace_get_windows_dllist(ws_ptr);
    for (struct wlr_scene_node *n_ptr = node_ptr; NULL != n_ptr;
         n_ptr = NULL != n_ptr->parent ? &n_ptr->parent->node : NULL) {
        for (bs_dllist_node_t *d_ptr = windows_ptr->head_ptr; NULL != d_ptr;
             d_ptr = d_ptr->next_ptr) {
            wlmtk_window_t *w_ptr = wlmtk_window_from_dlnode(d_ptr);
            if (wlmtk_window_element(w_ptr)->wlr_scene_node_ptr == n_ptr) {
                return w_ptr;
            }
        }
    }
    return NULL;
}

/** Argument for @ref _wlmaker_force_close_find_surface. */
typedef struct {
    /** The surface found. */
    struct wlr_surface        *surface_ptr;
} _wlmaker_force_close_find_t;

/* ------------------------------------------------------------------------- */
/** Iterator for wlr_scene_node_for_each_buffer: The first client surface. */
static void _wlmaker_force_close_find_surface(
    struct wlr_scene_buffer *buffer_ptr,
    __UNUSED__ int sx,
    __UNUSED__ int sy,
    void *ud_ptr)
{
    _wlmaker_force_close_find_t *f_ptr = ud_ptr;
    if (NULL != f_ptr->surface_ptr) return;
    struct wlr_scene_surface *s_ptr = wlr_scene_surface_try_from_buffer(
        buffer_ptr);
    if (NULL != s_ptr) f_ptr->surface_ptr = s_ptr->surface;
}

/* ------------------------------------------------------------------------- */
/**
 * @return The client's surface: The one clicked, else the window's (a
 *     click on the title bar or the borders). NULL for the compositor's own
 *     windows (menus, dialogs).
 */
struct wlr_surface *_wlmaker_force_close_surface_of(
    wlmtk_window_t *window_ptr,
    struct wlr_scene_node *node_ptr)
{
    if (WLR_SCENE_NODE_BUFFER == node_ptr->type) {
        struct wlr_scene_surface *s_ptr = wlr_scene_surface_try_from_buffer(
            wlr_scene_buffer_from_node(node_ptr));
        if (NULL != s_ptr) return s_ptr->surface;
    }
    _wlmaker_force_close_find_t find = {};
    wlr_scene_node_for_each_buffer(
        wlmtk_window_element(window_ptr)->wlr_scene_node_ptr,
        _wlmaker_force_close_find_surface, &find);
    return find.surface_ptr;
}

/* ------------------------------------------------------------------------- */
/** The notice's answer: Nothing to do. */
static void _wlmaker_force_close_noticed(
    __UNUSED__ void *ud_ptr,
    __UNUSED__ bool confirmed)
{
}

/* ------------------------------------------------------------------------- */
/** Tells why nothing was closed: A notice with the application's icon. */
void _wlmaker_force_close_notice(
    wlmaker_force_close_t *fc_ptr,
    wlmtk_window_t *window_ptr,
    const char *message_ptr)
{
    cairo_surface_t *icon_ptr = wlmaker_dialog_app_icon(
        fc_ptr->server_ptr,
        NULL != window_ptr ? wlmtk_window_get_app_id(window_ptr) : NULL,
        NULL != window_ptr ? wlmtk_window_get_instance(window_ptr) : NULL,
        NULL);
    wlmaker_dialog_confirm_with_icon(
        fc_ptr->server_ptr, wlmaker_i18n("Force Close"), message_ptr,
        wlmaker_i18n("OK"), NULL, icon_ptr,
        _wlmaker_force_close_noticed, NULL, NULL);
    if (NULL != icon_ptr) cairo_surface_destroy(icon_ptr);
}

/* ------------------------------------------------------------------------- */
/** Forgets the client to close. */
void _wlmaker_force_close_forget_client(wlmaker_force_close_t *fc_ptr)
{
    if (NULL != fc_ptr->wl_client_ptr) {
        wl_list_remove(&fc_ptr->client_destroy_listener.link);
        fc_ptr->wl_client_ptr = NULL;
    }
    fc_ptr->kind = _WLMAKER_FORCE_CLOSE_NONE;
    fc_ptr->pid = 0;
    fc_ptr->x11_window = 0;
}

/* ------------------------------------------------------------------------- */
/** The Wayland client went away (eg. it exited meanwhile). */
void _wlmaker_force_close_handle_client_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_force_close_t *fc_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_force_close_t, client_destroy_listener);
    wl_list_remove(&fc_ptr->client_destroy_listener.link);
    fc_ptr->wl_client_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** Closes the selected client, from the event loop. */
void _wlmaker_force_close_idle(void *data_ptr)
{
    wlmaker_force_close_t *fc_ptr = data_ptr;
    fc_ptr->idle_ptr = NULL;

#if defined(WLMAKER_HAVE_XWAYLAND)
    if (_WLMAKER_FORCE_CLOSE_XWAYLAND == fc_ptr->kind &&
        NULL != fc_ptr->server_ptr->xwl_ptr) {
        // As xkill: The X server closes the client's connection, and its
        // windows go away even if the process hangs.
        xcb_connection_t *c_ptr = wlr_xwayland_get_xwm_connection(
            wlmaker_xwl_wlr_xwayland(fc_ptr->server_ptr->xwl_ptr));
        if (NULL != c_ptr) {
            xcb_kill_client(c_ptr, fc_ptr->x11_window);
            xcb_flush(c_ptr);
        }
    }
#endif  // defined(WLMAKER_HAVE_XWAYLAND)

    // The process itself: It may hang, or keep running without windows.
    // Checked again: It may have exited, and the PID been taken meanwhile.
    if (0 < fc_ptr->pid &&
        !wlmaker_force_close_protected(fc_ptr->pid, NULL, NULL) &&
        0 != kill(fc_ptr->pid, SIGKILL)) {
        bs_log(BS_WARNING | BS_ERRNO, "Force close: Failed kill(%d, SIGKILL)",
               (int)fc_ptr->pid);
    }

    // And the connection: The windows go away right now.
    if (NULL != fc_ptr->wl_client_ptr) {
        struct wl_client *wl_client_ptr = fc_ptr->wl_client_ptr;
        _wlmaker_force_close_forget_client(fc_ptr);
        wl_client_destroy(wl_client_ptr);
    }
    _wlmaker_force_close_forget_client(fc_ptr);
}

/* ------------------------------------------------------------------------- */
/** Reads /proc/<pid>/<what> into `buf`. @return bytes read, or -1. */
static ssize_t _wlmaker_force_close_read_proc(
    pid_t pid, const char *what_ptr, char *buf, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, what_ptr);
    FILE *f_ptr = fopen(path, "r");
    if (NULL == f_ptr) return -1;
    size_t n = fread(buf, 1, len - 1, f_ptr);
    fclose(f_ptr);
    buf[n] = '\0';
    return (ssize_t)n;
}

/* ------------------------------------------------------------------------- */
/** @return The parent of `pid`, or 0. */
static pid_t _wlmaker_force_close_parent(pid_t pid)
{
    char buf[512];
    if (0 >= _wlmaker_force_close_read_proc(pid, "stat", buf, sizeof(buf))) {
        return 0;
    }
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses.
    const char *p_ptr = strrchr(buf, ')');
    int ppid = 0;
    if (NULL == p_ptr || 1 != sscanf(p_ptr + 1, " %*c %d", &ppid)) return 0;
    return ppid;
}

/* ------------------------------------------------------------------------- */
/**
 * @return Whether the process is the compositor, one of its ancestors (the
 *     session), another user's, or has a session process name.
 */
bool _wlmaker_force_close_protected_pid(pid_t pid)
{
    if (1 >= pid || getpid() == pid) return true;
    for (pid_t p = getppid(); 1 < p; p = _wlmaker_force_close_parent(p)) {
        if (p == pid) return true;
    }

    char path[64], buf[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    if (0 != stat(path, &st) || st.st_uid != getuid()) return true;

    if (0 < _wlmaker_force_close_read_proc(pid, "comm", buf, sizeof(buf))) {
        buf[strcspn(buf, "\n")] = '\0';
        if (_wlmaker_force_close_protected_name(buf)) return true;
    }
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (0 < n) {
        buf[n] = '\0';
        const char *b_ptr = strrchr(buf, '/');
        if (_wlmaker_force_close_protected_name(NULL != b_ptr ? b_ptr + 1 : buf)) {
            return true;
        }
    }
    // Scripts: The interpreter's first arguments (python3 /usr/bin/big-wm-x).
    n = _wlmaker_force_close_read_proc(pid, "cmdline", buf, sizeof(buf));
    for (ssize_t i = 0, arg = 0; 0 < n && i < n && arg < 3; ++arg) {
        const char *a_ptr = &buf[i];
        const char *b_ptr = strrchr(a_ptr, '/');
        if ('-' != *a_ptr &&
            _wlmaker_force_close_protected_name(NULL != b_ptr ? b_ptr + 1 : a_ptr)) {
            return true;
        }
        i += strlen(a_ptr) + 1;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/** @return Whether the process name is one of the session's. */
bool _wlmaker_force_close_protected_name(const char *name_ptr)
{
    if (NULL == name_ptr || '\0' == *name_ptr) return false;
    for (const char **n_ptr = _wlmaker_force_close_protected_names;
         NULL != *n_ptr; ++n_ptr) {
        if (0 == strcmp(*n_ptr, name_ptr)) return true;
    }
    for (const char **p_ptr = _wlmaker_force_close_protected_prefixes;
         NULL != *p_ptr; ++p_ptr) {
        if (0 == strncmp(*p_ptr, name_ptr, strlen(*p_ptr))) return true;
    }
    return false;
}

/* == Unit tests =========================================================== */

static void _wlmaker_force_close_test_protected(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_force_close_test_cases[] = {
    { true, "protected", _wlmaker_force_close_test_protected },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_force_close_test_set = BS_TEST_SET(
    true, "force_close", _wlmaker_force_close_test_cases);

/* ------------------------------------------------------------------------- */
/** The session's processes are protected; applications are not. */
void _wlmaker_force_close_test_protected(bs_test_t *test_ptr)
{
    const char *protected[] = {
        "wlmaker", "Xwayland", "systemd", "systemd-logind", "dbus-daemon",
        "dbus-broker-lau", "pipewire", "pipewire-pulse", "wireplumber",
        "big-wm-tray", "big-wm-locate-p", "wlmdock", "wlmclock", "wmclock",
        "wmcliphist", "xdg-desktop-por", NULL };
    for (const char **n = protected; NULL != *n; ++n) {
        BS_TEST_VERIFY_TRUE_OR_RETURN(
            test_ptr, wlmaker_force_close_protected(0, *n, NULL));
    }
    const char *apps[] = {
        "firefox", "chromium", "xterm", "foot", "python3", "gedit",
        "steam", "Web Content", NULL };
    for (const char **n = apps; NULL != *n; ++n) {
        BS_TEST_VERIFY_FALSE_OR_RETURN(
            test_ptr, wlmaker_force_close_protected(0, *n, *n));
    }
    // The compositor's own process, its parent and PID 1.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_force_close_protected(getpid(), NULL, NULL));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_force_close_protected(getppid(), NULL, NULL));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_force_close_protected(1, NULL, NULL));
}

/* == End of force_close.c ================================================= */
