/* ========================================================================= */
/**
 * @file wlmcalclock.c
 *
 * Calendar clock DockApp: Shows the time on a LED-style display, and weekday,
 * day and month on a calendar page. Similar in spirit to Window Maker's
 * `wmclock`.
 *
 * Weekday and month names are taken from the session's locale (LC_TIME). The
 * app wakes up only once per minute (or once per second, with --seconds).
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

#include <cairo.h>
#include <libbase/libbase.h>
#include <locale.h>
#include <primitives/primitives.h>
#include <primitives/segment_display.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client-core.h>
#include <wlclient/dblbuf.h>
#include <wlclient/icon.h>
#include <wlclient/wlclient.h>

#include "wlm_calendar.h"

/* == Declarations ========================================================= */

/** Logical size of the icon. Drawing is scaled to the actual size. */
#define ICON_SIZE 56.0

/** State of the client. */
static wlmcl_client_t *wlclient_ptr;
/** Double buffer. */
static wlmcl_dblbuf_t *dblbuf_ptr;

/** Whether to use the 12-hour format. */
static bool arg_twelve_hour = false;
/** Whether to show seconds. */
static bool arg_seconds = false;

/** Commandline arguments. */
static const bs_arg_t wlmcalclock_args[] = {
    BS_ARG_BOOL(
        "twelve_hour",
        "Optional: Show the time in 12-hour format, with AM/PM indicator. "
        "Default: 24-hour format.",
        false,
        &arg_twelve_hour),
    BS_ARG_BOOL(
        "seconds",
        "Optional: Also show seconds. The app then wakes up every second, "
        "instead of every minute.",
        false,
        &arg_seconds),
    bs_arg_log_level,
    BS_ARG_SENTINEL()
};

/** Foreground color of a LED in the VFD-style display. */
static const uint32_t color_led = 0xff55ffff;
/** Color of a turned-off element in the VFD-style display. */
static const uint32_t color_off = 0xff114444;
/** Background color in the VFD-style display. */
static const uint32_t color_background = 0xff111111;
/** Color of the calendar page. */
static const uint32_t color_paper = 0xffe8e4d8;
/** Color of the band with the weekday, and of the month's name. */
static const uint32_t color_red = 0xffa01c1c;
/** Color of the day's number. */
static const uint32_t color_ink = 0xff1c1c1c;

/** Segments for the hours & minutes digits. */
static const wlm_cairo_7segment_param_t param_large = {
    .offset = 0.8,
    .width = 1.4,
    .hlength = 6.5,
    .vlength = 6.0
};

/** Font face for the calendar page. */
static const char font_face[] = "Sans";

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Draws `text_ptr` horizontally centered at `x`, with baseline at `y`.
 *
 * If wider than `max_width`, the font size is reduced to fit.
 *
 * @param cairo_ptr
 * @param text_ptr
 * @param x
 * @param y
 * @param size
 * @param max_width
 */
static void _draw_text_centered(
    cairo_t *cairo_ptr,
    const char *text_ptr,
    double x,
    double y,
    double size,
    double max_width)
{
    cairo_set_font_size(cairo_ptr, size);
    cairo_text_extents_t extents;
    cairo_text_extents(cairo_ptr, text_ptr, &extents);
    if (extents.x_advance > max_width && 0 < extents.x_advance) {
        cairo_set_font_size(cairo_ptr, size * max_width / extents.x_advance);
        cairo_text_extents(cairo_ptr, text_ptr, &extents);
    }
    cairo_move_to(cairo_ptr, x - extents.x_bearing - extents.width / 2.0, y);
    cairo_show_text(cairo_ptr, text_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the digits of the time, on the LED panel.
 *
 * @param cairo_ptr
 * @param calendar_ptr
 */
static void _draw_time(cairo_t *cairo_ptr, const wlm_calendar_t *calendar_ptr)
{
    // LED panel, sunken.
    cairo_set_source_argb8888(cairo_ptr, color_background);
    cairo_rectangle(cairo_ptr, 1, 1, ICON_SIZE - 2, 19);
    cairo_fill(cairo_ptr);
    wlm_primitives_draw_bezel_at(cairo_ptr, 0, 0, ICON_SIZE, 21, 1.0, false);

    // Layout: Hours & minutes are centered, unless seconds or the AM/PM
    // indicator need space on the right.
    uint32_t x = 8;
    if (arg_seconds) {
        x = 2;
    } else if (arg_twelve_hour) {
        x = 4;
    }
    const uint32_t baseline = 17;
    for (int i = 0; i < 4; ++i) {
        char c = calendar_ptr->digits[i];
        uint32_t pos = x + i * 9 + (i / 2) * 5;
        if ('0' <= c && c <= '9') {
            wlm_cairo_7segment_display_digit(
                cairo_ptr, &param_large, pos, baseline,
                color_led, color_off, c - '0');
        } else {
            wlm_cairo_7segment_display_segments(
                cairo_ptr, &param_large, pos, baseline,
                color_led, color_off, 0);
        }
    }

    // The colon.
    cairo_set_source_argb8888(cairo_ptr, color_led);
    cairo_rectangle(cairo_ptr, x + 18.8, 7.0, 1.4, 1.4);
    cairo_rectangle(cairo_ptr, x + 18.8, 12.6, 1.4, 1.4);
    cairo_fill(cairo_ptr);

    if (arg_seconds) {
        for (int i = 4; i < 6; ++i) {
            wlm_cairo_7segment_display_digit(
                cairo_ptr, &wlm_cairo_7segment_param_6x8,
                44 + (i - 4) * 6, baseline,
                color_led, color_off, calendar_ptr->digits[i] - '0');
        }
    }

    if (arg_twelve_hour) {
        cairo_select_font_face(cairo_ptr, font_face, CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        if (arg_seconds) {
            // Just one indicator, above the seconds.
            cairo_set_source_argb8888(cairo_ptr, color_led);
            _draw_text_centered(cairo_ptr, calendar_ptr->pm ? "PM" : "AM",
                                49.5, 8.0, 5.5, 10.0);
        } else {
            cairo_set_source_argb8888(
                cairo_ptr, calendar_ptr->pm ? color_off : color_led);
            _draw_text_centered(cairo_ptr, "AM", 50.0, 9.5, 5.5, 8.0);
            cairo_set_source_argb8888(
                cairo_ptr, calendar_ptr->pm ? color_led : color_off);
            _draw_text_centered(cairo_ptr, "PM", 50.0, 17.0, 5.5, 8.0);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the calendar page, with weekday, day and month.
 *
 * @param cairo_ptr
 * @param calendar_ptr
 */
static void _draw_page(cairo_t *cairo_ptr, const wlm_calendar_t *calendar_ptr)
{
    const double top = 22;

    cairo_set_source_argb8888(cairo_ptr, color_paper);
    cairo_rectangle(cairo_ptr, 1, top + 1, ICON_SIZE - 2, ICON_SIZE - top - 2);
    cairo_fill(cairo_ptr);

    // The band with the weekday.
    cairo_set_source_argb8888(cairo_ptr, color_red);
    cairo_rectangle(cairo_ptr, 1, top + 1, ICON_SIZE - 2, 9);
    cairo_fill(cairo_ptr);

    wlm_primitives_draw_bezel_at(
        cairo_ptr, 0, top, ICON_SIZE, ICON_SIZE - top, 1.0, true);

    cairo_select_font_face(cairo_ptr, font_face, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_argb8888(cairo_ptr, 0xfff8f4ec);
    _draw_text_centered(cairo_ptr, calendar_ptr->weekday,
                        ICON_SIZE / 2, top + 8.6, 8.5, ICON_SIZE - 8);

    char day[4];
    snprintf(day, sizeof(day), "%d", BS_MIN(BS_MAX(calendar_ptr->mday, 0), 99));
    cairo_set_source_argb8888(cairo_ptr, color_ink);
    _draw_text_centered(cairo_ptr, day,
                        ICON_SIZE / 2, top + 24.0, 16.0, ICON_SIZE - 8);

    cairo_set_source_argb8888(cairo_ptr, color_red);
    _draw_text_centered(cairo_ptr, calendar_ptr->month,
                        ICON_SIZE / 2, top + 32.0, 7.5, ICON_SIZE - 8);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws contents into the icon buffer.
 *
 * @param gfxbuf_ptr
 * @param ud_ptr
 *
 * @return true on success.
 */
static bool _icon_callback(bs_gfxbuf_t *gfxbuf_ptr, __UNUSED__ void *ud_ptr)
{
    bs_gfxbuf_clear(gfxbuf_ptr, 0);

    cairo_t *cairo_ptr = cairo_create_from_bs_gfxbuf(gfxbuf_ptr);
    if (NULL == cairo_ptr) {
        bs_log(BS_ERROR, "Failed cairo_create_from_bs_gfxbuf(%p)", gfxbuf_ptr);
        return false;
    }
    cairo_scale(cairo_ptr,
                gfxbuf_ptr->width / ICON_SIZE,
                gfxbuf_ptr->height / ICON_SIZE);

    // Uses the timers' clock: `time()` may be served from a coarse clock that
    // still shows the previous second when the timer fires.
    time_t now = bs_usec() / 1000000;
    struct tm tm;
    if (NULL == localtime_r(&now, &tm)) memset(&tm, 0, sizeof(tm));
    wlm_calendar_t calendar;
    wlm_calendar_format(&tm, arg_twelve_hour, arg_seconds, &calendar);

    _draw_time(cairo_ptr, &calendar);
    _draw_page(cairo_ptr, &calendar);

    cairo_destroy(cairo_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** @return When to draw next: At the next full minute, or second. */
static uint64_t _next_draw_usec(void)
{
    return wlm_calendar_next_period_usec(
        bs_usec(), arg_seconds ? UINT64_C(1000000) : UINT64_C(60000000));
}

/* ------------------------------------------------------------------------- */
/** Called on each full minute (or second): Redraws. */
static void _timer_callback(wlmcl_client_t *client_ptr, void *ud_ptr)
{
    if (NULL != dblbuf_ptr) {
        wlmcl_dblbuf_register_ready_callback(dblbuf_ptr, _icon_callback, NULL);
    }
    wlmcl_client_register_timer(
        client_ptr, _next_draw_usec(), _timer_callback, ud_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles configure events: (Re)creates the buffer and draws. */
static void _handle_configure(void *ud_ptr, uint32_t width, uint32_t height)
{
    wlmcl_icon_t *icon_ptr = ud_ptr;
    if (NULL != dblbuf_ptr) {
        wlmcl_dblbuf_destroy(dblbuf_ptr);
    }
    dblbuf_ptr = wlmcl_dblbuf_create(
        wlmcl_client_attributes(wlclient_ptr)->app_id_ptr,
        wlmcl_icon_wl_surface(icon_ptr),
        wlmcl_client_attributes(wlclient_ptr)->wl_shm_ptr,
        width,
        height);
    if (NULL == dblbuf_ptr) {
        bs_log(BS_FATAL, "Failed wlmcl_dblbuf_create.");
        return;
    }
    wlmcl_dblbuf_register_ready_callback(dblbuf_ptr, _icon_callback, NULL);
}

/* == Main program ========================================================= */
/** Main program. */
int main(int argc, const char **argv)
{
    if (!bs_arg_parse(wlmcalclock_args, BS_ARG_MODE_EXTRA_ARGS, &argc, argv)) {
        fprintf(stderr, "Failed to parse commandline arguments.\n");
        bs_arg_print_usage(stderr, wlmcalclock_args);
        return EXIT_FAILURE;
    }
    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--help")) {
            bs_arg_print_usage(stdout, wlmcalclock_args);
            bs_arg_cleanup(wlmcalclock_args);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "Unknown argument \"%s\".\n", argv[i]);
        bs_arg_print_usage(stderr, wlmcalclock_args);
        bs_arg_cleanup(wlmcalclock_args);
        return EXIT_FAILURE;
    }

    // Names of weekday and month, and their case conversion, follow the
    // session's locale. Other categories (eg. LC_NUMERIC) remain "C".
    setlocale(LC_TIME, "");
    setlocale(LC_CTYPE, "");

    wlclient_ptr = wlmcl_client_create("wlmaker.wlmcalclock");
    if (NULL == wlclient_ptr) return EXIT_FAILURE;

    // TODO(kaeser@gubbe.ch): Move icon_protocol to @ref wlmcl_client_register,
    // and permit clients to be registerd as 'required'.
    wl_display_roundtrip(
        wlmcl_client_attributes(wlclient_ptr)->wl_display_ptr);
    wl_display_roundtrip(
        wlmcl_client_attributes(wlclient_ptr)->wl_display_ptr);
    int rv = EXIT_SUCCESS;
    if (wlmcl_icon_supported(wlclient_ptr)) {
        wlmcl_icon_t *icon_ptr = wlmcl_icon_create(wlclient_ptr);
        if (NULL == icon_ptr) {
            bs_log(BS_ERROR, "Failed wlmcl_icon_create(%p)", wlclient_ptr);
            rv = EXIT_FAILURE;
        } else {
            wlmcl_icon_register_configure_callback(
                icon_ptr, _handle_configure, icon_ptr);
            wlmcl_client_register_timer(
                wlclient_ptr, _next_draw_usec(), _timer_callback, icon_ptr);

            wlmcl_client_run(wlclient_ptr);
            wlmcl_icon_destroy(icon_ptr);
            if (NULL != dblbuf_ptr) {
                wlmcl_dblbuf_destroy(dblbuf_ptr);
                dblbuf_ptr = NULL;
            }
        }
    } else {
        bs_log(BS_ERROR, "icon protocol is not supported.");
        rv = EXIT_FAILURE;
    }

    wlmcl_client_destroy(wlclient_ptr);
    bs_arg_cleanup(wlmcalclock_args);
    return rv;
}
/* == End of wlmcalclock.c ================================================= */
