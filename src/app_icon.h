/* ========================================================================= */
/**
 * @file app_icon.h
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
#ifndef __WLMAKER_APP_ICON_H__
#define __WLMAKER_APP_ICON_H__

#include <cairo.h>
#include <stdbool.h>

#include <libbase/libbase.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * The icon of an application, as the desktop shows it: From its desktop entry
 * (`<app_id>.desktop`, or the one whose `StartupWMClass` is the X11 class),
 * looked up in the user's icon theme (GTK `settings.ini`), the themes it
 * inherits, hicolor and pixmaps. PNG or SVG, rendered at `size`.
 *
 * Results are cached (also misses), per application and size.
 *
 * @param app_id_ptr          Wayland app ID or WM_CLASS class. May be NULL.
 * @param instance_ptr        WM_CLASS instance. May be NULL.
 * @param size                Pixels, square.
 *
 * @return A cairo image surface owned by the cache (do not destroy), or
 *     NULL if no icon was found.
 */
cairo_surface_t *wlmaker_app_icon_get(
    const char *app_id_ptr,
    const char *instance_ptr,
    int size);

/** Clears the cache (eg. after the icon theme changed). */
void wlmaker_app_icon_flush(void);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_app_icon_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_APP_ICON_H__ */
/* == End of app_icon.h ==================================================== */
