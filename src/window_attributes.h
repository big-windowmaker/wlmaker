/* ========================================================================= */
/**
 * @file window_attributes.h
 *
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
#ifndef __WLMAKER_WINDOW_ATTRIBUTES_H__
#define __WLMAKER_WINDOW_ATTRIBUTES_H__

#include <stdbool.h>
#include <stdint.h>

#include <libbase/libbase.h>
#include <libbase/plist.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * The window properties (WLMTK_WINDOW_PROPERTY_*) of an application's
 * window, as Window Maker's `WMWindowAttributes`: From the dict
 * `WindowAttributes` of the configuration, keyed as there by
 * "instance.class", "instance", "class" or "*" (the most specific key with
 * the attribute wins). A Wayland app ID is both instance and class.
 *
 * Attributes: `NoMiniaturizeButton`, `NoCloseButton`, `NoResizebar`.
 * As on Window Maker, a window of fixed size has no resize bar.
 *
 * @param config_dict_ptr     The configuration, or NULL.
 * @param class_ptr           WM_CLASS class, or the app ID. May be NULL.
 * @param instance_ptr        WM_CLASS instance, or the app ID. May be NULL.
 * @param fixed_size          Whether the window's minimum and maximum
 *                            sizes are the same.
 *
 * @return The properties: RESIZABLE, ICONIFIABLE and CLOSABLE, less those
 *     the attributes take away.
 */
uint32_t wlmaker_window_attributes_properties(
    bspl_dict_t *config_dict_ptr,
    const char *class_ptr,
    const char *instance_ptr,
    bool fixed_size);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_window_attributes_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_WINDOW_ATTRIBUTES_H__ */
/* == End of window_attributes.h =========================================== */
