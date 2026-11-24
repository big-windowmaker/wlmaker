/* ========================================================================= */
/**
 * @file zoom.h
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
#ifndef __WLMBE_ZOOM_H__
#define __WLMBE_ZOOM_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * Accessibility zoom: Magnifies everything shown on the outputs, following
 * the pointer.
 *
 * The scene is rendered as usual (with damage tracking) into an offscreen
 * swapchain of the output's size; a single textured quad then draws the
 * magnified part of that frame into the output's buffer. The pointer is
 * drawn as a software cursor into the scene's frame, so it gets magnified
 * with everything else and is always shown exactly over the content it
 * points to: Input coordinates need no translation.
 */
typedef struct _wlmbe_zoom_t wlmbe_zoom_t;

struct wlr_allocator;
struct wlr_cursor;
struct wlr_drm_syncobj_timeline;
struct wlr_output;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output;
struct wlr_swapchain;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** How the magnified view follows the pointer. */
typedef enum {
    /**
     * The view's position is proportional to the pointer's position on the
     * output: The point under the pointer stays where it is unzoomed, and
     * the edges of the output are reached together with the pointer.
     */
    WLMBE_ZOOM_TRACKING_PROPORTIONAL,
    /** The view only moves when the pointer pushes against its edges. */
    WLMBE_ZOOM_TRACKING_PUSH,
    /** The view is centered at the pointer (except at the output's edges). */
    WLMBE_ZOOM_TRACKING_CENTERED,
} wlmbe_zoom_tracking_t;

/** Configurable parameters of the zoom. The `Zoom` dict of Config.plist. */
typedef struct {
    /** Maximum magnification. `MaxLevel`, >= 1. */
    double                    max_level;
    /** Magnification factor per wheel notch or key press. `Step`, > 1. */
    double                    step;
    /** Duration of the animation towards a new level. `AnimationTime`. */
    uint64_t                  animation_msec;
    /** How the view follows the pointer. `Tracking`. */
    int                       tracking;
    /** Whether to filter (smooth) the magnified pixels. `Smooth`. */
    bool                      smooth;
} wlmbe_zoom_config_t;

/** Per-output state of the zoom. Embedded in the output. */
typedef struct {
    /** Offscreen swapchain the unmagnified scene is rendered into. */
    struct wlr_swapchain      *swapchain_ptr;
    /** Timeline for explicit synchronization, if the frame needs one. */
    struct wlr_drm_syncobj_timeline *timeline_ptr;
    /** Last point signalled on @ref wlmbe_zoom_output_t::timeline_ptr. */
    uint64_t                  timeline_point;
    /** Whether zooming is active on the output (software cursors locked). */
    bool                      active;
    /** Whether @ref wlmbe_zoom_output_t::pointer_x is valid. */
    bool                      has_pointer;
    /** Last pointer position on this output, output-local logical coords. */
    double                    pointer_x;
    /** Last pointer position on this output, output-local logical coords. */
    double                    pointer_y;
    /** Origin of the view, output-local logical coordinates. */
    double                    origin_x;
    /** Origin of the view, output-local logical coordinates. */
    double                    origin_y;
    /** Level the origin was computed for. */
    double                    origin_level;
    /** Level of the last frame drawn. 0 if none was drawn. */
    double                    drawn_level;
    /** Origin of the last frame drawn. */
    double                    drawn_x;
    /** Origin of the last frame drawn. */
    double                    drawn_y;
} wlmbe_zoom_output_t;

/**
 * Creates the zoom.
 *
 * @param wlr_scene_ptr
 * @param wlr_output_layout_ptr
 * @param wlr_renderer_ptr
 * @param wlr_allocator_ptr
 * @param zoom_dict_ptr       The `Zoom` dict of the configuration, or NULL
 *                            for the defaults.
 *
 * @return The zoom handle, or NULL on error.
 */
wlmbe_zoom_t *wlmbe_zoom_create(
    struct wlr_scene *wlr_scene_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_renderer *wlr_renderer_ptr,
    struct wlr_allocator *wlr_allocator_ptr,
    bspl_dict_t *zoom_dict_ptr);

/** Destroys the zoom. */
void wlmbe_zoom_destroy(wlmbe_zoom_t *zoom_ptr);

/**
 * Re-applies the configuration.
 *
 * @param zoom_ptr
 * @param zoom_dict_ptr       The `Zoom` dict, or NULL for the defaults.
 *
 * @return true on success. On failure, the configuration remains unchanged.
 */
bool wlmbe_zoom_configure(wlmbe_zoom_t *zoom_ptr, bspl_dict_t *zoom_dict_ptr);

/** Sets the cursor the zoom follows. */
void wlmbe_zoom_set_cursor(
    wlmbe_zoom_t *zoom_ptr,
    struct wlr_cursor *wlr_cursor_ptr);

/**
 * Changes the magnification by `steps` times the configured step. Positive
 * steps magnify, negative steps reduce. Fractions are permitted (high
 * resolution wheels, touchpads).
 */
void wlmbe_zoom_scroll(wlmbe_zoom_t *zoom_ptr, double steps);

/** Returns to the unmagnified view. */
void wlmbe_zoom_reset(wlmbe_zoom_t *zoom_ptr);

/** @return The magnification level currently shown. 1.0 when unzoomed. */
double wlmbe_zoom_level(wlmbe_zoom_t *zoom_ptr);

/**
 * Renders and commits a frame of the output, if zooming. To be called from
 * the output's `frame` handler.
 *
 * @param zoom_ptr
 * @param zoom_output_ptr
 * @param wlr_scene_output_ptr
 *
 * @return true if the frame was handled (committed, or nothing needed
 *     committing). false if not zooming: The caller commits as usual.
 */
bool wlmbe_zoom_output_frame(
    wlmbe_zoom_t *zoom_ptr,
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_scene_output *wlr_scene_output_ptr);

/** Releases the per-output state. To be called when the output goes. */
void wlmbe_zoom_output_fini(
    wlmbe_zoom_output_t *zoom_output_ptr,
    struct wlr_output *wlr_output_ptr);

/**
 * Computes where the magnified view of an output starts.
 *
 * Coordinates are logical and local to the output, whose size is
 * `width` x `height`. The view is `width / level` x `height / level` large
 * and always remains within the output.
 *
 * @param tracking
 * @param width
 * @param height
 * @param level               Magnification, >= 1.
 * @param pointer_x           Pointer position, output-local.
 * @param pointer_y           Pointer position, output-local.
 * @param prev_level          Level of the previous origin (push tracking).
 * @param x_ptr               In: Previous origin (push tracking). Out: The
 *                            origin of the view.
 * @param y_ptr               See `x_ptr`.
 */
void wlmbe_zoom_view_origin(
    wlmbe_zoom_tracking_t tracking,
    double width,
    double height,
    double level,
    double pointer_x,
    double pointer_y,
    double prev_level,
    double *x_ptr,
    double *y_ptr);

/**
 * Advances the level shown towards the target level.
 *
 * Moves exponentially in log space, so each step of the wheel feels the
 * same, and snaps to the target once within 0.5%.
 *
 * @param level               Level currently shown.
 * @param target              Level to reach.
 * @param elapsed_msec        Time since the last advance.
 * @param animation_msec      Configured animation time. 0: Immediate.
 *
 * @return The new level.
 */
double wlmbe_zoom_level_advance(
    double level,
    double target,
    double elapsed_msec,
    double animation_msec);

/**
 * Computes the target level after scrolling `steps`.
 *
 * @return `target * step^steps`, clamped to [1, max_level].
 */
double wlmbe_zoom_level_scrolled(
    double target,
    double steps,
    double step,
    double max_level);

/** Unit test cases. */
extern const bs_test_set_t wlmbe_zoom_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMBE_ZOOM_H__ */
/* == End of zoom.h ======================================================== */
