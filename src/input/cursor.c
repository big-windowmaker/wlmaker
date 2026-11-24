/* ========================================================================= */
/**
 * @file cursor.c
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

#include "cursor.h"

#include <drm_fourcc.h>
#include <ini.h>
#include <inttypes.h>
#include <math.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
#undef WLR_USE_UNSTABLE

#include "keyboard.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** The kind of image requested for the cursor. */
typedef enum {
    _WLMIM_CURSOR_IMAGE_NONE,     ///< Hidden.
    _WLMIM_CURSOR_IMAGE_XCURSOR,  ///< A named image of the XCursor theme.
    _WLMIM_CURSOR_IMAGE_SURFACE,  ///< A client's cursor surface.
} _wlmim_cursor_image_t;

/** State and tools for handling wlmaker cursors. */
struct _wlmim_cursor_t {
    /** Points to a `wlr_cursor`. */
    struct wlr_cursor         *wlr_cursor_ptr;
    /** Points to a `wlr_xcursor_manager`. */
    struct wlr_xcursor_manager *wlr_xcursor_manager_ptr;
    /** Cursor options. */
    struct wlmim_cursor_options options;

    /** The toolkit wrapper for above. */
    wlmtk_pointer_t           *pointer_ptr;

    /** `destroy` signal by `wlr_cursor_shape_manager_v1::events`. */
    struct wl_listener        cursor_shape_manager_v1_destroy_listener;
    /** `request_set_shape` by `wlr_cursor_shape_manager_v1::events`. */
    struct wl_listener        cursor_shape_manager_v1_set_shape_listener;

    /** Listener for the `motion` event of `wlr_cursor`. */
    struct wl_listener        motion_listener;
    /** Listener for the `motion_absolute` event of `wlr_cursor`. */
    struct wl_listener        motion_absolute_listener;
    /** Listener for the `button` event of `wlr_cursor`. */
    struct wl_listener        button_listener;
    /** Listener for the `axis` event of `wlr_cursor`. */
    struct wl_listener        axis_listener;
    /** Listener for the `frame` event of `wlr_cursor`. */
    struct wl_listener        frame_listener;

    /** Listener for the `request_set_cursor` event of `wlr_seat`. */
    struct wl_listener        seat_request_set_cursor_listener;

    /** Back-link to input manager. */
    wlmim_t                   *input_manager_ptr;
    /** WLR seat. */
    struct wlr_seat           *wlr_seat_ptr;
    /** Root wrapper of the compositor. */
    wlmtk_root_t              *root_ptr;
    /** The cursor shape manager. */
    struct wlr_cursor_shape_manager_v1 *wlr_cursor_shape_manager_v1_ptr;
    /** The relative pointer manager. Owned by wl_display. */
    struct wlr_relative_pointer_manager_v1 *wlr_relative_pointer_manager_v1_ptr;
    /** The pointer constraints manager. Owned by wl_display. */
    struct wlr_pointer_constraints_v1 *wlr_pointer_constraints_v1_ptr;
    /** The constraint in effect: on the surface with pointer focus. */
    struct wlr_pointer_constraint_v1 *active_constraint_ptr;
    /** `new_constraint` of `wlr_pointer_constraints_v1::events`. */
    struct wl_listener        new_constraint_listener;
    /** `destroy` of the active constraint. */
    struct wl_listener        constraint_destroy_listener;
    /** `focus_change` of `wlr_seat::pointer_state.events`. */
    struct wl_listener        pointer_focus_change_listener;

    /** Whether the current left-button press emulates a right button. */
    bool                      left_button_emulates_right;

    /** The output layout, for the scale of the outputs. */
    struct wlr_output_layout  *wlr_output_layout_ptr;

    /**
     * The image asked for, by the toolkit or by clients. Normally shown as
     * is; magnified while locating the pointer; replaced by an override.
     */
    _wlmim_cursor_image_t     image;
    /** Name of the XCursor image, for _WLMIM_CURSOR_IMAGE_XCURSOR. */
    char                      *xcursor_name_ptr;
    /** The client's surface, for _WLMIM_CURSOR_IMAGE_SURFACE. */
    struct wlr_surface        *surface_ptr;
    /** Hotspot within the client's surface. */
    int32_t                   surface_hotspot_x;
    /** Hotspot within the client's surface. */
    int32_t                   surface_hotspot_y;
    /** Listener for `destroy` of @ref _wlmim_cursor_t::surface_ptr. */
    struct wl_listener        surface_destroy_listener;
    /** Listener for `commit` of @ref _wlmim_cursor_t::surface_ptr. */
    struct wl_listener        surface_commit_listener;

    /** Magnification of the image. 1.0: As is. */
    double                    magnification;
    /** Largest magnification to prepare the image for. */
    double                    max_magnification;
    /** The image to magnify, in high resolution. NULL if not prepared. */
    struct wlr_buffer         *source_buffer_ptr;
    /** Pixels of @ref _wlmim_cursor_t::source_buffer_ptr per logical pixel. */
    double                    source_ratio;
    /** Hotspot of the unmagnified image, in logical pixels. */
    double                    source_hotspot_x;
    /** Hotspot of the unmagnified image, in logical pixels. */
    double                    source_hotspot_y;
    /** The XCursor theme in large size, loaded when magnifying. */
    struct wlr_xcursor_theme  *large_theme_ptr;
    /** Size @ref _wlmim_cursor_t::large_theme_ptr was loaded at. */
    int                       large_theme_size;

    /** An image replacing the requested one (eg. for selecting a window). */
    struct wlr_buffer         *override_buffer_ptr;
    /** Hotspot of the override, in logical pixels. */
    int32_t                   override_hotspot_x;
    /** Hotspot of the override, in logical pixels. */
    int32_t                   override_hotspot_y;
    /** Scale of the override buffer. */
    float                     override_scale;
};

static void _wlmim_cursor_handle_cursor_shape_manager_v1_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_cursor_shape_manager_v1_set_shape(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmim_cursor_handle_motion(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_motion_absolute(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_button(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_axis(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_frame(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmim_cursor_handle_seat_request_set_cursor(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmim_cursor_process_motion(
    wlmim_cursor_t *cursor_ptr,
    uint32_t time_msec);

static void _wlmim_cursor_handle_new_constraint(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_constraint_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_pointer_focus_change(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_set_constraint(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_pointer_constraint_v1 *constraint_ptr);
static bool _wlmim_cursor_constrained(
    wlmim_cursor_t *cursor_ptr,
    double dx,
    double dy);

static void _wlmim_cursor_request_xcursor(
    void *ud_ptr,
    const char *name_ptr);
static void _wlmim_cursor_request_surface(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_surface *wlr_surface_ptr,
    int32_t hotspot_x,
    int32_t hotspot_y);
static void _wlmim_cursor_forget_surface(wlmim_cursor_t *cursor_ptr);
static void _wlmim_cursor_handle_surface_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmim_cursor_show(wlmim_cursor_t *cursor_ptr);
static bool _wlmim_cursor_prepare_source(wlmim_cursor_t *cursor_ptr);
static void _wlmim_cursor_drop_source(wlmim_cursor_t *cursor_ptr);
static float _wlmim_cursor_max_output_scale(wlmim_cursor_t *cursor_ptr);
static struct wlr_buffer *_wlmim_cursor_buffer_from_xcursor_image(
    const struct wlr_xcursor_image *image_ptr);

char *_wlmim_cursor_get_theme_name(const struct wlmim_cursor_style *style_ptr);
static int _wlmim_cursor_theme_handler(
    void *ud_ptr,
    const char *section_ptr,
    const char *name_ptr,
    const char *value_ptr);

/* == Data ================================================================= */

const bspl_desc_t             wlmim_cursor_style_desc[] = {
    BSPL_DESC_BOOL(
        "OverrideSystemConfiguration", false, struct wlmim_cursor_style,
        override_system_configuration, override_system_configuration, false),
    BSPL_DESC_STRING(
        "Name", true, struct wlmim_cursor_style, name_ptr, name_ptr,
        "default"),
    BSPL_DESC_UINT64(
        "Size", true, struct wlmim_cursor_style, size, size, 24),
    BSPL_DESC_SENTINEL()
};

const bspl_desc_t            wlmim_cursor_options_desc[] = {
    BSPL_DESC_ENUM("EmulateRightButtonModifier", false,
                   struct wlmim_cursor_options, emulate_right_button_modifier,
                   emulate_right_button_modifier, 0,
                   wlmim_keyboard_modifiers),
    BSPL_DESC_ENUM("MoveWindowModifier", false,
                   struct wlmim_cursor_options, move_window_modifier,
                   move_window_modifier, 0,
                   wlmim_keyboard_modifiers),
    BSPL_DESC_SENTINEL(),
};

/** Name of the system-wide configuration file for cursor themes. */
static const char *_wlmim_cursor_system_config_file =
    "/usr/share/icons/default/index.theme";
/** The debian specific versino of the system-wide cursor configuration. */
static const char *_wlmim_cursor_system_config_alternative_file =
    "/etc/alternatives/x-cursor-theme";

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmim_cursor_t *wlmim_cursor_create(
    struct wl_display *wl_display_ptr,
    wlmim_t *input_manager_ptr,
    const struct wlmim_cursor_style *style_ptr,
    const struct wlmim_cursor_options *options_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_seat *wlr_seat_ptr,
    wlmtk_root_t *root_ptr)
{
    wlmim_cursor_t *cursor_ptr = logged_calloc(1, sizeof(wlmim_cursor_t));
    if (NULL == cursor_ptr) return NULL;
    cursor_ptr->input_manager_ptr = input_manager_ptr;
    cursor_ptr->wlr_seat_ptr = wlr_seat_ptr;
    cursor_ptr->root_ptr = root_ptr;
    cursor_ptr->options = *options_ptr;
    cursor_ptr->wlr_output_layout_ptr = wlr_output_layout_ptr;
    cursor_ptr->magnification = 1.0;
    cursor_ptr->max_magnification = 1.0;

    // tinywl: wlr_cursor is a utility tracking the cursor image shown on
    // screen.
    cursor_ptr->wlr_cursor_ptr = wlr_cursor_create();
    if (NULL == cursor_ptr->wlr_cursor_ptr) {
        bs_log(BS_ERROR, "Failed wlr_cursor_create()");
        wlmim_cursor_destroy(cursor_ptr);
        return NULL;
    }
    wlr_cursor_attach_output_layout(
        cursor_ptr->wlr_cursor_ptr, wlr_output_layout_ptr);

    wlmim_cursor_set_style(cursor_ptr, style_ptr);
    cursor_ptr->pointer_ptr = wlmtk_pointer_create(
        cursor_ptr->wlr_cursor_ptr,
        cursor_ptr->wlr_xcursor_manager_ptr);
    if (NULL == cursor_ptr->pointer_ptr) {
        wlmim_cursor_destroy(cursor_ptr);
        return NULL;
    }
    // The toolkit's images go through here: So we know what is shown.
    wlmtk_pointer_set_xcursor_handler(
        cursor_ptr->pointer_ptr, _wlmim_cursor_request_xcursor, cursor_ptr);

    if (NULL != wl_display_ptr) {
        cursor_ptr->wlr_cursor_shape_manager_v1_ptr =
            wlr_cursor_shape_manager_v1_create(wl_display_ptr, 1);
        if (NULL == cursor_ptr->wlr_cursor_shape_manager_v1_ptr) {
            bs_log(BS_ERROR,
                   "Failed wlr_cursor_shape_manager_v1_create(%p, 1)",
                   wl_display_ptr);
            wlmim_cursor_destroy(cursor_ptr);
            return NULL;
        }
        wlmtk_util_connect_listener_signal(
            &cursor_ptr->wlr_cursor_shape_manager_v1_ptr->events.destroy,
            &cursor_ptr->cursor_shape_manager_v1_destroy_listener,
            _wlmim_cursor_handle_cursor_shape_manager_v1_destroy);
        wlmtk_util_connect_listener_signal(
            &cursor_ptr->wlr_cursor_shape_manager_v1_ptr->events.request_set_shape,
            &cursor_ptr->cursor_shape_manager_v1_set_shape_listener,
            _wlmim_cursor_handle_cursor_shape_manager_v1_set_shape);

        cursor_ptr->wlr_relative_pointer_manager_v1_ptr =
            wlr_relative_pointer_manager_v1_create(wl_display_ptr);
        if (NULL == cursor_ptr->wlr_relative_pointer_manager_v1_ptr) {
            bs_log(BS_ERROR,
                   "Failed wlr_relative_pointer_manager_v1_create(%p)",
                   wl_display_ptr);
            wlmim_cursor_destroy(cursor_ptr);
            return NULL;
        }

        // Pointer lock and confinement: VM viewers (SPICE, VNC) and games.
        // spice-gtk locks the pointer without checking for the global and
        // crashes (wl_proxy_get_version on NULL) when it is missing.
        cursor_ptr->wlr_pointer_constraints_v1_ptr =
            wlr_pointer_constraints_v1_create(wl_display_ptr);
        if (NULL == cursor_ptr->wlr_pointer_constraints_v1_ptr) {
            bs_log(BS_ERROR,
                   "Failed wlr_pointer_constraints_v1_create(%p)",
                   wl_display_ptr);
            wlmim_cursor_destroy(cursor_ptr);
            return NULL;
        }
        wlmtk_util_connect_listener_signal(
            &cursor_ptr->wlr_pointer_constraints_v1_ptr->events.new_constraint,
            &cursor_ptr->new_constraint_listener,
            _wlmim_cursor_handle_new_constraint);
        wlmtk_util_connect_listener_signal(
            &cursor_ptr->wlr_seat_ptr->pointer_state.events.focus_change,
            &cursor_ptr->pointer_focus_change_listener,
            _wlmim_cursor_handle_pointer_focus_change);
    }

    // tinywl: wlr_cursor *only* displays an image on screen. It does not move
    // around when the pointer moves. However, we can attach input devices to
    // it, and it will generate aggregate events for all of them. In these
    // events, we can choose how we want to process them, forwarding them to
    // clients and moving the cursor around. More detail on this process is
    // described in the input handling blog post:
    //
    // https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html

    wlmtk_util_connect_listener_signal(
        &cursor_ptr->wlr_cursor_ptr->events.motion,
        &cursor_ptr->motion_listener,
        _wlmim_cursor_handle_motion);
    wlmtk_util_connect_listener_signal(
        &cursor_ptr->wlr_cursor_ptr->events.motion_absolute,
        &cursor_ptr->motion_absolute_listener,
        _wlmim_cursor_handle_motion_absolute);
    wlmtk_util_connect_listener_signal(
        &cursor_ptr->wlr_cursor_ptr->events.button,
        &cursor_ptr->button_listener,
        _wlmim_cursor_handle_button);
    wlmtk_util_connect_listener_signal(
        &cursor_ptr->wlr_cursor_ptr->events.axis,
        &cursor_ptr->axis_listener,
        _wlmim_cursor_handle_axis);
    wlmtk_util_connect_listener_signal(
        &cursor_ptr->wlr_cursor_ptr->events.frame,
        &cursor_ptr->frame_listener,
        _wlmim_cursor_handle_frame);

    wlmtk_util_connect_listener_signal(
        &cursor_ptr->wlr_seat_ptr->events.request_set_cursor,
        &cursor_ptr->seat_request_set_cursor_listener,
        _wlmim_cursor_handle_seat_request_set_cursor);

    return cursor_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmim_cursor_destroy(wlmim_cursor_t *cursor_ptr)
{
    if (NULL != cursor_ptr->active_constraint_ptr) {
        wlmtk_util_disconnect_listener(&cursor_ptr->constraint_destroy_listener);
        cursor_ptr->active_constraint_ptr = NULL;
    }
    if (NULL != cursor_ptr->wlr_pointer_constraints_v1_ptr) {
        wlmtk_util_disconnect_listener(&cursor_ptr->pointer_focus_change_listener);
        wlmtk_util_disconnect_listener(&cursor_ptr->new_constraint_listener);
        cursor_ptr->wlr_pointer_constraints_v1_ptr = NULL;
    }
    wlmtk_util_disconnect_listener(
        &cursor_ptr->seat_request_set_cursor_listener);
    if (NULL != cursor_ptr->wlr_cursor_ptr) {
        wlmtk_util_disconnect_listener(&cursor_ptr->frame_listener);
        wlmtk_util_disconnect_listener(&cursor_ptr->axis_listener);
        wlmtk_util_disconnect_listener(&cursor_ptr->button_listener);
        wlmtk_util_disconnect_listener(&cursor_ptr->motion_absolute_listener);
        wlmtk_util_disconnect_listener(&cursor_ptr->motion_listener);
    }

    if (NULL != cursor_ptr->wlr_cursor_shape_manager_v1_ptr) {
        _wlmim_cursor_handle_cursor_shape_manager_v1_destroy(
            &cursor_ptr->cursor_shape_manager_v1_destroy_listener,
            NULL);
    }

    if (NULL != cursor_ptr->pointer_ptr) {
        wlmtk_pointer_destroy(cursor_ptr->pointer_ptr);
        cursor_ptr->pointer_ptr = NULL;
    }

    _wlmim_cursor_forget_surface(cursor_ptr);
    _wlmim_cursor_drop_source(cursor_ptr);
    if (NULL != cursor_ptr->override_buffer_ptr) {
        wlr_buffer_unlock(cursor_ptr->override_buffer_ptr);
        cursor_ptr->override_buffer_ptr = NULL;
    }
    if (NULL != cursor_ptr->large_theme_ptr) {
        wlr_xcursor_theme_destroy(cursor_ptr->large_theme_ptr);
        cursor_ptr->large_theme_ptr = NULL;
    }
    if (NULL != cursor_ptr->xcursor_name_ptr) {
        free(cursor_ptr->xcursor_name_ptr);
        cursor_ptr->xcursor_name_ptr = NULL;
    }

    if (NULL != cursor_ptr->wlr_xcursor_manager_ptr) {
        wlr_xcursor_manager_destroy(cursor_ptr->wlr_xcursor_manager_ptr);
        cursor_ptr->wlr_xcursor_manager_ptr = NULL;
    }

    if (NULL != cursor_ptr->wlr_cursor_ptr) {
        wlr_cursor_destroy(cursor_ptr->wlr_cursor_ptr);
        cursor_ptr->wlr_cursor_ptr = NULL;
    }

    free(cursor_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmim_cursor_set_style(
    wlmim_cursor_t *cursor_ptr,
    const struct wlmim_cursor_style *style_ptr)
{
    char *theme_name_ptr = _wlmim_cursor_get_theme_name(style_ptr);
    if (NULL == theme_name_ptr) return false;

    struct wlr_xcursor_manager *wxm_ptr = wlr_xcursor_manager_create(
        theme_name_ptr, style_ptr->size);
    if (NULL == wxm_ptr) {
        bs_log(BS_ERROR,
               "Failed wlr_xcursor_manager_create(\"%s\", %"PRIu64")",
               theme_name_ptr, style_ptr->size);
        free(theme_name_ptr);
        return false;
    }

    if (!wlr_xcursor_manager_load(wxm_ptr, 1.0)) {
        bs_log(BS_ERROR, "Failed wlr_xcursor_manager_load() for %s, %"PRIu64,
               theme_name_ptr, style_ptr->size);
        wlr_xcursor_manager_destroy(wxm_ptr);
        free(theme_name_ptr);
        return false;
    }
    free(theme_name_ptr);

    if (NULL != cursor_ptr->pointer_ptr) {
        wlmtk_pointer_set_xcursor_manager(cursor_ptr->pointer_ptr, wxm_ptr);
    }
    // Another theme or size: The large images are prepared again.
    _wlmim_cursor_drop_source(cursor_ptr);
    if (NULL != cursor_ptr->large_theme_ptr) {
        wlr_xcursor_theme_destroy(cursor_ptr->large_theme_ptr);
        cursor_ptr->large_theme_ptr = NULL;
    }
    if (NULL != cursor_ptr->wlr_xcursor_manager_ptr) {
        wlr_xcursor_manager_destroy(cursor_ptr->wlr_xcursor_manager_ptr);
    }
    cursor_ptr->wlr_xcursor_manager_ptr = wxm_ptr;
    _wlmim_cursor_show(cursor_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
struct wlr_cursor *wlmim_cursor_wlr_cursor(wlmim_cursor_t *cursor_ptr)
{
    return cursor_ptr->wlr_cursor_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmim_cursor_image_visible(wlmim_cursor_t *cursor_ptr)
{
    return _WLMIM_CURSOR_IMAGE_NONE != cursor_ptr->image;
}

/* ------------------------------------------------------------------------- */
void wlmim_cursor_set_magnification(
    wlmim_cursor_t *cursor_ptr,
    double magnification,
    double max_magnification)
{
    if (magnification < 1.0) magnification = 1.0;
    if (max_magnification < magnification) max_magnification = magnification;
    if (max_magnification != cursor_ptr->max_magnification &&
        magnification > 1.0) {
        // Prepared for a smaller size: Prepare again, sharp.
        if (max_magnification > cursor_ptr->max_magnification) {
            _wlmim_cursor_drop_source(cursor_ptr);
        }
    }
    cursor_ptr->max_magnification = max_magnification;
    if (magnification == cursor_ptr->magnification) return;
    cursor_ptr->magnification = magnification;
    if (1.0 == magnification) {
        // Done magnifying: Nothing kept for the next time but the theme.
        _wlmim_cursor_drop_source(cursor_ptr);
    }
    _wlmim_cursor_show(cursor_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmim_cursor_set_override(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int32_t hotspot_x,
    int32_t hotspot_y,
    float scale)
{
    if (NULL != cursor_ptr->override_buffer_ptr) {
        wlr_buffer_unlock(cursor_ptr->override_buffer_ptr);
        cursor_ptr->override_buffer_ptr = NULL;
    }
    if (NULL != wlr_buffer_ptr) {
        cursor_ptr->override_buffer_ptr = wlr_buffer_lock(wlr_buffer_ptr);
        cursor_ptr->override_hotspot_x = hotspot_x;
        cursor_ptr->override_hotspot_y = hotspot_y;
        cursor_ptr->override_scale = 0 < scale ? scale : 1;
    }
    _wlmim_cursor_show(cursor_ptr);
}

/* ------------------------------------------------------------------------- */
struct wlr_buffer *wlmim_cursor_create_xcursor_buffer(
    wlmim_cursor_t *cursor_ptr,
    const char *theme_name_ptr,
    const char *const *names_ptr,
    int32_t *hotspot_x_ptr,
    int32_t *hotspot_y_ptr,
    float *scale_ptr)
{
    struct wlr_xcursor_manager *wxm_ptr = cursor_ptr->wlr_xcursor_manager_ptr;
    if (NULL == wxm_ptr) return NULL;
    float scale = _wlmim_cursor_max_output_scale(cursor_ptr);
    int size = (int)lround(wxm_ptr->size * scale);
    struct wlr_xcursor_theme *theme_ptr = wlr_xcursor_theme_load(
        NULL != theme_name_ptr ? theme_name_ptr : wxm_ptr->name, size);
    if (NULL == theme_ptr) return NULL;

    struct wlr_buffer *wlr_buffer_ptr = NULL;
    for (; NULL == wlr_buffer_ptr && NULL != *names_ptr; ++names_ptr) {
        struct wlr_xcursor *xcursor_ptr = wlr_xcursor_theme_get_cursor(
            theme_ptr, *names_ptr);
        if (NULL == xcursor_ptr || 0 == xcursor_ptr->image_count) continue;
        const struct wlr_xcursor_image *i_ptr = xcursor_ptr->images[0];
        wlr_buffer_ptr = _wlmim_cursor_buffer_from_xcursor_image(i_ptr);
        if (NULL == wlr_buffer_ptr) break;
        // The theme's nominal size may differ from the images' size: Keep
        // the image's proportion to the regular cursor.
        float s = (float)i_ptr->height / (wxm_ptr->size > 0 ? wxm_ptr->size : 24);
        if (s <= 0) s = scale;
        *scale_ptr = s;
        *hotspot_x_ptr = (int32_t)lround(i_ptr->hotspot_x / s);
        *hotspot_y_ptr = (int32_t)lround(i_ptr->hotspot_y / s);
    }
    wlr_xcursor_theme_destroy(theme_ptr);
    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmim_cursor_attach_input_device(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_input_device *wlr_input_device_ptr)
{
     wlr_cursor_attach_input_device(
         cursor_ptr->wlr_cursor_ptr,
         wlr_input_device_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmim_cursor_detach_input_device(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_input_device *wlr_input_device_ptr)
{
     wlr_cursor_detach_input_device(
         cursor_ptr->wlr_cursor_ptr,
         wlr_input_device_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Handler for `destroy` signal by `wlr_cursor_shape_manager_v1::events`.
 *
 * Called when the cursor shape manager is destroyed. This makes us clear the
 * reference in @ref wlmim_cursor_t::wlr_cursor_shape_manager_v1_ptr.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmim_cursor_handle_cursor_shape_manager_v1_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr,
        wlmim_cursor_t,
        cursor_shape_manager_v1_destroy_listener);
    if (NULL == cursor_ptr->wlr_cursor_shape_manager_v1_ptr) return;

    wlmtk_util_disconnect_listener(
        &cursor_ptr->cursor_shape_manager_v1_set_shape_listener);
    wlmtk_util_disconnect_listener(
        &cursor_ptr->cursor_shape_manager_v1_destroy_listener);
    cursor_ptr->wlr_cursor_shape_manager_v1_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for `set_shape` signal by `wlr_cursor_shape_manager_v1::events`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmim_cursor_handle_cursor_shape_manager_v1_set_shape(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr,
        wlmim_cursor_t,
        cursor_shape_manager_v1_set_shape_listener);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *e = data_ptr;

    if (NULL != wlr_cursor_shape_v1_name(e->shape)) {
        _wlmim_cursor_request_xcursor(cursor_ptr,
                                      wlr_cursor_shape_v1_name(e->shape));
    } else {
        bs_log(BS_WARNING, "Cannot resolve shape name for %d", e->shape);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `motion` event of `wlr_cursor`.
 *
 * @param listener_ptr
 * @param data_ptr Points to a `wlr_pointer_motion_event`.
 */
void _wlmim_cursor_handle_motion(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, motion_listener);
    struct wlr_pointer_motion_event *wlr_pointer_motion_event_ptr = data_ptr;

    wlmim_report_activity(cursor_ptr->input_manager_ptr);

    // Relative motion goes to the client with pointer focus, unaccelerated
    // and not clipped by output boundaries.
    if (NULL != cursor_ptr->wlr_relative_pointer_manager_v1_ptr) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            cursor_ptr->wlr_relative_pointer_manager_v1_ptr,
            cursor_ptr->wlr_seat_ptr,
            (uint64_t)wlr_pointer_motion_event_ptr->time_msec * 1000,
            wlr_pointer_motion_event_ptr->delta_x,
            wlr_pointer_motion_event_ptr->delta_y,
            wlr_pointer_motion_event_ptr->unaccel_dx,
            wlr_pointer_motion_event_ptr->unaccel_dy);
    }

    // A locked pointer stays; a confined one stays inside its region.
    if (_wlmim_cursor_constrained(
            cursor_ptr,
            wlr_pointer_motion_event_ptr->delta_x,
            wlr_pointer_motion_event_ptr->delta_y)) {
        wlr_seat_pointer_notify_frame(cursor_ptr->wlr_seat_ptr);
        return;
    }

    wlr_cursor_move(
        cursor_ptr->wlr_cursor_ptr,
        &wlr_pointer_motion_event_ptr->pointer->base,
        wlr_pointer_motion_event_ptr->delta_x,
        wlr_pointer_motion_event_ptr->delta_y);

    _wlmim_cursor_process_motion(
        cursor_ptr,
        wlr_pointer_motion_event_ptr->time_msec);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `motion_absolute` event of `wlr_cursor`.
 *
 * @param listener_ptr
 * @param data_ptr Points to a `wlr_pointer_motion_absolute_event`.
 */
void _wlmim_cursor_handle_motion_absolute(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, motion_absolute_listener);
    struct wlr_pointer_motion_absolute_event
        *wlr_pointer_motion_absolute_event_ptr = data_ptr;

    wlmim_report_activity(cursor_ptr->input_manager_ptr);

    if (NULL != cursor_ptr->active_constraint_ptr &&
        WLR_POINTER_CONSTRAINT_V1_LOCKED ==
        cursor_ptr->active_constraint_ptr->type) return;

    wlr_cursor_warp_absolute(
        cursor_ptr->wlr_cursor_ptr,
        &wlr_pointer_motion_absolute_event_ptr->pointer->base,
        wlr_pointer_motion_absolute_event_ptr->x,
        wlr_pointer_motion_absolute_event_ptr->y);

    _wlmim_cursor_process_motion(
        cursor_ptr,
        wlr_pointer_motion_absolute_event_ptr->time_msec);
}

/* ------------------------------------------------------------------------- */
/**
 * Whether a motion by (dx, dy) is held back by the active constraint: always
 * for a locked pointer, and when it would leave the region of a confined one.
 */
bool _wlmim_cursor_constrained(
    wlmim_cursor_t *cursor_ptr,
    double dx,
    double dy)
{
    struct wlr_pointer_constraint_v1 *c_ptr = cursor_ptr->active_constraint_ptr;
    if (NULL == c_ptr) return false;
    if (WLR_POINTER_CONSTRAINT_V1_LOCKED == c_ptr->type) return true;

    // Confined: the new position, in surface coordinates, stays in the region.
    struct wlr_seat_pointer_state *ps_ptr = &cursor_ptr->wlr_seat_ptr->pointer_state;
    if (ps_ptr->focused_surface != c_ptr->surface) return false;
    double sx = ps_ptr->sx + dx;
    double sy = ps_ptr->sy + dy;
    return !pixman_region32_contains_point(
        &c_ptr->region, (int)floor(sx), (int)floor(sy), NULL);
}

/* ------------------------------------------------------------------------- */
/** Makes `constraint_ptr` (or none) the active constraint. */
void _wlmim_cursor_set_constraint(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_pointer_constraint_v1 *constraint_ptr)
{
    if (cursor_ptr->active_constraint_ptr == constraint_ptr) return;

    struct wlr_pointer_constraint_v1 *old_ptr = cursor_ptr->active_constraint_ptr;
    if (NULL != old_ptr) {
        wlmtk_util_disconnect_listener(&cursor_ptr->constraint_destroy_listener);
        cursor_ptr->active_constraint_ptr = NULL;
        // May destroy a oneshot constraint.
        wlr_pointer_constraint_v1_send_deactivated(old_ptr);
    }
    if (NULL == constraint_ptr) return;

    cursor_ptr->active_constraint_ptr = constraint_ptr;
    wlmtk_util_connect_listener_signal(
        &constraint_ptr->events.destroy,
        &cursor_ptr->constraint_destroy_listener,
        _wlmim_cursor_handle_constraint_destroy);
    wlr_pointer_constraint_v1_send_activated(constraint_ptr);
}

/* ------------------------------------------------------------------------- */
/** A client asks for a constraint: active once its surface has the pointer. */
void _wlmim_cursor_handle_new_constraint(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, new_constraint_listener);
    struct wlr_pointer_constraint_v1 *constraint_ptr = data_ptr;

    if (constraint_ptr->seat == cursor_ptr->wlr_seat_ptr &&
        constraint_ptr->surface ==
        cursor_ptr->wlr_seat_ptr->pointer_state.focused_surface) {
        _wlmim_cursor_set_constraint(cursor_ptr, constraint_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** The active constraint went away (unlocked by the client, surface gone). */
void _wlmim_cursor_handle_constraint_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, constraint_destroy_listener);
    wlmtk_util_disconnect_listener(&cursor_ptr->constraint_destroy_listener);
    cursor_ptr->active_constraint_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
/** The pointer moved to another surface: its constraint, if any, applies. */
void _wlmim_cursor_handle_pointer_focus_change(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, pointer_focus_change_listener);
    struct wlr_seat_pointer_focus_change_event *e_ptr = data_ptr;

    struct wlr_pointer_constraint_v1 *constraint_ptr = NULL;
    if (NULL != e_ptr->new_surface) {
        constraint_ptr = wlr_pointer_constraints_v1_constraint_for_surface(
            cursor_ptr->wlr_pointer_constraints_v1_ptr,
            e_ptr->new_surface,
            cursor_ptr->wlr_seat_ptr);
    }
    _wlmim_cursor_set_constraint(cursor_ptr, constraint_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `button` event of `wlr_cursor`.
 *
 * @param listener_ptr
 * @param data_ptr Points to a `wlr_pointer_button_event`.
 */
void _wlmim_cursor_handle_button(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, button_listener);
    struct wlr_pointer_button_event *wlr_pointer_button_event_ptr = data_ptr;

    wlmim_report_activity(cursor_ptr->input_manager_ptr);

    struct wlr_keyboard *wlr_keyboard_ptr = wlr_seat_get_keyboard(
        cursor_ptr->wlr_seat_ptr);
    uint32_t modifiers = 0;
    if (NULL != wlr_keyboard_ptr) {
        modifiers = wlr_keyboard_get_modifiers(wlr_keyboard_ptr);
    }

    // TODO(kaeser@gubbe.ch): This deserves unit testing.
    uint32_t mod_mask = cursor_ptr->options.emulate_right_button_modifier;
    struct wlr_pointer_button_event ev = *wlr_pointer_button_event_ptr;
    if (0 != mod_mask && ev.button == BTN_LEFT) {
        if (ev.state == WL_POINTER_BUTTON_STATE_PRESSED &&
            (modifiers & mod_mask) == mod_mask) {
            ev.button = BTN_RIGHT;
            cursor_ptr->left_button_emulates_right = true;
            modifiers &= ~mod_mask;
        } else if (ev.state == WL_POINTER_BUTTON_STATE_RELEASED &&
                   cursor_ptr->left_button_emulates_right) {
            ev.button = BTN_RIGHT;
            cursor_ptr->left_button_emulates_right = false;
            modifiers &= ~mod_mask;
        }
    }

    // A filter first: eg. selecting a window to close by force.
    if (wlmim_filter_button(cursor_ptr->input_manager_ptr, &ev, modifiers)) {
        return;
    }
    wlmtk_root_pointer_button(cursor_ptr->root_ptr, &ev, modifiers);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `axis` event of `wlr_cursor`.
 *
 * @param listener_ptr
 * @param data_ptr Points to a `wlr_pointer_axis_event`.
 */
void _wlmim_cursor_handle_axis(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, axis_listener);
    struct wlr_pointer_axis_event *wlr_pointer_axis_event_ptr = data_ptr;

    wlmim_report_activity(cursor_ptr->input_manager_ptr);

    // Bindings with modifiers (eg. Ctrl+Alt+wheel to zoom) come first.
    struct wlr_keyboard *wlr_keyboard_ptr = wlr_seat_get_keyboard(
        cursor_ptr->wlr_seat_ptr);
    uint32_t modifiers = 0;
    if (NULL != wlr_keyboard_ptr) {
        modifiers = wlr_keyboard_get_modifiers(wlr_keyboard_ptr);
    }
    if (wlmim_filter_axis(cursor_ptr->input_manager_ptr,
                          wlr_pointer_axis_event_ptr,
                          modifiers)) return;

    wlmtk_root_pointer_axis(
        cursor_ptr->root_ptr,
        wlr_pointer_axis_event_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `frame` event of `wlr_cursor`.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmim_cursor_handle_frame(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, frame_listener);

    /* Notify the client with pointer focus of the frame event. */
    wlr_seat_pointer_notify_frame(cursor_ptr->wlr_seat_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `request_set_cursor` event of `wlr_seat`.
 *
 * This event is raised when a client provides a cursor image. It is accepted
 * only if the client also has the pointer focus.
 *
 * @param listener_ptr
 * @param data_ptr Points to a `wlr_seat_pointer_request_set_cursor_event`.
 */
void _wlmim_cursor_handle_seat_request_set_cursor(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, seat_request_set_cursor_listener);
    struct wlr_seat_pointer_request_set_cursor_event
        *wlr_seat_pointer_request_set_cursor_event_ptr = data_ptr;

    struct wlr_seat_client *focused_wlr_seat_client_ptr =
        cursor_ptr->wlr_seat_ptr->pointer_state.focused_client;
    if (focused_wlr_seat_client_ptr ==
        wlr_seat_pointer_request_set_cursor_event_ptr->seat_client) {
        _wlmim_cursor_request_surface(
            cursor_ptr,
            wlr_seat_pointer_request_set_cursor_event_ptr->surface,
            wlr_seat_pointer_request_set_cursor_event_ptr->hotspot_x,
            wlr_seat_pointer_request_set_cursor_event_ptr->hotspot_y);

    } else {
        bs_log(BS_WARNING, "request_set_cursor called without pointer focus.");
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Processes the cursor motion: Lookups up the view & surface under the
 * pointer and sets (respectively: clears) the pointer focus. Also sets the
 * default cursor image, if no view is given (== no client-side cursor).
 *
 * @param cursor_ptr
 * @param time_msec
 */
void _wlmim_cursor_process_motion(
    wlmim_cursor_t *cursor_ptr,
    uint32_t time_msec)
{
    wl_signal_emit_mutable(
        &wlmim_events(cursor_ptr->input_manager_ptr)->cursor_position_updated,
        cursor_ptr->wlr_cursor_ptr);

    wlmtk_root_pointer_motion(
        cursor_ptr->root_ptr,
        cursor_ptr->wlr_cursor_ptr->x,
        cursor_ptr->wlr_cursor_ptr->y,
        time_msec,
        cursor_ptr->pointer_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_pointer_xcursor_handler_t, and the cursor shape
 * requests: A named image of the XCursor theme.
 *
 * @param ud_ptr              The @ref wlmim_cursor_t.
 * @param name_ptr
 */
void _wlmim_cursor_request_xcursor(void *ud_ptr, const char *name_ptr)
{
    wlmim_cursor_t *cursor_ptr = ud_ptr;
    if (_WLMIM_CURSOR_IMAGE_XCURSOR == cursor_ptr->image &&
        NULL != cursor_ptr->xcursor_name_ptr &&
        0 == strcmp(cursor_ptr->xcursor_name_ptr, name_ptr)) {
        // Unchanged. wlroots would also skip it.
        if (NULL == cursor_ptr->override_buffer_ptr &&
            1.0 == cursor_ptr->magnification) {
            wlr_cursor_set_xcursor(cursor_ptr->wlr_cursor_ptr,
                                   cursor_ptr->wlr_xcursor_manager_ptr,
                                   name_ptr);
        }
        return;
    }
    char *n_ptr = logged_strdup(name_ptr);
    if (NULL == n_ptr) return;
    _wlmim_cursor_forget_surface(cursor_ptr);
    if (NULL != cursor_ptr->xcursor_name_ptr) free(cursor_ptr->xcursor_name_ptr);
    cursor_ptr->xcursor_name_ptr = n_ptr;
    cursor_ptr->image = _WLMIM_CURSOR_IMAGE_XCURSOR;
    _wlmim_cursor_drop_source(cursor_ptr);
    _wlmim_cursor_show(cursor_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * A client's cursor surface, or none (hidden).
 *
 * @param cursor_ptr
 * @param wlr_surface_ptr     May be NULL: Hides the cursor.
 * @param hotspot_x
 * @param hotspot_y
 */
void _wlmim_cursor_request_surface(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_surface *wlr_surface_ptr,
    int32_t hotspot_x,
    int32_t hotspot_y)
{
    if (wlr_surface_ptr != cursor_ptr->surface_ptr) {
        _wlmim_cursor_forget_surface(cursor_ptr);
        if (NULL != wlr_surface_ptr) {
            cursor_ptr->surface_ptr = wlr_surface_ptr;
            wlmtk_util_connect_listener_signal(
                &wlr_surface_ptr->events.destroy,
                &cursor_ptr->surface_destroy_listener,
                _wlmim_cursor_handle_surface_destroy);
            wlmtk_util_connect_listener_signal(
                &wlr_surface_ptr->events.commit,
                &cursor_ptr->surface_commit_listener,
                _wlmim_cursor_handle_surface_commit);
        }
    }
    cursor_ptr->surface_hotspot_x = hotspot_x;
    cursor_ptr->surface_hotspot_y = hotspot_y;
    cursor_ptr->image = NULL != wlr_surface_ptr ?
        _WLMIM_CURSOR_IMAGE_SURFACE : _WLMIM_CURSOR_IMAGE_NONE;
    _wlmim_cursor_drop_source(cursor_ptr);
    _wlmim_cursor_show(cursor_ptr);
}

/* ------------------------------------------------------------------------- */
/** Stops tracking the client's cursor surface. */
void _wlmim_cursor_forget_surface(wlmim_cursor_t *cursor_ptr)
{
    if (NULL == cursor_ptr->surface_ptr) return;
    wlmtk_util_disconnect_listener(&cursor_ptr->surface_commit_listener);
    wlmtk_util_disconnect_listener(&cursor_ptr->surface_destroy_listener);
    cursor_ptr->surface_ptr = NULL;
    if (_WLMIM_CURSOR_IMAGE_SURFACE == cursor_ptr->image) {
        cursor_ptr->image = _WLMIM_CURSOR_IMAGE_NONE;
    }
}

/* ------------------------------------------------------------------------- */
/** The client's cursor surface is gone: The cursor is hidden (as wlroots). */
void _wlmim_cursor_handle_surface_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, surface_destroy_listener);
    _wlmim_cursor_forget_surface(cursor_ptr);
    _wlmim_cursor_drop_source(cursor_ptr);
    _wlmim_cursor_show(cursor_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * The client's cursor surface got a new state: The hotspot moves with the
 * surface's offset (as wlroots does), and a magnified image is redone.
 */
void _wlmim_cursor_handle_surface_commit(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmim_cursor_t *cursor_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmim_cursor_t, surface_commit_listener);
    struct wlr_surface *s_ptr = cursor_ptr->surface_ptr;
    cursor_ptr->surface_hotspot_x -= s_ptr->current.dx;
    cursor_ptr->surface_hotspot_y -= s_ptr->current.dy;
    if (1.0 < cursor_ptr->magnification &&
        NULL == cursor_ptr->override_buffer_ptr) {
        _wlmim_cursor_drop_source(cursor_ptr);
        _wlmim_cursor_show(cursor_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Shows the image: The override, if any. Else the requested image,
 * magnified if so asked. wlroots draws it, on the hardware cursor plane
 * when the output can, else in software (also while zooming).
 */
void _wlmim_cursor_show(wlmim_cursor_t *cursor_ptr)
{
    struct wlr_cursor *c_ptr = cursor_ptr->wlr_cursor_ptr;
    if (NULL == c_ptr) return;

    if (NULL != cursor_ptr->override_buffer_ptr) {
        wlr_cursor_set_buffer(c_ptr, cursor_ptr->override_buffer_ptr,
                              cursor_ptr->override_hotspot_x,
                              cursor_ptr->override_hotspot_y,
                              cursor_ptr->override_scale);
        return;
    }

    if (1.0 < cursor_ptr->magnification &&
        _wlmim_cursor_prepare_source(cursor_ptr)) {
        double m = cursor_ptr->magnification;
        wlr_cursor_set_buffer(
            c_ptr, cursor_ptr->source_buffer_ptr,
            (int32_t)lround(cursor_ptr->source_hotspot_x * m),
            (int32_t)lround(cursor_ptr->source_hotspot_y * m),
            (float)(cursor_ptr->source_ratio / m));
        return;
    }

    switch (cursor_ptr->image) {
    case _WLMIM_CURSOR_IMAGE_XCURSOR:
        if (NULL == cursor_ptr->wlr_xcursor_manager_ptr) break;
        wlr_cursor_set_xcursor(c_ptr, cursor_ptr->wlr_xcursor_manager_ptr,
                               cursor_ptr->xcursor_name_ptr);
        break;
    case _WLMIM_CURSOR_IMAGE_SURFACE:
        wlr_cursor_set_surface(c_ptr, cursor_ptr->surface_ptr,
                               cursor_ptr->surface_hotspot_x,
                               cursor_ptr->surface_hotspot_y);
        break;
    case _WLMIM_CURSOR_IMAGE_NONE:
    default:
        wlr_cursor_unset_image(c_ptr);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Prepares the image to magnify, in high resolution: For an XCursor image,
 * the same image of the theme in the largest size needed (loaded on
 * demand: the theme in large size stays loaded until the theme changes).
 * For a client's surface, its texture (magnified by the renderer).
 *
 * @return true if @ref _wlmim_cursor_t::source_buffer_ptr is ready.
 */
bool _wlmim_cursor_prepare_source(wlmim_cursor_t *cursor_ptr)
{
    if (NULL != cursor_ptr->source_buffer_ptr) return true;

    if (_WLMIM_CURSOR_IMAGE_XCURSOR == cursor_ptr->image) {
        struct wlr_xcursor_manager *wxm_ptr =
            cursor_ptr->wlr_xcursor_manager_ptr;
        if (NULL == wxm_ptr || NULL == cursor_ptr->xcursor_name_ptr) {
            return false;
        }
        // The regular image, at scale 1: Its size in logical pixels.
        struct wlr_xcursor *n_ptr = wlr_xcursor_manager_get_xcursor(
            wxm_ptr, cursor_ptr->xcursor_name_ptr, 1.0);
        if (NULL == n_ptr) {
            n_ptr = wlr_xcursor_manager_get_xcursor(wxm_ptr, "default", 1.0);
        }
        if (NULL == n_ptr || 0 == n_ptr->image_count) return false;
        const struct wlr_xcursor_image *ni_ptr = n_ptr->images[0];

        int size = (int)lround(wxm_ptr->size * cursor_ptr->max_magnification *
                               _wlmim_cursor_max_output_scale(cursor_ptr));
        if (NULL != cursor_ptr->large_theme_ptr &&
            size != cursor_ptr->large_theme_size) {
            wlr_xcursor_theme_destroy(cursor_ptr->large_theme_ptr);
            cursor_ptr->large_theme_ptr = NULL;
        }
        if (NULL == cursor_ptr->large_theme_ptr) {
            cursor_ptr->large_theme_ptr = wlr_xcursor_theme_load(
                wxm_ptr->name, size);
            cursor_ptr->large_theme_size = size;
        }
        struct wlr_xcursor *l_ptr = NULL;
        if (NULL != cursor_ptr->large_theme_ptr) {
            l_ptr = wlr_xcursor_theme_get_cursor(
                cursor_ptr->large_theme_ptr, n_ptr->name);
        }
        // Not in large size: The regular image, magnified by the renderer.
        const struct wlr_xcursor_image *i_ptr = ni_ptr;
        if (NULL != l_ptr && 0 < l_ptr->image_count) i_ptr = l_ptr->images[0];
        cursor_ptr->source_buffer_ptr =
            _wlmim_cursor_buffer_from_xcursor_image(i_ptr);
        if (NULL == cursor_ptr->source_buffer_ptr) return false;
        cursor_ptr->source_ratio = (double)i_ptr->height / ni_ptr->height;
        cursor_ptr->source_hotspot_x =
            i_ptr->hotspot_x / cursor_ptr->source_ratio;
        cursor_ptr->source_hotspot_y =
            i_ptr->hotspot_y / cursor_ptr->source_ratio;
        return true;
    }

    if (_WLMIM_CURSOR_IMAGE_SURFACE == cursor_ptr->image) {
        struct wlr_surface *s_ptr = cursor_ptr->surface_ptr;
        struct wlr_texture *t_ptr = wlr_surface_get_texture(s_ptr);
        if (NULL == t_ptr || 0 >= s_ptr->current.width ||
            0 >= s_ptr->current.height) return false;
        struct wlr_buffer *b_ptr = bs_gfxbuf_create_wlr_buffer(
            t_ptr->width, t_ptr->height);
        if (NULL == b_ptr) return false;
        bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(b_ptr);
        if (!wlr_texture_read_pixels(
                t_ptr, &(struct wlr_texture_read_pixels_options){
                    .data = g_ptr->data_ptr,
                    .format = DRM_FORMAT_ARGB8888,
                    .stride = g_ptr->pixels_per_line * sizeof(uint32_t) })) {
            bs_log(BS_DEBUG, "Cursor: Failed to read the surface's pixels.");
            wlr_buffer_drop(b_ptr);
            return false;
        }
        cursor_ptr->source_buffer_ptr = b_ptr;
        cursor_ptr->source_ratio = (double)t_ptr->width / s_ptr->current.width;
        cursor_ptr->source_hotspot_x = cursor_ptr->surface_hotspot_x;
        cursor_ptr->source_hotspot_y = cursor_ptr->surface_hotspot_y;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/** Releases the prepared image to magnify. */
void _wlmim_cursor_drop_source(wlmim_cursor_t *cursor_ptr)
{
    wlr_buffer_drop_nullify(&cursor_ptr->source_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/** @return The largest scale of the outputs, at least 1. */
float _wlmim_cursor_max_output_scale(wlmim_cursor_t *cursor_ptr)
{
    float scale = 1.0;
    if (NULL == cursor_ptr->wlr_output_layout_ptr) return scale;
    struct wlr_output_layout_output *lo_ptr;
    wl_list_for_each(lo_ptr, &cursor_ptr->wlr_output_layout_ptr->outputs,
                     link) {
        if (lo_ptr->output->enabled && lo_ptr->output->scale > scale) {
            scale = lo_ptr->output->scale;
        }
    }
    return scale;
}

/* ------------------------------------------------------------------------- */
/** @return A copy of the XCursor image, as a buffer. */
struct wlr_buffer *_wlmim_cursor_buffer_from_xcursor_image(
    const struct wlr_xcursor_image *image_ptr)
{
    if (0 == image_ptr->width || 0 == image_ptr->height) return NULL;
    struct wlr_buffer *b_ptr = bs_gfxbuf_create_wlr_buffer(
        image_ptr->width, image_ptr->height);
    if (NULL == b_ptr) return NULL;
    bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(b_ptr);
    // XCursor images are premultiplied ARGB, as the buffer.
    for (uint32_t y = 0; y < image_ptr->height; ++y) {
        memcpy(g_ptr->data_ptr + y * g_ptr->pixels_per_line,
               image_ptr->buffer + y * image_ptr->width * 4,
               image_ptr->width * 4);
    }
    return b_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Returns a copy of the cursor theme's name to use, from configured style.
 *
 * This will either be a copy of @ref wlmim_cursor_style::name_ptr, or what is
 * described by the system-wide cursor theme files.
 *
 * @param style_ptr
 *
 * @return Cursor theme name, as string. Must be released via free().
 */
char *_wlmim_cursor_get_theme_name(const struct wlmim_cursor_style *style_ptr)
{
    char *theme_name_ptr;

    if (!style_ptr->override_system_configuration) {
        if (0 == ini_parse(_wlmim_cursor_system_config_file,
                           _wlmim_cursor_theme_handler, &theme_name_ptr) ||
            0 == ini_parse(_wlmim_cursor_system_config_alternative_file,
                           _wlmim_cursor_theme_handler, &theme_name_ptr)) {
            return theme_name_ptr;
        }
        bs_log(BS_WARNING, "System-wide cursor theme configuration not found, "
               "neither in %s nor %s. Falling back...",
               _wlmim_cursor_system_config_file,
               _wlmim_cursor_system_config_alternative_file);
    }
    return logged_strdup(style_ptr->name_ptr);
}

/* ------------------------------------------------------------------------- */
/** inih library parser callback for the system-wide cursor theme files. */
int _wlmim_cursor_theme_handler(
    void *ud_ptr,
    const char *section_ptr,
    const char *name_ptr,
    const char *value_ptr)
{
    // Skip anything other than the "Inherits" key in "[Icon Theme]".
    if (0 != strcmp(section_ptr, "Icon Theme") ||
        0 != strcmp(name_ptr, "Inherits")) return 1;

    char **theme_name_ptr_ptr = ud_ptr;
    *theme_name_ptr_ptr = logged_strdup(value_ptr);
    if (NULL == *theme_name_ptr_ptr) return 0;
    return 1;
}

/* == Unit tests =========================================================== */

static void _wlmim_cursor_test_theme_name(bs_test_t *test_ptr);
static void _wlmim_cursor_test_config(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmim_cursor_test_cases[] = {
    { 1, "theme_name", _wlmim_cursor_test_theme_name },
    { 1, "config", _wlmim_cursor_test_config },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmim_cursor_test_set = BS_TEST_SET(
    true, "cursor", _wlmim_cursor_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests obtaining the theme name. */
void _wlmim_cursor_test_theme_name(bs_test_t *test_ptr)
{
    char *theme_name_ptr;

    int rv = ini_parse(bs_test_data_path(test_ptr, "input/cursor-index.theme"),
                       _wlmim_cursor_theme_handler, &theme_name_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 0, rv);
    BS_TEST_VERIFY_STREQ(test_ptr, "ThemeName", theme_name_ptr);
    free(theme_name_ptr);

    struct wlmim_cursor_style style = {
        .override_system_configuration = true,
        .name_ptr = "OverrideName"
    };
    theme_name_ptr = _wlmim_cursor_get_theme_name(&style);
    BS_TEST_VERIFY_STREQ(test_ptr, "OverrideName", theme_name_ptr);
    free(theme_name_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests that parsing the configuration populates the options. */
void _wlmim_cursor_test_config(bs_test_t *test_ptr)
{
    bspl_dict_t *d = bspl_dict_from_object(
        bspl_create_object_from_plist_string(
            "{"
            "EmulateRightButtonModifier=Logo;"
            "MoveWindowModifier = Ctrl;"
            "}"));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);

    struct wlmim_cursor_options opt = {};
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        bspl_decode_dict(d, wlmim_cursor_options_desc, &opt));
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_MODIFIER_LOGO,
        opt.emulate_right_button_modifier);
    BS_TEST_VERIFY_EQ(
        test_ptr,
        WLR_MODIFIER_CTRL,
        opt.move_window_modifier);

    bspl_dict_unref(d);
}

/* == End of cursor.c ====================================================== */
