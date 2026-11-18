/* ========================================================================= */
/**
 * @file i18n.c
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

#include "i18n.h"

#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* == Declarations ========================================================= */

#ifndef WLMAKER_LOCALEDIR
/** Where the catalogs are installed. Set by CMake. */
#define WLMAKER_LOCALEDIR "/usr/share/locale"
#endif  // WLMAKER_LOCALEDIR

static void _wlmaker_i18n_bind(const char *localedir_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
void wlmaker_i18n_init(void)
{
    // All categories from the environment, as any program. If the locale is
    // not available (eg. not generated), glibc keeps "C": English.
    if (NULL == setlocale(LC_ALL, "")) {
        bs_log(BS_WARNING, "Locale from the environment is not available, "
               "using \"C\" (texts in English).");
    }
    // Numbers stay with a dot: The plist parser (strtod), the configuration
    // files we write, and wlroots expect that.
    setlocale(LC_NUMERIC, "C");

    const char *dir_ptr = getenv("WLMAKER_LOCALEDIR");
    if (NULL == dir_ptr || '\0' == *dir_ptr) dir_ptr = WLMAKER_LOCALEDIR;
    _wlmaker_i18n_bind(dir_ptr);
}

/* ------------------------------------------------------------------------- */
const char *wlmaker_i18n(const char *text_ptr)
{
    // The empty msgid would return the catalog's header.
    if (NULL == text_ptr || '\0' == *text_ptr) return text_ptr;
    return dgettext(WLMAKER_I18N_DOMAIN, text_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Binds the domain to `localedir_ptr`, with translations in UTF-8. */
void _wlmaker_i18n_bind(const char *localedir_ptr)
{
    if (NULL == bindtextdomain(WLMAKER_I18N_DOMAIN, localedir_ptr) ||
        NULL == bind_textdomain_codeset(WLMAKER_I18N_DOMAIN, "UTF-8")) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed bindtextdomain(\"%s\", \"%s\")",
               WLMAKER_I18N_DOMAIN, localedir_ptr);
    }
    // Also the default domain, for gettext() in code shared with others.
    textdomain(WLMAKER_I18N_DOMAIN);
}

/* == Unit tests =========================================================== */

static void test_translate(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmaker_i18n_test_cases[] = {
    { true, "translate", test_translate },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_i18n_test_set = BS_TEST_SET(
    true, "i18n", _wlmaker_i18n_test_cases);

/* ------------------------------------------------------------------------- */
/**
 * Translates through the catalog compiled into the build tree (po/locale),
 * and falls back to English without a catalog for the language, or without
 * any catalog at all.
 */
void test_translate(bs_test_t *test_ptr)
{
#if defined(WLMAKER_BINARY_DIR)
    char *old_locale_ptr = logged_strdup(setlocale(LC_ALL, NULL));
    // gettext ignores LANGUAGE in the "C" locales (also C.UTF-8): The
    // messages' locale must be a real one. Any generated one does, LANGUAGE
    // picks the catalog. Preferably not a Portuguese one: gettext falls back
    // from LANGUAGE to it.
    static const char *locales[] = {
        "en_US.UTF-8", "en_GB.UTF-8", "de_DE.UTF-8", "es_ES.UTF-8",
        "fr_FR.UTF-8", "pt_BR.UTF-8", NULL };
    const char *locale_ptr = NULL;
    for (const char **l = locales; NULL == locale_ptr && NULL != *l; ++l) {
        bs_test_setenv(test_ptr, "LC_ALL", "%s", *l);
        if (NULL != setlocale(LC_ALL, "")) locale_ptr = *l;
    }
    bs_test_setenv(test_ptr, "LANGUAGE", "%s", "pt_BR");
    if (NULL == locale_ptr) {
        // Without any: The fallback to English can still be verified.
        bs_log(BS_WARNING, "No UTF-8 locale generated: Not verifying the "
               "translation, only the fallback to English.");
        bs_test_setenv(test_ptr, "LC_ALL", "%s", "C.UTF-8");
        setlocale(LC_ALL, "");
    }
    _wlmaker_i18n_bind(WLMAKER_BINARY_DIR "/po/locale");

    // Through variables: xgettext must not pick these up.
    const char *close_ptr = "Close", *unknown_ptr = "No such text, really";
    if (NULL != locale_ptr) {
        BS_TEST_VERIFY_STREQ(test_ptr, "Fechar", wlmaker_i18n(close_ptr));
    }
    BS_TEST_VERIFY_STREQ(test_ptr, unknown_ptr, wlmaker_i18n(unknown_ptr));
    BS_TEST_VERIFY_STREQ(test_ptr, "", wlmaker_i18n(""));

    // A language without a catalog: English. (setlocale() again: glibc
    // caches translations by locale, and re-reads LANGUAGE only then.)
    bs_test_setenv(test_ptr, "LANGUAGE", "%s", "es_ES");
    setlocale(LC_ALL, "");
    if (NULL == locale_ptr || 0 != strncmp(locale_ptr, "pt", 2)) {
        BS_TEST_VERIFY_STREQ(test_ptr, close_ptr, wlmaker_i18n(close_ptr));
    }

    // No catalog at all: English.
    bs_test_setenv(test_ptr, "LANGUAGE", "%s", "pt_BR");
    setlocale(LC_ALL, "");
    _wlmaker_i18n_bind(WLMAKER_BINARY_DIR "/po/no-such-dir");
    BS_TEST_VERIFY_STREQ(test_ptr, close_ptr, wlmaker_i18n(close_ptr));

    // Numbers keep the dot, also with a locale that writes a comma (when
    // that locale is generated on this machine).
    bs_test_setenv(test_ptr, "LC_ALL", "%s", "pt_BR.UTF-8");
    wlmaker_i18n_init();
    const char *ctype_ptr = setlocale(LC_CTYPE, NULL);
    if (NULL != ctype_ptr && 0 == strcmp(ctype_ptr, "pt_BR.UTF-8")) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", 1.5);
        BS_TEST_VERIFY_STREQ(test_ptr, "1.5", buf);
        BS_TEST_VERIFY_STREQ(test_ptr, "C", setlocale(LC_NUMERIC, NULL));
    } else {
        bs_log(BS_INFO, "No pt_BR.UTF-8 locale, skipping LC_NUMERIC test.");
    }

    setlocale(LC_ALL, old_locale_ptr);
    free(old_locale_ptr);
    _wlmaker_i18n_bind(WLMAKER_LOCALEDIR);
#else
    BS_TEST_FAIL(test_ptr, "WLMAKER_BINARY_DIR undefined");
#endif
}

/* == End of i18n.c ======================================================== */
