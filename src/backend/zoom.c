/* ========================================================================= */
/**
 * @file zoom.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
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

#include "zoom.h"

#include <libbase/libbase.h>
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** State of the zoom. */
struct _wlmbe_zoom_t {
    /** Configuration. */
    wlmbe_zoom_config_t       config;

    /** The level shown, animated towards @ref _wlmbe_zoom_t::target. */
    double                    level;
    /** The level to reach. */
    double                    target;
    /** Time of the last level advance, CLOCK_MONOTONIC msec. */
    uint64_t                  last_msec;

    // Elements below not owned by @ref wlmbe_zoom_t.
    /** The scene. */
    struct wlr_scene          *wlr_scene_ptr;
    /** The output layout. */
    struct wlr_output_layout  *wlr_output_layout_ptr;
    /** The renderer. */
    struct wlr_renderer       *wlr_renderer_ptr;
    /** The allocator. */
    struct wlr_allocator      *wlr_allocator_ptr;
    /** The cursor to follow. May be NULL. */
    struct wlr_cursor         *wlr_cursor_ptr;
};

static void _wlmbe_zoom_advance(wlmbe_zoom_t *zoom_ptr);
static void _wlmbe_zoom_schedule_frames(wlmbe_zoom_t *zoom_ptr);
static void _wlmbe_zoom_damage_whole(struct wlr_output *wlr_output_ptr);
static void _wlmbe_zoom_output_deactivate(
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_output *wlr_output_ptr);
static bool _wlmbe_zoom_output_render(
    wlmbe_zoom_t *zoom_ptr,
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_scene_output *wlr_scene_output_ptr,
    const struct wlr_box *box_ptr);
static uint64_t _wlmbe_zoom_now_msec(void);

/* == Data ================================================================= */

/** Names of @ref wlmbe_zoom_tracking_t. */
static const bspl_enum_desc_t _wlmbe_zoom_tracking_desc[] = {
    BSPL_ENUM("Proportional", WLMBE_ZOOM_TRACKING_PROPORTIONAL),
    BSPL_ENUM("Push", WLMBE_ZOOM_TRACKING_PUSH),
    BSPL_ENUM("Centered", WLMBE_ZOOM_TRACKING_CENTERED),
    BSPL_ENUM_SENTINEL()
};

/** Descriptor of the `Zoom` dict. */
static const bspl_desc_t _wlmbe_zoom_config_desc[] = {
    BSPL_DESC_DOUBLE("MaxLevel", false, wlmbe_zoom_config_t,
                     max_level, max_level, 8.0),
    BSPL_DESC_DOUBLE("Step", false, wlmbe_zoom_config_t,
                     step, step, 1.25),
    BSPL_DESC_UINT64("AnimationTime", false, wlmbe_zoom_config_t,
                     animation_msec, animation_msec, 120),
    BSPL_DESC_ENUM("Tracking", false, wlmbe_zoom_config_t,
                   tracking, tracking, WLMBE_ZOOM_TRACKING_PROPORTIONAL,
                   _wlmbe_zoom_tracking_desc),
    BSPL_DESC_BOOL("Smooth", false, wlmbe_zoom_config_t,
                   smooth, smooth, true),
    BSPL_DESC_SENTINEL()
};

/** Levels this close to 1.0 count as unzoomed. */
static const double _wlmbe_zoom_epsilon = 1e-6;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmbe_zoom_t *wlmbe_zoom_create(
    struct wlr_scene *wlr_scene_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_renderer *wlr_renderer_ptr,
    struct wlr_allocator *wlr_allocator_ptr,
    bspl_dict_t *zoom_dict_ptr)
{
    wlmbe_zoom_t *zoom_ptr = logged_calloc(1, sizeof(wlmbe_zoom_t));
    if (NULL == zoom_ptr) return NULL;
    zoom_ptr->wlr_scene_ptr = wlr_scene_ptr;
    zoom_ptr->wlr_output_layout_ptr = wlr_output_layout_ptr;
    zoom_ptr->wlr_renderer_ptr = wlr_renderer_ptr;
    zoom_ptr->wlr_allocator_ptr = wlr_allocator_ptr;
    zoom_ptr->level = 1.0;
    zoom_ptr->target = 1.0;

    if (!wlmbe_zoom_configure(zoom_ptr, zoom_dict_ptr)) {
        // Keep going with the defaults: The zoom is an accessibility aid.
        if (!wlmbe_zoom_configure(zoom_ptr, NULL)) {
            wlmbe_zoom_destroy(zoom_ptr);
            return NULL;
        }
    }
    return zoom_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmbe_zoom_destroy(wlmbe_zoom_t *zoom_ptr)
{
    free(zoom_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmbe_zoom_configure(wlmbe_zoom_t *zoom_ptr, bspl_dict_t *zoom_dict_ptr)
{
    wlmbe_zoom_config_t config = {};
    if (!bspl_decode_dict(zoom_dict_ptr, _wlmbe_zoom_config_desc, &config)) {
        bs_log(BS_WARNING, "Failed to decode the 'Zoom' configuration.");
        return false;
    }
    if (!isfinite(config.max_level) || config.max_level < 1.0) {
        bs_log(BS_WARNING, "Zoom: MaxLevel %f must be >= 1.",
               config.max_level);
        return false;
    }
    if (!isfinite(config.step) || config.step <= 1.0 || config.step > 4.0) {
        bs_log(BS_WARNING, "Zoom: Step %f must be > 1 and <= 4.",
               config.step);
        return false;
    }
    config.animation_msec = BS_MIN(config.animation_msec, (uint64_t)2000);
    zoom_ptr->config = config;

    // A lower maximum applies right away.
    if (zoom_ptr->target > config.max_level) {
        wlmbe_zoom_scroll(zoom_ptr, 0);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmbe_zoom_set_cursor(
    wlmbe_zoom_t *zoom_ptr,
    struct wlr_cursor *wlr_cursor_ptr)
{
    zoom_ptr->wlr_cursor_ptr = wlr_cursor_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmbe_zoom_scroll(wlmbe_zoom_t *zoom_ptr, double steps)
{
    double target = wlmbe_zoom_level_scrolled(
        zoom_ptr->target, steps,
        zoom_ptr->config.step, zoom_ptr->config.max_level);
    if (target == zoom_ptr->target) return;

    // Animation starts now, not at the (possibly long ago) last advance.
    if (zoom_ptr->level == zoom_ptr->target) {
        zoom_ptr->last_msec = _wlmbe_zoom_now_msec();
    }
    zoom_ptr->target = target;
    _wlmbe_zoom_schedule_frames(zoom_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmbe_zoom_reset(wlmbe_zoom_t *zoom_ptr)
{
    if (1.0 == zoom_ptr->target) return;
    if (zoom_ptr->level == zoom_ptr->target) {
        zoom_ptr->last_msec = _wlmbe_zoom_now_msec();
    }
    zoom_ptr->target = 1.0;
    _wlmbe_zoom_schedule_frames(zoom_ptr);
}

/* ------------------------------------------------------------------------- */
double wlmbe_zoom_level(wlmbe_zoom_t *zoom_ptr)
{
    return zoom_ptr->level;
}

/* ------------------------------------------------------------------------- */
bool wlmbe_zoom_output_frame(
    wlmbe_zoom_t *zoom_ptr,
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_scene_output *wlr_scene_output_ptr)
{
    struct wlr_output *wlr_output_ptr = wlr_scene_output_ptr->output;

    _wlmbe_zoom_advance(zoom_ptr);
    if (zoom_ptr->level <= 1.0 + _wlmbe_zoom_epsilon || !wlr_output_ptr->enabled) {
        if (zoom_output_ptr->active) {
            _wlmbe_zoom_output_deactivate(zoom_output_ptr, wlr_output_ptr);
        }
        return false;
    }

    struct wlr_box box;
    wlr_output_layout_get_box(
        zoom_ptr->wlr_output_layout_ptr, wlr_output_ptr, &box);
    if (wlr_box_empty(&box)) return false;

    if (!zoom_output_ptr->active) {
        // The pointer is drawn into the scene's frame, so it is magnified
        // along and always shown over the content it points to. And direct
        // scanout of a client's buffer would bypass the magnification.
        wlr_output_lock_software_cursors(wlr_output_ptr, true);
        wlr_output_lock_attach_render(wlr_output_ptr, true);
        zoom_output_ptr->active = true;
        zoom_output_ptr->drawn_level = 0;
    }

    // Track the pointer while it is on this output. Elsewhere, the view
    // stays where the pointer left it.
    if (NULL != zoom_ptr->wlr_cursor_ptr) {
        double x = zoom_ptr->wlr_cursor_ptr->x - box.x;
        double y = zoom_ptr->wlr_cursor_ptr->y - box.y;
        if (0 <= x && x < box.width && 0 <= y && y < box.height) {
            zoom_output_ptr->pointer_x = x;
            zoom_output_ptr->pointer_y = y;
            zoom_output_ptr->has_pointer = true;
        }
    }
    if (!zoom_output_ptr->has_pointer) {
        zoom_output_ptr->pointer_x = box.width / 2.0;
        zoom_output_ptr->pointer_y = box.height / 2.0;
        zoom_output_ptr->origin_x = 0;
        zoom_output_ptr->origin_y = 0;
        zoom_output_ptr->origin_level = 1.0;
        zoom_output_ptr->has_pointer = true;
    }
    if (0 == zoom_output_ptr->drawn_level &&
        WLMBE_ZOOM_TRACKING_PUSH == zoom_ptr->config.tracking) {
        // Push tracking starts out zooming into the pointer, too.
        zoom_output_ptr->origin_x = 0;
        zoom_output_ptr->origin_y = 0;
        zoom_output_ptr->origin_level = 1.0;
    }
    wlmbe_zoom_view_origin(
        zoom_ptr->config.tracking,
        box.width, box.height,
        zoom_ptr->level,
        zoom_output_ptr->pointer_x, zoom_output_ptr->pointer_y,
        zoom_output_ptr->origin_level,
        &zoom_output_ptr->origin_x, &zoom_output_ptr->origin_y);
    zoom_output_ptr->origin_level = zoom_ptr->level;

    bool view_changed = (
        zoom_output_ptr->drawn_level != zoom_ptr->level ||
        zoom_output_ptr->drawn_x != zoom_output_ptr->origin_x ||
        zoom_output_ptr->drawn_y != zoom_output_ptr->origin_y);
    if (!view_changed &&
        !wlr_scene_output_needs_frame(wlr_scene_output_ptr)) {
        // Nothing new to show: Stay idle.
        return true;
    }

    if (_wlmbe_zoom_output_render(
            zoom_ptr, zoom_output_ptr, wlr_scene_output_ptr, &box)) {
        zoom_output_ptr->drawn_level = zoom_ptr->level;
        zoom_output_ptr->drawn_x = zoom_output_ptr->origin_x;
        zoom_output_ptr->drawn_y = zoom_output_ptr->origin_y;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
void wlmbe_zoom_output_fini(
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_output *wlr_output_ptr)
{
    if (zoom_output_ptr->active && NULL != wlr_output_ptr) {
        wlr_output_lock_attach_render(wlr_output_ptr, false);
        wlr_output_lock_software_cursors(wlr_output_ptr, false);
    }
    zoom_output_ptr->active = false;
    if (NULL != zoom_output_ptr->swapchain_ptr) {
        wlr_swapchain_destroy(zoom_output_ptr->swapchain_ptr);
        zoom_output_ptr->swapchain_ptr = NULL;
    }
    if (NULL != zoom_output_ptr->timeline_ptr) {
        wlr_drm_syncobj_timeline_unref(zoom_output_ptr->timeline_ptr);
        zoom_output_ptr->timeline_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
void wlmbe_zoom_view_origin(
    wlmbe_zoom_tracking_t tracking,
    double width,
    double height,
    double level,
    double pointer_x,
    double pointer_y,
    double prev_level,
    double *x_ptr,
    double *y_ptr)
{
    level = BS_MAX(1.0, level);
    double view_w = width / level, view_h = height / level;
    pointer_x = BS_MIN(BS_MAX(0.0, pointer_x), width);
    pointer_y = BS_MIN(BS_MAX(0.0, pointer_y), height);
    double x, y;

    switch (tracking) {
    case WLMBE_ZOOM_TRACKING_PUSH:
        // First, zoom around the pointer: What is under the pointer stays
        // under the pointer. Then push the view to contain the pointer.
        prev_level = BS_MAX(1.0, prev_level);
        x = pointer_x - (pointer_x - *x_ptr) * prev_level / level;
        y = pointer_y - (pointer_y - *y_ptr) * prev_level / level;
        x = BS_MIN(BS_MAX(x, pointer_x - view_w), pointer_x);
        y = BS_MIN(BS_MAX(y, pointer_y - view_h), pointer_y);
        break;

    case WLMBE_ZOOM_TRACKING_CENTERED:
        x = pointer_x - view_w / 2.0;
        y = pointer_y - view_h / 2.0;
        break;

    case WLMBE_ZOOM_TRACKING_PROPORTIONAL:
    default:
        x = pointer_x * (1.0 - 1.0 / level);
        y = pointer_y * (1.0 - 1.0 / level);
        break;
    }

    *x_ptr = BS_MIN(BS_MAX(0.0, x), width - view_w);
    *y_ptr = BS_MIN(BS_MAX(0.0, y), height - view_h);
}

/* ------------------------------------------------------------------------- */
double wlmbe_zoom_level_advance(
    double level,
    double target,
    double elapsed_msec,
    double animation_msec)
{
    if (0 >= animation_msec || level <= 0) return target;
    if (0 >= elapsed_msec) return level;

    // Time constant: About 95% of the way after the animation time.
    double k = 1.0 - exp(-3.0 * elapsed_msec / animation_msec);
    double l = exp(log(level) + (log(target) - log(level)) * k);
    if (fabs(l - target) <= 0.005 * target) return target;
    return l;
}

/* ------------------------------------------------------------------------- */
double wlmbe_zoom_level_scrolled(
    double target,
    double steps,
    double step,
    double max_level)
{
    double t = target * pow(step, steps);
    if (!isfinite(t)) t = target;
    t = BS_MIN(BS_MAX(1.0, t), BS_MAX(1.0, max_level));
    // Snap the last bit towards 1.0: Back to an exact, unfiltered frame.
    if (t < 1.0 + 1e-3) t = 1.0;
    return t;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Advances the shown level and keeps frames coming while animating. */
void _wlmbe_zoom_advance(wlmbe_zoom_t *zoom_ptr)
{
    if (zoom_ptr->level == zoom_ptr->target) return;

    uint64_t now = _wlmbe_zoom_now_msec();
    zoom_ptr->level = wlmbe_zoom_level_advance(
        zoom_ptr->level,
        zoom_ptr->target,
        now - zoom_ptr->last_msec,
        zoom_ptr->config.animation_msec);
    zoom_ptr->last_msec = now;

    // Still on the way, or just arrived (for outputs to (de)activate).
    _wlmbe_zoom_schedule_frames(zoom_ptr);
}

/* ------------------------------------------------------------------------- */
/** Schedules a frame on every output in the layout. */
void _wlmbe_zoom_schedule_frames(wlmbe_zoom_t *zoom_ptr)
{
    struct wlr_output_layout_output *lo;
    wl_list_for_each(lo, &zoom_ptr->wlr_output_layout_ptr->outputs, link) {
        if (lo->output->enabled) wlr_output_schedule_frame(lo->output);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Damages the whole output, as seen by the scene: Raised like a software
 * cursor's damage, the scene adds it to its damage ring and to the damage
 * pending for the next commit, and a frame gets scheduled.
 */
void _wlmbe_zoom_damage_whole(struct wlr_output *wlr_output_ptr)
{
    int width, height;
    wlr_output_transformed_resolution(wlr_output_ptr, &width, &height);
    pixman_region32_t region;
    pixman_region32_init_rect(&region, 0, 0, width, height);
    struct wlr_output_event_damage event = {
        .output = wlr_output_ptr,
        .damage = &region,
    };
    wl_signal_emit_mutable(&wlr_output_ptr->events.damage, &event);
    pixman_region32_fini(&region);
    wlr_output_schedule_frame(wlr_output_ptr);
}

/* ------------------------------------------------------------------------- */
/** Ends zooming on the output: Back to the scene's regular rendering. */
void _wlmbe_zoom_output_deactivate(
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_output *wlr_output_ptr)
{
    wlmbe_zoom_output_fini(zoom_output_ptr, wlr_output_ptr);
    zoom_output_ptr->drawn_level = 0;
    zoom_output_ptr->origin_level = 1.0;
    // The output's own buffers hold magnified frames, unknown to the
    // scene's damage tracking: Have them all redrawn.
    _wlmbe_zoom_damage_whole(wlr_output_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Renders the scene into the offscreen swapchain, then the magnified view
 * of it into the output's buffer, and commits.
 *
 * @return true on success.
 */
bool _wlmbe_zoom_output_render(
    wlmbe_zoom_t *zoom_ptr,
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_scene_output *wlr_scene_output_ptr,
    const struct wlr_box *box_ptr)
{
    struct wlr_output *wlr_output_ptr = wlr_scene_output_ptr->output;
    struct wlr_renderer *renderer_ptr = wlr_output_ptr->renderer;
    struct wlr_buffer *out_buffer_ptr = NULL;
    struct wlr_texture *texture_ptr = NULL;
    bool rv = false;

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    if (!wlr_output_configure_primary_swapchain(
            wlr_output_ptr, &state, &wlr_output_ptr->swapchain)) {
        bs_log(BS_WARNING, "Zoom: Failed to configure swapchain of '%s'",
               wlr_output_ptr->name);
        goto out;
    }
    struct wlr_swapchain *out_swapchain_ptr = wlr_output_ptr->swapchain;

    // The offscreen swapchain matches the output's. (Re)create as needed.
    struct wlr_swapchain *sc_ptr = zoom_output_ptr->swapchain_ptr;
    if (NULL != sc_ptr &&
        (sc_ptr->width != out_swapchain_ptr->width ||
         sc_ptr->height != out_swapchain_ptr->height ||
         sc_ptr->format.format != out_swapchain_ptr->format.format ||
         NULL == sc_ptr->allocator)) {
        wlr_swapchain_destroy(sc_ptr);
        zoom_output_ptr->swapchain_ptr = NULL;
    }
    if (NULL == zoom_output_ptr->swapchain_ptr) {
        zoom_output_ptr->swapchain_ptr = wlr_swapchain_create(
            wlr_output_ptr->allocator,
            out_swapchain_ptr->width,
            out_swapchain_ptr->height,
            &out_swapchain_ptr->format);
        if (NULL == zoom_output_ptr->swapchain_ptr) {
            bs_log(BS_WARNING, "Zoom: Failed wlr_swapchain_create for '%s'",
                   wlr_output_ptr->name);
            goto out;
        }
    }

    // The scene, unmagnified and damage-tracked, into the offscreen buffer.
    struct wlr_scene_output_state_options options = {
        .swapchain = zoom_output_ptr->swapchain_ptr,
    };
    if (!wlr_scene_output_build_state(wlr_scene_output_ptr, &state, &options)) {
        bs_log(BS_WARNING, "Zoom: Failed wlr_scene_output_build_state for "
               "'%s'", wlr_output_ptr->name);
        goto out;
    }
    if (!(state.committed & WLR_OUTPUT_STATE_BUFFER) || NULL == state.buffer) {
        goto out;
    }
    texture_ptr = wlr_texture_from_buffer(renderer_ptr, state.buffer);
    if (NULL == texture_ptr) {
        bs_log(BS_WARNING, "Zoom: Failed wlr_texture_from_buffer");
        goto out;
    }

    out_buffer_ptr = wlr_swapchain_acquire(out_swapchain_ptr);
    if (NULL == out_buffer_ptr) goto out;

    // If the frame is synchronized explicitly, the backend waits for our
    // render pass, not the scene's.
    struct wlr_buffer_pass_options pass_options = {};
    if (state.committed & WLR_OUTPUT_STATE_WAIT_TIMELINE) {
        if (NULL == zoom_output_ptr->timeline_ptr) {
            zoom_output_ptr->timeline_ptr = wlr_drm_syncobj_timeline_create(
                wlr_renderer_get_drm_fd(renderer_ptr));
            zoom_output_ptr->timeline_point = 0;
        }
        if (NULL == zoom_output_ptr->timeline_ptr) goto out;
        pass_options.signal_timeline = zoom_output_ptr->timeline_ptr;
        pass_options.signal_point = ++zoom_output_ptr->timeline_point;
    }

    // The view, logical output-local coordinates, to buffer coordinates.
    double scale = wlr_output_ptr->scale;
    struct wlr_fbox view = {
        .x = zoom_output_ptr->origin_x * scale,
        .y = zoom_output_ptr->origin_y * scale,
        .width = box_ptr->width * scale / zoom_ptr->level,
        .height = box_ptr->height * scale / zoom_ptr->level,
    };
    struct wlr_fbox src_box;
    wlr_fbox_transform(
        &src_box, &view,
        wlr_output_transform_invert(wlr_output_ptr->transform),
        box_ptr->width * scale, box_ptr->height * scale);
    // Rounding of the logical size: Stay within the texture.
    src_box.x = BS_MAX(0.0, src_box.x);
    src_box.y = BS_MAX(0.0, src_box.y);
    src_box.width = BS_MIN(src_box.width, texture_ptr->width - src_box.x);
    src_box.height = BS_MIN(src_box.height, texture_ptr->height - src_box.y);

    struct wlr_render_pass *pass_ptr = wlr_renderer_begin_buffer_pass(
        renderer_ptr, out_buffer_ptr, &pass_options);
    if (NULL == pass_ptr) goto out;
    wlr_render_pass_add_texture(pass_ptr, &(struct wlr_render_texture_options){
            .texture = texture_ptr,
            .src_box = src_box,
            .dst_box = {
                .width = out_buffer_ptr->width,
                .height = out_buffer_ptr->height },
            .filter_mode = zoom_ptr->config.smooth ?
                WLR_SCALE_FILTER_BILINEAR : WLR_SCALE_FILTER_NEAREST,
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        });
    if (!wlr_render_pass_submit(pass_ptr)) {
        bs_log(BS_WARNING, "Zoom: Failed wlr_render_pass_submit");
        goto out;
    }

    wlr_output_state_set_buffer(&state, out_buffer_ptr);
    if (NULL != pass_options.signal_timeline) {
        wlr_output_state_set_wait_timeline(
            &state, pass_options.signal_timeline, pass_options.signal_point);
    }
    pixman_region32_t damage;
    pixman_region32_init_rect(
        &damage, 0, 0, out_buffer_ptr->width, out_buffer_ptr->height);
    wlr_output_state_set_damage(&state, &damage);
    pixman_region32_fini(&damage);

    rv = wlr_output_commit_state(wlr_output_ptr, &state);
    if (!rv) {
        bs_log(BS_WARNING, "Zoom: Failed wlr_output_commit_state on '%s'",
               wlr_output_ptr->name);
    }

out:
    if (NULL != texture_ptr) wlr_texture_destroy(texture_ptr);
    if (NULL != out_buffer_ptr) wlr_buffer_unlock(out_buffer_ptr);
    wlr_output_state_finish(&state);
    return rv;
}

/* ------------------------------------------------------------------------- */
/** @return CLOCK_MONOTONIC, in milliseconds. */
uint64_t _wlmbe_zoom_now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* == Unit tests =========================================================== */

static void _wlmbe_zoom_test_proportional(bs_test_t *test_ptr);
static void _wlmbe_zoom_test_centered(bs_test_t *test_ptr);
static void _wlmbe_zoom_test_push(bs_test_t *test_ptr);
static void _wlmbe_zoom_test_bounds(bs_test_t *test_ptr);
static void _wlmbe_zoom_test_scrolled(bs_test_t *test_ptr);
static void _wlmbe_zoom_test_advance(bs_test_t *test_ptr);
static void _wlmbe_zoom_test_configure(bs_test_t *test_ptr);

/** Test cases */
static const bs_test_case_t _wlmbe_zoom_test_cases[] = {
    { 1, "proportional", _wlmbe_zoom_test_proportional },
    { 1, "centered", _wlmbe_zoom_test_centered },
    { 1, "push", _wlmbe_zoom_test_push },
    { 1, "bounds", _wlmbe_zoom_test_bounds },
    { 1, "scrolled", _wlmbe_zoom_test_scrolled },
    { 1, "advance", _wlmbe_zoom_test_advance },
    { 1, "configure", _wlmbe_zoom_test_configure },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmbe_zoom_test_set = BS_TEST_SET(
    true, "zoom", _wlmbe_zoom_test_cases);

/** Verifies `_a` and `_b` are within 1e-9. */
#define _WLMBE_ZOOM_VERIFY_NEAR(_test, _a, _b) \
    BS_TEST_VERIFY_TRUE(_test, fabs((_a) - (_b)) < 1e-9)

/* ------------------------------------------------------------------------- */
/**
 * Proportional tracking: The content under the pointer is shown where the
 * pointer is. So the pointer's input position needs no translation.
 */
void _wlmbe_zoom_test_proportional(bs_test_t *test_ptr)
{
    const double w = 1920, h = 1080;
    const double levels[] = { 1.0, 1.5, 2.0, 3.7, 8.0 };
    const double points[][2] = {
        { 0, 0 }, { 960, 540 }, { 1919, 1079 }, { 100, 1000 }, { 1500, 7 } };
    for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); ++l) {
        for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); ++p) {
            double x = 0, y = 0;
            wlmbe_zoom_view_origin(
                WLMBE_ZOOM_TRACKING_PROPORTIONAL, w, h, levels[l],
                points[p][0], points[p][1], 1.0, &x, &y);
            // Shown at pointer's position: origin + position / level.
            _WLMBE_ZOOM_VERIFY_NEAR(
                test_ptr, points[p][0], x + points[p][0] / levels[l]);
            _WLMBE_ZOOM_VERIFY_NEAR(
                test_ptr, points[p][1], y + points[p][1] / levels[l]);
        }
    }

    // Top-left corner and bottom-right corner reach the output's corners.
    double x = -1, y = -1;
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_PROPORTIONAL,
                           w, h, 2.0, 0, 0, 1.0, &x, &y);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 0, x);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 0, y);
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_PROPORTIONAL,
                           w, h, 2.0, w, h, 1.0, &x, &y);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, w / 2, x);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, h / 2, y);
}

/* ------------------------------------------------------------------------- */
/** Centered tracking: Centered at the pointer, except at the edges. */
void _wlmbe_zoom_test_centered(bs_test_t *test_ptr)
{
    double x, y;
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_CENTERED,
                           1000, 800, 4.0, 500, 400, 1.0, &x, &y);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 500 - 125, x);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 400 - 100, y);

    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_CENTERED,
                           1000, 800, 4.0, 10, 790, 1.0, &x, &y);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 0, x);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 800 - 200, y);
}

/* ------------------------------------------------------------------------- */
/** Push tracking: Moves only when the pointer leaves the view. */
void _wlmbe_zoom_test_push(bs_test_t *test_ptr)
{
    // Zooming in from 1.0: The point under the pointer stays put.
    double x = 0, y = 0;
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_PUSH,
                           1000, 800, 2.0, 600, 200, 1.0, &x, &y);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 600, x + 600 / 2.0);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 200, y + 200 / 2.0);
    BS_TEST_VERIFY_EQ(test_ptr, 300, x);
    BS_TEST_VERIFY_EQ(test_ptr, 100, y);

    // Moving the pointer within the view [300, 800) x [100, 500): No change.
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_PUSH,
                           1000, 800, 2.0, 700, 450, 2.0, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, 300, x);
    BS_TEST_VERIFY_EQ(test_ptr, 100, y);

    // Pushing past the left and bottom edges.
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_PUSH,
                           1000, 800, 2.0, 250, 550, 2.0, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, 250, x);
    BS_TEST_VERIFY_EQ(test_ptr, 150, y);
}

/* ------------------------------------------------------------------------- */
/** The view remains within the output, also for odd inputs. */
void _wlmbe_zoom_test_bounds(bs_test_t *test_ptr)
{
    const wlmbe_zoom_tracking_t t[] = {
        WLMBE_ZOOM_TRACKING_PROPORTIONAL,
        WLMBE_ZOOM_TRACKING_PUSH,
        WLMBE_ZOOM_TRACKING_CENTERED };
    const double points[][2] = {
        { -50, -50 }, { 5000, 5000 }, { 0, 799 }, { 999, 0 } };
    for (size_t i = 0; i < 3; ++i) {
        for (size_t p = 0; p < 4; ++p) {
            for (double level = 0.5; level <= 9.0; level += 0.75) {
                double x = 4000, y = -4000;
                wlmbe_zoom_view_origin(t[i], 1000, 800, level,
                                       points[p][0], points[p][1], 3.0,
                                       &x, &y);
                double l = BS_MAX(1.0, level);
                BS_TEST_VERIFY_TRUE(test_ptr, 0 <= x);
                BS_TEST_VERIFY_TRUE(test_ptr, 0 <= y);
                BS_TEST_VERIFY_TRUE(test_ptr, x + 1000 / l <= 1000 + 1e-9);
                BS_TEST_VERIFY_TRUE(test_ptr, y + 800 / l <= 800 + 1e-9);
            }
        }
    }

    // Unzoomed, the view is the output.
    double x = 7, y = 7;
    wlmbe_zoom_view_origin(WLMBE_ZOOM_TRACKING_CENTERED,
                           1000, 800, 1.0, 300, 300, 1.0, &x, &y);
    BS_TEST_VERIFY_EQ(test_ptr, 0, x);
    BS_TEST_VERIFY_EQ(test_ptr, 0, y);
}

/* ------------------------------------------------------------------------- */
/** Scrolling multiplies by the step, clamped. */
void _wlmbe_zoom_test_scrolled(bs_test_t *test_ptr)
{
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 1.25,
                            wlmbe_zoom_level_scrolled(1.0, 1, 1.25, 8));
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 1.25 * 1.25,
                            wlmbe_zoom_level_scrolled(1.25, 1, 1.25, 8));
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 2.0,
                            wlmbe_zoom_level_scrolled(2.5, -1, 1.25, 8));
    // Fractions: Two half notches equal one notch.
    double t = wlmbe_zoom_level_scrolled(1.0, 0.5, 1.25, 8);
    _WLMBE_ZOOM_VERIFY_NEAR(test_ptr, 1.25,
                            wlmbe_zoom_level_scrolled(t, 0.5, 1.25, 8));
    // Clamps.
    BS_TEST_VERIFY_EQ(test_ptr, 1.0,
                      wlmbe_zoom_level_scrolled(1.0, -3, 1.25, 8));
    BS_TEST_VERIFY_EQ(test_ptr, 8.0,
                      wlmbe_zoom_level_scrolled(7.0, 5, 1.25, 8));
    BS_TEST_VERIFY_EQ(test_ptr, 1.0,
                      wlmbe_zoom_level_scrolled(1.0, 5, 1.25, 0.5));
    // Snaps to exactly 1.0.
    BS_TEST_VERIFY_EQ(test_ptr, 1.0,
                      wlmbe_zoom_level_scrolled(1.0005, -0.001, 1.25, 8));
}

/* ------------------------------------------------------------------------- */
/** The animation approaches the target and arrives exactly. */
void _wlmbe_zoom_test_advance(bs_test_t *test_ptr)
{
    BS_TEST_VERIFY_EQ(test_ptr, 2.0, wlmbe_zoom_level_advance(1.0, 2.0, 16, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 1.0, wlmbe_zoom_level_advance(1.0, 2.0, 0, 120));

    double l = 1.0;
    int frames = 0;
    for (; l != 2.0 && frames < 1000; ++frames) {
        double n = wlmbe_zoom_level_advance(l, 2.0, 16, 120);
        BS_TEST_VERIFY_TRUE(test_ptr, n > l);
        BS_TEST_VERIFY_TRUE(test_ptr, n <= 2.0);
        l = n;
    }
    BS_TEST_VERIFY_EQ(test_ptr, 2.0, l);
    // Arrives within about twice the animation time (16ms frames).
    BS_TEST_VERIFY_TRUE(test_ptr, frames <= 16);

    // And back, also exactly.
    for (frames = 0; l != 1.0 && frames < 1000; ++frames) {
        l = wlmbe_zoom_level_advance(l, 1.0, 16, 120);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 1.0, l);
}

/* ------------------------------------------------------------------------- */
/** Configuration: Defaults, values, and rejected values. */
void _wlmbe_zoom_test_configure(bs_test_t *test_ptr)
{
    wlmbe_zoom_t z = { .level = 1.0, .target = 1.0 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmbe_zoom_configure(&z, NULL));
    BS_TEST_VERIFY_EQ(test_ptr, 8.0, z.config.max_level);
    BS_TEST_VERIFY_EQ(test_ptr, 1.25, z.config.step);
    BS_TEST_VERIFY_EQ(test_ptr, 120, z.config.animation_msec);
    BS_TEST_VERIFY_EQ(test_ptr, WLMBE_ZOOM_TRACKING_PROPORTIONAL,
                      z.config.tracking);
    BS_TEST_VERIFY_TRUE(test_ptr, z.config.smooth);

    bspl_object_t *o = bspl_create_object_from_plist_string(
        "{MaxLevel = 4; Step = 1.5; AnimationTime = 0; Tracking = Push; "
        "Smooth = False;}");
    BS_TEST_VERIFY_TRUE(
        test_ptr, wlmbe_zoom_configure(&z, bspl_dict_from_object(o)));
    bspl_object_unref(o);
    BS_TEST_VERIFY_EQ(test_ptr, 4.0, z.config.max_level);
    BS_TEST_VERIFY_EQ(test_ptr, 1.5, z.config.step);
    BS_TEST_VERIFY_EQ(test_ptr, 0, z.config.animation_msec);
    BS_TEST_VERIFY_EQ(test_ptr, WLMBE_ZOOM_TRACKING_PUSH, z.config.tracking);
    BS_TEST_VERIFY_FALSE(test_ptr, z.config.smooth);

    o = bspl_create_object_from_plist_string("{Step = 1.0;}");
    BS_TEST_VERIFY_FALSE(
        test_ptr, wlmbe_zoom_configure(&z, bspl_dict_from_object(o)));
    bspl_object_unref(o);
    o = bspl_create_object_from_plist_string("{MaxLevel = 0.5;}");
    BS_TEST_VERIFY_FALSE(
        test_ptr, wlmbe_zoom_configure(&z, bspl_dict_from_object(o)));
    bspl_object_unref(o);
    // Kept the previous, valid, configuration.
    BS_TEST_VERIFY_EQ(test_ptr, 1.5, z.config.step);
}

/* == End of zoom.c ======================================================== */
