/* ========================================================================= */
/**
 * @file backend.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2025 Google LLC
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

#include "backend.h"

#include <errno.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <toolkit/toolkit.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#define WLR_USE_UNSTABLE
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#if defined(WLMBE_HAVE_GL_PROBE) && WLR_HAS_GLES2_RENDERER
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#endif  // defined(WLMBE_HAVE_GL_PROBE) && WLR_HAS_GLES2_RENDERER
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#undef WLR_USE_UNSTABLE

#include "output.h"
#include "output_config.h"
#include "output_manager.h"
#include "zoom.h"

/* == Declarations ========================================================= */

/** State of the server's backend. */
struct _wlmbe_backend_t {
    /** wlroots backend. */
    struct wlr_backend        *wlr_backend_ptr;
    /** wlroots session. Populated from wlr_backend_autocreate(). */
    struct wlr_session        *wlr_session_ptr;
    /** wlroots renderer. */
    struct wlr_renderer       *wlr_renderer_ptr;
    /** The allocator. */
    struct wlr_allocator      *wlr_allocator_ptr;
    /** The scene output layout. */
    struct wlr_scene_output_layout *wlr_scene_output_layout_ptr;
    /** The compositor is necessary for clients to allocate surfaces. */
    struct wlr_compositor     *wlr_compositor_ptr;
    /**
     * The subcompositor allows to assign the role of subsurfaces to
     * surfaces.
     */
    struct wlr_subcompositor  *wlr_subcompositor_ptr;
    /** The screencopy manager. */
    struct wlr_screencopy_manager_v1 *wlr_screencopy_manager_v1_ptr;
    /** Capture manager for output. wl_display will take ownership. */
    struct wlr_ext_output_image_capture_source_manager_v1 *wlr_ext_output_source_manager_ptr;
    /** Capture manager for sessions and frames. wl_display owns it */
    struct wlr_ext_image_copy_capture_manager_v1 *wlr_ext_image_copy_capture_manager_ptr;
    /** Presentation time feedback. wl_display will take ownership. */
    struct wlr_presentation   *wlr_presentation_ptr;
    /** Single-pixel buffer manager. wl_display will take ownership. */
    struct wlr_single_pixel_buffer_manager_v1 *wlr_single_pixel_buffer_manager_ptr;
    /** Gamma control manager. wl_display will take ownership. */
    struct wlr_gamma_control_manager_v1 *wlr_gamma_control_manager_ptr;
    /** Output power manager. wl_display will take ownership. */
    struct wlr_output_power_manager_v1 *wlr_output_power_manager_ptr;
    /** Listener for `set_mode` of `wlr_output_power_manager_v1::events`. */
    struct wl_listener        output_power_set_mode_listener;
    /** The output manager(s). */
    wlmbe_output_manager_t    *output_manager_ptr;
    /** The accessibility zoom of all outputs. */
    wlmbe_zoom_t              *zoom_ptr;

    /** Listener for wlr_backend::events::new_output. */
    struct wl_listener        new_output_listener;
    /** Listener for `add` of `wlr_output_layout::events`. */
    struct wl_listener        layout_add_listener;
    /**
     * Name of the primary output: The first one in the layout. When it
     * leaves the layout (unplugged, or a monitor that drops off for a
     * moment) and comes back, it is the primary output again.
     */
    char                      *primary_name_ptr;

    /** Desired output width, for windowed mode. 0 for no preference. */
    uint32_t                  width;
    /** Desired output height, for windowed mode. 0 for no preference. */
    uint32_t                  height;

    /**
     * A list of @ref wlmbe_output_config_t items, configured through
     * wlmaker's configuration file.
     *
     * Discovered outputs are attempted to matched through
     * @ref wlmbe_output_config_fnmatches for configured attributes.
     */
    bs_dllist_t               output_configs;
    /**
     * Another list of @ref wlmbe_output_config_t items. This is
     * initialized from wlmaker's state file upon startup, ie. in
     * @ref wlmbe_backend_create.
     *
     * If a discovered output equals to one of the nodes here, it's attributes
     * will be taken from here. Otherwise, a new entry is created in this list.
     * The intent is to memorize state of connected configs, so that
     * re-connected outputs are using the same attributes they left with.
     *
     * These configurations can be saved by calling
     * @ref wlmbe_backend_save_ephemeral_output_configs, otherwise any updates
     * will be lost on restart.
     */
    bs_dllist_t               ephemeral_output_configs;

    /** List of outputs. Connects @ref wlmbe_output_t::dlnode. */
    bs_dllist_t               outputs;

    // Elements below not owned by @ref wlmbe_backend_t.
    /** Back-link to the wlroots scene. */
    struct wlr_scene          *wlr_scene_ptr;
    /** Points to struct wlr_output_layout. */
    struct wlr_output_layout  *wlr_output_layout_ptr;

    /** Name of the file for storing the output's state. */
    char                      *state_fname_ptr;
};

static void _wlmbe_backend_handle_new_output(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmbe_backend_handle_output_power_set_mode(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmbe_backend_handle_layout_add(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static bool _wlmbe_backend_decode_item(
    bspl_object_t *obj_ptr,
    size_t i,
    void *dest_ptr);
static bspl_object_t *_wlmbe_backend_encode_all(
    const union bspl_desc_value *desc_value_ptr,
    const void *value_ptr);
static void _wlmbe_backend_decode_fini(void *dest_ptr);
static void _wlmbe_backend_config_dlnode_destroy(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static bool _wlmbe_autocreated_renderer_is_software(
    struct wlr_backend *wlr_backend_ptr);

/* == Data ================================================================= */

/** Descriptor for the output configuration. */
static const bspl_desc_t _wlmbe_output_configs_desc[] = {
    BSPL_DESC_ARRAY("Outputs", true, wlmbe_backend_t,
                    output_configs,
                    output_configs,
                    _wlmbe_backend_decode_item,
                    _wlmbe_backend_encode_all,
                    NULL,  // init.
                    _wlmbe_backend_decode_fini),
    BSPL_DESC_SENTINEL(),
};

/** Descriptor for ephemeral output onfigurations, stored as plist. */
static const bspl_desc_t _wlmbe_outputs_state_desc[] = {
    BSPL_DESC_ARRAY("Outputs", true, wlmbe_backend_t,
                    ephemeral_output_configs,
                    ephemeral_output_configs,
                    _wlmbe_backend_decode_item,
                    _wlmbe_backend_encode_all,
                    NULL,
                    _wlmbe_backend_decode_fini),
    BSPL_DESC_SENTINEL(),
};

/** Magnification factor: 3rd square root of 2.0. ~10, doubles exactly. */
static const double _wlmbke_backend_magnification = 1.0905077326652577;

/** The window title to set, when on a Wayland backend. */
static const char *_wlmbe_wl_title = "wlmaker";
/** The application ID to set, when on a Wayland backend. */
static const char *_wlmbe_wl_app_id = "wlmaker";

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmbe_backend_t *wlmbe_backend_create(
    struct wl_display *wl_display_ptr,
    struct wlr_scene *wlr_scene_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    int width,
    int height,
    bspl_dict_t *config_dict_ptr,
    const char *state_fname_ptr)
{
    wlmbe_backend_t *backend_ptr = logged_calloc(1, sizeof(wlmbe_backend_t));
    if (NULL == backend_ptr) return NULL;
    backend_ptr->wlr_scene_ptr = wlr_scene_ptr;
    backend_ptr->wlr_output_layout_ptr = wlr_output_layout_ptr;
    backend_ptr->width = width;
    backend_ptr->height = height;

    if (!bspl_decode_dict(
            config_dict_ptr,
            _wlmbe_output_configs_desc,
            backend_ptr)) {
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    // Loads state (if available).
    backend_ptr->state_fname_ptr = logged_strdup(state_fname_ptr);
    if (NULL == backend_ptr->state_fname_ptr) {
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    if (bs_file_realpath_is(backend_ptr->state_fname_ptr, S_IFREG)) {
        bspl_object_t *o = bspl_create_object_from_plist_file(
            backend_ptr->state_fname_ptr);
        if (NULL == o) {
            wlmbe_backend_destroy(backend_ptr);
            return NULL;
        }
        bool rv = bspl_decode_dict(
            bspl_dict_from_object(o),
            _wlmbe_outputs_state_desc,
            backend_ptr);
        bspl_object_unref(o);
        if (!rv) {
            wlmbe_backend_destroy(backend_ptr);
            return NULL;
        }
        bs_log(BS_INFO, "Loaded output state from \"%s\"", state_fname_ptr);
    }

    // Auto-create the wlroots backend. Can be X11 or direct.
    backend_ptr->wlr_backend_ptr = wlr_backend_autocreate(
        wl_display_get_event_loop(wl_display_ptr),
        &backend_ptr->wlr_session_ptr);
    if (NULL == backend_ptr->wlr_backend_ptr) {
        bs_log(BS_ERROR, "Failed wlr_backend_autocreate()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    // Auto-create a renderer. Can be specified using WLR_RENDERER env var.
    // When the automatic choice is a software rasterizer, use pixman.
    if (_wlmbe_autocreated_renderer_is_software(
            backend_ptr->wlr_backend_ptr)) {
        backend_ptr->wlr_renderer_ptr = wlr_pixman_renderer_create();
    } else {
        backend_ptr->wlr_renderer_ptr = wlr_renderer_autocreate(
            backend_ptr->wlr_backend_ptr);
    }
    if (NULL == backend_ptr->wlr_renderer_ptr) {
        bs_log(BS_ERROR, "Failed wlr_renderer_autocreate()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    if (!wlr_renderer_init_wl_display(
            backend_ptr->wlr_renderer_ptr, wl_display_ptr)) {
        bs_log(BS_ERROR, "Failed wlr_render_init_wl_display()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    backend_ptr->wlr_allocator_ptr = wlr_allocator_autocreate(
        backend_ptr->wlr_backend_ptr,
        backend_ptr->wlr_renderer_ptr);
    if (NULL == backend_ptr->wlr_allocator_ptr) {
        bs_log(BS_ERROR, "Failed wlr_allocator_autocreate()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    backend_ptr->zoom_ptr = wlmbe_zoom_create(
        wlr_scene_ptr,
        wlr_output_layout_ptr,
        backend_ptr->wlr_renderer_ptr,
        backend_ptr->wlr_allocator_ptr,
        bspl_dict_get_dict(config_dict_ptr, "Zoom"));
    if (NULL == backend_ptr->zoom_ptr) {
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    wlmtk_util_connect_listener_signal(
        &backend_ptr->wlr_output_layout_ptr->events.add,
        &backend_ptr->layout_add_listener,
        _wlmbe_backend_handle_layout_add);

    backend_ptr->wlr_scene_output_layout_ptr =
        wlr_scene_attach_output_layout(
            wlr_scene_ptr,
            backend_ptr->wlr_output_layout_ptr);
    if (NULL == backend_ptr->wlr_scene_output_layout_ptr) {
        bs_log(BS_ERROR, "Failed wlr_scene_attach_output_layout()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    backend_ptr->wlr_compositor_ptr = wlr_compositor_create(
        wl_display_ptr, 5, backend_ptr->wlr_renderer_ptr);
    if (NULL == backend_ptr->wlr_compositor_ptr) {
        bs_log(BS_ERROR, "Failed wlr_compositor_create()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    backend_ptr->wlr_subcompositor_ptr = wlr_subcompositor_create(
        wl_display_ptr);
    if (NULL == backend_ptr->wlr_subcompositor_ptr) {
        bs_log(BS_ERROR, "Failed wlr_subcompositor_create()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    // TODO(kaeser@gubbe.ch): Remove support for wlr-screencopy-unstable-v1
    // protocol. It is deprecated, and use is discouraged.
    backend_ptr->wlr_screencopy_manager_v1_ptr =
        wlr_screencopy_manager_v1_create(wl_display_ptr);
    if (NULL == backend_ptr->wlr_screencopy_manager_v1_ptr) {
        bs_log(BS_ERROR, "Failed wlr_screencopy_manager_v1_create()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    backend_ptr->wlr_ext_output_source_manager_ptr =
        wlr_ext_output_image_capture_source_manager_v1_create(
            wl_display_ptr, 1);
    if (NULL == backend_ptr->wlr_ext_output_source_manager_ptr) {
        bs_log(BS_ERROR, "Failed "
               "wlr_ext_output_image_capture_source_manager_v1_create()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    backend_ptr->wlr_ext_image_copy_capture_manager_ptr =
        wlr_ext_image_copy_capture_manager_v1_create(wl_display_ptr, 1);
    if (NULL == backend_ptr->wlr_ext_image_copy_capture_manager_ptr) {
        bs_log(BS_ERROR,
               "Failed wlr_ext_image_copy_capture_manager_v1_create()");
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    // Feedback is sent by the scene, for surfaces it renders. See wlr_scene.h.
    backend_ptr->wlr_presentation_ptr = wlr_presentation_create(
        wl_display_ptr, backend_ptr->wlr_backend_ptr, 2);
    if (NULL == backend_ptr->wlr_presentation_ptr) {
        bs_log(BS_ERROR, "Failed wlr_presentation_create(%p, %p, 2)",
               wl_display_ptr, backend_ptr->wlr_backend_ptr);
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    backend_ptr->wlr_single_pixel_buffer_manager_ptr =
        wlr_single_pixel_buffer_manager_v1_create(wl_display_ptr);
    if (NULL == backend_ptr->wlr_single_pixel_buffer_manager_ptr) {
        bs_log(BS_ERROR,
               "Failed wlr_single_pixel_buffer_manager_v1_create(%p)",
               wl_display_ptr);
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    // Gamma tables are applied by the scene, when committing the output. If
    // the output has no gamma LUT, the scene can apply it when rendering.
    backend_ptr->wlr_gamma_control_manager_ptr =
        wlr_gamma_control_manager_v1_create(wl_display_ptr);
    if (NULL == backend_ptr->wlr_gamma_control_manager_ptr) {
        bs_log(BS_ERROR, "Failed wlr_gamma_control_manager_v1_create(%p)",
               wl_display_ptr);
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    if (backend_ptr->wlr_renderer_ptr->features.output_color_transform) {
        backend_ptr->wlr_gamma_control_manager_ptr->fallback_gamma_size = 256;
    }
    wlr_scene_set_gamma_control_manager_v1(
        wlr_scene_ptr,
        backend_ptr->wlr_gamma_control_manager_ptr);

    backend_ptr->wlr_output_power_manager_ptr =
        wlr_output_power_manager_v1_create(wl_display_ptr);
    if (NULL == backend_ptr->wlr_output_power_manager_ptr) {
        bs_log(BS_ERROR, "Failed wlr_output_power_manager_v1_create(%p)",
               wl_display_ptr);
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &backend_ptr->wlr_output_power_manager_ptr->events.set_mode,
        &backend_ptr->output_power_set_mode_listener,
        _wlmbe_backend_handle_output_power_set_mode);

    backend_ptr->output_manager_ptr = wlmbe_output_manager_create(
        wl_display_ptr,
        wlr_scene_ptr,
        wlr_output_layout_ptr,
        backend_ptr->wlr_scene_output_layout_ptr,
        backend_ptr->wlr_backend_ptr,
        &backend_ptr->outputs);
    if (NULL == backend_ptr->output_manager_ptr) {
        wlmbe_backend_destroy(backend_ptr);
        return NULL;
    }

    wlmtk_util_connect_listener_signal(
        &backend_ptr->wlr_backend_ptr->events.new_output,
        &backend_ptr->new_output_listener,
        _wlmbe_backend_handle_new_output);

    return backend_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmbe_backend_destroy(wlmbe_backend_t *backend_ptr)
{
    wlmtk_util_disconnect_listener(&backend_ptr->output_power_set_mode_listener);
    wlmtk_util_disconnect_listener(&backend_ptr->new_output_listener);
    wlmtk_util_disconnect_listener(&backend_ptr->layout_add_listener);

    if (NULL != backend_ptr->output_manager_ptr) {
        wlmbe_output_manager_destroy(backend_ptr->output_manager_ptr);
        backend_ptr->output_manager_ptr = NULL;
    }

    // The outputs are destroyed with the wlr_backend, after this: They must
    // not refer to the list any longer.
    for (bs_dllist_node_t *dlnode_ptr = backend_ptr->outputs.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = backend_ptr->outputs.head_ptr) {
        wlmbe_output_set_list(wlmbe_output_from_dlnode(dlnode_ptr), NULL);
    }

    if (NULL != backend_ptr->zoom_ptr) {
        wlmbe_zoom_destroy(backend_ptr->zoom_ptr);
        backend_ptr->zoom_ptr = NULL;
    }

    if (NULL != backend_ptr->wlr_renderer_ptr) {
        wlr_renderer_destroy(backend_ptr->wlr_renderer_ptr);
        backend_ptr->wlr_renderer_ptr = NULL;
    }

     if (NULL != backend_ptr->wlr_allocator_ptr) {
        wlr_allocator_destroy(backend_ptr->wlr_allocator_ptr);
        backend_ptr->wlr_allocator_ptr = NULL;
     }

    bspl_decoded_destroy(_wlmbe_outputs_state_desc, backend_ptr);
    bspl_decoded_destroy(_wlmbe_output_configs_desc, backend_ptr);

    // @ref wlmbe_backend_t::wlr_backend_ptr is destroyed from wl_display.

    if (NULL != backend_ptr->state_fname_ptr) {
        free(backend_ptr->state_fname_ptr);
        backend_ptr->state_fname_ptr = NULL;
    }
    if (NULL != backend_ptr->primary_name_ptr) {
        free(backend_ptr->primary_name_ptr);
        backend_ptr->primary_name_ptr = NULL;
    }

    free(backend_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmbe_backend_switch_to_vt(wlmbe_backend_t *backend_ptr, unsigned vt_num)
{
    // Guard clause: @ref wlmbe_backend_t::session_ptr will be populated only
    // if wlroots created a session, eg. when running from the terminal.
    if (NULL == backend_ptr->wlr_session_ptr) {
        bs_log(BS_DEBUG, "wlmbe_backend_switch_to_vt: No session, ignored.");
        return;
    }

    if (!wlr_session_change_vt(backend_ptr->wlr_session_ptr, vt_num)) {
        bs_log(BS_WARNING, "Failed wlr_session_change_vt(, %u)", vt_num);
    }
}

/* ------------------------------------------------------------------------- */
struct wlr_backend *wlmbe_backend_wlr(wlmbe_backend_t *backend_ptr)
{
    return backend_ptr->wlr_backend_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_compositor *wlmbe_backend_compositor(wlmbe_backend_t *backend_ptr)
{
    return backend_ptr->wlr_compositor_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_output *wlmbe_primary_output(
    struct wlr_output_layout *wlr_output_layout_ptr)
{
    if (0 >= wl_list_length(&wlr_output_layout_ptr->outputs)) return NULL;

    struct wlr_output_layout_output* wolo = BS_CONTAINER_OF(
        wlr_output_layout_ptr->outputs.next,
        struct wlr_output_layout_output,
        link);
    return wolo->output;
}

/* ------------------------------------------------------------------------- */
size_t wlmbe_num_outputs(struct wlr_output_layout *wlr_output_layout_ptr)
{
    return wl_list_length(&wlr_output_layout_ptr->outputs);
}

/* ------------------------------------------------------------------------- */
void wlmbe_backend_magnify(wlmbe_backend_t *backend_ptr)
{
    wlmbe_output_manager_scale(
        backend_ptr->output_manager_ptr,
        _wlmbke_backend_magnification);
}

/* ------------------------------------------------------------------------- */
void wlmbe_backend_reduce(wlmbe_backend_t *backend_ptr)
{
    wlmbe_output_manager_scale(
        backend_ptr->output_manager_ptr,
        1.0 / _wlmbke_backend_magnification);
}

/* ------------------------------------------------------------------------- */
wlmbe_zoom_t *wlmbe_backend_zoom(wlmbe_backend_t *backend_ptr)
{
    return backend_ptr->zoom_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlmbe_backend_save_ephemeral_output_configs(wlmbe_backend_t *backend_ptr)
{
    bspl_dict_t *dict_ptr = bspl_encode_dict(
        _wlmbe_outputs_state_desc,
        backend_ptr);
    if (NULL != dict_ptr) {
        bs_dynbuf_t dynbuf = {};
        if (bs_dynbuf_init(&dynbuf, getpagesize(), SIZE_MAX)) {
            if (bspl_object_write(
                    bspl_object_from_dict(dict_ptr),
                    &dynbuf) &&
                bs_dynbuf_write_file(
                    &dynbuf,
                    backend_ptr->state_fname_ptr,
                    S_IRUSR | S_IWUSR)) {
                return true;
            }
            bs_dynbuf_fini(&dynbuf);
        }
        bspl_dict_unref(dict_ptr);
    }
    return false;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Adds the output to the backend.
 *
 * @param backend_ptr
 * @param output_ptr
 *
 * @return true on success.
 */
bool _wlmbe_backend_add_output(
    wlmbe_backend_t *backend_ptr,
    wlmbe_output_t *output_ptr)
{
    const wlmbe_output_config_attributes_t *attr_ptr =
        wlmbe_output_attributes(output_ptr);
    BS_ASSERT(NULL != attr_ptr);

    struct wlr_output *wlrop = wlmbe_wlr_output_from_output(output_ptr);

    // In the list before entering the layout: The layout's change event
    // makes the output manager list all outputs to its clients.
    wlmbe_output_set_list(output_ptr, &backend_ptr->outputs);

    // A disabled output stays out of the layout, but is kept: Output
    // management clients list it, and may enable it.
    if (!wlrop->enabled) {
        wlmbe_output_manager_update(backend_ptr->output_manager_ptr);
        bs_log(BS_INFO, "Created: Output <%s> disabled",
               wlmbe_output_description(output_ptr));
        return true;
    }

    struct wlr_output_layout_output *wlr_output_layout_output_ptr = NULL;
    if (attr_ptr->has_position) {
        wlr_output_layout_output_ptr = wlr_output_layout_add(
            backend_ptr->wlr_output_layout_ptr, wlrop,
            attr_ptr->position.x, attr_ptr->position.y);
    } else {
        wlr_output_layout_output_ptr = wlr_output_layout_add_auto(
            backend_ptr->wlr_output_layout_ptr, wlrop);
    }
    if (NULL == wlr_output_layout_output_ptr) {
        bs_log(BS_WARNING,
               "Failed wlr_output_layout_add(%p, %p, %d, %d) for \"%s\"",
               backend_ptr->wlr_output_layout_ptr, wlrop,
               attr_ptr->position.x, attr_ptr->position.y,
               wlrop->name);
        return false;
    }

    struct wlr_scene_output *wlr_scene_output_ptr = wlr_scene_output_create(
        backend_ptr->wlr_scene_ptr, wlrop);
    wlr_scene_output_layout_add_output(
        backend_ptr->wlr_scene_output_layout_ptr,
        wlr_output_layout_output_ptr,
        wlr_scene_output_ptr);

    bs_log(BS_INFO,
           "Created: Output <%s> %s to %dx%d@%.2f x%.2f position (%d,%d) %s",
           wlmbe_output_description(output_ptr),
           wlrop->enabled ? "enabled" : "disabled",
           wlrop->width, wlrop->height,
           1e-3 * wlrop->refresh,
           wlrop->scale,
           wlr_output_layout_output_ptr->x,
           wlr_output_layout_output_ptr->y,
           attr_ptr->has_position ? "explicit" : "auto");
    return true;
}

/* ------------------------------------------------------------------------- */
/** Handles new output events: Creates @ref wlmbe_output_t and adds them.  */
void _wlmbe_backend_handle_new_output(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmbe_backend_t *backend_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmbe_backend_t, new_output_listener);
    struct wlr_output *wlr_output_ptr = data_ptr;

    // New output: If it's using a Wayland backend, set title & App ID.
    if (wlr_output_is_wl(wlr_output_ptr)) {
        wlr_wl_output_set_app_id(wlr_output_ptr, _wlmbe_wl_app_id);
        wlr_wl_output_set_title(wlr_output_ptr, _wlmbe_wl_title);
    }

    // See if there is an exact match among the ephemeral output configs. If
    // yes, pick that configuration. Otherwise, create a new one. An output
    // explicitly configured as disabled is created disabled (and listed for
    // output management clients, which can enable it again).
    wlmbe_output_config_t *config_ptr = wlmbe_output_config_from_dlnode(
        bs_dllist_find(
            &backend_ptr->ephemeral_output_configs,
            wlmbe_output_config_equals,
            wlr_output_ptr));
    if (NULL == config_ptr) {
        config_ptr = wlmbe_output_config_create_from_wlr(wlr_output_ptr);
        if (NULL == config_ptr) {
            wlr_output_destroy(wlr_output_ptr);
            return;
        }
        wlmbe_output_config_init_discovered(config_ptr, wlr_output_ptr);
        bs_dllist_push_front(
            &backend_ptr->ephemeral_output_configs,
            wlmbe_dlnode_from_output_config(config_ptr));

        // See if we have a corresponding entry among configured outputs. If
        // yes, apply the attributes to our new config.
        wlmbe_output_config_t *outputs_config_ptr =
            wlmbe_output_config_from_dlnode(
                bs_dllist_find(
                    &backend_ptr->output_configs,
                    wlmbe_output_config_fnmatches,
                    wlr_output_ptr));
        if (NULL != outputs_config_ptr) {
            wlmbe_output_config_apply_attributes(
                config_ptr,
            wlmbe_output_config_attributes(outputs_config_ptr));
        }
    }

    wlmbe_output_t *output_ptr = wlmbe_output_create(
        wlr_output_ptr,
        backend_ptr->wlr_allocator_ptr,
        backend_ptr->wlr_renderer_ptr,
        backend_ptr->wlr_scene_ptr,
        backend_ptr->zoom_ptr,
        config_ptr,
        backend_ptr->width,
        backend_ptr->height);
    if (NULL != output_ptr) {
        if (_wlmbe_backend_add_output(backend_ptr, output_ptr)) return;
        wlmbe_output_destroy(output_ptr);
    }
    wlr_output_destroy(wlr_output_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles `add` of `wlr_output_layout`: The first output to join the layout
 * is the primary one (the Dock, the Clip, ...). wlroots appends an output
 * that joins again (re-plugged, a monitor waking up, enabled through output
 * management) at the end of the layout: Then the primary output is put
 * first again, and the layout's change is told once more, for those that
 * follow the primary output.
 *
 * @param listener_ptr
 * @param data_ptr            Points to struct wlr_output_layout_output.
 */
void _wlmbe_backend_handle_layout_add(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmbe_backend_t *backend_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmbe_backend_t, layout_add_listener);
    struct wlr_output_layout_output *wolo_ptr = data_ptr;
    struct wlr_output_layout *layout_ptr = backend_ptr->wlr_output_layout_ptr;

    if (NULL == backend_ptr->primary_name_ptr) {
        backend_ptr->primary_name_ptr = logged_strdup(wolo_ptr->output->name);
        return;
    }
    if (0 != strcmp(backend_ptr->primary_name_ptr, wolo_ptr->output->name) ||
        layout_ptr->outputs.next == &wolo_ptr->link) return;

    wl_list_remove(&wolo_ptr->link);
    wl_list_insert(&layout_ptr->outputs, &wolo_ptr->link);
    bs_log(BS_INFO, "Output \"%s\" is the primary output again.",
           wolo_ptr->output->name);
    wl_signal_emit_mutable(&layout_ptr->events.change, layout_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Handles `set_mode` of `wlr_output_power_manager_v1`: Powers the output
 * off or on, by disabling or enabling it.
 *
 * Only applies to outputs that are part of the output layout: Outputs that
 * were disabled through configuration are not powered on. The output remains
 * in the layout while powered off, so that windows remain where they are.
 *
 * @param listener_ptr
 * @param data_ptr            Points to struct wlr_output_power_v1_set_mode_event.
 */
void _wlmbe_backend_handle_output_power_set_mode(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmbe_backend_t *backend_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmbe_backend_t, output_power_set_mode_listener);
    struct wlr_output_power_v1_set_mode_event *event_ptr = data_ptr;
    struct wlr_output *wlr_output_ptr = event_ptr->output;
    bool enable = ZWLR_OUTPUT_POWER_V1_MODE_ON == event_ptr->mode;

    if (NULL == wlr_output_layout_get(backend_ptr->wlr_output_layout_ptr,
                                      wlr_output_ptr)) {
        bs_log(BS_INFO, "Output power: \"%s\" not in layout, ignored.",
               wlr_output_ptr->name);
        return;
    }
    if (enable == wlr_output_ptr->enabled) return;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enable);
    if (!wlr_output_commit_state(wlr_output_ptr, &state)) {
        bs_log(BS_WARNING, "Output power: Failed to power %s \"%s\"",
               enable ? "on" : "off", wlr_output_ptr->name);
    } else {
        bs_log(BS_INFO, "Output power: Powered %s \"%s\"",
               enable ? "on" : "off", wlr_output_ptr->name);
    }
    wlr_output_state_finish(&state);
}

/* ------------------------------------------------------------------------- */
/** Decodes an item of `Outputs`. */
bool _wlmbe_backend_decode_item(
    bspl_object_t *obj_ptr,
    size_t i,
    void *dest_ptr)
{
    bs_dllist_t *dllist_ptr = dest_ptr;
    bspl_dict_t *dict_ptr = bspl_dict_from_object(obj_ptr);
    if (NULL == dict_ptr) {
        bs_log(BS_WARNING, "Element %zu is not a dict", i);
        return false;
    }

    wlmbe_output_config_t *config_ptr =
        wlmbe_output_config_create_from_plist(dict_ptr);
    if (NULL != config_ptr) {
        bs_dllist_push_back(
            dllist_ptr,
            wlmbe_dlnode_from_output_config(config_ptr));

        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/** Encodes the dllist item and stores it into the array at `ud_ptr`. */
bool _wlmbe_backend_encode_item(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr)
{
    bspl_array_t *array_ptr = ud_ptr;

    wlmbe_output_config_t *c = wlmbe_output_config_from_dlnode(dlnode_ptr);

    bspl_dict_t *dict_ptr = wlmbe_output_config_create_into_plist(c);
    if (NULL == dict_ptr) return false;

    bool rv = bspl_array_push_back(array_ptr, bspl_object_from_dict(dict_ptr));
    bspl_dict_unref(dict_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** Encodes all array (items) into a plist array. */
bspl_object_t *_wlmbe_backend_encode_all(
    __UNUSED__ const union bspl_desc_value *desc_value_ptr,
    const void *value_ptr)
{
    const bs_dllist_t *output_configs_ptr = value_ptr;

    bspl_array_t *array_ptr = bspl_array_create();
    if (NULL != array_ptr &&
        bs_dllist_all(
            output_configs_ptr,
            _wlmbe_backend_encode_item,
            array_ptr)) {
        return bspl_object_from_array(array_ptr);
    }

    bspl_array_unref(array_ptr);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Frees all resources of the @ref wlmbe_output_config_t in the list. */
void _wlmbe_backend_decode_fini(void *dest_ptr)
{
    bs_dllist_t *dllist_ptr = dest_ptr;
    bs_dllist_for_each(dllist_ptr,
        _wlmbe_backend_config_dlnode_destroy,
        NULL);
}

/* ------------------------------------------------------------------------- */
/**
 * Iterator callback: Destroys a @ref wlmbe_output_config_t.
 *
 * @param dlnode_ptr          To @ref wlmbe_output_config_t::dlnode, and
 *                            identifies the config node to destroy.
 * @param ud_ptr              unused.
 */
void _wlmbe_backend_config_dlnode_destroy(
    bs_dllist_node_t *dlnode_ptr,
    __UNUSED__ void *ud_ptr)
{
    wlmbe_output_config_destroy(wlmbe_output_config_from_dlnode(dlnode_ptr));
}

/* ------------------------------------------------------------------------- */
/**
 * Returns whether the renderer that wlroots would choose automatically is
 * OpenGL ES on a software rasterizer (Mesa's llvmpipe or softpipe), as on
 * virtual machines without 3D acceleration, or GPUs without a Mesa driver.
 *
 * There, compositing with OpenGL ES costs more than with pixman: Each client
 * buffer is copied into a texture, and the rasterizer's libraries (eg.
 * libLLVM) stay loaded and resident. pixman reads the clients' buffers
 * directly. Measured on a QEMU VM (virtio-gpu without 3D, 1920x1080): The
 * compositor's PSS drops from 109 MiB to 28 MiB.
 *
 * The probe runs in a child process, so that the rasterizer's libraries
 * never get loaded into the compositor. With `WLR_RENDERER` set, wlroots'
 * choice is respected and nothing is probed.
 *
 * @param wlr_backend_ptr
 *
 * @return true if the automatically created renderer would be software GL.
 */
bool _wlmbe_autocreated_renderer_is_software(
    __UNUSED__ struct wlr_backend *wlr_backend_ptr)
{
#if defined(WLMBE_HAVE_GL_PROBE) && WLR_HAS_GLES2_RENDERER
    if (NULL != getenv("WLR_RENDERER")) return false;

    int fds[2];
    if (0 != pipe(fds)) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed pipe()");
        return false;
    }
    pid_t pid = fork();
    if (0 > pid) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed fork()");
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    if (0 == pid) {
        // Child: Only creates the renderer, and reports. No cleanup, the
        // parent owns everything inherited.
        close(fds[0]);
        char result = 'h';
        struct wlr_renderer *wlr_renderer_ptr = wlr_renderer_autocreate(
            wlr_backend_ptr);
        if (NULL != wlr_renderer_ptr && wlr_renderer_is_gles2(wlr_renderer_ptr)) {
            struct wlr_egl *wlr_egl_ptr = wlr_gles2_renderer_get_egl(
                wlr_renderer_ptr);
            if (eglMakeCurrent(wlr_egl_get_display(wlr_egl_ptr),
                               EGL_NO_SURFACE, EGL_NO_SURFACE,
                               wlr_egl_get_context(wlr_egl_ptr))) {
                const char *r = (const char*)glGetString(GL_RENDERER);
                if (NULL != r && (NULL != strstr(r, "llvmpipe") ||
                                  NULL != strstr(r, "softpipe"))) {
                    result = 's';
                }
            }
        }
        if (1 != write(fds[1], &result, 1)) _exit(1);
        _exit(0);
    }

    close(fds[1]);
    char result = 0;
    ssize_t rv;
    do {
        rv = read(fds[0], &result, 1);
    } while (0 > rv && EINTR == errno);
    close(fds[0]);
    while (0 > waitpid(pid, NULL, 0) && EINTR == errno) {}

    if ('s' == result) {
        bs_log(BS_INFO, "OpenGL ES would render in software: Using pixman. "
               "Set WLR_RENDERER to override.");
        return true;
    }
#endif  // defined(WLMBE_HAVE_GL_PROBE) && WLR_HAS_GLES2_RENDERER
    return false;
}

/* == Unit tests =========================================================== */

static void _wlmbe_backend_test_find(bs_test_t *test_ptr);
static void _wlmbe_backend_test_encode_output_configs(bs_test_t *test_ptr);
static void _wlmbe_backend_test_disabled_output(bs_test_t *test_ptr);

/** Test cases */
static const bs_test_case_t _wlmbe_backend_test_cases[] = {
    { 1, "find", _wlmbe_backend_test_find },
    { 1, "encode_output_configs", _wlmbe_backend_test_encode_output_configs },
    { 1, "disabled_output", _wlmbe_backend_test_disabled_output },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmbe_backend_test_set = BS_TEST_SET(
    true, "backend", _wlmbe_backend_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests that output configurations are found as desired. */
void _wlmbe_backend_test_find(bs_test_t *test_ptr)
{
    wlmbe_backend_t be = {};
    bspl_dict_t *config_dict_ptr = bspl_dict_from_object(
        bspl_create_object_from_plist_string(
            "{Outputs = ("
            "{Transformation=Normal; Scale=1.0; Name=\"DP-*\"},"
            ")}"));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, config_dict_ptr);
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr,
        bspl_decode_dict(config_dict_ptr, _wlmbe_output_configs_desc, &be));

    bspl_dict_t *state_dict_ptr = bspl_dict_from_object(
        bspl_create_object_from_plist_string(
            "{Outputs = ("
            "{Transformation=Normal; Scale=2.0; Name=\"DP-0\"},"
            "{Transformation=Flip; Scale=3.0; Name=\"DP-1\"},"
            ")}"));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, state_dict_ptr);
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr,
        bspl_decode_dict(state_dict_ptr, _wlmbe_outputs_state_desc, &be));
    BS_TEST_VERIFY_EQ(
        test_ptr, 2, bs_dllist_size(&be.ephemeral_output_configs));

    wlmbe_output_config_t *o;
    struct wlr_output wlr_output = {};

    // These are found in the configured state.
    wlr_output.name = "DP-0";
    o = wlmbe_output_config_from_dlnode(
        bs_dllist_find(
            &be.ephemeral_output_configs,
            wlmbe_output_config_equals,
            &wlr_output));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    BS_TEST_VERIFY_EQ(test_ptr, 2.0, wlmbe_output_config_attributes(o)->scale);


    wlr_output.name = "DP-1";
    o = wlmbe_output_config_from_dlnode(
        bs_dllist_find(
            &be.ephemeral_output_configs,
            wlmbe_output_config_equals,
            &wlr_output));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    BS_TEST_VERIFY_EQ(test_ptr, 3.0, wlmbe_output_config_attributes(o)->scale);

    // Only has a fit through config match. Will add a state entry.
    wlr_output.name = "DP-2";
    o = wlmbe_output_config_from_dlnode(
        bs_dllist_find(
            &be.output_configs,
            wlmbe_output_config_fnmatches,
            &wlr_output));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    BS_TEST_VERIFY_EQ(test_ptr, 1.0, wlmbe_output_config_attributes(o)->scale);

    bspl_decoded_destroy(_wlmbe_outputs_state_desc, &be);
    bspl_decoded_destroy(_wlmbe_output_configs_desc, &be);
    bspl_dict_unref(state_dict_ptr);
    bspl_dict_unref(config_dict_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests encoding of the ephemeral output configs */
void _wlmbe_backend_test_encode_output_configs(bs_test_t *test_ptr)
{
    wlmbe_backend_t backend = {};

    struct wlr_output w = {
        .name = "Name0",
        .width = 1024,
        .height = 768,
        .scale = 2.0,
        .refresh = 60000,
    };

    wlmbe_output_config_t *c1 = wlmbe_output_config_create_from_wlr(&w);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, c1);
    bs_dllist_push_back(
        &backend.ephemeral_output_configs,
        wlmbe_dlnode_from_output_config(c1));

    w = (struct wlr_output){
        .enabled = true,
        .name = "Name1",
        .width = 640,
        .height = 480,
        .scale = 1.0,
        .refresh = 80000
    };
    wlmbe_output_config_t *c2 = wlmbe_output_config_create_from_wlr(&w);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, c2);
    bs_dllist_push_back(
        &backend.ephemeral_output_configs,
        wlmbe_dlnode_from_output_config(c2));


    bspl_dict_t *dict_ptr = bspl_encode_dict(
        _wlmbe_outputs_state_desc,
        &backend);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dict_ptr);

    bs_dynbuf_t dynbuf = {};
    BS_TEST_VERIFY_TRUE(test_ptr, bs_dynbuf_init(&dynbuf, 1024, SIZE_MAX));
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        bspl_object_write(bspl_object_from_dict(dict_ptr), &dynbuf));

    static const char *expected =
        "{\n"
        "  Outputs = (\n"
        "    {\n"
        "      Enabled = False;\n"
        "      Mode = \"1024x768@60.0\";\n"
        "      Name = Name0;\n"
        "      Scale = \"2.000000e+00\";\n"
        "      Transformation = Normal;\n"
        "    },\n"
        "    {\n"
        "      Enabled = True;\n"
        "      Mode = \"640x480@80.0\";\n"
        "      Name = Name1;\n"
        "      Scale = \"1.000000e+00\";\n"
        "      Transformation = Normal;\n"
        "    }\n"
        "  );\n"
        "}\n";
    BS_TEST_VERIFY_MEMEQ(test_ptr, expected, dynbuf.data_ptr, dynbuf.length);

    bs_dynbuf_fini(&dynbuf);
    bspl_dict_unref(dict_ptr);
    wlmbe_output_config_destroy(c2);
    wlmbe_output_config_destroy(c1);
}

/* ------------------------------------------------------------------------- */
/** Callback for wlr_multi_for_each_backend: Finds the headless backend. */
static void _wlmbe_backend_test_find_headless(
    struct wlr_backend *wlr_backend_ptr,
    void *ud_ptr)
{
    if (wlr_backend_is_headless(wlr_backend_ptr)) {
        *(struct wlr_backend**)ud_ptr = wlr_backend_ptr;
    }
}

/** Returns the head of `wlr_output_ptr` in the output manager, or NULL. */
static struct wlr_output_head_v1 *_wlmbe_backend_test_head(
    wlmbe_backend_t *backend_ptr,
    struct wlr_output *wlr_output_ptr)
{
    struct wlr_output_manager_v1 *m = wlmbe_output_manager_wlr(
        backend_ptr->output_manager_ptr);
    struct wlr_output_head_v1 *head_ptr;
    wl_list_for_each(head_ptr, &m->heads, link) {
        if (head_ptr->state.output == wlr_output_ptr) return head_ptr;
    }
    return NULL;
}

/** Applies a configuration with `wlr_output_ptr` enabled or not, at x. */
static bool _wlmbe_backend_test_configure(
    wlmbe_backend_t *backend_ptr,
    struct wlr_output *wlr_output_ptr,
    bool enabled,
    int x)
{
    struct wlr_output_configuration_v1 *c =
        wlr_output_configuration_v1_create();
    for (bs_dllist_node_t *dlnode_ptr = backend_ptr->outputs.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        struct wlr_output *o = wlmbe_wlr_output_from_output(
            wlmbe_output_from_dlnode(dlnode_ptr));
        struct wlr_output_configuration_head_v1 *h =
            wlr_output_configuration_head_v1_create(c, o);
        if (o == wlr_output_ptr) {
            h->state.enabled = enabled;
            h->state.x = x;
        } else {
            struct wlr_box box;
            wlr_output_layout_get_box(
                backend_ptr->wlr_output_layout_ptr, o, &box);
            h->state.x = box.x;
            h->state.y = box.y;
        }
    }
    bool rv = wlmbe_output_manager_apply(backend_ptr->output_manager_ptr, c);
    wlr_output_configuration_v1_destroy(c);
    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Tests that a disabled output stays listed to output management clients,
 * can be enabled again (rendered at its new position), and that an output
 * configured as disabled is created disabled instead of being destroyed.
 */
void _wlmbe_backend_test_disabled_output(bs_test_t *test_ptr)
{
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    struct wl_display *wl_display_ptr = wl_display_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wl_display_ptr);
    struct wlr_scene *wlr_scene_ptr = wlr_scene_create();
    struct wlr_output_layout *wlr_output_layout_ptr =
        wlr_output_layout_create(wl_display_ptr);
    bspl_object_t *config_ptr = bspl_create_object_from_plist_string(
        "{Outputs = ();}");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, config_ptr);
    bspl_dict_t *config_dict_ptr = bspl_dict_from_object(config_ptr);
    bspl_object_t *state_ptr = bspl_create_object_from_plist_string(
        "{Outputs = ({Name = \"HEADLESS-4\"; Enabled = False; "
        "Transformation = Normal; Scale = 1.0;});}");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, state_ptr);

    wlmbe_backend_t *be = wlmbe_backend_create(
        wl_display_ptr, wlr_scene_ptr, wlr_output_layout_ptr, 0, 0,
        config_dict_ptr, "/nonexistent/OutputState.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, be);
    BS_TEST_VERIFY_TRUE(
        test_ptr,
        bspl_decode_dict(bspl_dict_from_object(state_ptr),
                         _wlmbe_outputs_state_desc, be));
    struct wlr_backend *headless_ptr = NULL;
    wlr_multi_for_each_backend(
        be->wlr_backend_ptr, _wlmbe_backend_test_find_headless, &headless_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, headless_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, wlr_backend_start(be->wlr_backend_ptr));

    struct wlr_output *a = wlr_headless_add_output(headless_ptr, 640, 480);
    struct wlr_output *b = wlr_headless_add_output(headless_ptr, 800, 600);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, a);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, b);
    // The headless backend starts with one output of its own.
    BS_TEST_VERIFY_EQ(test_ptr, 3, bs_dllist_size(&be->outputs));
    BS_TEST_VERIFY_TRUE(test_ptr, a->enabled);
    BS_TEST_VERIFY_TRUE(test_ptr, b->enabled);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, _wlmbe_backend_test_head(be, b));

    // Disable 'b': Leaves the layout, but stays listed (disabled).
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, b, false, 0));
    BS_TEST_VERIFY_FALSE(test_ptr, b->enabled);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlr_output_layout_get(
                          wlr_output_layout_ptr, b));
    struct wlr_output_head_v1 *h = _wlmbe_backend_test_head(be, b);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, h);
    BS_TEST_VERIFY_FALSE(test_ptr, h->state.enabled);

    // Enable 'b' again, at 2000: In the layout, scene output follows.
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, b, true, 2000));
    BS_TEST_VERIFY_TRUE(test_ptr, b->enabled);
    struct wlr_box box;
    wlr_output_layout_get_box(wlr_output_layout_ptr, b, &box);
    BS_TEST_VERIFY_EQ(test_ptr, 2000, box.x);
    struct wlr_scene_output *so = wlr_scene_get_scene_output(wlr_scene_ptr, b);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, so);
    BS_TEST_VERIFY_EQ(test_ptr, 2000, so->x);
    h = _wlmbe_backend_test_head(be, b);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, h);
    BS_TEST_VERIFY_TRUE(test_ptr, h->state.enabled);
    BS_TEST_VERIFY_EQ(test_ptr, 2000, h->state.x);

    // An output configured as disabled: Created disabled, listed, enabled.
    struct wlr_output *c = wlr_headless_add_output(headless_ptr, 640, 480);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, c);
    BS_TEST_VERIFY_STREQ(test_ptr, "HEADLESS-4", c->name);
    BS_TEST_VERIFY_EQ(test_ptr, 4, bs_dllist_size(&be->outputs));
    BS_TEST_VERIFY_FALSE(test_ptr, c->enabled);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlr_output_layout_get(
                          wlr_output_layout_ptr, c));
    h = _wlmbe_backend_test_head(be, c);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, h);
    BS_TEST_VERIFY_FALSE(test_ptr, h->state.enabled);
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, c, true, 3000));
    BS_TEST_VERIFY_TRUE(test_ptr, c->enabled);
    so = wlr_scene_get_scene_output(wlr_scene_ptr, c);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, so);
    BS_TEST_VERIFY_EQ(test_ptr, 3000, so->x);

    // The primary output (the headless backend's own, the first) leaves
    // the layout and comes back: It is the primary output again.
    struct wlr_output *p = wlmbe_primary_output(wlr_output_layout_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, p, false, 0));
    BS_TEST_VERIFY_NEQ(test_ptr, p, wlmbe_primary_output(wlr_output_layout_ptr));
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, p, true, 0));
    BS_TEST_VERIFY_EQ(test_ptr, p, wlmbe_primary_output(wlr_output_layout_ptr));
    // Another output coming back stays behind it.
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, b, false, 0));
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmbe_backend_test_configure(be, b, true, 2000));
    BS_TEST_VERIFY_EQ(test_ptr, p, wlmbe_primary_output(wlr_output_layout_ptr));

    // Unplugging an output removes it from the list and from the heads.
    wlr_output_destroy(a);
    BS_TEST_VERIFY_EQ(test_ptr, 3, bs_dllist_size(&be->outputs));
    wlmbe_output_manager_update(be->output_manager_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, _wlmbe_backend_test_head(be, a));

    bspl_object_unref(state_ptr);
    bspl_object_unref(config_ptr);
    wlmbe_backend_destroy(be);
    wl_display_destroy_clients(wl_display_ptr);
    wl_display_destroy(wl_display_ptr);
    wlr_scene_node_destroy(&wlr_scene_ptr->tree.node);
}

/* == End of backend.c ===================================================== */
