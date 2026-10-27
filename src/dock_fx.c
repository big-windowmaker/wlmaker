/* ========================================================================= */
/**
 * @file dock_fx.c
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

#include "dock_fx.h"

#include <cairo.h>
#include <stdlib.h>
#include <string.h>
#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#undef WLR_USE_UNSTABLE

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** Size of a piece of the kaboom, as Window Maker's ICON_KABOOM_PIECE_SIZE. */
#define _WLMAKER_DOCK_FX_PIECE_SIZE 4
/** Fixed-point precision of the pieces' horizontal motion (KAB_PRECISION). */
#define _WLMAKER_DOCK_FX_KAB_PRECISION 4

/** A piece of an exploding icon. */
typedef struct {
    /** Scene node showing the piece. NULL when the piece is gone. */
    struct wlr_scene_buffer   *wlr_scene_buffer_ptr;
    /** Horizontal position, in 1/16th of a pixel. */
    int                       px;
    /** Vertical position. */
    int                       py;
    /** Horizontal speed, in 1/16th of a pixel per frame. */
    int                       pvx;
    /** Vertical speed, in pixels per frame. */
    int                       pvy;
} wlmaker_dock_fx_piece_t;

/** An explosion, as Window Maker's `DoKaboom`. */
typedef struct {
    /** Node within @ref wlmaker_dock_fx_t::kabooms. */
    bs_dllist_node_t          dlnode;
    /** Back-link. */
    wlmaker_dock_fx_t         *fx_ptr;
    /** Holds the pieces. */
    struct wlr_scene_tree     *wlr_scene_tree_ptr;
    /** The pieces. */
    wlmaker_dock_fx_piece_t   *pieces_ptr;
    /** Number of pieces. */
    size_t                    pieces;
    /** Number of pieces still flying. */
    size_t                    flying;
    /** Pieces that leave this area are gone. */
    struct wlr_box            area;
    /** Size of the icon. */
    int                       size;
    /** Timer for the next frame. */
    struct wl_event_source    *timer_event_source_ptr;
} wlmaker_dock_fx_kaboom_t;

/** State of the dock's visual effects. */
struct _wlmaker_dock_fx_t {
    /** Event loop, for timers. */
    struct wl_event_loop      *wl_event_loop_ptr;
    /** Tree holding all of it. */
    struct wlr_scene_tree     *wlr_scene_tree_ptr;

    /** Tree for the ghost. Below the dragged icon. */
    struct wlr_scene_tree     *ghost_tree_ptr;
    /** The ghost. */
    struct wlr_scene_buffer   *ghost_ptr;
    /** Covers the slot of a lifted tile. Below the ghost. */
    struct wlr_scene_buffer   *cover_ptr;
    /** Tree for the dragged icon. */
    struct wlr_scene_tree     *drag_tree_ptr;
    /** The dragged icon. */
    struct wlr_scene_buffer   *drag_ptr;
    /** Tree for the explosions. */
    struct wlr_scene_tree     *kaboom_tree_ptr;
    /** Tree for the balloon. On top. */
    struct wlr_scene_tree     *balloon_tree_ptr;

    /** Timer for the slide. */
    struct wl_event_source    *slide_timer_event_source_ptr;
    /** Current position of the slide. */
    float                     slide_x;
    /** Current position of the slide. */
    float                     slide_y;
    /** Step of the slide. */
    float                     slide_px;
    /** Step of the slide. */
    float                     slide_py;
    /** Destination of the slide. */
    int                       slide_to_x;
    /** Destination of the slide. */
    int                       slide_to_y;
    /** Direction of the slide. */
    int                       slide_dx;
    /** Direction of the slide. */
    int                       slide_dy;
    /** Steps of the slide so far. */
    int                       slide_count;
    /** Callback once the slide is done. */
    wlmaker_dock_fx_slide_done_t slide_done;
    /** Argument to @ref wlmaker_dock_fx_t::slide_done. */
    void                      *slide_ud_ptr;

    /** Running explosions. */
    bs_dllist_t               kabooms;

    /** Colors of the balloon. */
    wlmaker_dock_fx_balloon_style_t balloon_style;
    /** The balloon, when shown. */
    struct wlr_scene_buffer   *balloon_ptr;
    /** What the balloon is (to be) shown for. */
    const void                *balloon_owner_ptr;
    /** Text of the balloon to show. */
    char                      *balloon_text_ptr;
    /** What the balloon points at. */
    struct wlr_box            balloon_box;
    /** Area to keep the balloon within. */
    struct wlr_box            balloon_area;
    /** Timer for the balloon's delay. */
    struct wl_event_source    *balloon_timer_event_source_ptr;
    /** A balloon was just shown: The next one shows without delay. */
    bool                      balloon_ignore_timer;
    /** Clears @ref wlmaker_dock_fx_t::balloon_ignore_timer, once idle. */
    struct wl_event_source    *balloon_idle_event_source_ptr;
};

static void _wlmaker_dock_fx_kaboom_destroy(
    wlmaker_dock_fx_kaboom_t *kaboom_ptr);
static int _wlmaker_dock_fx_handle_kaboom_timer(void *data_ptr);
static bool _wlmaker_dock_fx_kaboom_step(wlmaker_dock_fx_kaboom_t *kaboom_ptr);
static int _wlmaker_dock_fx_handle_slide_timer(void *data_ptr);
static bool _wlmaker_dock_fx_slide_step(wlmaker_dock_fx_t *fx_ptr);
static void _wlmaker_dock_fx_slide_end(wlmaker_dock_fx_t *fx_ptr);
static int _wlmaker_dock_fx_handle_balloon_timer(void *data_ptr);
static void _wlmaker_dock_fx_handle_balloon_idle(void *data_ptr);
static void _wlmaker_dock_fx_balloon_show(wlmaker_dock_fx_t *fx_ptr);
static void _wlmaker_dock_fx_balloon_hide(wlmaker_dock_fx_t *fx_ptr);
static void _wlmaker_dock_fx_balloon_measure(
    const char *text_ptr,
    int *width_ptr,
    int *height_ptr,
    double *ascent_ptr,
    double *line_height_ptr);

/* == Data ================================================================= */

/** Frame delay of the kaboom: MINIATURIZE_ANIMATION_DELAY_Z * 2, in ms. */
static const int _wlmaker_dock_fx_kaboom_delay_msec = 20;
/** Window Maker's BALLOON_DELAY, in ms. */
static const int _wlmaker_dock_fx_balloon_delay_msec = 1000;
/** Height of the balloon's pointer. As SPACE in Window Maker's balloon.c. */
static const int _wlmaker_dock_fx_balloon_space = 12;
/** Font of the balloon: Window Maker's bold system font, at 12 pixels. */
static const char *_wlmaker_dock_fx_balloon_font_face = "Noto Sans";
/** Font size of the balloon. */
static const double _wlmaker_dock_fx_balloon_font_size = 12;
/** Delay between the steps of a slide, in ms. */
static const int _wlmaker_dock_fx_slide_delay_msec = 10;
/** IconSlideSpeed = fast: Slowdown. */
static const float _wlmaker_dock_fx_slide_slowdown = 3;
/** IconSlideSpeed = fast: Minimum step. */
static const float _wlmaker_dock_fx_slide_steps = 50;
/** Ends a slide after this many steps, in any case. */
static const int _wlmaker_dock_fx_slide_max_steps = 200;

/** Window Maker's balloon colors: Black on white, with a black outline. */
static const wlmaker_dock_fx_balloon_style_t _wlmaker_dock_fx_balloon_style = {
    .color = 0xffffffff, .text_color = 0xff000000, .border_color = 0xff000000 };

const bspl_desc_t wlmaker_dock_fx_balloon_style_desc[] = {
    BSPL_DESC_ARGB32(
        "Color", false, wlmaker_dock_fx_balloon_style_t, color, color,
        0xffffffff),
    BSPL_DESC_ARGB32(
        "TextColor", false, wlmaker_dock_fx_balloon_style_t,
        text_color, text_color, 0xff000000),
    BSPL_DESC_ARGB32(
        "BorderColor", false, wlmaker_dock_fx_balloon_style_t,
        border_color, border_color, 0xff000000),
    BSPL_DESC_SENTINEL()
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_dock_fx_t *wlmaker_dock_fx_create(
    struct wlr_scene_tree *parent_ptr,
    struct wl_event_loop *wl_event_loop_ptr)
{
    wlmaker_dock_fx_t *fx_ptr = logged_calloc(1, sizeof(*fx_ptr));
    if (NULL == fx_ptr) return NULL;
    fx_ptr->wl_event_loop_ptr = wl_event_loop_ptr;
    fx_ptr->balloon_style = _wlmaker_dock_fx_balloon_style;

    fx_ptr->wlr_scene_tree_ptr = wlr_scene_tree_create(parent_ptr);
    if (NULL == fx_ptr->wlr_scene_tree_ptr) {
        wlmaker_dock_fx_destroy(fx_ptr);
        return NULL;
    }
    fx_ptr->ghost_tree_ptr = wlr_scene_tree_create(fx_ptr->wlr_scene_tree_ptr);
    fx_ptr->drag_tree_ptr = wlr_scene_tree_create(fx_ptr->wlr_scene_tree_ptr);
    fx_ptr->kaboom_tree_ptr = wlr_scene_tree_create(fx_ptr->wlr_scene_tree_ptr);
    fx_ptr->balloon_tree_ptr = wlr_scene_tree_create(
        fx_ptr->wlr_scene_tree_ptr);
    if (NULL == fx_ptr->ghost_tree_ptr ||
        NULL == fx_ptr->drag_tree_ptr ||
        NULL == fx_ptr->kaboom_tree_ptr ||
        NULL == fx_ptr->balloon_tree_ptr) {
        wlmaker_dock_fx_destroy(fx_ptr);
        return NULL;
    }
    return fx_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_destroy(wlmaker_dock_fx_t *fx_ptr)
{
    bs_dllist_node_t *dlnode_ptr;
    while (NULL != (dlnode_ptr = fx_ptr->kabooms.head_ptr)) {
        _wlmaker_dock_fx_kaboom_destroy(BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_dock_fx_kaboom_t, dlnode));
    }
    wlmaker_dock_fx_balloon_leave(fx_ptr, NULL);
    if (NULL != fx_ptr->balloon_idle_event_source_ptr) {
        wl_event_source_remove(fx_ptr->balloon_idle_event_source_ptr);
        fx_ptr->balloon_idle_event_source_ptr = NULL;
    }
    wlmaker_dock_fx_drag_hide(fx_ptr);
    wlmaker_dock_fx_ghost_hide(fx_ptr);
    wlmaker_dock_fx_cover_hide(fx_ptr);

    if (NULL != fx_ptr->wlr_scene_tree_ptr) {
        wlr_scene_node_destroy(&fx_ptr->wlr_scene_tree_ptr->node);
        fx_ptr->wlr_scene_tree_ptr = NULL;
    }
    free(fx_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_set_balloon_style(
    wlmaker_dock_fx_t *fx_ptr,
    const wlmaker_dock_fx_balloon_style_t *style_ptr)
{
    fx_ptr->balloon_style = *style_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_drag_show(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y)
{
    if (NULL == fx_ptr->drag_ptr) {
        if (NULL == wlr_buffer_ptr) return;
        fx_ptr->drag_ptr = wlr_scene_buffer_create(
            fx_ptr->drag_tree_ptr, wlr_buffer_ptr);
        if (NULL == fx_ptr->drag_ptr) return;
    } else if (NULL != wlr_buffer_ptr) {
        wlr_scene_buffer_set_buffer(fx_ptr->drag_ptr, wlr_buffer_ptr);
    }
    wlr_scene_node_raise_to_top(&fx_ptr->wlr_scene_tree_ptr->node);
    wlr_scene_node_set_position(&fx_ptr->drag_ptr->node, x, y);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_drag_hide(wlmaker_dock_fx_t *fx_ptr)
{
    if (NULL != fx_ptr->slide_timer_event_source_ptr) {
        wl_event_source_remove(fx_ptr->slide_timer_event_source_ptr);
        fx_ptr->slide_timer_event_source_ptr = NULL;
    }
    fx_ptr->slide_done = NULL;
    if (NULL != fx_ptr->drag_ptr) {
        wlr_scene_node_destroy(&fx_ptr->drag_ptr->node);
        fx_ptr->drag_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_slide(
    wlmaker_dock_fx_t *fx_ptr,
    int to_x,
    int to_y,
    wlmaker_dock_fx_slide_done_t done,
    void *ud_ptr)
{
    if (NULL != fx_ptr->slide_timer_event_source_ptr) {
        wl_event_source_remove(fx_ptr->slide_timer_event_source_ptr);
        fx_ptr->slide_timer_event_source_ptr = NULL;
    }
    fx_ptr->slide_done = done;
    fx_ptr->slide_ud_ptr = ud_ptr;
    if (NULL == fx_ptr->drag_ptr) {
        _wlmaker_dock_fx_slide_end(fx_ptr);
        return;
    }

    // As `slide_windows` in Window Maker's misc.c.
    int from_x = fx_ptr->drag_ptr->node.x;
    int from_y = fx_ptr->drag_ptr->node.y;
    fx_ptr->slide_x = from_x;
    fx_ptr->slide_y = from_y;
    fx_ptr->slide_to_x = to_x;
    fx_ptr->slide_to_y = to_y;
    fx_ptr->slide_count = 0;
    fx_ptr->slide_dx = to_x - from_x;
    fx_ptr->slide_dy = to_y - from_y;
    float dx = fx_ptr->slide_dx, dy = fx_ptr->slide_dy;
    float steps = _wlmaker_dock_fx_slide_steps;
    if (abs(fx_ptr->slide_dx) > abs(fx_ptr->slide_dy)) {
        float px = dx / _wlmaker_dock_fx_slide_slowdown;
        if (px < steps && px > 0) px = steps;
        else if (px > -steps && px < 0) px = -steps;
        fx_ptr->slide_px = px;
        fx_ptr->slide_py = (0 == fx_ptr->slide_dx) ? 0 : px * dy / dx;
    } else {
        float py = dy / _wlmaker_dock_fx_slide_slowdown;
        if (py < steps && py > 0) py = steps;
        else if (py > -steps && py < 0) py = -steps;
        fx_ptr->slide_py = py;
        fx_ptr->slide_px = (0 == fx_ptr->slide_dy) ? 0 : py * dx / dy;
    }

    fx_ptr->slide_timer_event_source_ptr = wl_event_loop_add_timer(
        fx_ptr->wl_event_loop_ptr,
        _wlmaker_dock_fx_handle_slide_timer,
        fx_ptr);
    if (NULL == fx_ptr->slide_timer_event_source_ptr) {
        _wlmaker_dock_fx_slide_end(fx_ptr);
        return;
    }
    wl_event_source_timer_update(fx_ptr->slide_timer_event_source_ptr,
                                 _wlmaker_dock_fx_slide_delay_msec);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_ghost_show(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y)
{
    if (NULL != wlr_buffer_ptr) {
        struct wlr_buffer *ghost_ptr = wlmaker_dock_fx_create_ghost_buffer(
            wlr_buffer_ptr);
        if (NULL == ghost_ptr) return;
        if (NULL == fx_ptr->ghost_ptr) {
            fx_ptr->ghost_ptr = wlr_scene_buffer_create(
                fx_ptr->ghost_tree_ptr, ghost_ptr);
        } else {
            wlr_scene_buffer_set_buffer(fx_ptr->ghost_ptr, ghost_ptr);
        }
        wlr_buffer_drop(ghost_ptr);
    }
    if (NULL == fx_ptr->ghost_ptr) return;
    wlr_scene_node_raise_to_top(&fx_ptr->wlr_scene_tree_ptr->node);
    wlr_scene_node_set_enabled(&fx_ptr->ghost_ptr->node, true);
    wlr_scene_node_set_position(&fx_ptr->ghost_ptr->node, x, y);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_cover_show(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y)
{
    wlmaker_dock_fx_cover_hide(fx_ptr);
    if (NULL == wlr_buffer_ptr) return;
    fx_ptr->cover_ptr = wlr_scene_buffer_create(
        fx_ptr->ghost_tree_ptr, wlr_buffer_ptr);
    if (NULL == fx_ptr->cover_ptr) return;
    wlr_scene_node_lower_to_bottom(&fx_ptr->cover_ptr->node);
    wlr_scene_node_raise_to_top(&fx_ptr->wlr_scene_tree_ptr->node);
    wlr_scene_node_set_position(&fx_ptr->cover_ptr->node, x, y);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_cover_hide(wlmaker_dock_fx_t *fx_ptr)
{
    if (NULL == fx_ptr->cover_ptr) return;
    wlr_scene_node_destroy(&fx_ptr->cover_ptr->node);
    fx_ptr->cover_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_ghost_hide(wlmaker_dock_fx_t *fx_ptr)
{
    if (NULL == fx_ptr->ghost_ptr) return;
    wlr_scene_node_destroy(&fx_ptr->ghost_ptr->node);
    fx_ptr->ghost_ptr = NULL;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_fx_ghost_visible(wlmaker_dock_fx_t *fx_ptr)
{
    return NULL != fx_ptr->ghost_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_kaboom(
    wlmaker_dock_fx_t *fx_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int x,
    int y,
    const struct wlr_box *area_ptr)
{
    if (NULL == wlr_buffer_ptr) return;
    wlmaker_dock_fx_kaboom_t *kaboom_ptr = logged_calloc(
        1, sizeof(*kaboom_ptr));
    if (NULL == kaboom_ptr) return;
    kaboom_ptr->fx_ptr = fx_ptr;
    kaboom_ptr->area = *area_ptr;
    kaboom_ptr->size = BS_MIN(wlr_buffer_ptr->width, wlr_buffer_ptr->height);
    bs_dllist_push_back(&fx_ptr->kabooms, &kaboom_ptr->dlnode);

    int n = kaboom_ptr->size / _WLMAKER_DOCK_FX_PIECE_SIZE;
    kaboom_ptr->wlr_scene_tree_ptr = wlr_scene_tree_create(
        fx_ptr->kaboom_tree_ptr);
    if (0 >= n || NULL == kaboom_ptr->wlr_scene_tree_ptr) {
        _wlmaker_dock_fx_kaboom_destroy(kaboom_ptr);
        return;
    }
    kaboom_ptr->pieces_ptr = logged_calloc(
        n * n, sizeof(wlmaker_dock_fx_piece_t));
    if (NULL == kaboom_ptr->pieces_ptr) {
        _wlmaker_dock_fx_kaboom_destroy(kaboom_ptr);
        return;
    }

    // As Window Maker's DoKaboom: Half of the pieces fly, chosen at random.
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (0 == rand() % 2) continue;
            wlmaker_dock_fx_piece_t *p_ptr =
                &kaboom_ptr->pieces_ptr[kaboom_ptr->pieces++];
            p_ptr->px = (x + i * _WLMAKER_DOCK_FX_PIECE_SIZE) <<
                _WLMAKER_DOCK_FX_KAB_PRECISION;
            p_ptr->py = y + j * _WLMAKER_DOCK_FX_PIECE_SIZE;
            p_ptr->pvx = rand() % (1 << (_WLMAKER_DOCK_FX_KAB_PRECISION + 3)) -
                (1 << (_WLMAKER_DOCK_FX_KAB_PRECISION + 3)) / 2;
            p_ptr->pvy = -15 - rand() % 7;

            p_ptr->wlr_scene_buffer_ptr = wlr_scene_buffer_create(
                kaboom_ptr->wlr_scene_tree_ptr, wlr_buffer_ptr);
            if (NULL == p_ptr->wlr_scene_buffer_ptr) continue;
            struct wlr_fbox src = {
                .x = i * _WLMAKER_DOCK_FX_PIECE_SIZE,
                .y = j * _WLMAKER_DOCK_FX_PIECE_SIZE,
                .width = _WLMAKER_DOCK_FX_PIECE_SIZE,
                .height = _WLMAKER_DOCK_FX_PIECE_SIZE };
            wlr_scene_buffer_set_source_box(p_ptr->wlr_scene_buffer_ptr, &src);
            wlr_scene_buffer_set_dest_size(
                p_ptr->wlr_scene_buffer_ptr,
                _WLMAKER_DOCK_FX_PIECE_SIZE, _WLMAKER_DOCK_FX_PIECE_SIZE);
            wlr_scene_node_set_position(
                &p_ptr->wlr_scene_buffer_ptr->node,
                p_ptr->px >> _WLMAKER_DOCK_FX_KAB_PRECISION, p_ptr->py);
            // Shown from the first step on, as in Window Maker.
            wlr_scene_node_set_enabled(
                &p_ptr->wlr_scene_buffer_ptr->node, false);
            ++kaboom_ptr->flying;
        }
    }

    wlr_scene_node_raise_to_top(&fx_ptr->wlr_scene_tree_ptr->node);
    kaboom_ptr->timer_event_source_ptr = wl_event_loop_add_timer(
        fx_ptr->wl_event_loop_ptr,
        _wlmaker_dock_fx_handle_kaboom_timer,
        kaboom_ptr);
    if (NULL == kaboom_ptr->timer_event_source_ptr) {
        _wlmaker_dock_fx_kaboom_destroy(kaboom_ptr);
        return;
    }
    // The first step comes right away.
    wl_event_source_timer_update(kaboom_ptr->timer_event_source_ptr, 1);
}

/* ------------------------------------------------------------------------- */
size_t wlmaker_dock_fx_kabooms(wlmaker_dock_fx_t *fx_ptr)
{
    return bs_dllist_size(&fx_ptr->kabooms);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_balloon_enter(
    wlmaker_dock_fx_t *fx_ptr,
    const void *owner_ptr,
    const char *text_ptr,
    const struct wlr_box *box_ptr,
    const struct wlr_box *area_ptr)
{
    // Same thing, same place: Nothing changes.
    if (owner_ptr == fx_ptr->balloon_owner_ptr &&
        NULL != fx_ptr->balloon_text_ptr && NULL != text_ptr &&
        0 == strcmp(text_ptr, fx_ptr->balloon_text_ptr) &&
        wlr_box_equal(box_ptr, &fx_ptr->balloon_box)) return;

    bool ignore_timer = fx_ptr->balloon_ignore_timer;
    _wlmaker_dock_fx_balloon_hide(fx_ptr);
    if (NULL == text_ptr || '\0' == *text_ptr) {
        fx_ptr->balloon_ignore_timer = false;
        return;
    }
    fx_ptr->balloon_owner_ptr = owner_ptr;
    fx_ptr->balloon_text_ptr = logged_strdup(text_ptr);
    if (NULL == fx_ptr->balloon_text_ptr) return;
    fx_ptr->balloon_box = *box_ptr;
    fx_ptr->balloon_area = *area_ptr;

    if (ignore_timer) {
        _wlmaker_dock_fx_balloon_show(fx_ptr);
        return;
    }
    fx_ptr->balloon_timer_event_source_ptr = wl_event_loop_add_timer(
        fx_ptr->wl_event_loop_ptr,
        _wlmaker_dock_fx_handle_balloon_timer,
        fx_ptr);
    if (NULL == fx_ptr->balloon_timer_event_source_ptr) return;
    wl_event_source_timer_update(fx_ptr->balloon_timer_event_source_ptr,
                                 _wlmaker_dock_fx_balloon_delay_msec);
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_fx_balloon_leave(
    wlmaker_dock_fx_t *fx_ptr,
    const void *owner_ptr)
{
    if (NULL != owner_ptr && owner_ptr != fx_ptr->balloon_owner_ptr) return;
    _wlmaker_dock_fx_balloon_hide(fx_ptr);
    if (NULL == owner_ptr) {
        fx_ptr->balloon_ignore_timer = false;
        return;
    }
    // Keeps showing without delay if the pointer enters another tile right
    // away, while processing the same event. Otherwise, from the next idle
    // round on, the delay applies again.
    if (fx_ptr->balloon_ignore_timer &&
        NULL == fx_ptr->balloon_idle_event_source_ptr) {
        fx_ptr->balloon_idle_event_source_ptr = wl_event_loop_add_idle(
            fx_ptr->wl_event_loop_ptr,
            _wlmaker_dock_fx_handle_balloon_idle,
            fx_ptr);
    }
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dock_fx_balloon_visible(wlmaker_dock_fx_t *fx_ptr)
{
    return NULL != fx_ptr->balloon_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_buffer *wlmaker_dock_fx_create_ghost_buffer(
    struct wlr_buffer *wlr_buffer_ptr)
{
    void *data_ptr;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(
            wlr_buffer_ptr, WLR_BUFFER_DATA_PTR_ACCESS_READ,
            &data_ptr, &format, &stride)) {
        bs_log(BS_WARNING, "Failed wlr_buffer_begin_data_ptr_access(%p)",
               wlr_buffer_ptr);
        return NULL;
    }
    struct wlr_buffer *ghost_ptr = bs_gfxbuf_create_wlr_buffer(
        wlr_buffer_ptr->width, wlr_buffer_ptr->height);
    cairo_t *cairo_ptr = NULL;
    if (NULL != ghost_ptr) cairo_ptr = cairo_create_from_wlr_buffer(ghost_ptr);
    if (NULL != cairo_ptr) {
        cairo_surface_t *s_ptr = cairo_image_surface_create_for_data(
            data_ptr, CAIRO_FORMAT_ARGB32,
            wlr_buffer_ptr->width, wlr_buffer_ptr->height, stride);
        cairo_set_source_surface(cairo_ptr, s_ptr, 0, 0);
        cairo_paint(cairo_ptr);
        cairo_surface_destroy(s_ptr);
        // Window Maker: RClearImage() with white, at alpha 200.
        cairo_set_operator(cairo_ptr, CAIRO_OPERATOR_ATOP);
        cairo_set_source_rgba(cairo_ptr, 1.0, 1.0, 1.0, 200.0 / 255.0);
        cairo_paint(cairo_ptr);
        cairo_destroy(cairo_ptr);
    } else if (NULL != ghost_ptr) {
        wlr_buffer_drop(ghost_ptr);
        ghost_ptr = NULL;
    }
    wlr_buffer_end_data_ptr_access(wlr_buffer_ptr);
    return ghost_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_buffer *wlmaker_dock_fx_create_balloon_buffer(
    const wlmaker_dock_fx_balloon_style_t *style_ptr,
    const char *text_ptr,
    bool pointer_right,
    bool pointer_top)
{
    if (NULL == style_ptr) style_ptr = &_wlmaker_dock_fx_balloon_style;
    int width, height;
    double ascent, line_height;
    _wlmaker_dock_fx_balloon_measure(
        text_ptr, &width, &height, &ascent, &line_height);
    const int space = _wlmaker_dock_fx_balloon_space;

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        width, height + space);
    if (NULL == wlr_buffer_ptr) return NULL;
    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }

    // As Window Maker's drawBalloon(): A box with corners rounded by `rad`
    // diameter circles, and a pointer 12 pixels high. Drawn without
    // antialiasing, like the X11 shapes.
    int x = 0, y = pointer_top ? space : 0, w = width, h = height;
    double rad = h * 3 / 10;
    int w1 = BS_MAX(h, 21);
    double pt[3][2], ipt[3][2];
    if (pointer_top) {
        pt[0][1] = y; pt[1][1] = y - space; pt[2][1] = y;
        ipt[0][1] = y + 1; ipt[1][1] = y - space + 1; ipt[2][1] = y + 1;
    } else {
        pt[0][1] = y + h - 1; pt[1][1] = y + h - 1 + space; pt[2][1] = y + h - 1;
        ipt[0][1] = y + h - 2; ipt[1][1] = y + h - 2 + space; ipt[2][1] = y + h - 2;
    }
    if (pointer_right) {
        pt[0][0] = x + w - w1 + 2 * w1 / 16;
        pt[1][0] = x + w - w1 + 11 * w1 / 16;
        pt[2][0] = x + w - w1 + 7 * w1 / 16;
        ipt[0][0] = x + 1 + w - w1 + 2 * (w1 - 1) / 16;
        ipt[1][0] = x + 1 + w - w1 + 11 * (w1 - 1) / 16;
        ipt[2][0] = x + 1 + w - w1 + 7 * (w1 - 1) / 16;
    } else {
        pt[0][0] = x + w1 - 2 * w1 / 16;
        pt[1][0] = x + w1 - 11 * w1 / 16;
        pt[2][0] = x + w1 - 7 * w1 / 16;
        ipt[0][0] = x - 1 + w1 - 2 * (w1 - 1) / 16;
        ipt[1][0] = x - 1 + w1 - 11 * (w1 - 1) / 16;
        ipt[2][0] = x - 1 + w1 - 7 * (w1 - 1) / 16;
    }

    cairo_set_antialias(cairo_ptr, CAIRO_ANTIALIAS_NONE);
    for (int pass = 0; pass < 2; ++pass) {
        // Pass 0: The outline, black. Pass 1: The interior, white.
        int i = pass;
        double r = rad / 2.0;
        cairo_new_path(cairo_ptr);
        cairo_arc(cairo_ptr, x + i + r, y + i + r, r, M_PI, 1.5 * M_PI);
        cairo_arc(cairo_ptr, x + w - i - r, y + i + r, r, 1.5 * M_PI, 2 * M_PI);
        cairo_arc(cairo_ptr, x + w - i - r, y + h - i - r, r, 0, 0.5 * M_PI);
        cairo_arc(cairo_ptr, x + i + r, y + h - i - r, r, 0.5 * M_PI, M_PI);
        cairo_close_path(cairo_ptr);
        double (*p)[2] = (0 == pass) ? pt : ipt;
        cairo_move_to(cairo_ptr, p[0][0], p[0][1]);
        cairo_line_to(cairo_ptr, p[1][0], p[1][1]);
        cairo_line_to(cairo_ptr, p[2][0], p[2][1]);
        cairo_close_path(cairo_ptr);
        cairo_set_fill_rule(cairo_ptr, CAIRO_FILL_RULE_WINDING);
        cairo_set_source_argb8888(
            cairo_ptr,
            (0 == pass) ? style_ptr->border_color : style_ptr->color);
        cairo_fill(cairo_ptr);
    }
    // The pointer's outline: Its two edges, 2 pixels wide.
    cairo_set_source_argb8888(cairo_ptr, style_ptr->border_color);
    cairo_set_line_width(cairo_ptr, 1.0);
    for (int i = 0; i < 2; ++i) {
        double shift = (0 == i) ? 0 : (pointer_right ? 1 : -1);
        cairo_new_path(cairo_ptr);
        cairo_move_to(cairo_ptr, pt[0][0] + 0.5 + shift, pt[0][1] + 0.5);
        cairo_line_to(cairo_ptr, pt[1][0] + 0.5, pt[1][1] + 0.5);
        cairo_line_to(cairo_ptr, pt[2][0] + 0.5 - shift, pt[2][1] + 0.5);
        cairo_stroke(cairo_ptr);
    }

    // The text: Left-aligned at 8 pixels, from 2 pixels below the top.
    cairo_set_antialias(cairo_ptr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_select_font_face(cairo_ptr, _wlmaker_dock_fx_balloon_font_face,
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cairo_ptr, _wlmaker_dock_fx_balloon_font_size);
    cairo_set_source_argb8888(cairo_ptr, style_ptr->text_color);
    char *t_ptr = logged_strdup(text_ptr);
    if (NULL != t_ptr) {
        double ty = y + 2 + ascent;
        for (char *line_ptr = t_ptr, *next_ptr; NULL != line_ptr;
             line_ptr = next_ptr) {
            next_ptr = strchr(line_ptr, '\n');
            if (NULL != next_ptr) *next_ptr++ = '\0';
            cairo_move_to(cairo_ptr, 8, ty);
            cairo_show_text(cairo_ptr, line_ptr);
            ty += line_height;
        }
        free(t_ptr);
    }
    cairo_destroy(cairo_ptr);
    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
struct wlr_box wlmaker_dock_fx_balloon_position(
    const struct wlr_box *box_ptr,
    const struct wlr_box *area_ptr,
    int width,
    int height,
    bool *pointer_right_ptr,
    bool *pointer_top_ptr)
{
    // As Window Maker's showText(), relative to the area.
    const int space = _wlmaker_dock_fx_balloon_space;
    int x = box_ptr->x - area_ptr->x, y = box_ptr->y - area_ptr->y;
    int w = box_ptr->width, h = box_ptr->height;
    int bx, by;
    if (x + width > area_ptr->width) {
        *pointer_right_ptr = true;
        bx = x - width + w / 2;
        if (bx < 0) bx = 0;
    } else {
        *pointer_right_ptr = false;
        bx = x + w / 2;
    }
    if (bx + width > area_ptr->width) bx = area_ptr->width - width;

    if (y - (height + space) < 0) {
        *pointer_top_ptr = true;
        by = y + h - 1;
    } else {
        *pointer_top_ptr = false;
        by = y - (height + space);
    }
    return (struct wlr_box){
        .x = area_ptr->x + bx, .y = area_ptr->y + by,
        .width = width, .height = height + space };
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Destroys the explosion. */
void _wlmaker_dock_fx_kaboom_destroy(wlmaker_dock_fx_kaboom_t *kaboom_ptr)
{
    if (NULL != kaboom_ptr->timer_event_source_ptr) {
        wl_event_source_remove(kaboom_ptr->timer_event_source_ptr);
        kaboom_ptr->timer_event_source_ptr = NULL;
    }
    if (NULL != kaboom_ptr->wlr_scene_tree_ptr) {
        wlr_scene_node_destroy(&kaboom_ptr->wlr_scene_tree_ptr->node);
        kaboom_ptr->wlr_scene_tree_ptr = NULL;
    }
    if (NULL != kaboom_ptr->pieces_ptr) free(kaboom_ptr->pieces_ptr);
    bs_dllist_remove(&kaboom_ptr->fx_ptr->kabooms, &kaboom_ptr->dlnode);
    free(kaboom_ptr);
}

/* ------------------------------------------------------------------------- */
/** Timer callback: Next frame of the explosion. */
int _wlmaker_dock_fx_handle_kaboom_timer(void *data_ptr)
{
    wlmaker_dock_fx_kaboom_t *kaboom_ptr = data_ptr;
    if (!_wlmaker_dock_fx_kaboom_step(kaboom_ptr)) {
        _wlmaker_dock_fx_kaboom_destroy(kaboom_ptr);
        return 0;
    }
    wl_event_source_timer_update(kaboom_ptr->timer_event_source_ptr,
                                 _wlmaker_dock_fx_kaboom_delay_msec);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Moves the pieces one step, as in Window Maker's DoKaboom: Gravity pulls
 * them down by one more pixel per frame.
 *
 * @return Whether any piece is still flying.
 */
bool _wlmaker_dock_fx_kaboom_step(wlmaker_dock_fx_kaboom_t *kaboom_ptr)
{
    const struct wlr_box *a_ptr = &kaboom_ptr->area;
    for (size_t i = 0; i < kaboom_ptr->pieces; ++i) {
        wlmaker_dock_fx_piece_t *p_ptr = &kaboom_ptr->pieces_ptr[i];
        if (NULL == p_ptr->wlr_scene_buffer_ptr) continue;
        p_ptr->px += p_ptr->pvx;
        p_ptr->py += p_ptr->pvy;
        p_ptr->pvy++;
        int px = p_ptr->px >> _WLMAKER_DOCK_FX_KAB_PRECISION;
        if (px < a_ptr->x - kaboom_ptr->size ||
            px > a_ptr->x + a_ptr->width ||
            p_ptr->py >= a_ptr->y + a_ptr->height) {
            wlr_scene_node_destroy(&p_ptr->wlr_scene_buffer_ptr->node);
            p_ptr->wlr_scene_buffer_ptr = NULL;
            --kaboom_ptr->flying;
            continue;
        }
        wlr_scene_node_set_enabled(&p_ptr->wlr_scene_buffer_ptr->node, true);
        wlr_scene_node_set_position(
            &p_ptr->wlr_scene_buffer_ptr->node, px, p_ptr->py);
    }
    return 0 < kaboom_ptr->flying;
}

/* ------------------------------------------------------------------------- */
/** Timer callback: Next step of the slide. */
int _wlmaker_dock_fx_handle_slide_timer(void *data_ptr)
{
    wlmaker_dock_fx_t *fx_ptr = data_ptr;
    if (_wlmaker_dock_fx_slide_step(fx_ptr)) {
        wl_event_source_timer_update(fx_ptr->slide_timer_event_source_ptr,
                                     _wlmaker_dock_fx_slide_delay_msec);
        return 0;
    }
    wl_event_source_remove(fx_ptr->slide_timer_event_source_ptr);
    fx_ptr->slide_timer_event_source_ptr = NULL;
    _wlmaker_dock_fx_slide_end(fx_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * One step of the slide, as in Window Maker's `slide_windows`.
 *
 * @return Whether the destination is not reached yet.
 */
bool _wlmaker_dock_fx_slide_step(wlmaker_dock_fx_t *fx_ptr)
{
    if (NULL == fx_ptr->drag_ptr ||
        ++fx_ptr->slide_count > _wlmaker_dock_fx_slide_max_steps) return false;
    int to_x = fx_ptr->slide_to_x, to_y = fx_ptr->slide_to_y;
    if ((int)fx_ptr->slide_x == to_x && (int)fx_ptr->slide_y == to_y) {
        return false;
    }
    float steps = _wlmaker_dock_fx_slide_steps;
    float slowdown = _wlmaker_dock_fx_slide_slowdown;
    float dx = fx_ptr->slide_dx, dy = fx_ptr->slide_dy;

    fx_ptr->slide_x += fx_ptr->slide_px;
    fx_ptr->slide_y += fx_ptr->slide_py;
    if ((fx_ptr->slide_px < 0 && (int)fx_ptr->slide_x < to_x) ||
        (fx_ptr->slide_px > 0 && (int)fx_ptr->slide_x > to_x)) {
        fx_ptr->slide_x = to_x;
    }
    if ((fx_ptr->slide_py < 0 && (int)fx_ptr->slide_y < to_y) ||
        (fx_ptr->slide_py > 0 && (int)fx_ptr->slide_y > to_y)) {
        fx_ptr->slide_y = to_y;
    }

    if (abs(fx_ptr->slide_dx) > abs(fx_ptr->slide_dy)) {
        float px = fx_ptr->slide_px * (1.0f - 1.0f / slowdown);
        if (px < steps && px > 0) px = steps;
        else if (px > -steps && px < 0) px = -steps;
        fx_ptr->slide_px = px;
        fx_ptr->slide_py = (0 == fx_ptr->slide_dx) ? 0 : px * dy / dx;
    } else {
        float py = fx_ptr->slide_py * (1.0f - 1.0f / slowdown);
        if (py < steps && py > 0) py = steps;
        else if (py > -steps && py < 0) py = -steps;
        fx_ptr->slide_py = py;
        fx_ptr->slide_px = (0 == fx_ptr->slide_dy) ? 0 : py * dx / dy;
    }
    wlr_scene_node_set_position(&fx_ptr->drag_ptr->node,
                                (int)fx_ptr->slide_x, (int)fx_ptr->slide_y);
    return (int)fx_ptr->slide_x != to_x || (int)fx_ptr->slide_y != to_y;
}

/* ------------------------------------------------------------------------- */
/** Ends the slide: Hides the dragged icon, and calls the callback. */
void _wlmaker_dock_fx_slide_end(wlmaker_dock_fx_t *fx_ptr)
{
    wlmaker_dock_fx_slide_done_t done = fx_ptr->slide_done;
    void *ud_ptr = fx_ptr->slide_ud_ptr;
    wlmaker_dock_fx_drag_hide(fx_ptr);
    if (NULL != done) done(ud_ptr);
}

/* ------------------------------------------------------------------------- */
/** Timer callback: The balloon delay is over. Shows the balloon. */
int _wlmaker_dock_fx_handle_balloon_timer(void *data_ptr)
{
    wlmaker_dock_fx_t *fx_ptr = data_ptr;
    wl_event_source_remove(fx_ptr->balloon_timer_event_source_ptr);
    fx_ptr->balloon_timer_event_source_ptr = NULL;
    _wlmaker_dock_fx_balloon_show(fx_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Idle callback: No balloon was entered right after leaving one. */
void _wlmaker_dock_fx_handle_balloon_idle(void *data_ptr)
{
    wlmaker_dock_fx_t *fx_ptr = data_ptr;
    fx_ptr->balloon_idle_event_source_ptr = NULL;
    if (NULL == fx_ptr->balloon_ptr &&
        NULL == fx_ptr->balloon_timer_event_source_ptr) {
        fx_ptr->balloon_ignore_timer = false;
    }
}

/* ------------------------------------------------------------------------- */
/** Shows the balloon for @ref wlmaker_dock_fx_t::balloon_text_ptr. */
void _wlmaker_dock_fx_balloon_show(wlmaker_dock_fx_t *fx_ptr)
{
    if (NULL == fx_ptr->balloon_text_ptr) return;
    int width, height;
    _wlmaker_dock_fx_balloon_measure(
        fx_ptr->balloon_text_ptr, &width, &height, NULL, NULL);
    bool right, top;
    struct wlr_box pos = wlmaker_dock_fx_balloon_position(
        &fx_ptr->balloon_box, &fx_ptr->balloon_area, width, height,
        &right, &top);
    struct wlr_buffer *wlr_buffer_ptr = wlmaker_dock_fx_create_balloon_buffer(
        &fx_ptr->balloon_style, fx_ptr->balloon_text_ptr, right, top);
    if (NULL == wlr_buffer_ptr) return;
    if (NULL != fx_ptr->balloon_ptr) {
        wlr_scene_node_destroy(&fx_ptr->balloon_ptr->node);
    }
    fx_ptr->balloon_ptr = wlr_scene_buffer_create(
        fx_ptr->balloon_tree_ptr, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
    if (NULL == fx_ptr->balloon_ptr) return;
    wlr_scene_node_raise_to_top(&fx_ptr->wlr_scene_tree_ptr->node);
    wlr_scene_node_set_position(&fx_ptr->balloon_ptr->node, pos.x, pos.y);
    fx_ptr->balloon_ignore_timer = true;
}

/* ------------------------------------------------------------------------- */
/** Hides the balloon, and cancels a pending one. */
void _wlmaker_dock_fx_balloon_hide(wlmaker_dock_fx_t *fx_ptr)
{
    if (NULL != fx_ptr->balloon_timer_event_source_ptr) {
        wl_event_source_remove(fx_ptr->balloon_timer_event_source_ptr);
        fx_ptr->balloon_timer_event_source_ptr = NULL;
    }
    if (NULL != fx_ptr->balloon_ptr) {
        wlr_scene_node_destroy(&fx_ptr->balloon_ptr->node);
        fx_ptr->balloon_ptr = NULL;
    }
    if (NULL != fx_ptr->balloon_text_ptr) {
        free(fx_ptr->balloon_text_ptr);
        fx_ptr->balloon_text_ptr = NULL;
    }
    fx_ptr->balloon_owner_ptr = NULL;
    fx_ptr->balloon_box = (struct wlr_box){};
}

/* ------------------------------------------------------------------------- */
/**
 * Measures the balloon's box for `text_ptr`, as Window Maker: The widest line
 * plus 16, the lines' height plus 4, at least 16 high and as wide as high.
 */
void _wlmaker_dock_fx_balloon_measure(
    const char *text_ptr,
    int *width_ptr,
    int *height_ptr,
    double *ascent_ptr,
    double *line_height_ptr)
{
    cairo_surface_t *s_ptr = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo_ptr = cairo_create(s_ptr);
    cairo_select_font_face(cairo_ptr, _wlmaker_dock_fx_balloon_font_face,
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cairo_ptr, _wlmaker_dock_fx_balloon_font_size);
    cairo_font_extents_t font_extents;
    cairo_font_extents(cairo_ptr, &font_extents);
    int line_height = (int)(font_extents.ascent + font_extents.descent + 0.5);

    double max_width = 0;
    int lines = 0;
    char *t_ptr = logged_strdup(NULL != text_ptr ? text_ptr : "");
    if (NULL != t_ptr) {
        for (char *line_ptr = t_ptr, *next_ptr; NULL != line_ptr;
             line_ptr = next_ptr) {
            next_ptr = strchr(line_ptr, '\n');
            if (NULL != next_ptr) *next_ptr++ = '\0';
            cairo_text_extents_t extents;
            cairo_text_extents(cairo_ptr, line_ptr, &extents);
            max_width = BS_MAX(max_width, extents.x_advance);
            ++lines;
        }
        free(t_ptr);
    }
    cairo_destroy(cairo_ptr);
    cairo_surface_destroy(s_ptr);

    int width = (int)(max_width + 0.5) + 16;
    int height = lines * line_height + 4;
    if (height < 16) height = 16;
    if (width < height) width = height;
    *width_ptr = width;
    *height_ptr = height;
    if (NULL != ascent_ptr) *ascent_ptr = font_extents.ascent;
    if (NULL != line_height_ptr) *line_height_ptr = line_height;
}

/* == Unit tests =========================================================== */

static void test_kaboom(bs_test_t *test_ptr);
static void test_slide(bs_test_t *test_ptr);
static void test_ghost(bs_test_t *test_ptr);
static void test_balloon(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_dock_fx_test_cases[] = {
    { true, "kaboom", test_kaboom },
    { true, "slide", test_slide },
    { true, "ghost", test_ghost },
    { true, "balloon", test_balloon },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dock_fx_test_set = BS_TEST_SET(
    true, "dock_fx", _wlmaker_dock_fx_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests the explosion: Half the pieces fly, rise, fall and are gone. */
void test_kaboom(bs_test_t *test_ptr)
{
    struct wl_event_loop *loop_ptr = wl_event_loop_create();
    struct wlr_scene *wlr_scene_ptr = wlr_scene_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_scene_ptr);
    wlmaker_dock_fx_t *fx_ptr = wlmaker_dock_fx_create(
        &wlr_scene_ptr->tree, loop_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fx_ptr);

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(64, 64);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_buffer_ptr);
    struct wlr_box area = { .width = 1024, .height = 768 };
    srand(42);
    wlmaker_dock_fx_kaboom(fx_ptr, wlr_buffer_ptr, 500, 300, &area);
    wlr_buffer_drop(wlr_buffer_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlmaker_dock_fx_kabooms(fx_ptr));

    wlmaker_dock_fx_kaboom_t *k_ptr = BS_CONTAINER_OF(
        fx_ptr->kabooms.head_ptr, wlmaker_dock_fx_kaboom_t, dlnode);
    // About half of the 16x16 pieces.
    BS_TEST_VERIFY_TRUE(test_ptr, 64 < k_ptr->pieces && 192 > k_ptr->pieces);
    BS_TEST_VERIFY_EQ(test_ptr, k_ptr->pieces, k_ptr->flying);
    for (size_t i = 0; i < k_ptr->pieces; ++i) {
        wlmaker_dock_fx_piece_t *p_ptr = &k_ptr->pieces_ptr[i];
        BS_TEST_VERIFY_TRUE(test_ptr, -21 <= p_ptr->pvy && -15 >= p_ptr->pvy);
        BS_TEST_VERIFY_TRUE(test_ptr, -64 <= p_ptr->pvx && 64 > p_ptr->pvx);
    }

    // First step: All pieces rose, by their initial speed.
    int py0 = k_ptr->pieces_ptr[0].py, pvy0 = k_ptr->pieces_ptr[0].pvy;
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmaker_dock_fx_kaboom_step(k_ptr));
    BS_TEST_VERIFY_EQ(test_ptr, py0 + pvy0, k_ptr->pieces_ptr[0].py);
    BS_TEST_VERIFY_EQ(test_ptr, pvy0 + 1, k_ptr->pieces_ptr[0].pvy);

    // From y=300 in 768 high: Gone after at most ~ 21 + sqrt(2*(768+230)).
    int frames = 1;
    while (_wlmaker_dock_fx_kaboom_step(k_ptr)) ++frames;
    BS_TEST_VERIFY_TRUE(test_ptr, 40 < frames && 70 > frames);
    BS_TEST_VERIFY_EQ(test_ptr, 0, k_ptr->flying);

    wlmaker_dock_fx_destroy(fx_ptr);
    wlr_scene_node_destroy(&wlr_scene_ptr->tree.node);
    wl_event_loop_destroy(loop_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests the slide: Reaches the destination in a few steps, calls back. */
static void _test_slide_done(void *ud_ptr) { *(int*)ud_ptr += 1; }
void test_slide(bs_test_t *test_ptr)
{
    struct wl_event_loop *loop_ptr = wl_event_loop_create();
    struct wlr_scene *wlr_scene_ptr = wlr_scene_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_scene_ptr);
    wlmaker_dock_fx_t *fx_ptr = wlmaker_dock_fx_create(
        &wlr_scene_ptr->tree, loop_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fx_ptr);

    // Without a dragged icon: Done right away.
    int done = 0;
    wlmaker_dock_fx_slide(fx_ptr, 10, 10, _test_slide_done, &done);
    BS_TEST_VERIFY_EQ(test_ptr, 1, done);

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(64, 64);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_buffer_ptr);
    wlmaker_dock_fx_drag_show(fx_ptr, wlr_buffer_ptr, 600, 300);
    wlr_buffer_drop(wlr_buffer_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 600, fx_ptr->drag_ptr->node.x);

    wlmaker_dock_fx_slide(fx_ptr, 1856, 64, _test_slide_done, &done);
    int steps = 0;
    while (_wlmaker_dock_fx_slide_step(fx_ptr)) {
        ++steps;
        BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, 100 > steps);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 1856, fx_ptr->drag_ptr->node.x);
    BS_TEST_VERIFY_EQ(test_ptr, 64, fx_ptr->drag_ptr->node.y);
    // Fast: First step 1/3 of the way, then at least 50 pixels.
    BS_TEST_VERIFY_TRUE(test_ptr, 3 < steps && 30 > steps);
    _wlmaker_dock_fx_slide_end(fx_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 2, done);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, fx_ptr->drag_ptr);

    wlmaker_dock_fx_destroy(fx_ptr);
    wlr_scene_node_destroy(&wlr_scene_ptr->tree.node);
    wl_event_loop_destroy(loop_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests the ghost: The image, whitened with white at alpha 200. */
void test_ghost(bs_test_t *test_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(4, 4);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_buffer_ptr);
    bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(wlr_buffer_ptr);
    bs_gfxbuf_clear(g_ptr, 0xff000000);
    g_ptr->data_ptr[1] = 0x00000000;

    struct wlr_buffer *ghost_ptr = wlmaker_dock_fx_create_ghost_buffer(
        wlr_buffer_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, ghost_ptr);
    bs_gfxbuf_t *gg_ptr = bs_gfxbuf_from_wlr_buffer(ghost_ptr);
    // Black becomes 200/255 white: 0xc8.
    BS_TEST_VERIFY_EQ(test_ptr, 0xffc8c8c8, gg_ptr->data_ptr[0]);
    // Transparent stays transparent.
    BS_TEST_VERIFY_EQ(test_ptr, 0x00000000, gg_ptr->data_ptr[1]);
    wlr_buffer_drop(ghost_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests the balloon's placement, drawing, and delay. */
void test_balloon(bs_test_t *test_ptr)
{
    struct wlr_box area = { .x = 0, .y = 0, .width = 1024, .height = 768 };
    bool right, top;

    // A tile at the top left: Below it, pointer on top, at the left.
    struct wlr_box tile = { .x = 0, .y = 0, .width = 64, .height = 62 };
    struct wlr_box b = wlmaker_dock_fx_balloon_position(
        &tile, &area, 100, 20, &right, &top);
    BS_TEST_VERIFY_FALSE(test_ptr, right);
    BS_TEST_VERIFY_TRUE(test_ptr, top);
    BS_TEST_VERIFY_EQ(test_ptr, 32, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 61, b.y);
    BS_TEST_VERIFY_EQ(test_ptr, 32, b.height);

    // A tile at the right, further down: Above, pointer at the right.
    tile = (struct wlr_box){ .x = 960, .y = 300, .width = 64, .height = 62 };
    b = wlmaker_dock_fx_balloon_position(&tile, &area, 100, 20, &right, &top);
    BS_TEST_VERIFY_TRUE(test_ptr, right);
    BS_TEST_VERIFY_FALSE(test_ptr, top);
    BS_TEST_VERIFY_EQ(test_ptr, 960 - 100 + 32, b.x);
    BS_TEST_VERIFY_EQ(test_ptr, 300 - 32, b.y);

    // Drawn: White inside, black outline, transparent outside.
    struct wlr_buffer *wlr_buffer_ptr = wlmaker_dock_fx_create_balloon_buffer(
        NULL, "wlmclock\n(wlmclock)", false, true);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_buffer_ptr);
    bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(wlr_buffer_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, 16 < (int)g_ptr->width);
    BS_TEST_VERIFY_TRUE(test_ptr, 12 + 2 * 12 < (int)g_ptr->height);
    int w = g_ptr->width, h = g_ptr->height;
    // Top-right corner: Outside the rounded box.
    BS_TEST_VERIFY_EQ(test_ptr, 0, g_ptr->data_ptr[12 * w + w - 1] >> 24);
    // Middle of the right edge: The outline.
    BS_TEST_VERIFY_EQ(test_ptr, 0xff000000,
                      g_ptr->data_ptr[(12 + (h - 12) / 2) * w + w - 1]);
    // Just inside: White.
    BS_TEST_VERIFY_EQ(test_ptr, 0xffffffff,
                      g_ptr->data_ptr[(12 + (h - 12) / 2) * w + w - 3]);
    // The pointer, on top at the left: Opaque pixels in its rows, at the
    // left; none at the right.
    bool tail_left = false, tail_right = false;
    for (int x = 0; x < w; ++x) {
        if (0 == g_ptr->data_ptr[6 * w + x] >> 24) continue;
        if (x < w / 2) tail_left = true; else tail_right = true;
    }
    BS_TEST_VERIFY_TRUE(test_ptr, tail_left);
    BS_TEST_VERIFY_FALSE(test_ptr, tail_right);
    wlr_buffer_drop(wlr_buffer_ptr);

    // Delay: Not shown right away. Shown right away when coming from one.
    struct wl_event_loop *loop_ptr = wl_event_loop_create();
    struct wlr_scene *wlr_scene_ptr = wlr_scene_create();
    wlmaker_dock_fx_t *fx_ptr = wlmaker_dock_fx_create(
        &wlr_scene_ptr->tree, loop_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fx_ptr);
    int o1, o2;
    wlmaker_dock_fx_balloon_enter(fx_ptr, &o1, "one", &tile, &area);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_fx_balloon_visible(fx_ptr));
    _wlmaker_dock_fx_handle_balloon_timer(fx_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_fx_balloon_visible(fx_ptr));
    wlmaker_dock_fx_balloon_leave(fx_ptr, &o1);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_fx_balloon_visible(fx_ptr));
    wlmaker_dock_fx_balloon_enter(fx_ptr, &o2, "two", &tile, &area);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_fx_balloon_visible(fx_ptr));
    // Leaving a different owner keeps it.
    wlmaker_dock_fx_balloon_leave(fx_ptr, &o1);
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dock_fx_balloon_visible(fx_ptr));
    // Leaving, and nothing entered until idle: The delay applies again.
    wlmaker_dock_fx_balloon_leave(fx_ptr, &o2);
    wl_event_loop_dispatch_idle(loop_ptr);
    wlmaker_dock_fx_balloon_enter(fx_ptr, &o1, "one", &tile, &area);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_dock_fx_balloon_visible(fx_ptr));

    wlmaker_dock_fx_destroy(fx_ptr);
    wlr_scene_node_destroy(&wlr_scene_ptr->tree.node);
    wl_event_loop_destroy(loop_ptr);
}

/* == End of dock_fx.c ===================================================== */
