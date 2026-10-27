/* ========================================================================= */
/**
 * @file i18n.h
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
#ifndef __WLMAKER_I18N_H__
#define __WLMAKER_I18N_H__

#include <libbase/libbase.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Translates a text of the user interface (eg. a menu item) into the
 * language of the user's locale: The first of `LANGUAGE`, `LC_ALL`,
 * `LC_MESSAGES` and `LANG` that is set. A small built-in table, for the
 * items of the Dock's, Clip's and dock-apps' menus.
 *
 * @param text_ptr            The text, in English.
 *
 * @return The translation, or `text_ptr` if there is none.
 */
const char *wlmaker_i18n(const char *text_ptr);

/**
 * Sets the locale to translate to, instead of the one from the environment.
 * For tests.
 *
 * @param locale_ptr          Eg. "pt_BR.UTF-8". NULL to use the environment.
 */
void wlmaker_i18n_set_locale(const char *locale_ptr);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_i18n_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_I18N_H__ */
/* == End of i18n.h ======================================================== */
