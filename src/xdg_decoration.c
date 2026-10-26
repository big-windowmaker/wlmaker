/* ========================================================================= */
/**
 * @file xdg_decoration.c
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

#include "xdg_decoration.h"

#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#undef WLR_USE_UNSTABLE

#include "config.h"
#include "toolkit/toolkit.h"
#include "xdg_shell.h"
#include "xdg_toplevel.h"

/* == Declarations ========================================================= */

/** State of the XDG decoration manager. */
struct _wlmaker_xdg_decoration_manager_t {
    /** The wlroots XDG decoration manager. */
    struct wlr_xdg_decoration_manager_v1* wlr_xdg_decoration_manager_v1_ptr;

    /** Injectable, for tests: wlr_xdg_toplevel_decoration_v1_set_mode(). */
    uint32_t (*set_mode)(
        struct wlr_xdg_toplevel_decoration_v1 *decoration,
        enum wlr_xdg_toplevel_decoration_v1_mode mode);
    /** Operation mode for the decoration manager. */
    wlmaker_config_decoration_t mode;

    /** Listener for `new_toplevel_decoration`. */
    struct wl_listener        new_toplevel_decoration_listener;
    /** Listener for `destroy` of `wlr_xdg_decoration_manager_v1`. */
    struct wl_listener        destroy_listener;

    /**
     * The (KDE) server decoration manager. Used by GTK3, which does not
     * implement the XDG decoration protocol. Owned by wl_display.
     */
    struct wlr_server_decoration_manager *wlr_server_decoration_manager_ptr;
    /** Listener for `new_decoration` of `wlr_server_decoration_manager`. */
    struct wl_listener        new_server_decoration_listener;
};

/** A (KDE) server decoration handle. */
typedef struct {
    /** Points to the wlroots `wlr_server_decoration`. */
    struct wlr_server_decoration *wlr_server_decoration_ptr;
    /** Back-link to the decoration manager. */
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr;

    /** Listener for `mode` of `wlr_server_decoration`. */
    struct wl_listener        mode_listener;
    /** Listener for `destroy` of `wlr_server_decoration`. */
    struct wl_listener        destroy_listener;
    /** Listener for `commit` of `wlr_surface`, until the toplevel exists. */
    struct wl_listener        surface_commit_listener;
} wlmaker_server_decoration_t;

/** A decoration handle. */
typedef struct {
    /** Points to the wlroots `wlr_xdg_toplevel_decoration_v1`. */
    struct wlr_xdg_toplevel_decoration_v1 *wlr_xdg_toplevel_decoration_v1_ptr;
    /** Back-link to the decoration manager. */
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr;

    /** Listener for `request_mode` of `wlr_xdg_toplevel_decoration_v1`. */
    struct wl_listener        request_mode_listener;
    /** Listener for `destroy` of `wlr_xdg_toplevel_decoration_v1.` */
    struct wl_listener        destroy_listener;
    /** Listener for `commit` of `wlr_surface::events`. */
    struct wl_listener        surface_commit_listener;
    /** Listener for `destroy` of `wlr_surface::events`. */
    struct wl_listener        surface_destroy_listener;
} wlmaker_xdg_decoration_t;

static void handle_new_toplevel_decoration(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void handle_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static wlmaker_xdg_decoration_t *wlmaker_xdg_decoration_create(
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr,
    struct wlr_xdg_toplevel_decoration_v1 *wlr_xdg_toplevel_decoration_v1_ptr);
static void wlmaker_xdg_decoration_destroy(
    wlmaker_xdg_decoration_t *decoration_ptr);

static void handle_decoration_request_mode(
    struct wl_listener *listener_ptr,
     void *data_ptr);
static void handle_decoration_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xdg_decoration_handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _xdg_decoration_handle_surface_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static uint32_t _wlmaker_server_decoration_default_mode(
    wlmaker_config_decoration_t config_mode);
static bool _wlmaker_server_decoration_is_server_side(
    wlmaker_config_decoration_t config_mode,
    uint32_t requested_mode);
static bool _wlmaker_server_decoration_apply(
    wlmaker_server_decoration_t *server_decoration_ptr);
static void _wlmaker_server_decoration_handle_new(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_server_decoration_handle_mode(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_server_decoration_handle_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_server_decoration_handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Data ================================================================= */

/** Plist descriptor of decoration mode. @see wlmaker_config_decoration_t. */
static const bspl_enum_desc_t _wlmaker_config_decoration_desc[] = {
    BSPL_ENUM("SuggestClient", WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT),
    BSPL_ENUM("SuggestServer", WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER),
    BSPL_ENUM("EnforceClient", WLMAKER_CONFIG_DECORATION_ENFORCE_CLIENT),
    BSPL_ENUM("EnforceServer", WLMAKER_CONFIG_DECORATION_ENFORCE_SERVER),
    BSPL_ENUM_SENTINEL()
};

/** Plist descriptor of the 'Decoration' dict contents. */
static const bspl_desc_t _wlmaker_xdg_decoration_config_desc[] = {
    BSPL_DESC_ENUM("Mode", true, wlmaker_xdg_decoration_manager_t, mode, mode,
                     WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER,
                     _wlmaker_config_decoration_desc),
    BSPL_DESC_SENTINEL()
};

/** Name of the top-level dict holding the decoration manager's config. */
static const char *_wlmaker_xdg_decoration_dict_name = "Decoration";

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_xdg_decoration_manager_t *wlmaker_xdg_decoration_manager_create(
    struct wl_display *wl_display_ptr,
    bspl_dict_t *config_dict_ptr)
{
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr = logged_calloc(
        1, sizeof(wlmaker_xdg_decoration_manager_t));
    if (NULL == decoration_manager_ptr) return NULL;
    decoration_manager_ptr->set_mode = wlr_xdg_toplevel_decoration_v1_set_mode;

    decoration_manager_ptr->wlr_xdg_decoration_manager_v1_ptr =
        wlr_xdg_decoration_manager_v1_create(wl_display_ptr);
    if (NULL == decoration_manager_ptr->wlr_xdg_decoration_manager_v1_ptr) {
        wlmaker_xdg_decoration_manager_destroy(decoration_manager_ptr);
        return NULL;
    }

    bspl_dict_t *decoration_dict_ptr = bspl_dict_ref(
        bspl_dict_get_dict(config_dict_ptr,
                           _wlmaker_xdg_decoration_dict_name));
    if (NULL == decoration_dict_ptr) {
        bs_log(BS_ERROR, "No '%s' dict.", _wlmaker_xdg_decoration_dict_name);
        wlmaker_xdg_decoration_manager_destroy(decoration_manager_ptr);
        return NULL;
    }
    bool decoded = bspl_decode_dict(
        decoration_dict_ptr,
        _wlmaker_xdg_decoration_config_desc,
        decoration_manager_ptr);
    bspl_dict_unref(decoration_dict_ptr);
    if (!decoded) {
        bs_log(BS_ERROR, "Failed to decode '%s' dict",
               _wlmaker_xdg_decoration_dict_name);
        wlmaker_xdg_decoration_manager_destroy(decoration_manager_ptr);
        return NULL;
    }

    decoration_manager_ptr->wlr_server_decoration_manager_ptr =
        wlr_server_decoration_manager_create(wl_display_ptr);
    if (NULL == decoration_manager_ptr->wlr_server_decoration_manager_ptr) {
        bs_log(BS_ERROR, "Failed wlr_server_decoration_manager_create(%p)",
               wl_display_ptr);
        wlmaker_xdg_decoration_manager_destroy(decoration_manager_ptr);
        return NULL;
    }
    wlr_server_decoration_manager_set_default_mode(
        decoration_manager_ptr->wlr_server_decoration_manager_ptr,
        _wlmaker_server_decoration_default_mode(decoration_manager_ptr->mode));
    wlmtk_util_connect_listener_signal(
        &decoration_manager_ptr->wlr_server_decoration_manager_ptr->
        events.new_decoration,
        &decoration_manager_ptr->new_server_decoration_listener,
        _wlmaker_server_decoration_handle_new);

    wlmtk_util_connect_listener_signal(
        &decoration_manager_ptr->wlr_xdg_decoration_manager_v1_ptr->
        events.new_toplevel_decoration,
        &decoration_manager_ptr->new_toplevel_decoration_listener,
        handle_new_toplevel_decoration);
    wlmtk_util_connect_listener_signal(
        &decoration_manager_ptr->wlr_xdg_decoration_manager_v1_ptr->
        events.destroy,
        &decoration_manager_ptr->destroy_listener,
        handle_destroy);

    return decoration_manager_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_xdg_decoration_manager_destroy(
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr)
{
    wlmtk_util_disconnect_listener(&decoration_manager_ptr->new_server_decoration_listener);
    wlmtk_util_disconnect_listener(&decoration_manager_ptr->new_toplevel_decoration_listener);
    wlmtk_util_disconnect_listener(&decoration_manager_ptr->destroy_listener);

    free(decoration_manager_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `new_toplevel_decoration` signal of
 * `wlr_xdg_decoration_manager_v1`.
 *
 * @param listener_ptr
 * @param data_ptr            Points to `wlr_xdg_toplevel_decoration_v1`.
 */
void handle_new_toplevel_decoration(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_decoration_manager_t,
        new_toplevel_decoration_listener);
    struct wlr_xdg_toplevel_decoration_v1
        *wlr_xdg_toplevel_decoration_v1_ptr = data_ptr;

    wlmaker_xdg_decoration_t *decoration_ptr = wlmaker_xdg_decoration_create(
        decoration_manager_ptr,
        wlr_xdg_toplevel_decoration_v1_ptr);
    if (NULL == decoration_ptr) return;

    handle_decoration_request_mode(
        &decoration_ptr->request_mode_listener, NULL);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `destroy` signal of `wlr_xdg_decoration_manager_v1`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
static void handle_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_decoration_manager_t, destroy_listener);

    wlmtk_util_disconnect_listener(
        &decoration_manager_ptr->new_server_decoration_listener);
    free(decoration_manager_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Creates a decoration handle.
 *
 * @param decoration_manager_ptr
 * @param wlr_xdg_toplevel_decoration_v1_ptr
 *
 * @returns The decoration handle or NULL on error.
 */
wlmaker_xdg_decoration_t *wlmaker_xdg_decoration_create(
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr,
    struct wlr_xdg_toplevel_decoration_v1 *wlr_xdg_toplevel_decoration_v1_ptr)
{
    wlmaker_xdg_decoration_t *decoration_ptr = logged_calloc(
        1, sizeof(wlmaker_xdg_decoration_t));
    if (NULL == decoration_ptr) return NULL;
    decoration_ptr->decoration_manager_ptr = decoration_manager_ptr;
    decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr =
        wlr_xdg_toplevel_decoration_v1_ptr;

    wlmtk_util_connect_listener_signal(
        &decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->events.destroy,
        &decoration_ptr->destroy_listener,
        handle_decoration_destroy);
    wlmtk_util_connect_listener_signal(
        &decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->events.request_mode,
        &decoration_ptr->request_mode_listener,
        handle_decoration_request_mode);

    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr =
        decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->toplevel;
    wlmtk_util_connect_listener_signal(
        &wlr_xdg_toplevel_ptr->base->surface->events.commit,
        &decoration_ptr->surface_commit_listener,
        _xdg_decoration_handle_surface_commit);
    wlmtk_util_connect_listener_signal(
        &wlr_xdg_toplevel_ptr->base->surface->events.destroy,
        &decoration_ptr->surface_destroy_listener,
        _xdg_decoration_handle_surface_destroy);

    return decoration_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Destroys the decoration handle.
 *
 * @param decoration_ptr
 */
void wlmaker_xdg_decoration_destroy(wlmaker_xdg_decoration_t *decoration_ptr)
{
    _xdg_decoration_handle_surface_destroy(
        &decoration_ptr->surface_destroy_listener, NULL);

    wlmtk_util_disconnect_listener(&decoration_ptr->destroy_listener);
    wlmtk_util_disconnect_listener(&decoration_ptr->request_mode_listener);
    free(decoration_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `request_mode` signal of `wlr_xdg_toplevel_decoration_v1`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void handle_decoration_request_mode(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_decoration_t *decoration_ptr = wl_container_of(
        listener_ptr, decoration_ptr, request_mode_listener);

    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr =
        decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->toplevel;

    enum wlr_xdg_toplevel_decoration_v1_mode mode =
        decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->requested_mode;
    switch (decoration_ptr->decoration_manager_ptr->mode) {
    case WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT:
        if (WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE == mode) {
            mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
        }
        break;

    case WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER:
        if (WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE == mode) {
            mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
        }
        break;

    case WLMAKER_CONFIG_DECORATION_ENFORCE_CLIENT:
        mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
        break;

    case WLMAKER_CONFIG_DECORATION_ENFORCE_SERVER:
        mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
        break;

    default:
        bs_log(BS_FATAL, "Unhandled config_decoration value %d",
               decoration_ptr->decoration_manager_ptr->mode);
        BS_ABORT();
    }

    if (wlr_xdg_toplevel_ptr->base->initialized) {
        decoration_ptr->decoration_manager_ptr->set_mode(
            decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr, mode);
    }

    struct wlmaker_xdg_toplevel *wlmaker_xdg_toplevel_ptr =
        wlr_xdg_toplevel_ptr->base->data;
    if (NULL == wlmaker_xdg_toplevel_ptr) {
        bs_log(BS_WARNING,
               "Decoration request for XDG toplevel %p w/o handle?",
               wlr_xdg_toplevel_ptr);
        return;
    }

    bs_log(BS_INFO, "XDG decoration request_mode for XDG surface %p, "
           "XDG toplevel handle %p: Current %d, pending %d, scheduled %d, "
           "requested %d. Set: %d",
           wlr_xdg_toplevel_ptr->base->surface,
           wlmaker_xdg_toplevel_ptr,
           decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->current.mode,
           decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->pending.mode,
           decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->scheduled_mode,
           decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->requested_mode,
           mode);

    wlmaker_xdg_toplevel_set_server_side_decorated(
        wlmaker_xdg_toplevel_ptr,
        mode != WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `destroy` signal of `wlr_xdg_toplevel_decoration_v1`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void handle_decoration_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_decoration_t *decoration_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_decoration_t, destroy_listener);
    wlmaker_xdg_decoration_destroy(decoration_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles surface commit: If initialized, set_mode and unsubscribe.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xdg_decoration_handle_surface_commit(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_decoration_t *decoration_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_decoration_t, surface_commit_listener);

    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr =
        decoration_ptr->wlr_xdg_toplevel_decoration_v1_ptr->toplevel;
    if (!wlr_xdg_toplevel_ptr->base->initialized) return;

    // Initialized! Unsubscribe from surface, and trigger a request_mode.
    _xdg_decoration_handle_surface_destroy(
        &decoration_ptr->surface_destroy_listener, NULL);
    handle_decoration_request_mode(
        &decoration_ptr->request_mode_listener, NULL);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles surface destroy: Unsubscribe surface listeners.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _xdg_decoration_handle_surface_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_decoration_t *decoration_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_decoration_t, surface_destroy_listener);

    wlmtk_util_disconnect_listener(&decoration_ptr->surface_commit_listener);
    wlmtk_util_disconnect_listener(&decoration_ptr->surface_destroy_listener);
}

/* ------------------------------------------------------------------------- */
/**
 * Returns the (KDE) server decoration default mode for the configured mode.
 *
 * @param config_mode
 *
 * @return A value of enum wlr_server_decoration_manager_mode.
 */
uint32_t _wlmaker_server_decoration_default_mode(
    wlmaker_config_decoration_t config_mode)
{
    switch (config_mode) {
    case WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT:
    case WLMAKER_CONFIG_DECORATION_ENFORCE_CLIENT:
        return WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
    case WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER:
    case WLMAKER_CONFIG_DECORATION_ENFORCE_SERVER:
    default:
        return WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Returns whether to decorate a toplevel with (KDE) server decoration mode
 * `requested_mode`, given the configured mode.
 *
 * @param config_mode
 * @param requested_mode      Value of enum wlr_server_decoration_manager_mode.
 *
 * @return true if wlmaker should draw the decoration.
 */
bool _wlmaker_server_decoration_is_server_side(
    wlmaker_config_decoration_t config_mode,
    uint32_t requested_mode)
{
    switch (config_mode) {
    case WLMAKER_CONFIG_DECORATION_ENFORCE_CLIENT:
        return false;
    case WLMAKER_CONFIG_DECORATION_ENFORCE_SERVER:
        return true;
    default:
        return WLR_SERVER_DECORATION_MANAGER_MODE_SERVER == requested_mode;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Applies the server decoration's mode to the XDG toplevel of its surface.
 *
 * @param server_decoration_ptr
 *
 * @return true if applied, false if there is no wlmaker XDG toplevel (yet).
 */
bool _wlmaker_server_decoration_apply(
    wlmaker_server_decoration_t *server_decoration_ptr)
{
    struct wlr_server_decoration *wsd_ptr =
        server_decoration_ptr->wlr_server_decoration_ptr;
    struct wlr_xdg_toplevel *wlr_xdg_toplevel_ptr =
        wlr_xdg_toplevel_try_from_wlr_surface(wsd_ptr->surface);
    if (NULL == wlr_xdg_toplevel_ptr) return false;
    struct wlmaker_xdg_toplevel *wlmaker_xdg_toplevel_ptr =
        wlr_xdg_toplevel_ptr->base->data;
    if (NULL == wlmaker_xdg_toplevel_ptr) return false;

    bool server_side = _wlmaker_server_decoration_is_server_side(
        server_decoration_ptr->decoration_manager_ptr->mode,
        wsd_ptr->mode);
    bs_log(BS_INFO, "Server decoration mode %"PRIu32" for XDG toplevel %p: "
           "Server-side decorated: %d",
           wsd_ptr->mode, wlmaker_xdg_toplevel_ptr, server_side);
    wlmaker_xdg_toplevel_set_server_side_decorated(
        wlmaker_xdg_toplevel_ptr, server_side);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for `new_decoration` of `wlr_server_decoration_manager`.
 *
 * @param listener_ptr
 * @param data_ptr            Points to struct wlr_server_decoration.
 */
void _wlmaker_server_decoration_handle_new(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xdg_decoration_manager_t *decoration_manager_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_decoration_manager_t,
        new_server_decoration_listener);
    struct wlr_server_decoration *wlr_server_decoration_ptr = data_ptr;

    wlmaker_server_decoration_t *server_decoration_ptr = logged_calloc(
        1, sizeof(wlmaker_server_decoration_t));
    if (NULL == server_decoration_ptr) return;
    server_decoration_ptr->wlr_server_decoration_ptr =
        wlr_server_decoration_ptr;
    server_decoration_ptr->decoration_manager_ptr = decoration_manager_ptr;

    wlmtk_util_connect_listener_signal(
        &wlr_server_decoration_ptr->events.destroy,
        &server_decoration_ptr->destroy_listener,
        _wlmaker_server_decoration_handle_destroy);
    wlmtk_util_connect_listener_signal(
        &wlr_server_decoration_ptr->events.mode,
        &server_decoration_ptr->mode_listener,
        _wlmaker_server_decoration_handle_mode);

    // The decoration may be created before the XDG toplevel: Then apply it
    // with the first commit that has a toplevel.
    if (!_wlmaker_server_decoration_apply(server_decoration_ptr)) {
        wlmtk_util_connect_listener_signal(
            &wlr_server_decoration_ptr->surface->events.commit,
            &server_decoration_ptr->surface_commit_listener,
            _wlmaker_server_decoration_handle_surface_commit);
    }
}

/* ------------------------------------------------------------------------- */
/** Handler for `mode` of `wlr_server_decoration`: Applies the mode. */
void _wlmaker_server_decoration_handle_mode(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_server_decoration_t *server_decoration_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_server_decoration_t, mode_listener);
    _wlmaker_server_decoration_apply(server_decoration_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handler for `destroy` of `wlr_server_decoration`: Cleans up. */
void _wlmaker_server_decoration_handle_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_server_decoration_t *server_decoration_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_server_decoration_t, destroy_listener);

    wlmtk_util_disconnect_listener(
        &server_decoration_ptr->surface_commit_listener);
    wlmtk_util_disconnect_listener(&server_decoration_ptr->mode_listener);
    wlmtk_util_disconnect_listener(&server_decoration_ptr->destroy_listener);
    free(server_decoration_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handler for `commit` of the surface: Applies, once there's a toplevel. */
void _wlmaker_server_decoration_handle_surface_commit(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_server_decoration_t *server_decoration_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_server_decoration_t, surface_commit_listener);
    if (_wlmaker_server_decoration_apply(server_decoration_ptr)) {
        wlmtk_util_disconnect_listener(
            &server_decoration_ptr->surface_commit_listener);
    }
}

/* == Unit tests =========================================================== */

static void test_manager(bs_test_t *test_ptr);
static void test_decoration_initialized(bs_test_t *test_ptr);
static void test_decoration_uninitialized(bs_test_t *test_ptr);
static void test_server_decoration_modes(bs_test_t *test_ptr);
static void test_server_decoration_no_toplevel(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t wlmaker_xdg_decoration_test_cases[] = {
    { true, "manager", test_manager },
    { true, "decoration_initialized", test_decoration_initialized },
    { true, "decoration_uninitialized", test_decoration_uninitialized },
    { true, "server_decoration_modes", test_server_decoration_modes },
    { true, "server_decoration_no_toplevel",
      test_server_decoration_no_toplevel },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_xdg_decoration_test_set = BS_TEST_SET(
    true, "xdg_decoration", wlmaker_xdg_decoration_test_cases);

/** Argument to injected set_mode. */
struct _xdg_decoration_test_arg {
    /** The decoration handle. */
    struct wlr_xdg_toplevel_decoration_v1 decoration;
    /** Counter for calls to wlmaker_xdg_decoration_manager_t::set_mode. */
    int set_mode_calls;
    /** Last `mode` arg to wlmaker_xdg_decoration_manager_t::set_mode. */
    enum wlr_xdg_toplevel_decoration_v1_mode set_mode_arg;
};

/** Injected method, for wlr_xdg_toplevel_decoration_v1_set_mode(). */
static uint32_t _xdg_decoration_fake_set_mode(
    __UNUSED__ struct wlr_xdg_toplevel_decoration_v1 *decoration_ptr,
    enum wlr_xdg_toplevel_decoration_v1_mode mode)
{
    struct _xdg_decoration_test_arg *arg_ptr = BS_CONTAINER_OF(
        decoration_ptr, struct _xdg_decoration_test_arg, decoration);
    ++arg_ptr->set_mode_calls;
    arg_ptr->set_mode_arg = mode;
    return 0;
}


/* ------------------------------------------------------------------------- */
/** Setup and teardown of XDG decoration manager. */
void test_manager(bs_test_t *test_ptr)
{
    static const char *c = "{ Decoration = { Mode = SuggestClient }}";

    struct wl_display *wl_display_ptr = wl_display_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wl_display_ptr);
    bspl_object_t *o = bspl_create_object_from_plist_string(c);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);

    wlmaker_xdg_decoration_manager_t *d =
        wlmaker_xdg_decoration_manager_create(
            wl_display_ptr, bspl_dict_from_object(o));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT,
        d->wlr_server_decoration_manager_ptr->default_mode);

    wlmaker_xdg_decoration_manager_destroy(d);
    bspl_object_unref(o);
    wl_display_destroy(wl_display_ptr);
}

/* ------------------------------------------------------------------------- */
/** Test decoration for an initialized surface. */
void test_decoration_initialized(bs_test_t *test_ptr)
{
    wlmaker_xdg_decoration_manager_t m = {
        .mode = WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT,
        .set_mode = _xdg_decoration_fake_set_mode
    };
    struct wlr_surface ws = {};
    wl_signal_init(&ws.events.commit);
    wl_signal_init(&ws.events.destroy);
    struct wlr_xdg_surface s = { .initialized = true, .surface = &ws };
    struct wlr_xdg_toplevel tl = { .base = &s };
    struct _xdg_decoration_test_arg t = { .decoration = { .toplevel = &tl } };
    wl_signal_init(&t.decoration.events.destroy);
    wl_signal_init(&t.decoration.events.request_mode);

    // New decoration: Set_mode right away.
    handle_new_toplevel_decoration(
        &m.new_toplevel_decoration_listener,
        &t.decoration);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.set_mode_calls);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE,
        t.set_mode_arg);

    // Upon request_mode: Respond with set_mode.
    wl_signal_emit(&t.decoration.events.request_mode, NULL);
    BS_TEST_VERIFY_EQ(test_ptr, 2, t.set_mode_calls);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE,
        t.set_mode_arg);

    // Client-side mode is kept.
    t.decoration.requested_mode =
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    wl_signal_emit(&t.decoration.events.request_mode, NULL);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE,
        t.set_mode_arg);

    // Server-side mode is kept, too.
    t.decoration.requested_mode =
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    wl_signal_emit(&t.decoration.events.request_mode, NULL);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
        t.set_mode_arg);

    wl_signal_emit(&t.decoration.events.destroy, NULL);
}

/* ------------------------------------------------------------------------- */
/** Test decoration for an uninitialized surface. */
void test_decoration_uninitialized(bs_test_t *test_ptr)
{
    wlmaker_xdg_decoration_manager_t m = {
        .mode = WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT,
        .set_mode = _xdg_decoration_fake_set_mode
    };
    struct wlr_surface ws = {};
    wl_signal_init(&ws.events.commit);
    wl_signal_init(&ws.events.destroy);
    struct wlr_xdg_surface s = { .initialized = false, .surface = &ws };
    struct wlr_xdg_toplevel tl = { .base = &s };
    struct _xdg_decoration_test_arg t = { .decoration = { .toplevel = &tl } };
    wl_signal_init(&t.decoration.events.destroy);
    wl_signal_init(&t.decoration.events.request_mode);
    t.decoration.requested_mode =
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;

    // New decoration: Do not set_mode right away.
    handle_new_toplevel_decoration(
        &m.new_toplevel_decoration_listener,
        &t.decoration);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.set_mode_calls);

    // A surface commit, but still not initialized: Keep.
    wl_signal_emit(&ws.events.commit, NULL);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.set_mode_calls);

    // Set to initialized. A surface commit triggers set_mode.
    s.initialized = true;
    wl_signal_emit(&ws.events.commit, NULL);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.set_mode_calls);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
        t.set_mode_arg);
    wl_signal_emit(&t.decoration.events.destroy, NULL);

    // Reset surface. Not initialized. A request_mode won't set_mode.
    t.set_mode_calls = 0;
    s.initialized = false;
    handle_new_toplevel_decoration(
        &m.new_toplevel_decoration_listener,
        &t.decoration);
    wl_signal_emit(&t.decoration.events.request_mode, NULL);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.set_mode_calls);

    // A surface commit, but still not initialized: Keep.
    wl_signal_emit(&ws.events.commit, NULL);
    BS_TEST_VERIFY_EQ(test_ptr, 0, t.set_mode_calls);

    // Set to initialized. A surface commit triggers set_mode.
    s.initialized = true;
    wl_signal_emit(&ws.events.commit, NULL);
    BS_TEST_VERIFY_EQ(test_ptr, 1, t.set_mode_calls);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
        t.set_mode_arg);

    wl_signal_emit(&t.decoration.events.destroy, NULL);
}

/* ------------------------------------------------------------------------- */
/** Tests the mapping of configured & requested modes for server decoration. */
void test_server_decoration_modes(bs_test_t *test_ptr)
{
    BS_TEST_VERIFY_EQ(
        test_ptr, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER,
        _wlmaker_server_decoration_default_mode(
            WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER,
        _wlmaker_server_decoration_default_mode(
            WLMAKER_CONFIG_DECORATION_ENFORCE_SERVER));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT,
        _wlmaker_server_decoration_default_mode(
            WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT));
    BS_TEST_VERIFY_EQ(
        test_ptr, WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT,
        _wlmaker_server_decoration_default_mode(
            WLMAKER_CONFIG_DECORATION_ENFORCE_CLIENT));

    // Suggest*: Follows what the client requests.
    BS_TEST_VERIFY_TRUE(
        test_ptr, _wlmaker_server_decoration_is_server_side(
            WLMAKER_CONFIG_DECORATION_SUGGEST_CLIENT,
            WLR_SERVER_DECORATION_MANAGER_MODE_SERVER));
    BS_TEST_VERIFY_FALSE(
        test_ptr, _wlmaker_server_decoration_is_server_side(
            WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER,
            WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT));
    BS_TEST_VERIFY_FALSE(
        test_ptr, _wlmaker_server_decoration_is_server_side(
            WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER,
            WLR_SERVER_DECORATION_MANAGER_MODE_NONE));

    // Enforce*: Ignores what the client requests.
    BS_TEST_VERIFY_TRUE(
        test_ptr, _wlmaker_server_decoration_is_server_side(
            WLMAKER_CONFIG_DECORATION_ENFORCE_SERVER,
            WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT));
    BS_TEST_VERIFY_FALSE(
        test_ptr, _wlmaker_server_decoration_is_server_side(
            WLMAKER_CONFIG_DECORATION_ENFORCE_CLIENT,
            WLR_SERVER_DECORATION_MANAGER_MODE_SERVER));
}

/* ------------------------------------------------------------------------- */
/**
 * Tests a server decoration for a surface without (wlmaker) XDG toplevel:
 * Waits for commits, does not crash on mode, and cleans up on destroy.
 */
void test_server_decoration_no_toplevel(bs_test_t *test_ptr)
{
    wlmaker_xdg_decoration_manager_t m = {
        .mode = WLMAKER_CONFIG_DECORATION_SUGGEST_SERVER,
    };
    struct wlr_surface ws = {};
    wl_signal_init(&ws.events.commit);
    struct wlr_server_decoration d = {
        .surface = &ws,
        .mode = WLR_SERVER_DECORATION_MANAGER_MODE_SERVER };
    wl_signal_init(&d.events.destroy);
    wl_signal_init(&d.events.mode);

    _wlmaker_server_decoration_handle_new(
        &m.new_server_decoration_listener, &d);
    BS_TEST_VERIFY_EQ(
        test_ptr, 1, wl_list_length(&d.events.destroy.listener_list));
    BS_TEST_VERIFY_EQ(
        test_ptr, 1, wl_list_length(&ws.events.commit.listener_list));

    // No toplevel yet: Commit and mode keep waiting.
    wl_signal_emit(&ws.events.commit, NULL);
    wl_signal_emit(&d.events.mode, NULL);
    BS_TEST_VERIFY_EQ(
        test_ptr, 1, wl_list_length(&ws.events.commit.listener_list));

    // Destroy disconnects all listeners.
    wl_signal_emit(&d.events.destroy, NULL);
    BS_TEST_VERIFY_EQ(
        test_ptr, 0, wl_list_length(&ws.events.commit.listener_list));
    BS_TEST_VERIFY_EQ(
        test_ptr, 0, wl_list_length(&d.events.mode.listener_list));
}

/* == End of xdg_decoration.c ============================================== */
