/* ========================================================================= */
/**
 * @file wlm_calendar.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2026 Google LLC
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

#include "wlm_calendar.h"

#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* == Declarations ========================================================= */

static void _wlm_calendar_name(
    char *dest_ptr,
    size_t dest_size,
    nl_item item);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
void wlm_calendar_format(
    const struct tm *tm_ptr,
    bool twelve_hour,
    bool seconds,
    wlm_calendar_t *calendar_ptr)
{
    *calendar_ptr = (wlm_calendar_t){};

    int hour = tm_ptr->tm_hour;
    if (twelve_hour) {
        calendar_ptr->pm = 12 <= hour;
        hour %= 12;
        if (0 == hour) hour = 12;
    }
    // Values are clamped for the compiler to see they fit the buffer.
    snprintf(calendar_ptr->digits, sizeof(calendar_ptr->digits),
             seconds ? "%02d%02d%02d" : "%02d%02d",
             BS_MIN(BS_MAX(hour, 0), 23),
             BS_MIN(BS_MAX(tm_ptr->tm_min, 0), 59),
             BS_MIN(BS_MAX(tm_ptr->tm_sec, 0), 60));
    if (twelve_hour && '0' == calendar_ptr->digits[0]) {
        calendar_ptr->digits[0] = ' ';
    }

    calendar_ptr->mday = tm_ptr->tm_mday;
    _wlm_calendar_name(calendar_ptr->weekday, sizeof(calendar_ptr->weekday),
                       ABDAY_1 + BS_MIN(BS_MAX(tm_ptr->tm_wday, 0), 6));
    _wlm_calendar_name(calendar_ptr->month, sizeof(calendar_ptr->month),
                       ABMON_1 + BS_MIN(BS_MAX(tm_ptr->tm_mon, 0), 11));
}

/* ------------------------------------------------------------------------- */
void wlm_calendar_upcase(char *dest_ptr, size_t dest_size, const char *src_ptr)
{
    if (0 == dest_size) return;
    mbstate_t in_state = {}, out_state = {};
    size_t src_len = strlen(src_ptr);
    size_t pos = 0;

    while (0 < src_len) {
        wchar_t wc;
        char mb[MB_LEN_MAX];
        size_t in_len = mbrtowc(&wc, src_ptr, src_len, &in_state);
        size_t out_len;
        if ((size_t)-1 == in_len || (size_t)-2 == in_len || 0 == in_len) {
            // Invalid or incomplete sequence: Copy the byte as-is.
            in_state = (mbstate_t){};
            mb[0] = *src_ptr;
            in_len = 1;
            out_len = 1;
        } else {
            out_len = wcrtomb(mb, (wchar_t)towupper((wint_t)wc), &out_state);
            if ((size_t)-1 == out_len) {
                out_state = (mbstate_t){};
                memcpy(mb, src_ptr, in_len);
                out_len = in_len;
            }
        }
        if (pos + out_len >= dest_size) break;
        memcpy(dest_ptr + pos, mb, out_len);
        pos += out_len;
        src_ptr += in_len;
        src_len -= in_len;
    }
    dest_ptr[pos] = '\0';
}

/* ------------------------------------------------------------------------- */
uint64_t wlm_calendar_next_period_usec(uint64_t now_usec, uint64_t period_usec)
{
    if (0 == period_usec) return now_usec + 1;
    return (now_usec / period_usec + 1) * period_usec;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Stores the upper-cased locale's string for `item` into `dest_ptr`, with
 * trailing periods removed.
 *
 * @param dest_ptr
 * @param dest_size
 * @param item
 */
void _wlm_calendar_name(char *dest_ptr, size_t dest_size, nl_item item)
{
    wlm_calendar_upcase(dest_ptr, dest_size, nl_langinfo(item));
    size_t len = strlen(dest_ptr);
    while (0 < len && '.' == dest_ptr[len - 1]) dest_ptr[--len] = '\0';
}

/* == Unit tests =========================================================== */

static void test_format(bs_test_t *test_ptr);
static void test_format_12h(bs_test_t *test_ptr);
static void test_format_pt_br(bs_test_t *test_ptr);
static void test_upcase(bs_test_t *test_ptr);
static void test_next_period(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlm_calendar_test_cases[] = {
    { 1, "format", test_format },
    { 1, "format_12h", test_format_12h },
    { 1, "format_pt_br", test_format_pt_br },
    { 1, "upcase", test_upcase },
    { 1, "next_period", test_next_period },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlm_calendar_test_set = BS_TEST_SET(
    true, "calendar", _wlm_calendar_test_cases);

/** Monday, 2026-10-26 09:05:07. */
static const struct tm _test_tm = {
    .tm_year = 126, .tm_mon = 9, .tm_mday = 26, .tm_wday = 1,
    .tm_hour = 9, .tm_min = 5, .tm_sec = 7
};

/* ------------------------------------------------------------------------- */
/** Tests formatting in 24-hour mode, in the "C" locale. */
void test_format(bs_test_t *test_ptr)
{
    char *old_locale_ptr = logged_strdup(setlocale(LC_ALL, NULL));
    setlocale(LC_ALL, "C");

    wlm_calendar_t c;
    wlm_calendar_format(&_test_tm, false, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "0905", c.digits);
    BS_TEST_VERIFY_FALSE(test_ptr, c.pm);
    BS_TEST_VERIFY_EQ(test_ptr, 26, c.mday);
    BS_TEST_VERIFY_STREQ(test_ptr, "MON", c.weekday);
    BS_TEST_VERIFY_STREQ(test_ptr, "OCT", c.month);

    wlm_calendar_format(&_test_tm, false, true, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "090507", c.digits);

    struct tm tm = _test_tm;
    tm.tm_hour = 23;
    tm.tm_wday = 6;
    tm.tm_mon = 0;
    wlm_calendar_format(&tm, false, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "2305", c.digits);
    BS_TEST_VERIFY_STREQ(test_ptr, "SAT", c.weekday);
    BS_TEST_VERIFY_STREQ(test_ptr, "JAN", c.month);

    if (NULL != old_locale_ptr) {
        setlocale(LC_ALL, old_locale_ptr);
        free(old_locale_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Tests formatting in 12-hour mode. */
void test_format_12h(bs_test_t *test_ptr)
{
    wlm_calendar_t c;
    struct tm tm = _test_tm;
    wlm_calendar_format(&tm, true, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, " 905", c.digits);
    BS_TEST_VERIFY_FALSE(test_ptr, c.pm);

    tm.tm_hour = 0;
    wlm_calendar_format(&tm, true, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "1205", c.digits);
    BS_TEST_VERIFY_FALSE(test_ptr, c.pm);

    tm.tm_hour = 12;
    wlm_calendar_format(&tm, true, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "1205", c.digits);
    BS_TEST_VERIFY_TRUE(test_ptr, c.pm);

    tm.tm_hour = 21;
    wlm_calendar_format(&tm, true, true, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, " 90507", c.digits);
    BS_TEST_VERIFY_TRUE(test_ptr, c.pm);
}

/* ------------------------------------------------------------------------- */
/** Tests the Brazilian Portuguese names, if that locale is available. */
void test_format_pt_br(bs_test_t *test_ptr)
{
    char *old_locale_ptr = logged_strdup(setlocale(LC_ALL, NULL));
    if (NULL == setlocale(LC_ALL, "pt_BR.UTF-8")) {
        bs_log(BS_WARNING, "Locale pt_BR.UTF-8 not available, skipping.");
        free(old_locale_ptr);
        return;
    }

    wlm_calendar_t c;
    wlm_calendar_format(&_test_tm, false, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "SEG", c.weekday);
    BS_TEST_VERIFY_STREQ(test_ptr, "OUT", c.month);

    // Saturday has an accented letter, which must be upper-cased, too.
    struct tm tm = _test_tm;
    tm.tm_wday = 6;
    tm.tm_mon = 1;
    wlm_calendar_format(&tm, false, false, &c);
    BS_TEST_VERIFY_STREQ(test_ptr, "S\xc3\x81" "B", c.weekday);
    BS_TEST_VERIFY_STREQ(test_ptr, "FEV", c.month);

    if (NULL != old_locale_ptr) {
        setlocale(LC_ALL, old_locale_ptr);
        free(old_locale_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Tests @ref wlm_calendar_upcase. */
void test_upcase(bs_test_t *test_ptr)
{
    char *old_locale_ptr = logged_strdup(setlocale(LC_ALL, NULL));
    setlocale(LC_ALL, "C");

    char buf[8];
    wlm_calendar_upcase(buf, sizeof(buf), "abc");
    BS_TEST_VERIFY_STREQ(test_ptr, "ABC", buf);
    wlm_calendar_upcase(buf, sizeof(buf), "");
    BS_TEST_VERIFY_STREQ(test_ptr, "", buf);
    // Truncates.
    wlm_calendar_upcase(buf, 3, "abcdef");
    BS_TEST_VERIFY_STREQ(test_ptr, "AB", buf);
    // Non-ASCII bytes are kept unchanged in the "C" locale.
    wlm_calendar_upcase(buf, sizeof(buf), "s\xc3\xa1" "b");
    BS_TEST_VERIFY_STREQ(test_ptr, "S\xc3\xa1" "B", buf);

    if (NULL != setlocale(LC_ALL, "C.UTF-8")) {
        wlm_calendar_upcase(buf, sizeof(buf), "s\xc3\xa1" "b");
        BS_TEST_VERIFY_STREQ(test_ptr, "S\xc3\x81" "B", buf);
        // Does not split a multi-byte character when truncating.
        wlm_calendar_upcase(buf, 3, "s\xc3\xa1" "b");
        BS_TEST_VERIFY_STREQ(test_ptr, "S", buf);
    }

    if (NULL != old_locale_ptr) {
        setlocale(LC_ALL, old_locale_ptr);
        free(old_locale_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Tests @ref wlm_calendar_next_period_usec. */
void test_next_period(bs_test_t *test_ptr)
{
    const uint64_t minute = UINT64_C(60000000);
    BS_TEST_VERIFY_EQ(test_ptr, minute,
                      wlm_calendar_next_period_usec(0, minute));
    BS_TEST_VERIFY_EQ(test_ptr, minute,
                      wlm_calendar_next_period_usec(1, minute));
    BS_TEST_VERIFY_EQ(test_ptr, minute,
                      wlm_calendar_next_period_usec(minute - 1, minute));
    // Exactly at the boundary: The *next* boundary.
    BS_TEST_VERIFY_EQ(test_ptr, 2 * minute,
                      wlm_calendar_next_period_usec(minute, minute));
    BS_TEST_VERIFY_EQ(test_ptr, UINT64_C(1793000000000000),
                      wlm_calendar_next_period_usec(
                          UINT64_C(1792999999999999), UINT64_C(1000000)));
    BS_TEST_VERIFY_EQ(test_ptr, 43, wlm_calendar_next_period_usec(42, 0));
}

/* == End of wlm_calendar.c ================================================ */
