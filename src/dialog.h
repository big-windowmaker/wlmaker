/* ========================================================================= */
/**
 * @file dialog.h
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
#ifndef __WLMAKER_DIALOG_H__
#define __WLMAKER_DIALOG_H__

#include <stdbool.h>

#include <libbase/libbase.h>

/** Forward declaration: A dialog of the compositor. */
typedef struct _wlmaker_dialog_t wlmaker_dialog_t;

#include "server.h"

struct wlr_buffer;
typedef struct _cairo_surface cairo_surface_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Called with the answer: true for the default button ("Yes"). */
typedef void (*wlmaker_dialog_callback_t)(void *ud_ptr, bool confirmed);
/** Releases the callback's argument. */
typedef void (*wlmaker_dialog_free_t)(void *ud_ptr);

/**
 * Asks for confirmation, as Window Maker's wMessageDialog: A window with an
 * icon, the title in large letters, the message, and the buttons; the
 * default one with Window Maker's "return" arrow. Return or a click on the
 * default button confirms; Escape, the other button or closing the window
 * declines. Drawn with the menu's style (font, colors, bezel).
 *
 * @param server_ptr
 * @param title_ptr
 * @param message_ptr         May have multiple lines.
 * @param default_ptr         Text of the default button, eg. "Yes".
 * @param other_ptr           Text of the other button, eg. "No". May be
 *                            NULL: Only the default button (a notice).
 * @param icon_path_ptr       PNG for the icon. May be NULL.
 * @param callback            Called once, with the answer.
 * @param ud_ptr
 * @param free_fn             Releases `ud_ptr`, after the callback. Or NULL.
 *
 * @return The dialog, or NULL on error (then `free_fn` was called). It
 *     destroys itself once answered.
 */
wlmaker_dialog_t *wlmaker_dialog_confirm(
    wlmaker_server_t *server_ptr,
    const char *title_ptr,
    const char *message_ptr,
    const char *default_ptr,
    const char *other_ptr,
    const char *icon_path_ptr,
    wlmaker_dialog_callback_t callback,
    void *ud_ptr,
    wlmaker_dialog_free_t free_fn);

/**
 * As @ref wlmaker_dialog_confirm, with the icon as an image: eg. the
 * application's own icon, from @ref wlmaker_dialog_app_icon.
 *
 * @param server_ptr
 * @param title_ptr
 * @param message_ptr
 * @param default_ptr
 * @param other_ptr           May be NULL: Only the default button.
 * @param icon_ptr            The icon; a reference is taken. May be NULL.
 * @param callback
 * @param ud_ptr
 * @param free_fn
 *
 * @return The dialog, or NULL on error (then `free_fn` was called).
 */
wlmaker_dialog_t *wlmaker_dialog_confirm_with_icon(
    wlmaker_server_t *server_ptr,
    const char *title_ptr,
    const char *message_ptr,
    const char *default_ptr,
    const char *other_ptr,
    cairo_surface_t *icon_ptr,
    wlmaker_dialog_callback_t callback,
    void *ud_ptr,
    wlmaker_dialog_free_t free_fn);

/**
 * The icon for a dialog about an application: Its own icon, as the desktop
 * shows it (desktop entry of the app ID or the X11 class, icon theme,
 * Flatpak exports); else the Dock's logo, as Window Maker's panels.
 *
 * @param server_ptr
 * @param app_id_ptr          App ID or WM_CLASS class. May be NULL.
 * @param instance_ptr        WM_CLASS instance. May be NULL.
 * @param icon_path_ptr       An image to use if the application's icon is
 *                            not found (eg. the appicon's). May be NULL.
 *
 * @return A new reference to an image surface (release it with
 *     cairo_surface_destroy), or NULL.
 */
cairo_surface_t *wlmaker_dialog_app_icon(
    wlmaker_server_t *server_ptr,
    const char *app_id_ptr,
    const char *instance_ptr,
    const char *icon_path_ptr);

/** Answers the dialog, as if a button was clicked. For tests. */
void wlmaker_dialog_answer(wlmaker_dialog_t *dialog_ptr, bool confirmed);

/**
 * Draws the dialog's content: For tests, and the dialog itself.
 *
 * @param style_ptr           Menu style.
 * @param title_ptr
 * @param message_ptr
 * @param default_ptr
 * @param other_ptr           May be NULL: Only the default button.
 * @param icon_ptr            May be NULL.
 * @param pressed             0 none, 1 default button, 2 the other.
 * @param default_box_ptr     Returns the default button's box.
 * @param other_box_ptr       Returns the other button's box.
 *
 * @return A buffer, or NULL on error.
 */
struct wlr_buffer *wlmaker_dialog_draw(
    const struct wlmtk_menu_style *style_ptr,
    const char *title_ptr,
    const char *message_ptr,
    const char *default_ptr,
    const char *other_ptr,
    cairo_surface_t *icon_ptr,
    int pressed,
    struct wlr_box *default_box_ptr,
    struct wlr_box *other_box_ptr);

/**
 * @return The panel's flat color: The fill's color, or a gradient's first
 *     color (the light end in the themes).
 */
uint32_t wlmaker_dialog_panel_color(const wlmtk_style_fill_t *fill_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_dialog_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_DIALOG_H__ */
/* == End of dialog.h ====================================================== */
