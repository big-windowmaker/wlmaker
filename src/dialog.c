/* ========================================================================= */
/**
 * @file dialog.c
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

#include "dialog.h"

#include <cairo.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of a dialog. */
struct _wlmaker_dialog_t {
    /** The content: A buffer, drawn by @ref wlmaker_dialog_draw. */
    wlmtk_buffer_t            super_buffer;
    /** Original virtual method table of the buffer's element. */
    wlmtk_element_vmt_t       orig_element_vmt;
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** The window holding the dialog. */
    wlmtk_window_t            *window_ptr;

    /** Texts, and the icon. */
    char                      *title_ptr;
    /** See above. */
    char                      *message_ptr;
    /** See above. */
    char                      *default_ptr;
    /** See above. */
    char                      *other_ptr;
    /** See above. May be NULL. */
    char                      *icon_path_ptr;

    /** Box of the default button. */
    struct wlr_box            default_box;
    /** Box of the other button. */
    struct wlr_box            other_box;
    /** Button pressed: 0 none, 1 default, 2 other. */
    int                       pressed;

    /** Callback with the answer. NULL once answered. */
    wlmaker_dialog_callback_t callback;
    /** Argument to the callback. */
    void                      *ud_ptr;
    /** Releases @ref wlmaker_dialog_t::ud_ptr. */
    wlmaker_dialog_free_t     free_fn;
    /** Destroys the dialog, once answered. */
    struct wl_event_source    *idle_destroy_ptr;

    /** Listener for @ref wlmtk_window_events_t::request_close. */
    struct wl_listener        window_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        window_set_activated_listener;
};

static void _wlmaker_dialog_destroy(wlmaker_dialog_t *dialog_ptr);
static void _wlmaker_dialog_redraw(wlmaker_dialog_t *dialog_ptr);
static void _wlmaker_dialog_idle_destroy(void *data_ptr);
static bool _wlmaker_dialog_element_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);
static bool _wlmaker_dialog_element_keyboard_sym(
    wlmtk_element_t *element_ptr,
    xkb_keysym_t keysym,
    enum xkb_key_direction direction,
    uint32_t modifiers);
static void _wlmaker_dialog_handle_window_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dialog_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/** The dialog's extension of the buffer element. */
static const wlmtk_element_vmt_t _wlmaker_dialog_element_vmt = {
    .pointer_button = _wlmaker_dialog_element_pointer_button,
    .keyboard_sym = _wlmaker_dialog_element_keyboard_sym,
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_dialog_t *wlmaker_dialog_confirm(
    wlmaker_server_t *server_ptr,
    const char *title_ptr,
    const char *message_ptr,
    const char *default_ptr,
    const char *other_ptr,
    const char *icon_path_ptr,
    wlmaker_dialog_callback_t callback,
    void *ud_ptr,
    wlmaker_dialog_free_t free_fn)
{
    wlmaker_dialog_t *dialog_ptr = logged_calloc(1, sizeof(*dialog_ptr));
    if (NULL == dialog_ptr) {
        if (NULL != free_fn) free_fn(ud_ptr);
        return NULL;
    }
    dialog_ptr->server_ptr = server_ptr;
    dialog_ptr->callback = callback;
    dialog_ptr->ud_ptr = ud_ptr;
    dialog_ptr->free_fn = free_fn;
    dialog_ptr->title_ptr = logged_strdup(title_ptr);
    dialog_ptr->message_ptr = logged_strdup(message_ptr);
    dialog_ptr->default_ptr = logged_strdup(default_ptr);
    dialog_ptr->other_ptr = logged_strdup(other_ptr);
    if (NULL != icon_path_ptr) {
        dialog_ptr->icon_path_ptr = logged_strdup(icon_path_ptr);
    }
    if (NULL == dialog_ptr->title_ptr || NULL == dialog_ptr->message_ptr ||
        NULL == dialog_ptr->default_ptr || NULL == dialog_ptr->other_ptr ||
        !wlmtk_buffer_init(&dialog_ptr->super_buffer)) {
        _wlmaker_dialog_destroy(dialog_ptr);
        return NULL;
    }
    dialog_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_buffer_element(&dialog_ptr->super_buffer),
        &_wlmaker_dialog_element_vmt);
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&dialog_ptr->super_buffer), true);
    _wlmaker_dialog_redraw(dialog_ptr);
    if (NULL == dialog_ptr->super_buffer.wlr_buffer_ptr ||
        NULL == server_ptr->desktop_ptr) {
        _wlmaker_dialog_destroy(dialog_ptr);
        return NULL;
    }

    dialog_ptr->window_ptr = wlmtk_window_create(
        wlmtk_buffer_element(&dialog_ptr->super_buffer),
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == dialog_ptr->window_ptr) {
        _wlmaker_dialog_destroy(dialog_ptr);
        return NULL;
    }
    wlmtk_window_set_title(dialog_ptr->window_ptr, title_ptr);
    wlmtk_window_set_server_side_decorated(dialog_ptr->window_ptr, true);
    wlmtk_window_set_properties(dialog_ptr->window_ptr,
                                WLMTK_WINDOW_PROPERTY_CLOSABLE);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(dialog_ptr->window_ptr)->request_close,
        &dialog_ptr->window_request_close_listener,
        _wlmaker_dialog_handle_window_request_close);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(dialog_ptr->window_ptr)->set_activated,
        &dialog_ptr->window_set_activated_listener,
        _wlmaker_dialog_handle_window_set_activated);

    // Centered on the usable area of the output with the pointer.
    wlmtk_workspace_t *workspace_ptr = wlmtk_desktop_get_current_workspace(
        server_ptr->desktop_ptr);
    wlmtk_workspace_map_window(workspace_ptr, dialog_ptr->window_ptr);
    struct wlr_box area = wlmtk_desktop_get_usable_area(
        server_ptr->desktop_ptr, wlmaker_server_get_output_at_cursor(server_ptr));
    struct wlr_box bbox = wlmtk_window_get_bounding_box(dialog_ptr->window_ptr);
    wlmtk_workspace_set_window_position(
        workspace_ptr, dialog_ptr->window_ptr,
        area.x + (area.width - bbox.width) / 2,
        area.y + (area.height - bbox.height) / 3);
    wlmtk_workspace_activate_window(workspace_ptr, dialog_ptr->window_ptr);
    return dialog_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dialog_answer(wlmaker_dialog_t *dialog_ptr, bool confirmed)
{
    wlmaker_dialog_callback_t callback = dialog_ptr->callback;
    if (NULL == callback) return;
    dialog_ptr->callback = NULL;
    callback(dialog_ptr->ud_ptr, confirmed);
    if (NULL != dialog_ptr->free_fn) dialog_ptr->free_fn(dialog_ptr->ud_ptr);
    dialog_ptr->free_fn = NULL;

    // Not right away: We may be in the window's or element's handlers.
    if (NULL == dialog_ptr->idle_destroy_ptr &&
        NULL != dialog_ptr->server_ptr->wl_display_ptr) {
        dialog_ptr->idle_destroy_ptr = wl_event_loop_add_idle(
            wl_display_get_event_loop(dialog_ptr->server_ptr->wl_display_ptr),
            _wlmaker_dialog_idle_destroy, dialog_ptr);
    }
}

/* ------------------------------------------------------------------------- */
struct wlr_buffer *wlmaker_dialog_draw(
    const struct wlmtk_menu_style *style_ptr,
    const char *title_ptr,
    const char *message_ptr,
    const char *default_ptr,
    const char *other_ptr,
    const char *icon_path_ptr,
    int pressed,
    struct wlr_box *default_box_ptr,
    struct wlr_box *other_box_ptr)
{
    const struct wlmtk_menu_item_style *is_ptr = &style_ptr->item;
    const wlmtk_style_font_t *f_ptr = &is_ptr->font;
    double size = 0 < f_ptr->size ? f_ptr->size : 14;
    const int pad = 12, icon = 48;

    // Measure.
    cairo_surface_t *ms_ptr = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *m_ptr = cairo_create(ms_ptr);
    cairo_select_font_face(m_ptr, f_ptr->face, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(m_ptr, size * 1.6);
    cairo_text_extents_t te;
    cairo_text_extents(m_ptr, title_ptr, &te);
    double title_w = te.x_advance;
    cairo_select_font_face(m_ptr, f_ptr->face, CAIRO_FONT_SLANT_NORMAL,
                           wlmtk_style_font_weight_cairo_from_wlmtk(
                               f_ptr->weight));
    cairo_set_font_size(m_ptr, size);
    cairo_font_extents_t fe;
    cairo_font_extents(m_ptr, &fe);
    double line_h = fe.ascent + fe.descent;
    double msg_w = 0;
    int lines = 0;
    char *t_ptr = logged_strdup(message_ptr);
    for (char *l = t_ptr, *n; NULL != l; l = n) {
        n = strchr(l, '\n');
        if (NULL != n) *n++ = '\0';
        cairo_text_extents(m_ptr, l, &te);
        msg_w = BS_MAX(msg_w, te.x_advance);
        ++lines;
    }
    cairo_text_extents(m_ptr, default_ptr, &te);
    int default_w = BS_MAX(80, (int)te.x_advance + 40);
    cairo_text_extents(m_ptr, other_ptr, &te);
    int other_w = BS_MAX(80, (int)te.x_advance + 24);
    int button_h = (int)line_h + 12;

    int width = BS_MAX(360, (int)BS_MAX(pad + icon + pad + title_w + pad,
                                        msg_w + 4 * pad));
    width = BS_MAX(width, default_w + other_w + 3 * pad);
    int header_h = pad + icon + pad;
    int msg_y = header_h + pad;
    int buttons_y = msg_y + lines * (int)line_h + 2 * pad;
    int height = buttons_y + button_h + pad;

    *default_box_ptr = (struct wlr_box){
        .x = width - pad - default_w, .y = buttons_y,
        .width = default_w, .height = button_h };
    *other_box_ptr = (struct wlr_box){
        .x = default_box_ptr->x - pad / 2 - other_w, .y = buttons_y,
        .width = other_w, .height = button_h };

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        width, height);
    cairo_t *c_ptr = NULL;
    if (NULL != wlr_buffer_ptr) c_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == c_ptr) {
        if (NULL != wlr_buffer_ptr) wlr_buffer_drop(wlr_buffer_ptr);
        wlr_buffer_ptr = NULL;
        goto done;
    }
    // Panel: The menu item's fill.
    wlmaker_primitives_cairo_fill(c_ptr, &is_ptr->fill);

    // Icon, and the title in large letters.
    if (NULL != icon_path_ptr) {
        cairo_surface_t *i_ptr = cairo_image_surface_create_from_png(
            icon_path_ptr);
        if (CAIRO_STATUS_SUCCESS == cairo_surface_status(i_ptr)) {
            int iw = cairo_image_surface_get_width(i_ptr);
            int ih = cairo_image_surface_get_height(i_ptr);
            double s = BS_MIN((double)icon / iw, (double)icon / ih);
            cairo_save(c_ptr);
            cairo_translate(c_ptr, pad, pad);
            cairo_scale(c_ptr, s, s);
            cairo_set_source_surface(c_ptr, i_ptr, 0, 0);
            cairo_paint(c_ptr);
            cairo_restore(c_ptr);
        }
        cairo_surface_destroy(i_ptr);
    }
    cairo_set_source_argb8888(c_ptr, is_ptr->enabled_text_color);
    cairo_select_font_face(c_ptr, f_ptr->face, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(c_ptr, size * 1.6);
    cairo_font_extents_t tfe;
    cairo_font_extents(c_ptr, &tfe);
    cairo_move_to(c_ptr, pad + icon + pad,
                  pad + icon / 2 + (tfe.ascent - tfe.descent) / 2);
    cairo_show_text(c_ptr, title_ptr);

    // Separator, engraved, as on Window Maker's panels.
    cairo_set_line_width(c_ptr, 1);
    cairo_set_source_rgba(c_ptr, 0, 0, 0, 0.5);
    cairo_move_to(c_ptr, pad, header_h + 0.5);
    cairo_line_to(c_ptr, width - pad, header_h + 0.5);
    cairo_stroke(c_ptr);
    cairo_set_source_rgba(c_ptr, 1, 1, 1, 0.6);
    cairo_move_to(c_ptr, pad, header_h + 1.5);
    cairo_line_to(c_ptr, width - pad, header_h + 1.5);
    cairo_stroke(c_ptr);

    // Message, centered.
    cairo_select_font_face(c_ptr, f_ptr->face, CAIRO_FONT_SLANT_NORMAL,
                           wlmtk_style_font_weight_cairo_from_wlmtk(
                               f_ptr->weight));
    cairo_set_font_size(c_ptr, size);
    cairo_set_source_argb8888(c_ptr, is_ptr->enabled_text_color);
    strcpy(t_ptr, message_ptr);
    double y = msg_y + fe.ascent;
    for (char *l = t_ptr, *n; NULL != l; l = n) {
        n = strchr(l, '\n');
        if (NULL != n) *n++ = '\0';
        cairo_text_extents(c_ptr, l, &te);
        cairo_move_to(c_ptr, (width - te.x_advance) / 2, y);
        cairo_show_text(c_ptr, l);
        y += line_h;
    }

    // Buttons: Raised, or pressed. The default one with the return arrow.
    const struct { struct wlr_box *b; const char *t; int id; } buttons[2] = {
        { other_box_ptr, other_ptr, 2 }, { default_box_ptr, default_ptr, 1 } };
    for (int i = 0; i < 2; ++i) {
        struct wlr_box *b = buttons[i].b;
        bool down = pressed == buttons[i].id;
        cairo_save(c_ptr);
        cairo_translate(c_ptr, b->x, b->y);
        wlmaker_primitives_cairo_fill_at(c_ptr, 0, 0, b->width, b->height,
                                         &is_ptr->fill);
        wlmaker_primitives_draw_bezel_at(c_ptr, 0, 0, b->width, b->height,
                                         BS_MAX(1.0, (double)is_ptr->bezel_width), !down);
        cairo_set_source_argb8888(c_ptr, is_ptr->enabled_text_color);
        cairo_text_extents(c_ptr, buttons[i].t, &te);
        double tx = (1 == buttons[i].id) ? 12 : (b->width - te.x_advance) / 2;
        double ty = (b->height + fe.ascent - fe.descent) / 2;
        if (down) { tx += 1; ty += 1; }
        cairo_move_to(c_ptr, tx, ty);
        cairo_show_text(c_ptr, buttons[i].t);
        if (1 == buttons[i].id) {
            // Window Maker's return arrow, right of the text.
            double ax = b->width - 18 + (down ? 1 : 0);
            double ay = b->height / 2.0 + (down ? 1 : 0);
            cairo_set_line_width(c_ptr, 1.5);
            cairo_move_to(c_ptr, ax + 8, ay - 5);
            cairo_line_to(c_ptr, ax + 8, ay + 1);
            cairo_line_to(c_ptr, ax, ay + 1);
            cairo_stroke(c_ptr);
            cairo_move_to(c_ptr, ax - 1, ay + 1);
            cairo_line_to(c_ptr, ax + 3, ay - 2);
            cairo_line_to(c_ptr, ax + 3, ay + 4);
            cairo_close_path(c_ptr);
            cairo_fill(c_ptr);
        }
        cairo_restore(c_ptr);
    }
    cairo_destroy(c_ptr);

done:
    if (NULL != t_ptr) free(t_ptr);
    cairo_destroy(m_ptr);
    cairo_surface_destroy(ms_ptr);
    return wlr_buffer_ptr;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Destroys the dialog. */
void _wlmaker_dialog_destroy(wlmaker_dialog_t *dialog_ptr)
{
    if (NULL != dialog_ptr->idle_destroy_ptr) {
        wl_event_source_remove(dialog_ptr->idle_destroy_ptr);
        dialog_ptr->idle_destroy_ptr = NULL;
    }
    if (NULL != dialog_ptr->callback) {
        dialog_ptr->callback = NULL;
        if (NULL != dialog_ptr->free_fn) dialog_ptr->free_fn(dialog_ptr->ud_ptr);
        dialog_ptr->free_fn = NULL;
    }
    if (NULL != dialog_ptr->window_ptr) {
        wlmtk_util_disconnect_listener(
            &dialog_ptr->window_request_close_listener);
        wlmtk_util_disconnect_listener(
            &dialog_ptr->window_set_activated_listener);
        wlmtk_workspace_t *w_ptr = wlmtk_window_get_workspace(
            dialog_ptr->window_ptr);
        if (NULL != w_ptr) {
            wlmtk_workspace_unmap_window(w_ptr, dialog_ptr->window_ptr);
        }
        wlmtk_window_destroy(dialog_ptr->window_ptr);
        dialog_ptr->window_ptr = NULL;
    }
    wlmtk_buffer_fini(&dialog_ptr->super_buffer);
    free(dialog_ptr->title_ptr);
    free(dialog_ptr->message_ptr);
    free(dialog_ptr->default_ptr);
    free(dialog_ptr->other_ptr);
    if (NULL != dialog_ptr->icon_path_ptr) free(dialog_ptr->icon_path_ptr);
    free(dialog_ptr);
}

/* ------------------------------------------------------------------------- */
/** Redraws the content, eg. with a button pressed. */
void _wlmaker_dialog_redraw(wlmaker_dialog_t *dialog_ptr)
{
    const struct wlmtk_menu_style *s_ptr =
        dialog_ptr->server_ptr->style_ptr->menu_style_ptr;
    if (NULL == s_ptr) return;
    struct wlr_buffer *b_ptr = wlmaker_dialog_draw(
        s_ptr, dialog_ptr->title_ptr, dialog_ptr->message_ptr,
        dialog_ptr->default_ptr, dialog_ptr->other_ptr,
        dialog_ptr->icon_path_ptr, dialog_ptr->pressed,
        &dialog_ptr->default_box, &dialog_ptr->other_box);
    if (NULL == b_ptr) return;
    wlmtk_buffer_set(&dialog_ptr->super_buffer, b_ptr);
    wlr_buffer_drop(b_ptr);
}

/* ------------------------------------------------------------------------- */
/** Destroys the answered dialog, from the event loop. */
void _wlmaker_dialog_idle_destroy(void *data_ptr)
{
    wlmaker_dialog_t *dialog_ptr = data_ptr;
    dialog_ptr->idle_destroy_ptr = NULL;
    _wlmaker_dialog_destroy(dialog_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_button. The buttons. */
bool _wlmaker_dialog_element_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_dialog_t *dialog_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_dialog_t, super_buffer.super_element);
    if (BTN_LEFT != button_event_ptr->button) return true;
    double x = element_ptr->last_pointer_motion_event.x;
    double y = element_ptr->last_pointer_motion_event.y;
    int at = 0;
    if (wlr_box_contains_point(&dialog_ptr->default_box, x, y)) at = 1;
    if (wlr_box_contains_point(&dialog_ptr->other_box, x, y)) at = 2;

    if (WLMTK_BUTTON_DOWN == button_event_ptr->type) {
        dialog_ptr->pressed = at;
        _wlmaker_dialog_redraw(dialog_ptr);
    } else if (WLMTK_BUTTON_UP == button_event_ptr->type) {
        int pressed = dialog_ptr->pressed;
        dialog_ptr->pressed = 0;
        _wlmaker_dialog_redraw(dialog_ptr);
        if (0 != pressed && pressed == at) {
            wlmaker_dialog_answer(dialog_ptr, 1 == at);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::keyboard_sym: Return and Escape. */
bool _wlmaker_dialog_element_keyboard_sym(
    wlmtk_element_t *element_ptr,
    xkb_keysym_t keysym,
    enum xkb_key_direction direction,
    __UNUSED__ uint32_t modifiers)
{
    wlmaker_dialog_t *dialog_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_dialog_t, super_buffer.super_element);
    if (XKB_KEY_DOWN != direction) return true;
    switch (keysym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        wlmaker_dialog_answer(dialog_ptr, true);
        break;
    case XKB_KEY_Escape:
        wlmaker_dialog_answer(dialog_ptr, false);
        break;
    default:
        break;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** The window's close button: Declines. */
void _wlmaker_dialog_handle_window_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_dialog_t *dialog_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dialog_t, window_request_close_listener);
    wlmaker_dialog_answer(dialog_ptr, false);
}

/* ------------------------------------------------------------------------- */
/** Activated: The keyboard goes to the dialog. */
void _wlmaker_dialog_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_dialog_t *dialog_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dialog_t, window_set_activated_listener);
    wlmtk_element_t *e = wlmtk_buffer_element(&dialog_ptr->super_buffer);
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_set_keyboard_focus_element(
            e->parent_container_ptr, e,
            wlmtk_window_is_activated(dialog_ptr->window_ptr));
    }
}

/* == Unit tests =========================================================== */

static void test_draw(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_dialog_test_cases[] = {
    { true, "draw", test_draw },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dialog_test_set = BS_TEST_SET(
    true, "dialog", _wlmaker_dialog_test_cases);

/* ------------------------------------------------------------------------- */
/** Draws the dialog: Buttons at the lower right, the default rightmost. */
void test_draw(bs_test_t *test_ptr)
{
    struct wlmtk_menu_style style = {
        .item = {
            .fill = { .type = WLMTK_STYLE_COLOR_SOLID,
                      .param = { .solid = { .color = 0xffc0c0c0 } } },
            .font = { .face = "Sans", .size = 14 },
            .bezel_width = 1,
            .enabled_text_color = 0xff000000 } };
    struct wlr_box d, o;
    struct wlr_buffer *b_ptr = wlmaker_dialog_draw(
        &style, "Terminar Aplicação",
        "firefox será fechado à força.\nQualquer mudança não salva será "
        "perdida.\nConfirme, por favor.", "Sim", "Não", NULL, 1, &d, &o);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, b_ptr);
    BS_TEST_VERIFY_TRUE(test_ptr, 360 <= b_ptr->width);
    BS_TEST_VERIFY_TRUE(test_ptr, d.x > o.x);
    BS_TEST_VERIFY_EQ(test_ptr, d.y, o.y);
    BS_TEST_VERIFY_TRUE(test_ptr, d.x + d.width < b_ptr->width);
    BS_TEST_VERIFY_TRUE(test_ptr, d.y + d.height < b_ptr->height);
    // The panel's fill, in the corner.
    bs_gfxbuf_t *g_ptr = bs_gfxbuf_from_wlr_buffer(b_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 0xffc0c0c0, g_ptr->data_ptr[2 * b_ptr->width + 2]);
    wlr_buffer_drop(b_ptr);
}

/* == End of dialog.c ====================================================== */
