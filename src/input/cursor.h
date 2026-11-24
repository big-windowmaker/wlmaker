/* ========================================================================= */
/**
 * @file cursor.h
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
#ifndef __WLMAKER_INPUT_CURSOR_H__
#define __WLMAKER_INPUT_CURSOR_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>

#include "manager.h"
#include "toolkit/toolkit.h"

/** Forward declaration of wlmaker cursor state. */
typedef struct _wlmim_cursor_t wlmim_cursor_t;

struct wl_display;
struct wlr_buffer;
struct wlr_input_device;
struct wlr_output_layout;
struct wlr_seat;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Style of the cursor. */
struct wlmim_cursor_style {
    /** Whether to override the system-wide configuration. */
    bool                      override_system_configuration;
    /** Name of the XCursor theme to use. */
    char                      *name_ptr;
    /** Size, when non-scaled. */
    uint64_t                  size;
};

/** Options that configure cursor behaviour. */
struct wlmim_cursor_options {
    /** Keyboard modifier to change a left button into right button action. */
    uint32_t                  emulate_right_button_modifier;

    /** Keyboard modifier for moving a window when left-dragging. */
    uint32_t                  move_window_modifier;
};

/**
 * Creates the cursor handlers.
 *
 * @param wl_display_ptr
 * @param input_manager_ptr
 * @param style_ptr
 * @param options_ptr
 * @param wlr_output_layout_ptr
 * @param wlr_seat_ptr
 * @param root_ptr
 *
 * @return The cursor handler or NULL on error.
 */
wlmim_cursor_t *wlmim_cursor_create(
    struct wl_display *wl_display_ptr,
    wlmim_t *input_manager_ptr,
    const struct wlmim_cursor_style *style_ptr,
    const struct wlmim_cursor_options *options_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_seat *wlr_seat_ptr,
    wlmtk_root_t *root_ptr);

/**
 * Destroys the cursor handlers.
 *
 * @param cursor_ptr
 */
void wlmim_cursor_destroy(wlmim_cursor_t *cursor_ptr);

/**
 * Updates the cursor's style.
 *
 * @param cursor_ptr
 * @param style_ptr
 *
 * @return true on success.
 */
bool wlmim_cursor_set_style(
    wlmim_cursor_t *cursor_ptr,
    const struct wlmim_cursor_style *style_ptr);

/** @return @ref wlmim_cursor_t::wlr_cursor_ptr. */
struct wlr_cursor *wlmim_cursor_wlr_cursor(wlmim_cursor_t *cursor_ptr);

/** @return Whether an image is asked for: false if clients hid the cursor. */
bool wlmim_cursor_image_visible(wlmim_cursor_t *cursor_ptr);

/**
 * Magnifies the cursor's image: The image shown right now, whatever it is
 * (theme image or client surface), with its hotspot at the same place.
 * Follows image changes while magnified.
 *
 * @param cursor_ptr
 * @param magnification       1.0 shows the image as is.
 * @param max_magnification   Largest magnification to come: The image is
 *                            prepared once, in this size.
 */
void wlmim_cursor_set_magnification(
    wlmim_cursor_t *cursor_ptr,
    double magnification,
    double max_magnification);

/**
 * Replaces the cursor's image (eg. while selecting a window). Clients' and
 * the toolkit's requests are kept and shown again once cleared.
 *
 * @param cursor_ptr
 * @param wlr_buffer_ptr      The image, or NULL to clear. Gets locked.
 * @param hotspot_x           Hotspot, in logical pixels.
 * @param hotspot_y           Hotspot, in logical pixels.
 * @param scale               Buffer pixels per logical pixel.
 */
void wlmim_cursor_set_override(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int32_t hotspot_x,
    int32_t hotspot_y,
    float scale);

/**
 * Loads an image of an XCursor theme, at the cursor's size and the outputs'
 * largest scale.
 *
 * @param cursor_ptr
 * @param theme_name_ptr      Theme to load from; NULL for the current one.
 * @param names_ptr           NULL-terminated names to try, in order.
 * @param hotspot_x_ptr       Hotspot, in logical pixels.
 * @param hotspot_y_ptr       Hotspot, in logical pixels.
 * @param scale_ptr           Buffer pixels per logical pixel.
 *
 * @return The image, or NULL. Must be released with wlr_buffer_drop().
 */
struct wlr_buffer *wlmim_cursor_create_xcursor_buffer(
    wlmim_cursor_t *cursor_ptr,
    const char *theme_name_ptr,
    const char *const *names_ptr,
    int32_t *hotspot_x_ptr,
    int32_t *hotspot_y_ptr,
    float *scale_ptr);

/**
 * Attaches the input device. May be a pointer, touch or tablet_tool device.
 *
 * @param cursor_ptr
 * @param wlr_input_device_ptr
 */
void wlmim_cursor_attach_input_device(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_input_device *wlr_input_device_ptr);

/**
 * Detaches the input device. @see wlmim_cursor_attach_input_device.
 *
 * @param cursor_ptr
 * @param wlr_input_device_ptr
 */
void wlmim_cursor_detach_input_device(
    wlmim_cursor_t *cursor_ptr,
    struct wlr_input_device *wlr_input_device_ptr);

/** Descriptor for decoding the "Cursor" style dictionary. */
extern const bspl_desc_t wlmim_cursor_style_desc[];

/** Descriptor for decoding Cursor-relevant config dictionary. */
extern const bspl_desc_t wlmim_cursor_options_desc[];

/** Unit tests for cursor. */
extern const bs_test_set_t wlmim_cursor_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_INPUT_CURSOR_H__ */
/* == End of cursor.h ====================================================== */
