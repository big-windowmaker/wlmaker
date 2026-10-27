/* ========================================================================= */
/**
 * @file menu_scroll.c
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

#include "menu_scroll.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#undef WLR_USE_UNSTABLE

#include "input/manager.h"

/* == Declarations ========================================================= */

/** State of the menu scroller. */
struct _wlmaker_menu_scroll_t {
    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** The window holding the menu. */
    wlmtk_window_t            *window_ptr;
    /** The menu. */
    wlmtk_menu_t              *menu_ptr;
    /** Timer for the next step. */
    struct wl_event_source    *timer_event_source_ptr;
    /** Whether the timer is armed. */
    bool                      armed;
    /** Listener for @ref wlmim_events::cursor_position_updated. */
    struct wl_listener        cursor_position_updated_listener;
};

static bool _wlmaker_menu_scroll_compute(
    wlmaker_menu_scroll_t *menu_scroll_ptr,
    int *dx_ptr,
    int *dy_ptr);
static void _wlmaker_menu_scroll_handle_cursor_position_updated(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static int _wlmaker_menu_scroll_handle_timer(void *data_ptr);

/** Width of the band at the output's edge that scrolls, in pixels. */
static const int _wlmaker_menu_scroll_border = 5;
/** Distance to move per step, in pixels. */
static const int _wlmaker_menu_scroll_step = 8;
/** Delay between steps, in milliseconds. */
static const int _wlmaker_menu_scroll_delay_msec = 10;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_menu_scroll_t *wlmaker_menu_scroll_create(
    wlmaker_server_t *server_ptr,
    wlmtk_window_t *window_ptr,
    wlmtk_menu_t *menu_ptr)
{
    wlmaker_menu_scroll_t *menu_scroll_ptr = logged_calloc(
        1, sizeof(*menu_scroll_ptr));
    if (NULL == menu_scroll_ptr) return NULL;
    menu_scroll_ptr->server_ptr = server_ptr;
    menu_scroll_ptr->window_ptr = window_ptr;
    menu_scroll_ptr->menu_ptr = menu_ptr;

    if (NULL != server_ptr->wl_display_ptr) {
        menu_scroll_ptr->timer_event_source_ptr = wl_event_loop_add_timer(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            _wlmaker_menu_scroll_handle_timer,
            menu_scroll_ptr);
        if (NULL == menu_scroll_ptr->timer_event_source_ptr) {
            bs_log(BS_ERROR, "Failed wl_event_loop_add_timer()");
            wlmaker_menu_scroll_destroy(menu_scroll_ptr);
            return NULL;
        }
    }
    if (NULL != server_ptr->input_manager_ptr) {
        wlmtk_util_connect_listener_signal(
            &wlmim_events(server_ptr->input_manager_ptr)->
            cursor_position_updated,
            &menu_scroll_ptr->cursor_position_updated_listener,
            _wlmaker_menu_scroll_handle_cursor_position_updated);
    }
    return menu_scroll_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_menu_scroll_destroy(wlmaker_menu_scroll_t *menu_scroll_ptr)
{
    wlmtk_util_disconnect_listener(
        &menu_scroll_ptr->cursor_position_updated_listener);
    if (NULL != menu_scroll_ptr->timer_event_source_ptr) {
        wl_event_source_remove(menu_scroll_ptr->timer_event_source_ptr);
        menu_scroll_ptr->timer_event_source_ptr = NULL;
    }
    free(menu_scroll_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_menu_scroll_amount(
    const struct wlr_box *output_ptr,
    const struct wlr_box *area_ptr,
    const struct wlr_box *extents_ptr,
    double x,
    double y,
    int step,
    int *dx_ptr,
    int *dy_ptr)
{
    *dx_ptr = 0;
    *dy_ptr = 0;
    if (wlr_box_empty(output_ptr) || wlr_box_empty(area_ptr) ||
        wlr_box_empty(extents_ptr)) return false;

    const int b = _wlmaker_menu_scroll_border;
    bool in_rows = (extents_ptr->y <= y &&
                    y < extents_ptr->y + extents_ptr->height);
    bool in_columns = (extents_ptr->x <= x &&
                       x < extents_ptr->x + extents_ptr->width);
    int overflow;

    if (in_rows && x < output_ptr->x + b) {
        overflow = area_ptr->x - extents_ptr->x;
        if (0 < overflow) *dx_ptr = BS_MIN(step, overflow);
    } else if (in_rows && x >= output_ptr->x + output_ptr->width - b) {
        overflow = (extents_ptr->x + extents_ptr->width) -
            (area_ptr->x + area_ptr->width);
        if (0 < overflow) *dx_ptr = -BS_MIN(step, overflow);
    }

    if (in_columns && y < output_ptr->y + b) {
        overflow = area_ptr->y - extents_ptr->y;
        if (0 < overflow) *dy_ptr = BS_MIN(step, overflow);
    } else if (in_columns && y >= output_ptr->y + output_ptr->height - b) {
        overflow = (extents_ptr->y + extents_ptr->height) -
            (area_ptr->y + area_ptr->height);
        if (0 < overflow) *dy_ptr = -BS_MIN(step, overflow);
    }
    return 0 != *dx_ptr || 0 != *dy_ptr;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Computes the next step for the menu, if it is open. */
bool _wlmaker_menu_scroll_compute(
    wlmaker_menu_scroll_t *menu_scroll_ptr,
    int *dx_ptr,
    int *dy_ptr)
{
    wlmaker_server_t *server_ptr = menu_scroll_ptr->server_ptr;
    if (!wlmtk_menu_is_open(menu_scroll_ptr->menu_ptr) ||
        NULL == wlmtk_window_get_workspace(menu_scroll_ptr->window_ptr) ||
        NULL == server_ptr->input_manager_ptr) return false;

    struct wlr_cursor *wlr_cursor_ptr = wlmim_wlr_cursor(
        server_ptr->input_manager_ptr);
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, wlr_cursor_ptr->x, wlr_cursor_ptr->y);
    if (NULL == wlr_output_ptr) return false;
    struct wlr_box output;
    wlr_output_layout_get_box(
        server_ptr->wlr_output_layout_ptr, wlr_output_ptr, &output);
    struct wlr_box area = wlmtk_desktop_get_usable_area(
        server_ptr->desktop_ptr, wlr_output_ptr);

    struct wlr_box extents = wlmtk_window_get_bounding_box(
        menu_scroll_ptr->window_ptr);
    struct wlr_box menus = wlmtk_menu_get_extents(menu_scroll_ptr->menu_ptr);
    extents = wlmtk_util_box_union(&extents, &menus);

    return wlmaker_menu_scroll_amount(
        &output, &area, &extents, wlr_cursor_ptr->x, wlr_cursor_ptr->y,
        _wlmaker_menu_scroll_step, dx_ptr, dy_ptr);
}

/* ------------------------------------------------------------------------- */
/** Starts scrolling, if the pointer moved to the output's edge. */
void _wlmaker_menu_scroll_handle_cursor_position_updated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_menu_scroll_t *menu_scroll_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_menu_scroll_t, cursor_position_updated_listener);
    int dx, dy;
    if (menu_scroll_ptr->armed ||
        NULL == menu_scroll_ptr->timer_event_source_ptr ||
        !_wlmaker_menu_scroll_compute(menu_scroll_ptr, &dx, &dy)) return;
    menu_scroll_ptr->armed = true;
    wl_event_source_timer_update(menu_scroll_ptr->timer_event_source_ptr,
                                 _wlmaker_menu_scroll_delay_msec);
}

/* ------------------------------------------------------------------------- */
/** Moves the menu by one step, and continues while needed. */
int _wlmaker_menu_scroll_handle_timer(void *data_ptr)
{
    wlmaker_menu_scroll_t *menu_scroll_ptr = data_ptr;
    menu_scroll_ptr->armed = false;
    int dx, dy;
    if (!_wlmaker_menu_scroll_compute(menu_scroll_ptr, &dx, &dy)) return 0;

    wlmtk_window_t *window_ptr = menu_scroll_ptr->window_ptr;
    int x, y;
    wlmtk_element_get_position(wlmtk_window_element(window_ptr), &x, &y);
    wlmtk_workspace_set_window_position(
        wlmtk_window_get_workspace(window_ptr), window_ptr, x + dx, y + dy);

    menu_scroll_ptr->armed = true;
    wl_event_source_timer_update(menu_scroll_ptr->timer_event_source_ptr,
                                 _wlmaker_menu_scroll_delay_msec);
    return 0;
}

/* == Unit tests =========================================================== */

static void test_amount(bs_test_t *test_ptr);

static const bs_test_case_t _wlmaker_menu_scroll_test_cases[] = {
    { true, "amount", test_amount },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_menu_scroll_test_set = BS_TEST_SET(
    true, "menu_scroll", _wlmaker_menu_scroll_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests @ref wlmaker_menu_scroll_amount. */
void test_amount(bs_test_t *test_ptr)
{
    struct wlr_box o = { .x = 0, .y = 0, .width = 1280, .height = 720 };
    struct wlr_box a = { .x = 64, .y = 0, .width = 1152, .height = 720 };
    // A tall menu: From 100 to 1000, at x 500..700.
    struct wlr_box e = { .x = 500, .y = 100, .width = 200, .height = 900 };
    int dx, dy;

    // Pointer in the middle: Nothing.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_menu_scroll_amount(
                             &o, &a, &e, 600, 400, 8, &dx, &dy));
    // At the bottom edge, below the menu: Moves up, by a step.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_menu_scroll_amount(
                            &o, &a, &e, 600, 719, 8, &dx, &dy));
    BS_TEST_VERIFY_EQ(test_ptr, 0, dx);
    BS_TEST_VERIFY_EQ(test_ptr, -8, dy);
    // At the bottom edge, but not below the menu: Nothing.
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_menu_scroll_amount(
                             &o, &a, &e, 100, 719, 8, &dx, &dy));
    // Almost all in: Just the remainder.
    e.y = 723 - 900;
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_menu_scroll_amount(
                            &o, &a, &e, 600, 719, 8, &dx, &dy));
    BS_TEST_VERIFY_EQ(test_ptr, -3, dy);
    // At the top edge: Moves down, to get the top in.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_menu_scroll_amount(
                            &o, &a, &e, 600, 0, 8, &dx, &dy));
    BS_TEST_VERIFY_EQ(test_ptr, 8, dy);
    // All in: Nothing.
    e = (struct wlr_box){ .x = 500, .y = 0, .width = 200, .height = 720 };
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_menu_scroll_amount(
                             &o, &a, &e, 600, 719, 8, &dx, &dy));

    // Beyond the right of the usable area (under the Dock): At the output's
    // right edge, within the menu's rows, moves left.
    e = (struct wlr_box){ .x = 1100, .y = 100, .width = 150, .height = 200 };
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_menu_scroll_amount(
                            &o, &a, &e, 1279, 150, 8, &dx, &dy));
    BS_TEST_VERIFY_EQ(test_ptr, -8, dx);
    BS_TEST_VERIFY_EQ(test_ptr, 0, dy);
    BS_TEST_VERIFY_FALSE(test_ptr, wlmaker_menu_scroll_amount(
                             &o, &a, &e, 1279, 400, 8, &dx, &dy));
    // Beyond the left: Moves right.
    e.x = 30;
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_menu_scroll_amount(
                            &o, &a, &e, 0, 150, 8, &dx, &dy));
    BS_TEST_VERIFY_EQ(test_ptr, 8, dx);
}

/* == End of menu_scroll.c ================================================= */
