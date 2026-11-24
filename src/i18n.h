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

/** The gettext domain of wlmaker: Its catalogs are `wlmaker.mo`. */
#define WLMAKER_I18N_DOMAIN "wlmaker"

/**
 * Marks a text for translation without translating it, eg. in a table of
 * constants. Translate it later through @ref wlmaker_i18n. `xgettext` knows
 * the keyword (see po/update-pot.sh).
 */
#define N_(text) (text)

/**
 * Sets up gettext for the program: The locale from the environment, except
 * for `LC_NUMERIC`, which stays "C" (the plist parser and wlroots read and
 * write numbers with a dot), and the catalogs of @ref WLMAKER_I18N_DOMAIN in
 * the locale directory (`WLMAKER_LOCALEDIR` from the environment, or the
 * install's `share/locale`). Translations come out in UTF-8.
 *
 * Call it once, at the start of `main()`.
 */
void wlmaker_i18n_init(void);

/**
 * Translates a text of the user interface (eg. a menu item) into the
 * language of the user's locale, from the gettext catalog of the domain
 * @ref WLMAKER_I18N_DOMAIN: `dgettext("wlmaker", text_ptr)`.
 *
 * Without a catalog for the locale, or without a translation in it, this
 * returns the text in English. `xgettext` extracts the literal arguments of
 * this function (`--keyword=wlmaker_i18n`).
 *
 * @param text_ptr            The text, in English.
 *
 * @return The translation, or `text_ptr` if there is none.
 */
const char *wlmaker_i18n(const char *text_ptr)
    __attribute__((format_arg(1)));

/** Unit test cases. */
extern const bs_test_set_t wlmaker_i18n_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMAKER_I18N_H__ */
/* == End of i18n.h ======================================================== */
