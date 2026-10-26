/* ========================================================================= */
/**
 * @file wlmmixer.c
 *
 * Volume mixer DockApp: Shows and controls the volume of the default audio
 * output ("sink"). Similar in spirit to Window Maker's `wmix`.
 *
 * Talks to the sound server through the PulseAudio client library, which
 * works with PulseAudio and with PipeWire (through `pipewire-pulse`). Changes
 * are received as events, there is no polling. The mouse wheel changes the
 * volume, a left click toggles mute, and a right or middle click starts a
 * full mixer application (`pavucontrol`, by default).
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
#include <spawn.h>
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

#include "wlm_volume.h"

/* == Declarations ========================================================= */

/** Logical size of the icon. Drawing is scaled to the actual size. */
#define ICON_SIZE 56.0

/** Number of bars in the volume meter. */
#define BARS 12

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
} mixer_t;

static void _context_connect(mixer_t *mixer_ptr);
static void _schedule_reconnect(mixer_t *mixer_ptr);
static void _notify(mixer_t *mixer_ptr);

/** Command to run on right or middle click. */
static char *arg_mixer_command_ptr = NULL;
/** Volume step, in percent. */
static uint32_t arg_step = 5;
/** Maximum volume to step up to, in percent. */
static uint32_t arg_max_volume = 100;

/** Commandline arguments. */
static const bs_arg_t wlmmixer_args[] = {
    BS_ARG_STRING(
        "mixer_command",
        "Optional: Command to run on a right or middle click. It is run "
        "through `/bin/sh -c`. An empty string disables it.",
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

/** Illuminated LED, as in the other DockApps. */
static const uint32_t color_led = 0xff55ffff;
/** Color of a turned-off element in the VFD-style display. */
static const uint32_t color_off = 0xff114444;
/** Background color in the VFD-style display. */
static const uint32_t color_background = 0xff111111;
/** Color for 'muted'. */
static const uint32_t color_mute = 0xffff4040;
/** Color of illuminated meter bars while muted. */
static const uint32_t color_bar_muted = 0xff3c6464;

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
static void _draw_speaker(cairo_t *cairo_ptr, const sink_state_t *state_ptr)
{
    bool active = state_ptr->connected && state_ptr->have_sink;
    uint32_t color = active ? color_led : color_off;
    if (active && state_ptr->mute) color = color_mute;

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
        cairo_set_source_argb8888(cairo_ptr, lit ? color_led : color_off);
        cairo_new_sub_path(cairo_ptr);
        cairo_arc(cairo_ptr, 10.5, 12.5, 4.5 + 3.8 * i, -M_PI / 4, M_PI / 4);
        cairo_stroke(cairo_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Draws the volume in percent, or "--" if there is no sink. */
static void _draw_percent(cairo_t *cairo_ptr, const sink_state_t *state_ptr)
{
    const uint32_t baseline = 18;
    const uint32_t x = 27;
    if (!state_ptr->connected || !state_ptr->have_sink) {
        for (int i = 0; i < 3; ++i) {
            wlm_cairo_7segment_display_segments(
                cairo_ptr, &param_digits, x + i * 8, baseline,
                color_led, color_off,
                0 < i ? WLM_CAIRO_7SEGMENT_MINUS : 0);
        }
        return;
    }

    unsigned percent = BS_MIN(
        wlm_volume_to_percent(pa_cvolume_max(&state_ptr->cvolume)), 999U);
    uint32_t color = state_ptr->mute ? color_mute : color_led;
    char digits[4];
    snprintf(digits, sizeof(digits), "%3u", percent);
    for (int i = 0; i < 3; ++i) {
        if (' ' == digits[i]) {
            wlm_cairo_7segment_display_segments(
                cairo_ptr, &param_digits, x + i * 8, baseline,
                color, color_off, 0);
        } else {
            wlm_cairo_7segment_display_digit(
                cairo_ptr, &param_digits, x + i * 8, baseline,
                color, color_off, digits[i] - '0');
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the volume meter: Bars of increasing height, illuminated up to the
 * volume. Colors go from green over yellow to red.
 */
static void _draw_meter(cairo_t *cairo_ptr, const sink_state_t *state_ptr)
{
    const double left = 5.0, bottom = 50.0, max_height = 22.0;
    const double bar_width = 2.6, bar_pitch = 3.85;

    double ratio = 0;
    if (state_ptr->connected && state_ptr->have_sink) {
        ratio = (double)pa_cvolume_max(&state_ptr->cvolume) / PA_VOLUME_NORM;
    }
    // Each bar stands for 1/BARS of 100%. Bars are lit if at least half-way.
    int lit = (int)floor(ratio * BARS + 0.5);

    for (int i = 0; i < BARS; ++i) {
        double height = 4.0 + (max_height - 4.0) * i / (BARS - 1);
        uint32_t color = color_off;
        if (i < lit) {
            if (state_ptr->mute) {
                color = color_bar_muted;
            } else if (i < BARS / 2) {
                color = 0xff40e040;
            } else if (i < BARS * 5 / 6) {
                color = 0xffe0e040;
            } else {
                color = 0xffff5030;
            }
        }
        cairo_set_source_argb8888(cairo_ptr, color);
        cairo_rectangle(cairo_ptr, left + i * bar_pitch, bottom - height,
                        bar_width, height);
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

    cairo_set_source_argb8888(cairo_ptr, color_background);
    cairo_rectangle(cairo_ptr, 1, 1, ICON_SIZE - 2, ICON_SIZE - 2);
    cairo_fill(cairo_ptr);
    wlm_primitives_draw_bezel_at(
        cairo_ptr, 0, 0, ICON_SIZE, ICON_SIZE, 1.0, false);

    _draw_speaker(cairo_ptr, &mixer_ptr->shown);
    _draw_percent(cairo_ptr, &mixer_ptr->shown);
    _draw_meter(cairo_ptr, &mixer_ptr->shown);

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
            arg_step, arg_max_volume);
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
/** Starts the mixer command, in its own process group, default signals. */
static void _start_mixer_command(void)
{
    if (NULL == arg_mixer_command_ptr || '\0' == *arg_mixer_command_ptr) return;

    posix_spawnattr_t attr;
    if (0 != posix_spawnattr_init(&attr)) return;
    sigset_t sigset;
    sigemptyset(&sigset);
    posix_spawnattr_setsigmask(&attr, &sigset);
    sigaddset(&sigset, SIGCHLD);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &sigset);
    // A process group of its own: Keeps it running when we are stopped.
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(
        &attr,
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

    char *const argv[] = {
        "/bin/sh", "-c", arg_mixer_command_ptr, NULL };
    extern char **environ;
    pid_t pid;
    int rv = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
    if (0 != rv) {
        bs_log(BS_WARNING, "Failed to start \"%s\": %s",
               arg_mixer_command_ptr, strerror(rv));
    } else {
        bs_log(BS_INFO, "Started \"%s\", pid %d", arg_mixer_command_ptr, pid);
    }
    posix_spawnattr_destroy(&attr);
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
    if (event_ptr->wl_surface_ptr != wlmcl_icon_wl_surface(mixer_ptr->icon_ptr) ||
        !event_ptr->pressed) return;

    switch (event_ptr->button) {
    case BTN_LEFT:
        _toggle_mute(mixer_ptr);
        break;
    case BTN_RIGHT:
    case BTN_MIDDLE:
        _start_mixer_command();
        break;
    default:
        break;
    }
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
        WL_POINTER_AXIS_VERTICAL_SCROLL != event_ptr->axis) return;

    int steps = wlm_volume_axis_steps(
        &mixer_ptr->axis_accumulator, event_ptr->discrete, event_ptr->value);
    if (0 != steps) _change_volume(mixer_ptr, steps);
}

/* ------------------------------------------------------------------------- */
/** Handles configure events: (Re)creates the buffer and draws. */
static void _handle_configure(void *ud_ptr, uint32_t width, uint32_t height)
{
    mixer_t *mixer_ptr = ud_ptr;
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

    bs_arg_cleanup(wlmmixer_args);
    return rv;
}
/* == End of wlmmixer.c ==================================================== */
