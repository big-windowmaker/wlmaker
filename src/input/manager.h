/* ========================================================================= */
/**
 * @file manager.h
 *
 * TODO(kaeser@gubbe.ch): Minor cleanups:
 * - Make wlr_seat (and similar) as accessors, and use in device modules.
 * - Add abstract device module with a dtor method.
 * - Make the config dict parsing part of the file here.
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright (c) 2026 Google LLC and Philipp Kaeser
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
#ifndef __WLMAKER_INPUT_MANAGER_H__
#define __WLMAKER_INPUT_MANAGER_H__

#include <stdbool.h>
#include <stdint.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "toolkit/toolkit.h"

struct wl_display;
struct wlr_backend;
struct wlr_output_layout;
struct wlr_seat;
struct xkb_keymap;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

struct wlmim_cursor_style;
struct wlr_buffer;

/** Forward declaration. */
typedef struct _wlmim_t wlmim_t;

/** Events provided by the input manager. */
struct wlmim_events {
    /**
     * Signals when the cursor's position is updated.
     *
     * Will be called from @ref _wlmim_cursor_handle_motion and
     * @ref _wlmim_cursor_handle_motion_absolute
     * handlers, after issuing wlr_cursor_move(), respectively
     * wlr_cursor_warp_absolute().
     *
     * Offers struct wlr_cursor as argument.
     */
    struct wl_signal          cursor_position_updated;

    /**
     * Signals when there has been any input activity.
     *
     * Useful to eg. reset an idle timer.
     */
    struct wl_signal          activity;

    /**
     * Hack: Signals when ALT modifier is lost, indicating that the
     * task list should be de-activated.
     *
     * TODO(kaeser@gubbe.ch): Handle this better -- should respect the
     * modifiers of the task list actions, and be more generalized.
     */
    struct wl_signal          deactivate_task_list;

};

/** Handle for a key binding. */
typedef struct _wlmim_keybinding_t wlmim_keybinding_t;

/** Specifies the key + modifier to bind. */
struct wlmim_keybinding_combo {
    /** Modifiers required. See `enum wlr_keyboard_modifiers`. */
    uint32_t                  modifiers;
    /** Modifier mask: Only masked modifiers are considered. */
    uint32_t                  modifiers_mask;
    /** XKB Keysym to trigger on. */
    xkb_keysym_t              keysym;
    /** Whether to ignore case when matching. */
    bool                      ignore_case;
};

/**
 * Callback for a key binding.
 *
 * @param kc                  The key combo that triggered the callback.
 *
 * @return true if the key can be considered "consumed".
 */
struct wlr_pointer_axis_event;

/**
 * Filter for axis events (wheel, touchpad scrolling).
 *
 * @param ud_ptr
 * @param wlr_pointer_axis_event_ptr
 * @param modifiers           The keyboard modifiers currently held.
 *
 * @return true if the event was consumed: It is not passed on to clients.
 */
typedef bool (*wlmim_axis_filter_t)(
    void *ud_ptr,
    const struct wlr_pointer_axis_event *wlr_pointer_axis_event_ptr,
    uint32_t modifiers);

struct wlr_pointer_button_event;

/**
 * Filter for button events: Runs before the toolkit and clients.
 *
 * @param ud_ptr
 * @param wlr_pointer_button_event_ptr
 * @param modifiers           The keyboard modifiers currently held.
 *
 * @return true if the event was consumed: It is not passed on.
 */
typedef bool (*wlmim_button_filter_t)(
    void *ud_ptr,
    const struct wlr_pointer_button_event *wlr_pointer_button_event_ptr,
    uint32_t modifiers);

typedef bool (*wlmim_keybinding_callback_t)(
    const struct wlmim_keybinding_combo *kc);

/**
 * Creates an input manager: Registers input devices and ensures event routing
 * will be set up as desired.
 *
 * @param wl_display_ptr
 * @param wlr_backend_ptr     Note: In order to not miss any input devices, the
 *                            backend should be started *after* creating the
 *                            input manager.
 * @param wlr_output_layout_ptr
 * @param wlr_seat_ptr
 * @param config_dict_ptr     May be NULL, would indicate an empty dict.
 * @param cursor_style_ptr
 * @param root_ptr
 *
 * @return Pointer to the input manager handle.
 */
wlmim_t *wlmim_input_manager_create(
    struct wl_display *wl_display_ptr,
    struct wlr_backend *wlr_backend_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    struct wlr_seat *wlr_seat_ptr,
    bspl_dict_t *config_dict_ptr,
    const struct wlmim_cursor_style *cursor_style_ptr,
    wlmtk_root_t *root_ptr);

/**
 * Destroys the input manager.
 *
 * @param input_manager_ptr
 */
void wlmim_input_manager_destroy(wlmim_t *input_manager_ptr);

/** @return Pointer to wlmim_t::events. */
struct wlmim_events *wlmim_events(wlmim_t *input_manager_ptr);

/**
 * Updates the cursor style of the input manager.
 *
 * @param input_manager_ptr
 * @param style_ptr
 *
 * @return true on success.
 */
bool wlmim_set_style(
    wlmim_t *input_manager_ptr,
    const struct wlmim_cursor_style *style_ptr);

/**
 * Sets the filter for axis events. There is one filter; this replaces it.
 *
 * @param input_manager_ptr
 * @param filter              The filter, or NULL to clear it.
 * @param ud_ptr              Passed to `filter`.
 */
void wlmim_set_axis_filter(
    wlmim_t *input_manager_ptr,
    wlmim_axis_filter_t filter,
    void *ud_ptr);

/**
 * Runs the axis filter, if any.
 *
 * @return true if the event was consumed by the filter.
 */
bool wlmim_filter_axis(
    wlmim_t *input_manager_ptr,
    const struct wlr_pointer_axis_event *wlr_pointer_axis_event_ptr,
    uint32_t modifiers);

/**
 * Sets the filter for button events. There is one filter; this replaces it.
 *
 * @param input_manager_ptr
 * @param filter              The filter, or NULL to clear it.
 * @param ud_ptr              Passed to `filter`.
 */
void wlmim_set_button_filter(
    wlmim_t *input_manager_ptr,
    wlmim_button_filter_t filter,
    void *ud_ptr);

/**
 * Runs the button filter, if any.
 *
 * @return true if the event was consumed by the filter.
 */
bool wlmim_filter_button(
    wlmim_t *input_manager_ptr,
    const struct wlr_pointer_button_event *wlr_pointer_button_event_ptr,
    uint32_t modifiers);

/** @return Pointer to struct wlr_cursor of @ref wlmim_t::cursor_ptr. */
struct wlr_cursor *wlmim_wlr_cursor(wlmim_t *input_manager_ptr);

/**
 * Magnifies the cursor image. See @ref wlmim_cursor_set_magnification.
 *
 * @param input_manager_ptr
 * @param magnification       1.0: The regular image.
 * @param max_magnification
 */
void wlmim_set_cursor_magnification(
    wlmim_t *input_manager_ptr,
    double magnification,
    double max_magnification);

/**
 * Replaces the cursor image. See @ref wlmim_cursor_set_override.
 *
 * @param input_manager_ptr
 * @param wlr_buffer_ptr      NULL to show the regular image again.
 * @param hotspot_x
 * @param hotspot_y
 * @param scale
 */
void wlmim_set_cursor_override(
    wlmim_t *input_manager_ptr,
    struct wlr_buffer *wlr_buffer_ptr,
    int32_t hotspot_x,
    int32_t hotspot_y,
    float scale);

/** See @ref wlmim_cursor_create_xcursor_buffer. */
struct wlr_buffer *wlmim_create_cursor_buffer(
    wlmim_t *input_manager_ptr,
    const char *theme_name_ptr,
    const char *const *names_ptr,
    int32_t *hotspot_x_ptr,
    int32_t *hotspot_y_ptr,
    float *scale_ptr);

/** @return Whether the cursor has an image: Clients may hide it. */
bool wlmim_cursor_visible(wlmim_t *input_manager_ptr);

/**
 * Reports input activity. Used to reset idle timers.
 *
 * @param input_manager_ptr
 */
void wlmim_report_activity(wlmim_t *input_manager_ptr);

/** Sets @ref wlmim_t::last_keyboard_group_index. */
void wlmim_set_keyboard_group_index(
    wlmim_t *input_manager_ptr,
    uint32_t index);
/** @return @ref wlmim_t::last_keyboard_group_index. */
uint32_t wlmim_get_keyboard_group_index(
    wlmim_t *input_manager_ptr);

/**
 * Sets the active xkb_keymap and applies it to all keyboards.
 *
 * @param input_manager_ptr
 * @param xkb_keymap_ptr      Takes a reference to `xkb_keymap_ptr`.
 */
void wlmim_input_manager_set_keymap(
    wlmim_t *input_manager_ptr,
    struct xkb_keymap *xkb_keymap_ptr);

/**
 * Re-reads the input sections of the configuration ('Keyboard', 'Touchpad',
 * 'Mouse', 'Cursor') and applies them to the devices: Keymap and repeat
 * rate to the keyboards, acceleration and touchpad options to the pointers.
 * The cursor's modifiers apply to windows created afterwards.
 *
 * @param input_manager_ptr
 * @param config_dict_ptr
 *
 * @return false on error, then the current configuration stays.
 */
bool wlmim_input_manager_reconfigure(
    wlmim_t *input_manager_ptr,
    bspl_dict_t *config_dict_ptr);

/**
 * Sets repeat information for all keyboards of the input manager.
 *
 * @param input_manager_ptr
 * @param rate
 * @param delay
 */
void wlmim_input_manager_set_repeat_info(
    wlmim_t *input_manager_ptr,
    int32_t rate,
    int32_t delay);

/**
 * Binds a particular key combination to a callback.
 *
 * @param input_manager_ptr
 * @param key_combo_ptr
 * @param callback
 *
 * @return The key binding handle or NULL on error.
 */
wlmim_keybinding_t *wlmim_bind_key(
    wlmim_t *input_manager_ptr,
    const struct wlmim_keybinding_combo *key_combo_ptr,
    wlmim_keybinding_callback_t callback);

/**
 * Releases a key binding. @see wlmim_bind_key.
 *
 * @param input_manager_ptr
 * @param keybinding_ptr
 */
void wlmim_unbind_key(
    wlmim_t *input_manager_ptr,
    wlmim_keybinding_t *keybinding_ptr);

/**
 * Callback of a keyboard grab, for each key press.
 *
 * @param keysym
 * @param modifiers           See `enum wlr_keyboard_modifiers`.
 * @param ud_ptr
 *
 * @return Whether the press was consumed. False lets it go on as if there
 *     were no grab (eg. the grab gave up because the session got locked).
 */
typedef bool (*wlmim_key_grab_callback_t)(
    xkb_keysym_t keysym,
    uint32_t modifiers,
    void *ud_ptr);

/**
 * Grabs the keyboard: While set, every key press goes to `callback` and no
 * key binding runs; clients get no presses. Releases still reach clients,
 * so that no key stays down for them. A NULL callback ends the grab.
 *
 * @param input_manager_ptr
 * @param callback            May be NULL.
 * @param ud_ptr
 */
void wlmim_set_key_grab(
    wlmim_t *input_manager_ptr,
    wlmim_key_grab_callback_t callback,
    void *ud_ptr);

/** @return Whether the keyboard is grabbed, see @ref wlmim_set_key_grab. */
bool wlmim_has_key_grab(wlmim_t *input_manager_ptr);

/**
 * Processes key combo: Call back if a matching binding is found.
 *
 * @param input_manager_ptr
 * @param keysym
 * @param modifiers
 *
 * @return true if a binding was found AND the callback returned true.
 */
bool wlmim_process_key(
    wlmim_t *input_manager_ptr,
    xkb_keysym_t keysym,
    uint32_t modifiers);

/** @return A pointer to @ref _wlmim_t::cursor_options. */
const struct wlmim_cursor_options *wlmim_cursor_options(
    wlmim_t *input_manager_ptr);

/** All modifiers to use by default. */
extern const uint32_t wlmim_modifiers_default_mask;

/** Unit test set. */
extern const bs_test_set_t    wlmim_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // __WLMAKER_INPUT_MANAGER_H__
/* == End of manager.h ===================================================== */
