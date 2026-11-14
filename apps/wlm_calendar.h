/* ========================================================================= */
/**
 * @file wlm_calendar.h
 *
 * Date & time formatting for the calendar clock DockApp (`wlmcalclock`).
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
#ifndef __WLM_CALENDAR_H__
#define __WLM_CALENDAR_H__

#include <libbase/libbase.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Size of the buffers for weekday and month names, including the NUL. */
#define WLM_CALENDAR_NAME_SIZE 32

/** What the calendar clock shows, formatted for display. */
typedef struct {
    /**
     * Digits of the time: "HHMM", or "HHMMSS" if seconds are shown. In 12-hour
     * mode, a leading zero of the hour is a ' '. NUL-terminated.
     */
    char                      digits[7];
    /** In 12-hour mode: Whether it is after noon. False otherwise. */
    bool                      pm;
    /** Day of the month, 1..31. */
    int                       mday;
    /** Abbreviated name of the weekday, in upper case, UTF-8. Eg. "MON". */
    char                      weekday[WLM_CALENDAR_NAME_SIZE];
    /** Abbreviated name of the month, in upper case, UTF-8. Eg. "OCT". */
    char                      month[WLM_CALENDAR_NAME_SIZE];
} wlm_calendar_t;

/**
 * Formats `tm_ptr` for display.
 *
 * Weekday and month names are taken from the locale's LC_TIME category, and
 * converted to upper case using the LC_CTYPE category. Trailing periods (as in
 * some locales' abbreviations, eg. "lun.") are removed.
 *
 * @param tm_ptr
 * @param twelve_hour         Whether to use the 12-hour format.
 * @param seconds             Whether to include seconds in the digits.
 * @param calendar_ptr        Output.
 */
void wlm_calendar_format(
    const struct tm *tm_ptr,
    bool twelve_hour,
    bool seconds,
    wlm_calendar_t *calendar_ptr);

/**
 * Converts the multibyte string `src_ptr` to upper case, as per LC_CTYPE.
 *
 * Invalid sequences are copied unchanged. The output is always terminated,
 * and truncated at a character boundary if it does not fit.
 *
 * @param dest_ptr
 * @param dest_size
 * @param src_ptr
 */
void wlm_calendar_upcase(char *dest_ptr, size_t dest_size, const char *src_ptr);

/**
 * Computes when the next period starts, eg. the next full minute.
 *
 * @param now_usec            Current time, in microseconds since the epoch.
 * @param period_usec         Length of the period, eg. 60000000 for minutes.
 *
 * @return The start of the next period, in microseconds since the epoch. This
 *     is always later than `now_usec`.
 */
uint64_t wlm_calendar_next_period_usec(uint64_t now_usec, uint64_t period_usec);

/** Unit tests. */
extern const bs_test_set_t wlm_calendar_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLM_CALENDAR_H__ */
/* == End of wlm_calendar.h ================================================ */
