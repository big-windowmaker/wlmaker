/* ========================================================================= */
/**
 * @file dock_fx.h
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
#ifndef __WLMAKER_DOCK_FX_H__
#define __WLMAKER_DOCK_FX_H__

#include <stdbool.h>
#include <stdint.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

struct wlr_buffer;
struct wlr_scene_tree;

/** Forward declaration: The dock's visual effects. */
typedef struct _wlmaker_dock_fx_t wlmaker_dock_fx_t;

/** Colors of the balloons. */
typedef struct {
    /** Fill color. Window Maker: White. */
    uint32_t                  color;
    /** Text color. Window Maker: Black. */
    uint32_t                  text_color;
    /** Color of the outline. Window Maker: Black. */
    uint32_t                  border_color;
} wlmaker_dock_fx_balloon_style_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Visual effects of the Dock and Clip, as on Window Maker: The tile dragged
 * along with the pointer, the "ghost" of the slot it will be attached to,
 * the icon sliding into its slot, the "kaboom" of a tile dropped off the
 * docks, and the balloons with the name of what's under the pointer.
 *
 * All of it is drawn on a scene tree above everything else. None of it
 * takes input.
 *
 * @param parent_ptr          Scene tree to draw into: Created as its last
 *                            child, ie. on top of its other children.
 * @param wl_event_loop_ptr   For the animations' and balloon's timers.
 *
 * @return The effects handle, or NULL on error. Must be destroyed with
 *     @ref wlmaker_dock_fx_destroy.
 */
wlmaker_dock_fx_t *wlmaker_dock_fx_create(
    struct wlr_scene_tree *parent_ptr,
    struct wl_event_loop *wl_event_loop_ptr);

/** Destroys the effects, and stops all animations. */
void wlmaker_dock_fx_destroy(wlmaker_dock_fx_t *fx_ptr);

/** Sets the colors of the balloons. By default, Window Maker's. */
void wlmaker_dock_fx_set_balloon_style(
    wlmaker_dock_fx_t *fx_ptr,
    const wlmaker_dock_fx_balloon_style_t *style_ptr);

/**
 * Shows `wlr_buffer_ptr` (eg. a tile) at layout position `x`, `y`, as the
 * icon being dragged. Moves it there, if already shown.
 *
 * @param fx_ptr
 * @param wlr_buffer_ptr      The image. NULL keeps the current image.
 * @param x
 * @param y
 */
void wlmaker_dock_fx_drag_show(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y);

/** Hides the dragged icon. Stops a slide, without calling its callback. */
void wlmaker_dock_fx_drag_hide(wlmaker_dock_fx_t *fx_ptr);

/** Callback when a slide has finished. */
typedef void (*wlmaker_dock_fx_slide_done_t)(void *ud_ptr);

/**
 * Slides the dragged icon to `to_x`, `to_y`, like Window Maker's
 * `slide_window` with `IconSlideSpeed = fast`. Then hides it and calls
 * `done`. Without a dragged icon shown, `done` is called right away.
 *
 * @param fx_ptr
 * @param to_x
 * @param to_y
 * @param done                May be NULL.
 * @param ud_ptr              Argument to `done`.
 */
void wlmaker_dock_fx_slide(
    wlmaker_dock_fx_t *fx_ptr,
    int to_x,
    int to_y,
    wlmaker_dock_fx_slide_done_t done,
    void *ud_ptr);

/**
 * Shows the "ghost" of `wlr_buffer_ptr` at `x`, `y`: Where the dragged icon
 * will be attached. Like Window Maker's `MakeGhostIcon`: The image, mostly
 * whitened.
 *
 * @param fx_ptr
 * @param wlr_buffer_ptr      The image. NULL keeps the current ghost.
 * @param x
 * @param y
 */
void wlmaker_dock_fx_ghost_show(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y);

/**
 * Covers the slot a tile was lifted from with `wlr_buffer_ptr` (eg. an
 * empty tile), at `x`, `y`. For tiles drawn by a client.
 */
void wlmaker_dock_fx_cover_show(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y);

/** Removes the cover. */
void wlmaker_dock_fx_cover_hide(wlmaker_dock_fx_t *fx_ptr);

/** Hides the ghost. */
void wlmaker_dock_fx_ghost_hide(wlmaker_dock_fx_t *fx_ptr);

/** @return Whether the ghost is shown. */
bool wlmaker_dock_fx_ghost_visible(wlmaker_dock_fx_t *fx_ptr);

/**
 * Makes `wlr_buffer_ptr` explode at `x`, `y`, like Window Maker's `DoKaboom`:
 * The image breaks into 4x4 pieces, that fly off and fall out of `area`.
 *
 * @param fx_ptr
 * @param wlr_buffer_ptr      The image. A reference is held while needed.
 * @param x
 * @param y
 * @param area_ptr            Pieces leaving this area (the output) are gone.
 */
void wlmaker_dock_fx_kaboom(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y,
    const struct wlr_box *area_ptr);

/** @return Number of explosions still running. */
size_t wlmaker_dock_fx_kabooms(wlmaker_dock_fx_t *fx_ptr);

/**
 * The pointer entered something with a balloon: Shows the balloon with
 * `text_ptr` next to `box_ptr`, after Window Maker's balloon delay. Right
 * away, if the pointer comes straight from something with a balloon shown.
 *
 * @param fx_ptr
 * @param owner_ptr           Identifies what the balloon is for.
 * @param text_ptr            Text, may have multiple lines.
 * @param box_ptr             What the balloon points at, in layout coords.
 * @param area_ptr            The output, to keep the balloon within.
 */
void wlmaker_dock_fx_balloon_enter(
    wlmaker_dock_fx_t *fx_ptr,
    const void *owner_ptr,
    const char *text_ptr,
    const struct wlr_box *box_ptr,
    const struct wlr_box *area_ptr);

/**
 * The pointer left `owner_ptr`: Hides its balloon. Entering another one
 * right away (while still processing the same event) shows it without delay.
 *
 * @param fx_ptr
 * @param owner_ptr           As given to @ref wlmaker_dock_fx_balloon_enter.
 *                            NULL hides any balloon, and resets the delay.
 */
void wlmaker_dock_fx_balloon_leave(
    wlmaker_dock_fx_t *fx_ptr,
    const void *owner_ptr);

/** @return Whether a balloon is shown. */
bool wlmaker_dock_fx_balloon_visible(wlmaker_dock_fx_t *fx_ptr);

/**
 * Creates the ghost of `wlr_buffer_ptr`: A copy, whitened like Window Maker
 * does with white at alpha 200.
 *
 * @return A new buffer, or NULL on error. Drop with `wlr_buffer_drop`.
 */
struct wlr_buffer *wlmaker_dock_fx_create_ghost_buffer(
    struct wlr_buffer *wlr_buffer_ptr);

/**
 * Draws a balloon, like Window Maker's: Black text on white, in a rounded
 * box with a black outline and a pointer to what it is for.
 *
 * @param style_ptr           Colors. NULL for Window Maker's.
 * @param text_ptr
 * @param pointer_right       Whether the pointer is at the right end.
 * @param pointer_top         Whether the pointer is on top (else bottom).
 *
 * @return A new buffer, or NULL on error.
 */
struct wlr_buffer *wlmaker_dock_fx_create_balloon_buffer(
    const wlmaker_dock_fx_balloon_style_t *style_ptr,
    const char *text_ptr,
    bool pointer_right,
    bool pointer_top);

/**
 * Computes where to place a balloon of `width` x `height` (with the pointer
 * of 12 pixels included in `height`) for `box_ptr`, within `area_ptr`. As
 * Window Maker's `showText`.
 *
 * @param box_ptr             What the balloon is for.
 * @param area_ptr
 * @param width
 * @param height              Height of the box, without the pointer.
 * @param pointer_right_ptr   Returns whether the pointer is at the right.
 * @param pointer_top_ptr     Returns whether the pointer is on top.
 *
 * @return The position of the balloon's buffer (including its pointer).
 */
struct wlr_box wlmaker_dock_fx_balloon_position(
    const struct wlr_box *box_ptr,
    const struct wlr_box *area_ptr,
    int width,
    int height,
    bool *pointer_right_ptr,
    bool *pointer_top_ptr);

/** Plist descriptor of @ref wlmaker_dock_fx_balloon_style_t. */
extern const bspl_desc_t wlmaker_dock_fx_balloon_style_desc[];

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dock_fx_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DOCK_FX_H__ */
/* == End of dock_fx.h ===================================================== */
