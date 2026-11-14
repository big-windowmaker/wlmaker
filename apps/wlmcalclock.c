/* ========================================================================= */
/**
 * @file wlmcalclock.c
 *
 * Calendar clock DockApp: Shows the time on a LED-style display, and weekday,
 * day and month on a calendar page. Similar in spirit to Window Maker's
 * `wmclock`.
 *
 * Weekday and month names are taken from the session's locale (LC_TIME). The
 * app wakes up only once per minute (or once per second, with seconds or a
 * blinking colon).
 *
 * Preferences (see wlm_settings.h), set from the dock-app's menu: Theme,
 * ClockStyle (Digital, Analog), TwelveHour, Seconds, BlinkColon,
 * CalendarStyle (Paper, LCD), ShowYear, TimeZone (Local, or eg.
 * America/Sao_Paulo), CalendarCommand (run on a left click).
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
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <primitives/primitives.h>
#include <primitives/segment_display.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlclient/dblbuf.h>
#include <wlclient/icon.h>
#include <wlclient/wlclient.h>

#include "wlm_calendar.h"
#include "wlm_settings.h"

/* == Declarations ========================================================= */

/** Logical size of the icon. Drawing is scaled to the actual size. */
#define ICON_SIZE 56.0

/** State of the client. */
static wlmcl_client_t *wlclient_ptr;
/** Double buffer. */
static wlmcl_dblbuf_t *dblbuf_ptr;
/** The icon. */
static wlmcl_icon_t *icon_ptr;
/** Preferences. */
static wlm_settings_t *settings_ptr;
/** Counts preference changes: A timer of an earlier generation expires. */
static uintptr_t timer_generation;
/** Value of the TZ variable at start, or NULL. For the 'Local' time zone. */
static char *original_tz_ptr;

/** Whether to use the 12-hour format. */
static bool arg_twelve_hour = false;
/** Whether to show seconds. */
static bool arg_seconds = false;

/** Commandline arguments. */
static const bs_arg_t wlmcalclock_args[] = {
    BS_ARG_BOOL(
        "twelve_hour",
        "Optional: Show the time in 12-hour format, with AM/PM indicator. "
        "Default: 24-hour format. The preference 'TwelveHour' overrides it.",
        false,
        &arg_twelve_hour),
    BS_ARG_BOOL(
        "seconds",
        "Optional: Also show seconds. The app then wakes up every second, "
        "instead of every minute. The preference 'Seconds' overrides it.",
        false,
        &arg_seconds),
    bs_arg_log_level,
    BS_ARG_SENTINEL()
};

/** The preferences, as applied. */
typedef struct {
    /** 12-hour format. */
    bool                      twelve_hour;
    /** Show seconds. */
    bool                      seconds;
    /** Blink the colon, every second. */
    bool                      blink;
    /** An analog clock, instead of the LED display and calendar page. */
    bool                      analog;
    /** The calendar page as a LCD, instead of paper. */
    bool                      lcd_calendar;
    /** Show the year next to the month. */
    bool                      show_year;
    /** Colors. */
    const wlm_theme_t         *theme_ptr;
    /** Command to run on a left click. Empty for none. */
    const char                *click_command_ptr;
} prefs_t;

/** Current preferences. */
static prefs_t prefs;

/** The app's own colors: A VFD-style display, and a paper calendar page. */
static const wlm_theme_t default_theme = {
    .name_ptr = "Default",
    .background = 0xff111111,
    .led = 0xff55ffff,
    .off = 0xff114444,
    .accent = 0xffa01c1c,
    .paper = 0xffe8e4d8,
    .ink = 0xff1c1c1c,
};

/** Color of the weekday on the paper calendar's band. */
static const uint32_t color_band_text = 0xfff8f4ec;

/** Segments for the hours & minutes digits. */
static const wlm_cairo_7segment_param_t param_large = {
    .offset = 0.8,
    .width = 1.4,
    .hlength = 6.5,
    .vlength = 6.0
};

/** Font face for the calendar page. */
static const char font_face[] = "Sans";

/** Clock styles. */
static const char *const clock_styles[] = { "Digital", "Analog", NULL };
/** Calendar styles. */
static const char *const calendar_styles[] = { "Paper", "LCD", NULL };

static void _schedule(void);

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Applies the preferences: Into @ref prefs, and the time zone. */
static void _apply_prefs(void)
{
    prefs.twelve_hour = wlm_settings_bool(
        settings_ptr, "TwelveHour", arg_twelve_hour);
    prefs.seconds = wlm_settings_bool(settings_ptr, "Seconds", arg_seconds);
    prefs.blink = wlm_settings_bool(settings_ptr, "BlinkColon", false);
    prefs.analog = 1 == wlm_settings_choice(
        settings_ptr, "ClockStyle", clock_styles, 0);
    prefs.lcd_calendar = 1 == wlm_settings_choice(
        settings_ptr, "CalendarStyle", calendar_styles, 0);
    prefs.show_year = wlm_settings_bool(settings_ptr, "ShowYear", false);
    prefs.theme_ptr = wlm_settings_theme(settings_ptr, &default_theme);
    prefs.click_command_ptr = wlm_settings_string(
        settings_ptr, "CalendarCommand", "");

    const char *tz_ptr = wlm_settings_string(settings_ptr, "TimeZone", "Local");
    if ('\0' == *tz_ptr || 0 == strcmp(tz_ptr, "Local")) {
        if (NULL != original_tz_ptr) {
            setenv("TZ", original_tz_ptr, 1);
        } else {
            unsetenv("TZ");
        }
    } else {
        setenv("TZ", tz_ptr, 1);
    }
    tzset();
}

/* ------------------------------------------------------------------------- */
/**
 * Draws text centered at x, with the baseline at y. Shrinks the font to fit
 * into `max_width`.
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
 * Draws the LED display with the time, in the top part.
 *
 * @param cairo_ptr
 * @param calendar_ptr
 * @param tm_ptr
 */
static void _draw_time(cairo_t *cairo_ptr, const wlm_calendar_t *calendar_ptr,
                       const struct tm *tm_ptr)
{
    const wlm_theme_t *t = prefs.theme_ptr;

    // LED panel, sunken.
    cairo_set_source_argb8888(cairo_ptr, t->background);
    cairo_rectangle(cairo_ptr, 1, 1, ICON_SIZE - 2, 19);
    cairo_fill(cairo_ptr);
    wlm_primitives_draw_bezel_at(cairo_ptr, 0, 0, ICON_SIZE, 21, 1.0, false);

    // Layout: Hours & minutes are centered, unless seconds or the AM/PM
    // indicator need space on the right.
    uint32_t x = 8;
    if (prefs.seconds) {
        x = 2;
    } else if (prefs.twelve_hour) {
        x = 4;
    }
    const uint32_t baseline = 17;
    for (int i = 0; i < 4; ++i) {
        char c = calendar_ptr->digits[i];
        uint32_t pos = x + i * 9 + (i / 2) * 5;
        if ('0' <= c && c <= '9') {
            wlm_cairo_7segment_display_digit(
                cairo_ptr, &param_large, pos, baseline,
                t->led, t->off, c - '0');
        } else {
            wlm_cairo_7segment_display_segments(
                cairo_ptr, &param_large, pos, baseline,
                t->led, t->off, 0);
        }
    }

    // The colon. Blinking: Off on every other second.
    bool colon_lit = !prefs.blink || 0 == tm_ptr->tm_sec % 2;
    cairo_set_source_argb8888(cairo_ptr, colon_lit ? t->led : t->off);
    cairo_rectangle(cairo_ptr, x + 18.8, 7.0, 1.4, 1.4);
    cairo_rectangle(cairo_ptr, x + 18.8, 12.6, 1.4, 1.4);
    cairo_fill(cairo_ptr);

    if (prefs.seconds) {
        for (int i = 4; i < 6; ++i) {
            wlm_cairo_7segment_display_digit(
                cairo_ptr, &wlm_cairo_7segment_param_6x8,
                44 + (i - 4) * 6, baseline,
                t->led, t->off, calendar_ptr->digits[i] - '0');
        }
    }

    if (prefs.twelve_hour) {
        cairo_select_font_face(cairo_ptr, font_face, CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        if (prefs.seconds) {
            // Just one indicator, above the seconds.
            cairo_set_source_argb8888(cairo_ptr, t->led);
            _draw_text_centered(cairo_ptr, calendar_ptr->pm ? "PM" : "AM",
                                49.5, 8.0, 5.5, 10.0);
        } else {
            cairo_set_source_argb8888(
                cairo_ptr, calendar_ptr->pm ? t->off : t->led);
            _draw_text_centered(cairo_ptr, "AM", 50.0, 9.5, 5.5, 8.0);
            cairo_set_source_argb8888(
                cairo_ptr, calendar_ptr->pm ? t->led : t->off);
            _draw_text_centered(cairo_ptr, "PM", 50.0, 17.0, 5.5, 8.0);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the calendar page: As a sheet of paper, or as a LCD.
 *
 * @param cairo_ptr
 * @param calendar_ptr
 * @param tm_ptr
 */
static void _draw_page(cairo_t *cairo_ptr, const wlm_calendar_t *calendar_ptr,
                       const struct tm *tm_ptr)
{
    const wlm_theme_t *t = prefs.theme_ptr;
    const double top = 22;
    uint32_t page = t->paper, band = t->accent, band_text = color_band_text;
    uint32_t day_color = t->ink, month_color = t->accent;
    if (prefs.lcd_calendar) {
        page = t->background;
        band = t->off;
        band_text = t->led;
        day_color = t->led;
        month_color = t->led;
    }

    cairo_set_source_argb8888(cairo_ptr, page);
    cairo_rectangle(cairo_ptr, 1, top + 1, ICON_SIZE - 2, ICON_SIZE - top - 2);
    cairo_fill(cairo_ptr);

    // The band with the weekday.
    cairo_set_source_argb8888(cairo_ptr, band);
    cairo_rectangle(cairo_ptr, 1, top + 1, ICON_SIZE - 2, 9);
    cairo_fill(cairo_ptr);

    // Paper is raised, a display is sunken.
    wlm_primitives_draw_bezel_at(
        cairo_ptr, 0, top, ICON_SIZE, ICON_SIZE - top, 1.0,
        !prefs.lcd_calendar);

    cairo_select_font_face(cairo_ptr, font_face, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_argb8888(cairo_ptr, band_text);
    _draw_text_centered(cairo_ptr, calendar_ptr->weekday,
                        ICON_SIZE / 2, top + 8.6, 8.5, ICON_SIZE - 8);

    char day[4];
    snprintf(day, sizeof(day), "%d", BS_MIN(BS_MAX(calendar_ptr->mday, 0), 99));
    cairo_set_source_argb8888(cairo_ptr, day_color);
    _draw_text_centered(cairo_ptr, day,
                        ICON_SIZE / 2, top + 24.0, 16.0, ICON_SIZE - 8);

    char month[WLM_CALENDAR_NAME_SIZE + 16];
    if (prefs.show_year) {
        snprintf(month, sizeof(month), "%s %d", calendar_ptr->month,
                 tm_ptr->tm_year + 1900);
    } else {
        snprintf(month, sizeof(month), "%s", calendar_ptr->month);
    }
    cairo_set_source_argb8888(cairo_ptr, month_color);
    _draw_text_centered(cairo_ptr, month,
                        ICON_SIZE / 2, top + 32.0, 7.5, ICON_SIZE - 8);
}

/* ------------------------------------------------------------------------- */
/** Draws a hand of the analog clock, at `fraction` of the full circle. */
static void _draw_hand(cairo_t *cairo_ptr, double fraction, double length,
                       double width, uint32_t color)
{
    double a = fraction * 2 * M_PI;
    cairo_set_source_argb8888(cairo_ptr, color);
    cairo_set_line_width(cairo_ptr, width);
    cairo_set_line_cap(cairo_ptr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cairo_ptr, ICON_SIZE / 2 - 2.5 * sin(a),
                  ICON_SIZE / 2 + 2.5 * cos(a));
    cairo_line_to(cairo_ptr, ICON_SIZE / 2 + length * sin(a),
                  ICON_SIZE / 2 - length * cos(a));
    cairo_stroke(cairo_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws an analog clock on the whole icon: Marks, hands, and the weekday and
 * day of the month on the face.
 *
 * @param cairo_ptr
 * @param calendar_ptr
 * @param tm_ptr
 */
static void _draw_analog(cairo_t *cairo_ptr, const wlm_calendar_t *calendar_ptr,
                         const struct tm *tm_ptr)
{
    const wlm_theme_t *t = prefs.theme_ptr;
    const double c = ICON_SIZE / 2;

    cairo_set_source_argb8888(cairo_ptr, t->background);
    cairo_rectangle(cairo_ptr, 1, 1, ICON_SIZE - 2, ICON_SIZE - 2);
    cairo_fill(cairo_ptr);
    wlm_primitives_draw_bezel_at(
        cairo_ptr, 0, 0, ICON_SIZE, ICON_SIZE, 1.0, false);

    // Marks: Each 5 minutes; longer and lit for the quarters.
    cairo_set_line_cap(cairo_ptr, CAIRO_LINE_CAP_BUTT);
    for (int i = 0; i < 12; ++i) {
        double a = i * M_PI / 6;
        bool quarter = 0 == i % 3;
        double r1 = quarter ? 20.5 : 22.0, r2 = 25.0;
        cairo_set_source_argb8888(cairo_ptr, quarter ? t->led : t->off);
        cairo_set_line_width(cairo_ptr, quarter ? 2.0 : 1.4);
        cairo_move_to(cairo_ptr, c + r1 * sin(a), c - r1 * cos(a));
        cairo_line_to(cairo_ptr, c + r2 * sin(a), c - r2 * cos(a));
        cairo_stroke(cairo_ptr);
    }

    // Weekday above the center. The day of the month in a window at the
    // right, like on a watch.
    cairo_select_font_face(cairo_ptr, font_face, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_argb8888(cairo_ptr, t->led);
    _draw_text_centered(cairo_ptr, calendar_ptr->weekday, c, 18.5, 6.0, 18.0);
    cairo_set_source_argb8888(cairo_ptr, t->off);
    cairo_rectangle(cairo_ptr, 33.5, 23.5, 12.0, 9.0);
    cairo_fill(cairo_ptr);
    char day[4];
    snprintf(day, sizeof(day), "%d", BS_MIN(BS_MAX(calendar_ptr->mday, 0), 99));
    cairo_set_source_argb8888(cairo_ptr, t->led);
    _draw_text_centered(cairo_ptr, day, 39.5, 31.0, 7.0, 11.0);

    double minutes = tm_ptr->tm_min + tm_ptr->tm_sec / 60.0;
    _draw_hand(cairo_ptr, ((tm_ptr->tm_hour % 12) + minutes / 60.0) / 12.0,
               12.5, 2.6, t->led);
    _draw_hand(cairo_ptr, minutes / 60.0, 19.5, 1.8, t->led);
    if (prefs.seconds) {
        _draw_hand(cairo_ptr, tm_ptr->tm_sec / 60.0, 21.0, 0.8, t->accent);
    }
    cairo_set_source_argb8888(cairo_ptr, t->led);
    cairo_arc(cairo_ptr, c, c, 2.0, 0, 2 * M_PI);
    cairo_fill(cairo_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Renders the icon: Time and calendar page, or the analog clock.
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
    wlm_calendar_format(&tm, prefs.twelve_hour, prefs.seconds, &calendar);

    if (prefs.analog) {
        _draw_analog(cairo_ptr, &calendar, &tm);
    } else {
        _draw_time(cairo_ptr, &calendar, &tm);
        _draw_page(cairo_ptr, &calendar, &tm);
    }

    cairo_destroy(cairo_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** @return When to draw next: At the next full minute, or second. */
static uint64_t _next_draw_usec(void)
{
    bool each_second = prefs.seconds || prefs.blink;
    return wlm_calendar_next_period_usec(
        bs_usec(), each_second ? UINT64_C(1000000) : UINT64_C(60000000));
}

/* ------------------------------------------------------------------------- */
/** Requests drawing the icon. */
static void _redraw(void)
{
    if (NULL != dblbuf_ptr) {
        wlmcl_dblbuf_register_ready_callback(dblbuf_ptr, _icon_callback, NULL);
    }
}

/* ------------------------------------------------------------------------- */
/** Called on each full minute (or second): Redraws. */
static void _timer_callback(__UNUSED__ wlmcl_client_t *client_ptr,
                            void *ud_ptr)
{
    // A timer from before the preferences changed: Its period may be wrong.
    if ((uintptr_t)ud_ptr != timer_generation) return;
    _redraw();
    _schedule();
}

/* ------------------------------------------------------------------------- */
/** Registers the timer for the next drawing. */
void _schedule(void)
{
    wlmcl_client_register_timer(
        wlclient_ptr, _next_draw_usec(), _timer_callback,
        (void*)timer_generation);
}

/* ------------------------------------------------------------------------- */
/** The preferences file changed: Applies it, redraws, restarts the timer. */
static void _handle_settings_fd(__UNUSED__ wlmcl_client_t *client_ptr,
                                __UNUSED__ void *ud_ptr)
{
    if (!wlm_settings_handle_fd(settings_ptr)) return;
    _apply_prefs();
    ++timer_generation;
    _schedule();
    _redraw();
}

/* ------------------------------------------------------------------------- */
/** Handles pointer buttons: A left click runs the calendar command. */
static void _handle_pointer_button(
    __UNUSED__ struct wl_listener *listener_ptr,
    void *data_ptr)
{
    struct wlmcl_client_pointer_button_event *event_ptr = data_ptr;
    if (NULL == icon_ptr ||
        event_ptr->wl_surface_ptr != wlmcl_icon_wl_surface(icon_ptr) ||
        BTN_LEFT != event_ptr->button || !event_ptr->pressed) return;
    wlm_settings_spawn(prefs.click_command_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles configure events: (Re)creates the buffer and draws. */
static void _handle_configure(void *ud_ptr, uint32_t width, uint32_t height)
{
    wlmcl_icon_t *i_ptr = ud_ptr;
    if (NULL != dblbuf_ptr) {
        wlmcl_dblbuf_destroy(dblbuf_ptr);
    }
    dblbuf_ptr = wlmcl_dblbuf_create(
        wlmcl_client_attributes(wlclient_ptr)->app_id_ptr,
        wlmcl_icon_wl_surface(i_ptr),
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

    if (NULL != getenv("TZ")) original_tz_ptr = logged_strdup(getenv("TZ"));
    settings_ptr = wlm_settings_create("wlmcalclock", NULL);
    if (NULL == settings_ptr) {
        bs_arg_cleanup(wlmcalclock_args);
        return EXIT_FAILURE;
    }
    _apply_prefs();

    wlclient_ptr = wlmcl_client_create("wlmaker.wlmcalclock");
    if (NULL == wlclient_ptr) {
        wlm_settings_destroy(settings_ptr);
        bs_arg_cleanup(wlmcalclock_args);
        return EXIT_FAILURE;
    }

    // TODO(kaeser@gubbe.ch): Move icon_protocol to @ref wlmcl_client_register,
    // and permit clients to be registerd as 'required'.
    wl_display_roundtrip(
        wlmcl_client_attributes(wlclient_ptr)->wl_display_ptr);
    wl_display_roundtrip(
        wlmcl_client_attributes(wlclient_ptr)->wl_display_ptr);
    int rv = EXIT_SUCCESS;
    if (wlmcl_icon_supported(wlclient_ptr)) {
        icon_ptr = wlmcl_icon_create(wlclient_ptr);
        if (NULL == icon_ptr) {
            bs_log(BS_ERROR, "Failed wlmcl_icon_create(%p)", wlclient_ptr);
            rv = EXIT_FAILURE;
        } else {
            struct wl_listener pointer_button_listener = {
                .notify = _handle_pointer_button };
            wl_signal_add(&wlmcl_client_events(wlclient_ptr)->pointer_button,
                          &pointer_button_listener);
            int fd = wlm_settings_watch(settings_ptr);
            if (0 <= fd) {
                wlmcl_client_register_fd(
                    wlclient_ptr, fd, _handle_settings_fd, NULL);
            }
            wlmcl_icon_register_configure_callback(
                icon_ptr, _handle_configure, icon_ptr);
            _schedule();

            wlmcl_client_run(wlclient_ptr);
            if (0 <= fd) wlmcl_client_unregister_fd(wlclient_ptr, fd);
            wl_list_remove(&pointer_button_listener.link);
            wlmcl_icon_destroy(icon_ptr);
            icon_ptr = NULL;
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
    wlm_settings_destroy(settings_ptr);
    if (NULL != original_tz_ptr) free(original_tz_ptr);
    bs_arg_cleanup(wlmcalclock_args);
    return rv;
}
/* == End of wlmcalclock.c ================================================= */
