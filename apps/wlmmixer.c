/* ========================================================================= */
/**
 * @file wlmmixer.c
 *
 * Volume mixer DockApp: Shows and controls the volume of the default audio
 * output ("sink"). Similar in spirit to Window Maker's `wmix`.
 *
 * Talks to the sound server through the PulseAudio client library, which
 * works with PulseAudio and with PipeWire (through `pipewire-pulse`). Changes
 * are received as events, there is no polling. The meter's bars light up
 * in proportion to the volume (0% at the left, 100% at the right). Clicking
 * on the meter sets the volume to that position, also when dragging along
 * it; the mouse wheel changes it by steps. A left click elsewhere toggles
 * mute, and a middle click starts a full mixer application (`pavucontrol`,
 * by default). The right button opens the dock-app's menu, in wlmaker: It
 * offers muting and the mixer application, too.
 *
 * Preferences (see wlm_settings.h), set from the dock-app's menu: Theme,
 * ShowPercent, ClickMutes, ScrollVolume, Step, MaxVolume, MixerCommand.
 *
 * The PulseAudio mainloop runs in a separate thread, and signals updates to
 * the Wayland mainloop through an `eventfd`.
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
#include <errno.h>
#include <libbase/libbase.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <primitives/primitives.h>
#include <primitives/segment_display.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlclient/dblbuf.h>
#include <wlclient/icon.h>
#include <wlclient/wlclient.h>

#include "wlm_settings.h"
#include "wlm_volume.h"

/* == Declarations ========================================================= */

/** Logical size of the icon. Drawing is scaled to the actual size. */
#define ICON_SIZE 56.0

/** Number of bars in the volume meter. */
#define BARS 12
/** Left end of the volume meter (0%), in icon coordinates. */
#define METER_LEFT 5.0
/** Bottom of the volume meter's bars. */
#define METER_BOTTOM 50.0
/** Height of the highest bar. */
#define METER_MAX_HEIGHT 22.0
/** Width of a bar. */
#define BAR_WIDTH 2.6
/** Distance from one bar to the next. */
#define BAR_PITCH 3.85
/** Length of the meter, from 0% to 100%. */
#define METER_LENGTH ((BARS - 1) * BAR_PITCH + BAR_WIDTH)
/** Margin around the meter that still counts as a click on it. */
#define METER_MARGIN 3.0

/** Initial delay for reconnecting to the sound server. */
#define RECONNECT_MIN_USEC (500 * PA_USEC_PER_MSEC)
/** Maximum delay for reconnecting to the sound server. */
#define RECONNECT_MAX_USEC (8 * PA_USEC_PER_SEC)

/** What is shown: State of the default sink. */
typedef struct {
    /** Whether connected to the sound server. */
    bool                      connected;
    /** Whether there is a default sink. */
    bool                      have_sink;
    /** Index of the sink. */
    uint32_t                  sink_index;
    /** Volume of the sink, per channel. */
    pa_cvolume                cvolume;
    /** Whether the sink is muted. */
    bool                      mute;
} sink_state_t;

/** State of the mixer. */
typedef struct {
    /** Wayland client. */
    wlmcl_client_t            *wlclient_ptr;
    /** The icon. */
    wlmcl_icon_t              *icon_ptr;
    /** Double buffer of the icon's surface. */
    wlmcl_dblbuf_t            *dblbuf_ptr;
    /** Listener for @ref wlmcl_client_events::pointer_button. */
    struct wl_listener        pointer_button_listener;
    /** Listener for @ref wlmcl_client_events::pointer_axis. */
    struct wl_listener        pointer_axis_listener;
    /** Listener for @ref wlmcl_client_events::pointer_motion. */
    struct wl_listener        pointer_motion_listener;
    /** Listener for @ref wlmcl_client_events::pointer_leave. */
    struct wl_listener        pointer_leave_listener;
    /** Size of the icon's surface, in surface coordinates. */
    uint32_t                  width;
    /** Size of the icon's surface, in surface coordinates. */
    uint32_t                  height;
    /** Whether the volume is being set by dragging along the meter. */
    bool                      dragging;
    /** The volume, in percent, last set by clicking or dragging. */
    int                       dragged_percent;
    /** Accumulated continuous scroll motion. */
    double                    axis_accumulator;
    /** The state as shown on the icon. Accessed from the Wayland thread. */
    sink_state_t              shown;

    /** Signals the Wayland thread that @ref mixer_t::state was updated. */
    int                       event_fd;

    /** The PulseAudio threaded mainloop. */
    pa_threaded_mainloop      *mainloop_ptr;
    /** Context. Accessed with the mainloop lock held. */
    pa_context                *context_ptr;
    /** Timer event for reconnecting. Accessed with the mainloop lock held. */
    pa_time_event             *reconnect_event_ptr;
    /** Current delay before reconnecting. */
    pa_usec_t                 reconnect_usec;
    /** State of the sink. Accessed with the mainloop lock held. */
    sink_state_t              state;

    /** Preferences. */
    wlm_settings_t            *settings_ptr;
    /** Colors. */
    const wlm_theme_t         *theme_ptr;
    /** Whether to show the volume in percent. */
    bool                      show_percent;
    /** Whether a left click (not on the meter) toggles mute. */
    bool                      click_mutes;
    /** Whether the mouse wheel changes the volume. */
    bool                      scroll_volume;
    /** Volume step, in percent. */
    unsigned                  step;
    /** Maximum volume to raise to, in percent. */
    unsigned                  max_volume;
    /** Command for a middle click. */
    const char                *mixer_command_ptr;
} mixer_t;

static void _context_connect(mixer_t *mixer_ptr);
static void _schedule_reconnect(mixer_t *mixer_ptr);
static void _notify(mixer_t *mixer_ptr);

/** Command to run on a middle click. */
static char *arg_mixer_command_ptr = NULL;
/** Volume step, in percent. */
static uint32_t arg_step = 5;
/** Maximum volume to step up to, in percent. */
static uint32_t arg_max_volume = 100;

/** Commandline arguments. */
static const bs_arg_t wlmmixer_args[] = {
    BS_ARG_STRING(
        "mixer_command",
        "Optional: Command to run on a middle click. It is run through "
        "`/bin/sh -c`. An empty string disables it. The preference "
        "'MixerCommand' overrides it.",
        "pavucontrol",
        &arg_mixer_command_ptr),
    BS_ARG_UINT32(
        "step",
        "Optional: Volume change per mouse wheel step, in percent.",
        5, 1, 50,
        &arg_step),
    BS_ARG_UINT32(
        "max_volume",
        "Optional: Maximum volume to raise to with the mouse wheel, in "
        "percent.",
        100, 10, 150,
        &arg_max_volume),
    bs_arg_log_level,
    BS_ARG_SENTINEL()
};

/** The app's own colors: A VFD-style display, as in the other DockApps. */
static const wlm_theme_t default_theme = {
    .name_ptr = "Default",
    .background = 0xff111111,
    .led = 0xff55ffff,
    .off = 0xff114444,
    .accent = 0xffff4040,
};

/** Segments for the percentage. */
static const wlm_cairo_7segment_param_t param_digits = {
    .offset = 0.8,
    .width = 1.2,
    .hlength = 5.0,
    .vlength = 5.0
};

/* == PulseAudio thread ==================================================== */

/* ------------------------------------------------------------------------- */
/** Callback for pa_context_get_sink_info_by_name: Stores the sink's state. */
static void _sink_info_callback(
    __UNUSED__ pa_context *context_ptr,
    const pa_sink_info *info_ptr,
    int eol,
    void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;
    if (0 > eol) {
        // No default sink.
        if (mixer_ptr->state.have_sink) {
            mixer_ptr->state.have_sink = false;
            _notify(mixer_ptr);
        }
        return;
    }
    if (NULL == info_ptr) return;  // End of list.

    mixer_ptr->state.have_sink = true;
    mixer_ptr->state.sink_index = info_ptr->index;
    mixer_ptr->state.cvolume = info_ptr->volume;
    mixer_ptr->state.mute = info_ptr->mute;
    _notify(mixer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Queries the state of the default sink. */
static void _query_default_sink(mixer_t *mixer_ptr)
{
    pa_operation *op_ptr = pa_context_get_sink_info_by_name(
        mixer_ptr->context_ptr, "@DEFAULT_SINK@",
        _sink_info_callback, mixer_ptr);
    if (NULL != op_ptr) pa_operation_unref(op_ptr);
}

/* ------------------------------------------------------------------------- */
/** Subscription callback: A sink or the server (default sink) changed. */
static void _subscribe_callback(
    __UNUSED__ pa_context *context_ptr,
    pa_subscription_event_type_t type,
    __UNUSED__ uint32_t index,
    void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;
    switch (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK:
    case PA_SUBSCRIPTION_EVENT_SERVER:
        _query_default_sink(mixer_ptr);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/** Timer event callback: Reconnects to the server. */
static void _reconnect_callback(
    pa_mainloop_api *api_ptr,
    pa_time_event *time_event_ptr,
    __UNUSED__ const struct timeval *tv_ptr,
    void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;
    api_ptr->time_free(time_event_ptr);
    mixer_ptr->reconnect_event_ptr = NULL;
    _context_connect(mixer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Schedules a reconnect, with exponential backoff. */
void _schedule_reconnect(mixer_t *mixer_ptr)
{
    if (NULL != mixer_ptr->reconnect_event_ptr) return;
    struct timeval tv;
    pa_timeval_add(pa_gettimeofday(&tv), mixer_ptr->reconnect_usec);
    pa_mainloop_api *api_ptr = pa_threaded_mainloop_get_api(
        mixer_ptr->mainloop_ptr);
    mixer_ptr->reconnect_event_ptr = api_ptr->time_new(
        api_ptr, &tv, _reconnect_callback, mixer_ptr);
    mixer_ptr->reconnect_usec = BS_MIN(
        2 * mixer_ptr->reconnect_usec, RECONNECT_MAX_USEC);
}

/* ------------------------------------------------------------------------- */
/** Context state callback: Subscribes when ready, reconnects on failure. */
static void _context_state_callback(pa_context *context_ptr, void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;

    switch (pa_context_get_state(context_ptr)) {
    case PA_CONTEXT_READY: {
        bs_log(BS_INFO, "Connected to sound server %s",
               pa_context_get_server(context_ptr));
        mixer_ptr->reconnect_usec = RECONNECT_MIN_USEC;
        mixer_ptr->state.connected = true;
        pa_context_set_subscribe_callback(
            context_ptr, _subscribe_callback, mixer_ptr);
        pa_operation *op_ptr = pa_context_subscribe(
            context_ptr,
            PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER,
            NULL, NULL);
        if (NULL != op_ptr) pa_operation_unref(op_ptr);
        _query_default_sink(mixer_ptr);
        _notify(mixer_ptr);
        break;
    }

    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        // Logs the loss of a connection, but not each failed retry.
        bs_log(mixer_ptr->state.connected ? BS_INFO : BS_DEBUG,
               "Disconnected from sound server: %s",
               pa_strerror(pa_context_errno(context_ptr)));
        mixer_ptr->state = (sink_state_t){};
        _notify(mixer_ptr);
        // Retry later, the context is replaced from the timer callback.
        _schedule_reconnect(mixer_ptr);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * (Re)creates the context and connects to the sound server. Must be called
 * from the mainloop thread, or with the mainloop lock held.
 *
 * @param mixer_ptr
 */
void _context_connect(mixer_t *mixer_ptr)
{
    if (NULL != mixer_ptr->context_ptr) {
        pa_context_set_state_callback(mixer_ptr->context_ptr, NULL, NULL);
        pa_context_set_subscribe_callback(mixer_ptr->context_ptr, NULL, NULL);
        pa_context_disconnect(mixer_ptr->context_ptr);
        pa_context_unref(mixer_ptr->context_ptr);
        mixer_ptr->context_ptr = NULL;
    }

    pa_proplist *proplist_ptr = pa_proplist_new();
    pa_proplist_sets(proplist_ptr, PA_PROP_APPLICATION_NAME, "wlmmixer");
    pa_proplist_sets(proplist_ptr, PA_PROP_APPLICATION_ID, "wlmaker.wlmmixer");
    pa_proplist_sets(proplist_ptr, PA_PROP_APPLICATION_ICON_NAME,
                     "multimedia-volume-control");
    mixer_ptr->context_ptr = pa_context_new_with_proplist(
        pa_threaded_mainloop_get_api(mixer_ptr->mainloop_ptr),
        "wlmmixer",
        proplist_ptr);
    pa_proplist_free(proplist_ptr);
    if (NULL == mixer_ptr->context_ptr) {
        bs_log(BS_ERROR, "Failed pa_context_new_with_proplist()");
        _schedule_reconnect(mixer_ptr);
        return;
    }
    pa_context_set_state_callback(
        mixer_ptr->context_ptr, _context_state_callback, mixer_ptr);

    // Don't auto-spawn a server: That's for the session to do. Should the
    // server be absent, the state callback will retry later.
    if (0 > pa_context_connect(mixer_ptr->context_ptr, NULL,
                               PA_CONTEXT_NOAUTOSPAWN, NULL)) {
        bs_log(BS_DEBUG, "Failed pa_context_connect(): %s",
               pa_strerror(pa_context_errno(mixer_ptr->context_ptr)));
        _schedule_reconnect(mixer_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Wakes up the Wayland thread, to show the updated state. */
void _notify(mixer_t *mixer_ptr)
{
    uint64_t one = 1;
    if (0 > write(mixer_ptr->event_fd, &one, sizeof(one)) && EAGAIN != errno) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed write(%d, ...)",
               mixer_ptr->event_fd);
    }
}

/* == Drawing ============================================================== */

/* ------------------------------------------------------------------------- */
/** Draws a speaker symbol, with sound waves or a cross for 'muted'. */
static void _draw_speaker(cairo_t *cairo_ptr, const sink_state_t *state_ptr,
                          const wlm_theme_t *t)
{
    bool active = state_ptr->connected && state_ptr->have_sink;
    uint32_t color = active ? t->led : t->off;
    if (active && state_ptr->mute) color = t->accent;

    cairo_set_source_argb8888(cairo_ptr, color);
    cairo_move_to(cairo_ptr, 4, 10);
    cairo_line_to(cairo_ptr, 7, 10);
    cairo_line_to(cairo_ptr, 11.5, 5.5);
    cairo_line_to(cairo_ptr, 11.5, 19.5);
    cairo_line_to(cairo_ptr, 7, 15);
    cairo_line_to(cairo_ptr, 4, 15);
    cairo_close_path(cairo_ptr);
    cairo_fill(cairo_ptr);

    cairo_set_line_width(cairo_ptr, 1.3);
    if (active && state_ptr->mute) {
        cairo_move_to(cairo_ptr, 14.5, 9.5);
        cairo_line_to(cairo_ptr, 20, 15.5);
        cairo_move_to(cairo_ptr, 20, 9.5);
        cairo_line_to(cairo_ptr, 14.5, 15.5);
        cairo_stroke(cairo_ptr);
        return;
    }

    // Sound waves; the outer one lights up above 50%.
    unsigned percent = wlm_volume_to_percent(pa_cvolume_max(&state_ptr->cvolume));
    for (int i = 0; i < 2; ++i) {
        bool lit = active && 0 < percent && (0 == i || 50 < percent);
        cairo_set_source_argb8888(cairo_ptr, lit ? t->led : t->off);
        cairo_new_sub_path(cairo_ptr);
        cairo_arc(cairo_ptr, 10.5, 12.5, 4.5 + 3.8 * i, -M_PI / 4, M_PI / 4);
        cairo_stroke(cairo_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Draws the volume in percent, or "--" if there is no sink. */
static void _draw_percent(cairo_t *cairo_ptr, const sink_state_t *state_ptr,
                          const wlm_theme_t *t)
{
    const uint32_t baseline = 18;
    const uint32_t x = 27;
    if (!state_ptr->connected || !state_ptr->have_sink) {
        for (int i = 0; i < 3; ++i) {
            wlm_cairo_7segment_display_segments(
                cairo_ptr, &param_digits, x + i * 8, baseline,
                t->led, t->off,
                0 < i ? WLM_CAIRO_7SEGMENT_MINUS : 0);
        }
        return;
    }

    unsigned percent = BS_MIN(
        wlm_volume_to_percent(pa_cvolume_max(&state_ptr->cvolume)), 999U);
    uint32_t color = state_ptr->mute ? t->accent : t->led;
    char digits[4];
    snprintf(digits, sizeof(digits), "%3u", percent);
    for (int i = 0; i < 3; ++i) {
        if (' ' == digits[i]) {
            wlm_cairo_7segment_display_segments(
                cairo_ptr, &param_digits, x + i * 8, baseline,
                color, t->off, 0);
        } else {
            wlm_cairo_7segment_display_digit(
                cairo_ptr, &param_digits, x + i * 8, baseline,
                color, t->off, digits[i] - '0');
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Color of the illuminated bar `i`: From green over yellow to red, with the
 * app's own colors. With a theme: Its LED color, the last ones accented.
 * Dimmed while muted.
 */
static uint32_t _bar_color(int i, bool mute, const wlm_theme_t *t)
{
    bool own = 0 == strcmp(t->name_ptr, default_theme.name_ptr);
    if (mute) return wlm_theme_blend(t->off, t->led, 80);
    if (!own) return i < BARS * 5 / 6 ? t->led : t->accent;
    if (i < BARS / 2) return 0xff40e040;
    if (i < BARS * 5 / 6) return 0xffe0e040;
    return 0xffff5030;
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the volume meter: Bars of increasing height, from 0% at the left to
 * 100% at the right. They are illuminated up to the volume, exactly: The
 * last one partly. Clicking on the meter sets the volume to that position.
 */
static void _draw_meter(cairo_t *cairo_ptr, const sink_state_t *state_ptr,
                        const wlm_theme_t *t)
{
    double fraction = 0;
    if (state_ptr->connected && state_ptr->have_sink) {
        fraction = wlm_volume_meter_fraction(
            pa_cvolume_max(&state_ptr->cvolume));
    }
    double lit_until = METER_LEFT + fraction * METER_LENGTH;

    for (int i = 0; i < BARS; ++i) {
        double height = 4.0 + (METER_MAX_HEIGHT - 4.0) * i / (BARS - 1);
        double x = METER_LEFT + i * BAR_PITCH;
        cairo_set_source_argb8888(cairo_ptr, t->off);
        cairo_rectangle(cairo_ptr, x, METER_BOTTOM - height, BAR_WIDTH, height);
        cairo_fill(cairo_ptr);

        double lit_width = BS_MIN(BAR_WIDTH, lit_until - x);
        if (0 >= lit_width) continue;
        cairo_set_source_argb8888(cairo_ptr, _bar_color(i, state_ptr->mute, t));
        cairo_rectangle(cairo_ptr, x, METER_BOTTOM - height, lit_width, height);
        cairo_fill(cairo_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Draws the icon. */
static bool _icon_callback(bs_gfxbuf_t *gfxbuf_ptr, void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;

    bs_gfxbuf_clear(gfxbuf_ptr, 0);
    cairo_t *cairo_ptr = cairo_create_from_bs_gfxbuf(gfxbuf_ptr);
    if (NULL == cairo_ptr) {
        bs_log(BS_ERROR, "Failed cairo_create_from_bs_gfxbuf(%p)", gfxbuf_ptr);
        return false;
    }
    cairo_scale(cairo_ptr,
                gfxbuf_ptr->width / ICON_SIZE,
                gfxbuf_ptr->height / ICON_SIZE);

    const wlm_theme_t *t = mixer_ptr->theme_ptr;
    cairo_set_source_argb8888(cairo_ptr, t->background);
    cairo_rectangle(cairo_ptr, 1, 1, ICON_SIZE - 2, ICON_SIZE - 2);
    cairo_fill(cairo_ptr);
    wlm_primitives_draw_bezel_at(
        cairo_ptr, 0, 0, ICON_SIZE, ICON_SIZE, 1.0, false);

    _draw_speaker(cairo_ptr, &mixer_ptr->shown, t);
    if (mixer_ptr->show_percent) {
        _draw_percent(cairo_ptr, &mixer_ptr->shown, t);
    }
    _draw_meter(cairo_ptr, &mixer_ptr->shown, t);

    cairo_destroy(cairo_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Requests a redraw of the icon. */
static void _redraw(mixer_t *mixer_ptr)
{
    if (NULL == mixer_ptr->dblbuf_ptr) return;
    wlmcl_dblbuf_register_ready_callback(
        mixer_ptr->dblbuf_ptr, _icon_callback, mixer_ptr);
}

/* == Wayland thread ======================================================= */

/* ------------------------------------------------------------------------- */
/** Called when @ref mixer_t::event_fd is readable: Shows the new state. */
static void _handle_event_fd(__UNUSED__ wlmcl_client_t *client_ptr, void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;
    uint64_t value;
    if (0 > read(mixer_ptr->event_fd, &value, sizeof(value)) &&
        EAGAIN != errno) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed read(%d, ...)",
               mixer_ptr->event_fd);
    }

    pa_threaded_mainloop_lock(mixer_ptr->mainloop_ptr);
    sink_state_t state = mixer_ptr->state;
    pa_threaded_mainloop_unlock(mixer_ptr->mainloop_ptr);

    if (0 != memcmp(&state, &mixer_ptr->shown, sizeof(state))) {
        mixer_ptr->shown = state;
        _redraw(mixer_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Changes the volume by `steps` steps. */
static void _change_volume(mixer_t *mixer_ptr, int steps)
{
    pa_threaded_mainloop_lock(mixer_ptr->mainloop_ptr);
    sink_state_t *state_ptr = &mixer_ptr->state;
    if (state_ptr->connected && state_ptr->have_sink &&
        pa_cvolume_valid(&state_ptr->cvolume)) {
        pa_volume_t volume = wlm_volume_step(
            pa_cvolume_max(&state_ptr->cvolume), steps,
            mixer_ptr->step, mixer_ptr->max_volume);
        // Scales all channels, keeping the balance.
        pa_cvolume_scale(&state_ptr->cvolume, volume);
        pa_operation *op_ptr = pa_context_set_sink_volume_by_index(
            mixer_ptr->context_ptr, state_ptr->sink_index,
            &state_ptr->cvolume, NULL, NULL);
        if (NULL != op_ptr) pa_operation_unref(op_ptr);
        _notify(mixer_ptr);
    }
    pa_threaded_mainloop_unlock(mixer_ptr->mainloop_ptr);
}

/* ------------------------------------------------------------------------- */
/** Sets the volume to `percent`, at most to `--max_volume`. */
static void _set_volume_percent(mixer_t *mixer_ptr, unsigned percent)
{
    percent = BS_MIN(percent, mixer_ptr->max_volume);
    pa_threaded_mainloop_lock(mixer_ptr->mainloop_ptr);
    sink_state_t *state_ptr = &mixer_ptr->state;
    if (state_ptr->connected && state_ptr->have_sink &&
        pa_cvolume_valid(&state_ptr->cvolume)) {
        pa_volume_t volume = wlm_volume_from_percent(percent);
        // Scales all channels, keeping the balance. Muted stays as is.
        if (0 == pa_cvolume_max(&state_ptr->cvolume)) {
            pa_cvolume_set(&state_ptr->cvolume, state_ptr->cvolume.channels,
                           volume);
        } else {
            pa_cvolume_scale(&state_ptr->cvolume, volume);
        }
        pa_operation *op_ptr = pa_context_set_sink_volume_by_index(
            mixer_ptr->context_ptr, state_ptr->sink_index,
            &state_ptr->cvolume, NULL, NULL);
        if (NULL != op_ptr) pa_operation_unref(op_ptr);
        _notify(mixer_ptr);
    }
    pa_threaded_mainloop_unlock(mixer_ptr->mainloop_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Converts a pointer position on the surface into icon coordinates, and
 * tells whether that is on the volume meter.
 *
 * @param mixer_ptr
 * @param x
 * @param y
 * @param icon_x_ptr          Set to the x position, in icon coordinates.
 *
 * @return true if on the meter.
 */
static bool _on_meter(mixer_t *mixer_ptr, double x, double y,
                      double *icon_x_ptr)
{
    if (0 == mixer_ptr->width || 0 == mixer_ptr->height) return false;
    double ix = x * ICON_SIZE / mixer_ptr->width;
    double iy = y * ICON_SIZE / mixer_ptr->height;
    *icon_x_ptr = ix;
    return (METER_LEFT - METER_MARGIN <= ix &&
            ix <= METER_LEFT + METER_LENGTH + METER_MARGIN &&
            METER_BOTTOM - METER_MAX_HEIGHT - METER_MARGIN <= iy &&
            iy <= METER_BOTTOM + METER_MARGIN);
}

/* ------------------------------------------------------------------------- */
/** Sets the volume for the pointer at icon position `icon_x`. */
static void _set_volume_at(mixer_t *mixer_ptr, double icon_x)
{
    int percent = wlm_volume_percent_at(icon_x, METER_LEFT, METER_LENGTH);
    if (percent == mixer_ptr->dragged_percent) return;
    mixer_ptr->dragged_percent = percent;
    _set_volume_percent(mixer_ptr, percent);
}

/* ------------------------------------------------------------------------- */
/** Toggles mute of the sink. */
static void _toggle_mute(mixer_t *mixer_ptr)
{
    pa_threaded_mainloop_lock(mixer_ptr->mainloop_ptr);
    sink_state_t *state_ptr = &mixer_ptr->state;
    if (state_ptr->connected && state_ptr->have_sink) {
        state_ptr->mute = !state_ptr->mute;
        pa_operation *op_ptr = pa_context_set_sink_mute_by_index(
            mixer_ptr->context_ptr, state_ptr->sink_index,
            state_ptr->mute, NULL, NULL);
        if (NULL != op_ptr) pa_operation_unref(op_ptr);
        _notify(mixer_ptr);
    }
    pa_threaded_mainloop_unlock(mixer_ptr->mainloop_ptr);
}

/* ------------------------------------------------------------------------- */
/** Applies the preferences. */
static void _apply_prefs(mixer_t *mixer_ptr)
{
    wlm_settings_t *s = mixer_ptr->settings_ptr;
    mixer_ptr->theme_ptr = wlm_settings_theme(s, &default_theme);
    mixer_ptr->show_percent = wlm_settings_bool(s, "ShowPercent", true);
    mixer_ptr->click_mutes = wlm_settings_bool(s, "ClickMutes", true);
    mixer_ptr->scroll_volume = wlm_settings_bool(s, "ScrollVolume", true);
    mixer_ptr->step = (unsigned)wlm_settings_number(s, "Step", arg_step, 1, 50);
    mixer_ptr->max_volume = (unsigned)wlm_settings_number(
        s, "MaxVolume", arg_max_volume, 10, 150);
    mixer_ptr->mixer_command_ptr = wlm_settings_string(
        s, "MixerCommand",
        NULL != arg_mixer_command_ptr ? arg_mixer_command_ptr : "");
}

/* ------------------------------------------------------------------------- */
/** The preferences file changed: Applies it, and redraws. */
static void _handle_settings_fd(__UNUSED__ wlmcl_client_t *client_ptr,
                                void *ud_ptr)
{
    mixer_t *mixer_ptr = ud_ptr;
    if (!wlm_settings_handle_fd(mixer_ptr->settings_ptr)) return;
    _apply_prefs(mixer_ptr);
    _redraw(mixer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles pointer buttons on the icon. */
static void _handle_pointer_button(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    mixer_t *mixer_ptr = BS_CONTAINER_OF(
        listener_ptr, mixer_t, pointer_button_listener);
    struct wlmcl_client_pointer_button_event *event_ptr = data_ptr;
    if (BTN_LEFT == event_ptr->button && !event_ptr->pressed) {
        mixer_ptr->dragging = false;
        return;
    }
    if (event_ptr->wl_surface_ptr != wlmcl_icon_wl_surface(mixer_ptr->icon_ptr) ||
        !event_ptr->pressed) return;

    double icon_x;
    switch (event_ptr->button) {
    case BTN_LEFT:
        // On the meter: Sets the volume there, also while dragging.
        // Elsewhere: Toggles mute.
        if (_on_meter(mixer_ptr, event_ptr->x, event_ptr->y, &icon_x)) {
            mixer_ptr->dragging = true;
            mixer_ptr->dragged_percent = -1;
            _set_volume_at(mixer_ptr, icon_x);
        } else if (mixer_ptr->click_mutes) {
            _toggle_mute(mixer_ptr);
        }
        break;
    case BTN_RIGHT:
        // Not seen within wlmaker: It opens the dock-app's menu.
    case BTN_MIDDLE:
        wlm_settings_spawn(mixer_ptr->mixer_command_ptr);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/** Handles pointer motion: While dragging, sets the volume. */
static void _handle_pointer_motion(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    mixer_t *mixer_ptr = BS_CONTAINER_OF(
        listener_ptr, mixer_t, pointer_motion_listener);
    struct wlmcl_client_pointer_motion_event *event_ptr = data_ptr;
    if (!mixer_ptr->dragging ||
        event_ptr->wl_surface_ptr != wlmcl_icon_wl_surface(mixer_ptr->icon_ptr)) {
        return;
    }
    double icon_x;
    _on_meter(mixer_ptr, event_ptr->x, event_ptr->y, &icon_x);
    _set_volume_at(mixer_ptr, icon_x);
}

/* ------------------------------------------------------------------------- */
/** Handles the pointer leaving: Ends dragging. */
static void _handle_pointer_leave(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    mixer_t *mixer_ptr = BS_CONTAINER_OF(
        listener_ptr, mixer_t, pointer_leave_listener);
    mixer_ptr->dragging = false;
}

/* ------------------------------------------------------------------------- */
/** Handles the scroll wheel on the icon. */
static void _handle_pointer_axis(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    mixer_t *mixer_ptr = BS_CONTAINER_OF(
        listener_ptr, mixer_t, pointer_axis_listener);
    struct wlmcl_client_pointer_axis_event *event_ptr = data_ptr;
    if (event_ptr->wl_surface_ptr != wlmcl_icon_wl_surface(mixer_ptr->icon_ptr) ||
        WL_POINTER_AXIS_VERTICAL_SCROLL != event_ptr->axis ||
        !mixer_ptr->scroll_volume) return;

    int steps = wlm_volume_axis_steps(
        &mixer_ptr->axis_accumulator, event_ptr->discrete, event_ptr->value);
    if (0 != steps) _change_volume(mixer_ptr, steps);
}

/* ------------------------------------------------------------------------- */
/** Handles configure events: (Re)creates the buffer and draws. */
static void _handle_configure(void *ud_ptr, uint32_t width, uint32_t height)
{
    mixer_t *mixer_ptr = ud_ptr;
    mixer_ptr->width = width;
    mixer_ptr->height = height;
    if (NULL != mixer_ptr->dblbuf_ptr) {
        wlmcl_dblbuf_destroy(mixer_ptr->dblbuf_ptr);
    }
    mixer_ptr->dblbuf_ptr = wlmcl_dblbuf_create(
        wlmcl_client_attributes(mixer_ptr->wlclient_ptr)->app_id_ptr,
        wlmcl_icon_wl_surface(mixer_ptr->icon_ptr),
        wlmcl_client_attributes(mixer_ptr->wlclient_ptr)->wl_shm_ptr,
        width,
        height);
    if (NULL == mixer_ptr->dblbuf_ptr) {
        bs_log(BS_FATAL, "Failed wlmcl_dblbuf_create.");
        return;
    }
    _redraw(mixer_ptr);
}

/* ------------------------------------------------------------------------- */
/** Runs the mixer: Sets up the icon and the sound server connection. */
static int _run(mixer_t *mixer_ptr)
{
    // TODO(kaeser@gubbe.ch): Move icon_protocol to @ref wlmcl_client_register,
    // and permit clients to be registerd as 'required'.
    wl_display_roundtrip(
        wlmcl_client_attributes(mixer_ptr->wlclient_ptr)->wl_display_ptr);
    wl_display_roundtrip(
        wlmcl_client_attributes(mixer_ptr->wlclient_ptr)->wl_display_ptr);
    if (!wlmcl_icon_supported(mixer_ptr->wlclient_ptr)) {
        bs_log(BS_ERROR, "icon protocol is not supported.");
        return EXIT_FAILURE;
    }
    mixer_ptr->icon_ptr = wlmcl_icon_create(mixer_ptr->wlclient_ptr);
    if (NULL == mixer_ptr->icon_ptr) {
        bs_log(BS_ERROR, "Failed wlmcl_icon_create(%p)", mixer_ptr->wlclient_ptr);
        return EXIT_FAILURE;
    }
    wlmcl_icon_register_configure_callback(
        mixer_ptr->icon_ptr, _handle_configure, mixer_ptr);

    struct wlmcl_client_events *events_ptr = wlmcl_client_events(
        mixer_ptr->wlclient_ptr);
    mixer_ptr->pointer_button_listener.notify = _handle_pointer_button;
    wl_signal_add(&events_ptr->pointer_button,
                  &mixer_ptr->pointer_button_listener);
    mixer_ptr->pointer_axis_listener.notify = _handle_pointer_axis;
    wl_signal_add(&events_ptr->pointer_axis,
                  &mixer_ptr->pointer_axis_listener);
    mixer_ptr->pointer_motion_listener.notify = _handle_pointer_motion;
    wl_signal_add(&events_ptr->pointer_motion,
                  &mixer_ptr->pointer_motion_listener);
    mixer_ptr->pointer_leave_listener.notify = _handle_pointer_leave;
    wl_signal_add(&events_ptr->pointer_leave,
                  &mixer_ptr->pointer_leave_listener);

    const int settings_fd = wlm_settings_watch(mixer_ptr->settings_ptr);
    if (0 <= settings_fd) {
        wlmcl_client_register_fd(mixer_ptr->wlclient_ptr, settings_fd,
                                 _handle_settings_fd, mixer_ptr);
    }

    int rv = EXIT_FAILURE;
    if (wlmcl_client_register_fd(mixer_ptr->wlclient_ptr, mixer_ptr->event_fd,
                                 _handle_event_fd, mixer_ptr)) {
        // Created after the Wayland client: The thread inherits the signal
        // mask, and leaves SIGINT to the client's signalfd.
        mixer_ptr->mainloop_ptr = pa_threaded_mainloop_new();
        if (NULL != mixer_ptr->mainloop_ptr) {
            pa_threaded_mainloop_set_name(mixer_ptr->mainloop_ptr, "wlmmixer-pa");
            pa_threaded_mainloop_lock(mixer_ptr->mainloop_ptr);
            mixer_ptr->reconnect_usec = RECONNECT_MIN_USEC;
            _context_connect(mixer_ptr);
            if (0 == pa_threaded_mainloop_start(mixer_ptr->mainloop_ptr)) {
                pa_threaded_mainloop_unlock(mixer_ptr->mainloop_ptr);

                wlmcl_client_run(mixer_ptr->wlclient_ptr);
                rv = EXIT_SUCCESS;

                pa_threaded_mainloop_lock(mixer_ptr->mainloop_ptr);
            } else {
                bs_log(BS_ERROR, "Failed pa_threaded_mainloop_start()");
            }
            if (NULL != mixer_ptr->reconnect_event_ptr) {
                pa_threaded_mainloop_get_api(mixer_ptr->mainloop_ptr)->time_free(
                    mixer_ptr->reconnect_event_ptr);
                mixer_ptr->reconnect_event_ptr = NULL;
            }
            if (NULL != mixer_ptr->context_ptr) {
                pa_context_set_state_callback(mixer_ptr->context_ptr, NULL, NULL);
                pa_context_disconnect(mixer_ptr->context_ptr);
                pa_context_unref(mixer_ptr->context_ptr);
                mixer_ptr->context_ptr = NULL;
            }
            pa_threaded_mainloop_unlock(mixer_ptr->mainloop_ptr);
            pa_threaded_mainloop_stop(mixer_ptr->mainloop_ptr);
            pa_threaded_mainloop_free(mixer_ptr->mainloop_ptr);
            mixer_ptr->mainloop_ptr = NULL;
        } else {
            bs_log(BS_ERROR, "Failed pa_threaded_mainloop_new()");
        }
        wlmcl_client_unregister_fd(mixer_ptr->wlclient_ptr, mixer_ptr->event_fd);
    }

    if (0 <= settings_fd) {
        wlmcl_client_unregister_fd(mixer_ptr->wlclient_ptr, settings_fd);
    }
    wl_list_remove(&mixer_ptr->pointer_leave_listener.link);
    wl_list_remove(&mixer_ptr->pointer_motion_listener.link);
    wl_list_remove(&mixer_ptr->pointer_axis_listener.link);
    wl_list_remove(&mixer_ptr->pointer_button_listener.link);
    if (NULL != mixer_ptr->dblbuf_ptr) {
        wlmcl_dblbuf_destroy(mixer_ptr->dblbuf_ptr);
        mixer_ptr->dblbuf_ptr = NULL;
    }
    wlmcl_icon_destroy(mixer_ptr->icon_ptr);
    mixer_ptr->icon_ptr = NULL;
    return rv;
}

/* == Main program ========================================================= */
/** Main program. */
int main(int argc, const char **argv)
{
    if (!bs_arg_parse(wlmmixer_args, BS_ARG_MODE_EXTRA_ARGS, &argc, argv)) {
        fprintf(stderr, "Failed to parse commandline arguments.\n");
        bs_arg_print_usage(stderr, wlmmixer_args);
        return EXIT_FAILURE;
    }
    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--help")) {
            bs_arg_print_usage(stdout, wlmmixer_args);
            bs_arg_cleanup(wlmmixer_args);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "Unknown argument \"%s\".\n", argv[i]);
        bs_arg_print_usage(stderr, wlmmixer_args);
        bs_arg_cleanup(wlmmixer_args);
        return EXIT_FAILURE;
    }

    // The mixer command's processes are reaped automatically. SIGPIPE is
    // ignored, for writes to a sound server that went away.
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    mixer_t mixer = { .event_fd = -1 };
    int rv = EXIT_FAILURE;
    mixer.settings_ptr = wlm_settings_create("wlmmixer", NULL);
    if (NULL == mixer.settings_ptr) {
        bs_arg_cleanup(wlmmixer_args);
        return EXIT_FAILURE;
    }
    _apply_prefs(&mixer);
    mixer.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (0 > mixer.event_fd) {
        bs_log(BS_ERROR | BS_ERRNO, "Failed eventfd()");
    } else {
        mixer.wlclient_ptr = wlmcl_client_create("wlmaker.wlmmixer");
        if (NULL != mixer.wlclient_ptr) {
            rv = _run(&mixer);
            wlmcl_client_destroy(mixer.wlclient_ptr);
        }
        close(mixer.event_fd);
    }

    wlm_settings_destroy(mixer.settings_ptr);
    bs_arg_cleanup(wlmmixer_args);
    return rv;
}
/* == End of wlmmixer.c ==================================================== */
