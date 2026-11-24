/* ========================================================================= */
/**
 * @file locate_pointer.c
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
 *
 * Locating the pointer by shaking the mouse: The pointer itself grows for a
 * moment, smoothly, and goes back. It is the cursor's own image, magnified
 * by the cursor (see @ref wlmim_set_cursor_magnification): It moves with
 * the pointer as always, keeps its hotspot, and follows image changes. The
 * shake is told from the cursor's motion, without any polling: Direction
 * reversals of long, quick strokes, as the X11 session's
 * big-wm-locate-pointer does.
 */

#include "locate_pointer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#undef WLR_USE_UNSTABLE

#include "input/manager.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** Decoded 'LocatePointer' configuration. */
typedef struct {
    /** Path of the `key=value` settings file. May be NULL. */
    char                      *settings_file_ptr;
    /** Defaults, overridden by the settings file. */
    bool                      enabled;
    /** See @ref wlmaker_locate_pointer_settings_t::sensitivity. */
    int                       sensitivity;
    /** See @ref wlmaker_locate_pointer_settings_t::max_scale. */
    double                    max_scale;
    /** See @ref wlmaker_locate_pointer_settings_t::grow_msec. */
    int64_t                   grow_msec;
    /** See @ref wlmaker_locate_pointer_settings_t::duration_msec. */
    int64_t                   duration_msec;
    /** See @ref wlmaker_locate_pointer_settings_t::return_msec. */
    int64_t                   return_msec;
} _wlmaker_locate_pointer_config_t;

/** State of locating the pointer. */
struct _wlmaker_locate_pointer_t {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** The configuration. */
    _wlmaker_locate_pointer_config_t config;
    /** Settings in effect: Configuration and settings file. */
    wlmaker_locate_pointer_settings_t settings;

    /** inotify file descriptor, watching the settings' directory. */
    int                       inotify_fd;
    /** Event source for @ref _wlmaker_locate_pointer_t::inotify_fd. */
    struct wl_event_source    *inotify_source_ptr;
    /** Base name of the settings file, within the watched directory. */
    char                      *settings_basename_ptr;

    /** The detector. */
    wlmaker_shake_detector_t  detector;
    /** Height of the output the detector was set up for. */
    int                       detector_output_height;

    /** Listener for `cursor_position_updated`. */
    struct wl_listener        cursor_position_updated_listener;

    /** Animation timer. */
    struct wl_event_source    *timer_source_ptr;
    /** Whether the pointer is magnified. */
    bool                      animating;
    /** Start of the animation, in ms. */
    long                      start;
    /** The pointer goes back from here on, in ms. */
    long                      hold_until;
};

static bool _wlmaker_locate_pointer_configure(
    wlmaker_locate_pointer_t *lp_ptr,
    bspl_dict_t *config_dict_ptr);
static void _wlmaker_locate_pointer_load_settings(
    wlmaker_locate_pointer_t *lp_ptr);
static void _wlmaker_locate_pointer_watch(wlmaker_locate_pointer_t *lp_ptr);
static void _wlmaker_locate_pointer_unwatch(wlmaker_locate_pointer_t *lp_ptr);
static int _wlmaker_locate_pointer_handle_inotify(
    int fd, uint32_t mask, void *data_ptr);
static void _wlmaker_locate_pointer_handle_cursor_position_updated(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_locate_pointer_start(wlmaker_locate_pointer_t *lp_ptr);
static void _wlmaker_locate_pointer_stop(wlmaker_locate_pointer_t *lp_ptr);
static int _wlmaker_locate_pointer_handle_timer(void *data_ptr);
static long _wlmaker_locate_pointer_now(void);
static char *_wlmaker_locate_pointer_expand(const char *path_ptr);

/* == Data ================================================================= */

/** Sensitivities. */
static const bspl_enum_desc_t _wlmaker_locate_pointer_sensitivity_desc[] = {
    BSPL_ENUM("Low", 0),
    BSPL_ENUM("Medium", 1),
    BSPL_ENUM("High", 2),
    BSPL_ENUM_SENTINEL()
};

/** Descriptor of the 'LocatePointer' dictionary. */
static const bspl_desc_t _wlmaker_locate_pointer_config_desc[] = {
    BSPL_DESC_STRING("SettingsFile", false, _wlmaker_locate_pointer_config_t,
                     settings_file_ptr, settings_file_ptr, ""),
    BSPL_DESC_BOOL("Enabled", false, _wlmaker_locate_pointer_config_t,
                   enabled, enabled, false),
    BSPL_DESC_ENUM("Sensitivity", false, _wlmaker_locate_pointer_config_t,
                   sensitivity, sensitivity, 1,
                   _wlmaker_locate_pointer_sensitivity_desc),
    BSPL_DESC_DOUBLE("MaxScale", false, _wlmaker_locate_pointer_config_t,
                     max_scale, max_scale, 3.0),
    BSPL_DESC_INT64("GrowMsec", false, _wlmaker_locate_pointer_config_t,
                    grow_msec, grow_msec, 160),
    BSPL_DESC_INT64("DurationMsec", false, _wlmaker_locate_pointer_config_t,
                    duration_msec, duration_msec, 700),
    BSPL_DESC_INT64("ReturnMsec", false, _wlmaker_locate_pointer_config_t,
                    return_msec, return_msec, 260),
    BSPL_DESC_SENTINEL()
};

/** Animation frame interval, in ms. */
static const int _wlmaker_locate_pointer_frame_msec = 16;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_locate_pointer_t *wlmaker_locate_pointer_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr)
{
    wlmaker_locate_pointer_t *lp_ptr = logged_calloc(1, sizeof(*lp_ptr));
    if (NULL == lp_ptr) return NULL;
    lp_ptr->server_ptr = server_ptr;
    lp_ptr->inotify_fd = -1;

    if (!_wlmaker_locate_pointer_configure(lp_ptr, config_dict_ptr)) {
        wlmaker_locate_pointer_destroy(lp_ptr);
        return NULL;
    }

    if (NULL != server_ptr->input_manager_ptr) {
        wlmtk_util_connect_listener_signal(
            &wlmim_events(server_ptr->input_manager_ptr)->cursor_position_updated,
            &lp_ptr->cursor_position_updated_listener,
            _wlmaker_locate_pointer_handle_cursor_position_updated);
    }
    if (NULL != server_ptr->wl_display_ptr) {
        lp_ptr->timer_source_ptr = wl_event_loop_add_timer(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            _wlmaker_locate_pointer_handle_timer,
            lp_ptr);
        if (NULL == lp_ptr->timer_source_ptr) {
            bs_log(BS_ERROR, "Failed wl_event_loop_add_timer()");
            wlmaker_locate_pointer_destroy(lp_ptr);
            return NULL;
        }
    }
    return lp_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_locate_pointer_destroy(wlmaker_locate_pointer_t *lp_ptr)
{
    if (lp_ptr->animating) _wlmaker_locate_pointer_stop(lp_ptr);
    if (NULL != lp_ptr->timer_source_ptr) {
        wl_event_source_remove(lp_ptr->timer_source_ptr);
        lp_ptr->timer_source_ptr = NULL;
    }
    wlmtk_util_disconnect_listener(&lp_ptr->cursor_position_updated_listener);
    _wlmaker_locate_pointer_unwatch(lp_ptr);
    bspl_decoded_destroy(_wlmaker_locate_pointer_config_desc, &lp_ptr->config);
    free(lp_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_locate_pointer_reconfigure(
    wlmaker_locate_pointer_t *lp_ptr,
    bspl_dict_t *config_dict_ptr)
{
    return _wlmaker_locate_pointer_configure(lp_ptr, config_dict_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_locate_pointer_show(wlmaker_locate_pointer_t *lp_ptr)
{
    _wlmaker_locate_pointer_start(lp_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_locate_pointer_parse_settings(
    const char *text_ptr,
    wlmaker_locate_pointer_settings_t *s_ptr)
{
    char line[256];
    while (NULL != text_ptr && '\0' != *text_ptr) {
        size_t n = strcspn(text_ptr, "\n");
        snprintf(line, sizeof(line), "%.*s",
                 (int)BS_MIN(n, sizeof(line) - 1), text_ptr);
        text_ptr += n;
        if ('\n' == *text_ptr) ++text_ptr;

        char *k = line;
        while (' ' == *k || '\t' == *k) ++k;
        if ('#' == *k || '\0' == *k) continue;
        char *eq = strchr(k, '=');
        if (NULL == eq) continue;
        *eq = '\0';
        char *v = eq + 1;
        for (char *e = eq; e > k && (' ' == e[-1] || '\t' == e[-1]); *--e = '\0') {}
        while (' ' == *v || '\t' == *v) ++v;
        for (char *e = v + strlen(v);
             e > v && (' ' == e[-1] || '\t' == e[-1] || '\r' == e[-1]);
             *--e = '\0') {}

        if (0 == strcmp(k, "enabled")) {
            s_ptr->enabled = !(0 == strcmp(v, "false") ||
                               0 == strcmp(v, "no") || 0 == strcmp(v, "0"));
        } else if (0 == strcmp(k, "sensitivity")) {
            s_ptr->sensitivity = 0 == strcmp(v, "low") ? 0 :
                0 == strcmp(v, "high") ? 2 : 1;
        } else if (0 == strcmp(k, "max-scale")) {
            double m = strtod(v, NULL);
            if (1.5 <= m && m <= 8.0) s_ptr->max_scale = m;
        } else if (0 == strcmp(k, "grow-ms")) {
            long m = strtol(v, NULL, 10);
            if (0 <= m && m <= 2000) s_ptr->grow_msec = m;
        } else if (0 == strcmp(k, "duration-ms")) {
            long m = strtol(v, NULL, 10);
            if (200 <= m && m <= 10000) s_ptr->duration_msec = m;
        } else if (0 == strcmp(k, "return-ms")) {
            long m = strtol(v, NULL, 10);
            if (0 <= m && m <= 5000) s_ptr->return_msec = m;
        }
    }
}

/* ------------------------------------------------------------------------- */
void wlmaker_shake_detector_reset(wlmaker_shake_detector_t *d_ptr)
{
    memset(d_ptr->ax, 0, sizeof(d_ptr->ax));
    d_ptr->have_last = false;
}

/* ------------------------------------------------------------------------- */
void wlmaker_shake_detector_setup(
    wlmaker_shake_detector_t *d_ptr,
    int sensitivity,
    int output_height)
{
    // Stroke lengths at 1080 lines, and reversals needed: Grows with the
    // output's height (in logical pixels, as the pointer moves).
    static const int base[3] = { 150, 110, 80 };
    static const int need[3] = { 5, 4, 3 };
    if (sensitivity < 0 || sensitivity > 2) sensitivity = 1;
    double k = 0 < output_height ? output_height / 1080.0 : 1.0;
    if (k < 0.75) k = 0.75;
    d_ptr->min_seg = (int)(base[sensitivity] * k);
    d_ptr->need = need[sensitivity];
    d_ptr->max_seg_ms = 260;
    d_ptr->window_ms = 1100;
    d_ptr->idle_ms = 160;
    wlmaker_shake_detector_reset(d_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_shake_detector_sample(
    wlmaker_shake_detector_t *d,
    int x,
    int y,
    long t,
    bool button)
{
    if (button) {
        wlmaker_shake_detector_reset(d);
        return false;
    }
    if (!d->have_last || t - d->last_t > d->idle_ms) {
        memset(d->ax, 0, sizeof(d->ax));
        d->last[0] = x;
        d->last[1] = y;
        d->last_t = t;
        d->have_last = true;
        return false;
    }
    int pos[2] = { x, y };
    bool hit = false;
    for (int i = 0; i < 2; ++i) {
        int delta = pos[i] - d->last[i];
        if (0 == delta) continue;
        int s = delta > 0 ? 1 : -1;
        int mag = delta > 0 ? delta : -delta;
        if (0 == d->ax[i].dir) {
            d->ax[i].dir = s;
            d->ax[i].seg = mag;
            d->ax[i].seg_start = d->last_t;
        } else if (s == d->ax[i].dir) {
            d->ax[i].seg += mag;
        } else {
            bool good = (d->ax[i].seg >= d->min_seg &&
                         d->last_t - d->ax[i].seg_start <= d->max_seg_ms);
            if (good) {
                if (0 == d->ax[i].reversals ||
                    t - d->ax[i].first_rev > d->window_ms) {
                    d->ax[i].reversals = 0;
                    d->ax[i].first_rev = t;
                }
                d->ax[i].reversals++;
            } else {
                d->ax[i].reversals = 0;
            }
            d->ax[i].dir = s;
            d->ax[i].seg = mag;
            d->ax[i].seg_start = d->last_t;
            if (d->ax[i].reversals >= d->need &&
                t - d->ax[i].first_rev <= d->window_ms) hit = true;
        }
    }
    d->last[0] = x;
    d->last[1] = y;
    d->last_t = t;
    if (hit) memset(d->ax, 0, sizeof(d->ax));
    return hit;
}

/* ------------------------------------------------------------------------- */
double wlmaker_locate_pointer_scale_at(
    const wlmaker_locate_pointer_settings_t *s_ptr,
    long t,
    long start,
    long hold_until)
{
    double k;
    long grow = BS_MAX(1, s_ptr->grow_msec);
    long ret = BS_MAX(1, s_ptr->return_msec);
    if (t < start + grow) {
        double x = BS_MAX(0.0, (double)(t - start) / grow);
        k = 1.0 - pow(1.0 - x, 3);  // Ease out: Quick, then gentle.
    } else if (t < hold_until) {
        k = 1.0;
    } else if (t < hold_until + ret) {
        double x = (double)(t - hold_until) / ret;
        k = 1.0 - (1.0 - pow(1.0 - x, 3));
    } else {
        return -1;
    }
    return 1.0 + (s_ptr->max_scale - 1.0) * k;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Decodes the configuration, loads the settings, (re)starts watching. */
bool _wlmaker_locate_pointer_configure(
    wlmaker_locate_pointer_t *lp_ptr,
    bspl_dict_t *config_dict_ptr)
{
    _wlmaker_locate_pointer_config_t config = {};
    bspl_dict_t *empty_ptr = NULL;
    if (NULL == config_dict_ptr) {
        empty_ptr = bspl_dict_create();
        config_dict_ptr = empty_ptr;
    }
    bool ok = (NULL != config_dict_ptr &&
               bspl_decode_dict(config_dict_ptr,
                                _wlmaker_locate_pointer_config_desc, &config));
    if (NULL != empty_ptr) bspl_dict_unref(empty_ptr);
    if (!ok) {
        bs_log(BS_WARNING, "Failed to decode the 'LocatePointer' configuration.");
        bspl_decoded_destroy(_wlmaker_locate_pointer_config_desc, &config);
        return false;
    }
    _wlmaker_locate_pointer_unwatch(lp_ptr);
    bspl_decoded_destroy(_wlmaker_locate_pointer_config_desc, &lp_ptr->config);
    lp_ptr->config = config;
    _wlmaker_locate_pointer_load_settings(lp_ptr);
    _wlmaker_locate_pointer_watch(lp_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Settings: The configuration's, then the settings file's. */
void _wlmaker_locate_pointer_load_settings(wlmaker_locate_pointer_t *lp_ptr)
{
    const _wlmaker_locate_pointer_config_t *c_ptr = &lp_ptr->config;
    wlmaker_locate_pointer_settings_t s = {
        .enabled = c_ptr->enabled,
        .sensitivity = c_ptr->sensitivity,
        .max_scale = BS_MIN(8.0, BS_MAX(1.5, c_ptr->max_scale)),
        .grow_msec = (int)BS_MAX(0, c_ptr->grow_msec),
        .duration_msec = (int)BS_MAX(200, c_ptr->duration_msec),
        .return_msec = (int)BS_MAX(0, c_ptr->return_msec),
    };
    char *path_ptr = _wlmaker_locate_pointer_expand(c_ptr->settings_file_ptr);
    if (NULL != path_ptr) {
        // As the X11 session: A settings file, missing or not, means "on"
        // unless it says otherwise.
        s.enabled = true;
        FILE *f_ptr = fopen(path_ptr, "r");
        if (NULL != f_ptr) {
            char text[4096];
            size_t n = fread(text, 1, sizeof(text) - 1, f_ptr);
            text[n] = '\0';
            fclose(f_ptr);
            wlmaker_locate_pointer_parse_settings(text, &s);
        }
        free(path_ptr);
    }
    bool was_enabled = lp_ptr->settings.enabled;
    lp_ptr->settings = s;
    lp_ptr->detector_output_height = 0;  // Set up again on next motion.
    if (was_enabled != s.enabled) {
        bs_log(BS_INFO, "Locating the pointer by shaking the mouse: %s",
               s.enabled ? "on" : "off");
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Watches the settings file's directory: The file is replaced (renamed
 * over) when changed. The directory is created if missing, so that the
 * first change is seen, too.
 */
void _wlmaker_locate_pointer_watch(wlmaker_locate_pointer_t *lp_ptr)
{
    if (NULL == lp_ptr->server_ptr->wl_display_ptr) return;
    char *path_ptr = _wlmaker_locate_pointer_expand(
        lp_ptr->config.settings_file_ptr);
    if (NULL == path_ptr) return;
    char *slash_ptr = strrchr(path_ptr, '/');
    if (NULL == slash_ptr || slash_ptr == path_ptr) {
        free(path_ptr);
        return;
    }
    *slash_ptr = '\0';
    lp_ptr->settings_basename_ptr = logged_strdup(slash_ptr + 1);
    if (0 != mkdir(path_ptr, 0755) && EEXIST != errno) {
        bs_log(BS_DEBUG | BS_ERRNO, "Failed mkdir(%s)", path_ptr);
    }

    lp_ptr->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (0 > lp_ptr->inotify_fd ||
        0 > inotify_add_watch(
            lp_ptr->inotify_fd, path_ptr,
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM)) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed to watch %s", path_ptr);
        free(path_ptr);
        _wlmaker_locate_pointer_unwatch(lp_ptr);
        return;
    }
    free(path_ptr);
    lp_ptr->inotify_source_ptr = wl_event_loop_add_fd(
        wl_display_get_event_loop(lp_ptr->server_ptr->wl_display_ptr),
        lp_ptr->inotify_fd, WL_EVENT_READABLE,
        _wlmaker_locate_pointer_handle_inotify, lp_ptr);
    if (NULL == lp_ptr->inotify_source_ptr) _wlmaker_locate_pointer_unwatch(lp_ptr);
}

/* ------------------------------------------------------------------------- */
/** Stops watching the settings file. */
void _wlmaker_locate_pointer_unwatch(wlmaker_locate_pointer_t *lp_ptr)
{
    if (NULL != lp_ptr->inotify_source_ptr) {
        wl_event_source_remove(lp_ptr->inotify_source_ptr);
        lp_ptr->inotify_source_ptr = NULL;
    }
    if (0 <= lp_ptr->inotify_fd) {
        close(lp_ptr->inotify_fd);
        lp_ptr->inotify_fd = -1;
    }
    if (NULL != lp_ptr->settings_basename_ptr) {
        free(lp_ptr->settings_basename_ptr);
        lp_ptr->settings_basename_ptr = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/** A change in the settings' directory: Reloads, if it is the file. */
int _wlmaker_locate_pointer_handle_inotify(
    int fd,
    __UNUSED__ uint32_t mask,
    void *data_ptr)
{
    wlmaker_locate_pointer_t *lp_ptr = data_ptr;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool reload = false;
    for (;;) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (0 >= len) break;
        for (char *p = buf; p < buf + len; ) {
            struct inotify_event *e_ptr = (struct inotify_event *)p;
            if (0 < e_ptr->len && NULL != lp_ptr->settings_basename_ptr &&
                0 == strcmp(e_ptr->name, lp_ptr->settings_basename_ptr)) {
                reload = true;
            }
            p += sizeof(struct inotify_event) + e_ptr->len;
        }
    }
    if (reload) {
        _wlmaker_locate_pointer_load_settings(lp_ptr);
        if (!lp_ptr->settings.enabled && lp_ptr->animating) {
            _wlmaker_locate_pointer_stop(lp_ptr);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/** The pointer moved: Feeds the detector. Cheap; no polling. */
void _wlmaker_locate_pointer_handle_cursor_position_updated(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_locate_pointer_t *lp_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_locate_pointer_t,
        cursor_position_updated_listener);
    struct wlr_cursor *wlr_cursor_ptr = data_ptr;
    wlmaker_server_t *server_ptr = lp_ptr->server_ptr;
    if (!lp_ptr->settings.enabled) return;

    // A hidden pointer (games, videos) or a fullscreen window: Nothing.
    if (!wlmim_cursor_visible(server_ptr->input_manager_ptr)) return;
    if (NULL != server_ptr->desktop_ptr) {
        wlmtk_workspace_t *ws_ptr = wlmtk_desktop_get_current_workspace(
            server_ptr->desktop_ptr);
        wlmtk_window_t *w_ptr = NULL != ws_ptr ?
            wlmtk_workspace_get_activated_window(ws_ptr) : NULL;
        if (NULL != w_ptr && wlmtk_window_is_fullscreen(w_ptr)) return;
    }

    struct wlr_output *o_ptr = wlr_output_layout_output_at(
        server_ptr->wlr_output_layout_ptr, wlr_cursor_ptr->x, wlr_cursor_ptr->y);
    if (NULL != o_ptr) {
        struct wlr_box box;
        wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr, o_ptr, &box);
        if (box.height != lp_ptr->detector_output_height) {
            wlmaker_shake_detector_setup(&lp_ptr->detector,
                                         lp_ptr->settings.sensitivity,
                                         box.height);
            lp_ptr->detector_output_height = box.height;
        }
    }

    long t = _wlmaker_locate_pointer_now();
    bool button = (NULL != server_ptr->wlr_seat_ptr &&
                   0 < server_ptr->wlr_seat_ptr->pointer_state.button_count);
    if (!wlmaker_shake_detector_sample(&lp_ptr->detector,
                                       (int)floor(wlr_cursor_ptr->x),
                                       (int)floor(wlr_cursor_ptr->y),
                                       t, button)) return;

    if (!lp_ptr->animating) {
        _wlmaker_locate_pointer_start(lp_ptr);
    } else if (t >= lp_ptr->start + lp_ptr->settings.grow_msec) {
        // Still shaking: Stays large a bit longer, up to a limit.
        long limit = lp_ptr->start + lp_ptr->settings.duration_msec + 800;
        long hold = BS_MIN(t + 500, limit);
        if (hold > lp_ptr->hold_until) lp_ptr->hold_until = hold;
    }
}

/* ------------------------------------------------------------------------- */
/** Starts magnifying the pointer. */
void _wlmaker_locate_pointer_start(wlmaker_locate_pointer_t *lp_ptr)
{
    long t = _wlmaker_locate_pointer_now();
    if (!lp_ptr->animating) {
        lp_ptr->start = t;
    } else {
        // Shown again while going back: Large again, right away.
        lp_ptr->start = t - lp_ptr->settings.grow_msec;
    }
    lp_ptr->hold_until = lp_ptr->start + lp_ptr->settings.duration_msec;
    lp_ptr->animating = true;
    _wlmaker_locate_pointer_handle_timer(lp_ptr);
}

/* ------------------------------------------------------------------------- */
/** Back to the regular pointer. */
void _wlmaker_locate_pointer_stop(wlmaker_locate_pointer_t *lp_ptr)
{
    lp_ptr->animating = false;
    if (NULL != lp_ptr->timer_source_ptr) {
        wl_event_source_timer_update(lp_ptr->timer_source_ptr, 0);
    }
    if (NULL != lp_ptr->server_ptr->input_manager_ptr) {
        wlmim_set_cursor_magnification(
            lp_ptr->server_ptr->input_manager_ptr, 1.0, 1.0);
    }
    wlmaker_shake_detector_reset(&lp_ptr->detector);
}

/* ------------------------------------------------------------------------- */
/** A frame of the animation. Runs only while the pointer is magnified. */
int _wlmaker_locate_pointer_handle_timer(void *data_ptr)
{
    wlmaker_locate_pointer_t *lp_ptr = data_ptr;
    if (!lp_ptr->animating) return 0;
    double scale = wlmaker_locate_pointer_scale_at(
        &lp_ptr->settings, _wlmaker_locate_pointer_now(),
        lp_ptr->start, lp_ptr->hold_until);
    if (0 > scale) {
        _wlmaker_locate_pointer_stop(lp_ptr);
        return 0;
    }
    if (NULL != lp_ptr->server_ptr->input_manager_ptr) {
        wlmim_set_cursor_magnification(
            lp_ptr->server_ptr->input_manager_ptr,
            scale, lp_ptr->settings.max_scale);
    }
    if (NULL != lp_ptr->timer_source_ptr) {
        wl_event_source_timer_update(lp_ptr->timer_source_ptr,
                                     _wlmaker_locate_pointer_frame_msec);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/** @return Monotonic time, in ms. */
long _wlmaker_locate_pointer_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------------- */
/** @return The path with "~/" expanded, or NULL. Release with free(). */
char *_wlmaker_locate_pointer_expand(const char *path_ptr)
{
    if (NULL == path_ptr || '\0' == *path_ptr) return NULL;
    if (0 == strncmp(path_ptr, "~/", 2)) {
        const char *home_ptr = getenv("HOME");
        if (NULL == home_ptr || '\0' == *home_ptr) return NULL;
        return bs_strdupf("%s/%s", home_ptr, path_ptr + 2);
    }
    return logged_strdup(path_ptr);
}

/* == Unit tests =========================================================== */

static void _wlmaker_locate_pointer_test_detector(bs_test_t *test_ptr);
static void _wlmaker_locate_pointer_test_settings(bs_test_t *test_ptr);
static void _wlmaker_locate_pointer_test_scale(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t _wlmaker_locate_pointer_test_cases[] = {
    { true, "detector", _wlmaker_locate_pointer_test_detector },
    { true, "settings", _wlmaker_locate_pointer_test_settings },
    { true, "scale", _wlmaker_locate_pointer_test_scale },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_locate_pointer_test_set = BS_TEST_SET(
    true, "locate_pointer", _wlmaker_locate_pointer_test_cases);

/** Feeds samples every 16 ms, from a to b, in `dur` ms. @return hits. */
static int _feed(wlmaker_shake_detector_t *d, long *t,
                 int ax, int ay, int bx, int by, int dur, bool button)
{
    int hits = 0, steps = BS_MAX(1, dur / 16);
    for (int i = 1; i <= steps; ++i) {
        *t += 16;
        hits += wlmaker_shake_detector_sample(
            d, ax + (bx - ax) * i / steps, ay + (by - ay) * i / steps,
            *t, button);
    }
    return hits;
}

/* ------------------------------------------------------------------------- */
/** Shakes are recognized; regular motion is not. Same cases as on X11. */
void _wlmaker_locate_pointer_test_detector(bs_test_t *test_ptr)
{
    wlmaker_shake_detector_t d;
    long t;
    int hits;

    // Quick horizontal shake: 6 strokes of 300 px in 120 ms.
    wlmaker_shake_detector_setup(&d, 1, 1080);
    t = 1000; hits = 0;
    _feed(&d, &t, 500, 500, 500, 500, 16, false);
    for (int i = 0; i < 6; ++i) {
        hits += _feed(&d, &t, i % 2 ? 800 : 500, 500, i % 2 ? 500 : 800, 500,
                      120, false);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 1, hits);

    // Slow back and forth: 300 px in 500 ms.
    wlmaker_shake_detector_setup(&d, 1, 1080);
    t = 1000; hits = 0;
    _feed(&d, &t, 500, 500, 500, 500, 16, false);
    for (int i = 0; i < 8; ++i) {
        hits += _feed(&d, &t, i % 2 ? 800 : 500, 500, i % 2 ? 500 : 800, 500,
                      500, false);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, hits);

    // Short tremor (30 px).
    wlmaker_shake_detector_setup(&d, 1, 1080);
    t = 1000; hits = 0;
    for (int i = 0; i < 20; ++i) {
        hits += _feed(&d, &t, i % 2 ? 530 : 500, 500, i % 2 ? 500 : 530, 500,
                      40, false);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, hits);

    // Dragging with a button pressed.
    wlmaker_shake_detector_setup(&d, 1, 1080);
    t = 1000; hits = 0;
    for (int i = 0; i < 8; ++i) {
        hits += _feed(&d, &t, i % 2 ? 800 : 500, 500, i % 2 ? 500 : 800, 500,
                      120, true);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, hits);

    // Pauses between the strokes.
    wlmaker_shake_detector_setup(&d, 1, 1080);
    t = 1000; hits = 0;
    for (int i = 0; i < 8; ++i) {
        _feed(&d, &t, 500, 500, 500, 500, 16, false);
        hits += _feed(&d, &t, i % 2 ? 800 : 500, 500, i % 2 ? 500 : 800, 500,
                      120, false);
        t += 400;
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, hits);

    // A tall output needs longer strokes.
    wlmaker_shake_detector_setup(&d, 1, 2160);
    t = 1000; hits = 0;
    _feed(&d, &t, 500, 500, 500, 500, 16, false);
    for (int i = 0; i < 6; ++i) {
        hits += _feed(&d, &t, i % 2 ? 680 : 500, 500, i % 2 ? 500 : 680, 500,
                      100, false);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, hits);

    // Low sensitivity: 3 reversals are not enough.
    wlmaker_shake_detector_setup(&d, 0, 1080);
    t = 1000; hits = 0;
    _feed(&d, &t, 500, 500, 500, 500, 16, false);
    for (int i = 0; i < 4; ++i) {
        hits += _feed(&d, &t, i % 2 ? 800 : 500, 500, i % 2 ? 500 : 800, 500,
                      120, false);
    }
    BS_TEST_VERIFY_EQ(test_ptr, 0, hits);
}

/* ------------------------------------------------------------------------- */
/** Parses the settings file, ignoring invalid values. */
void _wlmaker_locate_pointer_test_settings(bs_test_t *test_ptr)
{
    wlmaker_locate_pointer_settings_t s = {
        .enabled = true, .sensitivity = 1, .max_scale = 3.0,
        .grow_msec = 160, .duration_msec = 700, .return_msec = 260 };
    wlmaker_locate_pointer_parse_settings(
        "# comment\n"
        "enabled = false\n"
        "sensitivity=high\n"
        "max-scale=12\n"
        "duration-ms=1500\n"
        "return-ms = 400\r\n"
        "unknown=1\n", &s);
    BS_TEST_VERIFY_FALSE(test_ptr, s.enabled);
    BS_TEST_VERIFY_EQ(test_ptr, 2, s.sensitivity);
    BS_TEST_VERIFY_EQ(test_ptr, 3.0, s.max_scale);
    BS_TEST_VERIFY_EQ(test_ptr, 160, s.grow_msec);
    BS_TEST_VERIFY_EQ(test_ptr, 1500, s.duration_msec);
    BS_TEST_VERIFY_EQ(test_ptr, 400, s.return_msec);

    wlmaker_locate_pointer_parse_settings("enabled=true\nmax-scale=4.5", &s);
    BS_TEST_VERIFY_TRUE(test_ptr, s.enabled);
    BS_TEST_VERIFY_EQ(test_ptr, 4.5, s.max_scale);
}

/* ------------------------------------------------------------------------- */
/** Grows smoothly, holds, goes back gradually, then ends. */
void _wlmaker_locate_pointer_test_scale(bs_test_t *test_ptr)
{
    wlmaker_locate_pointer_settings_t s = {
        .max_scale = 3.0, .grow_msec = 160, .duration_msec = 700,
        .return_msec = 260 };
    BS_TEST_VERIFY_EQ(test_ptr, 1.0,
                      wlmaker_locate_pointer_scale_at(&s, 0, 0, 700));
    double a = wlmaker_locate_pointer_scale_at(&s, 40, 0, 700);
    double b = wlmaker_locate_pointer_scale_at(&s, 80, 0, 700);
    BS_TEST_VERIFY_TRUE(test_ptr, 1.0 < a && a < b && b < 3.0);
    BS_TEST_VERIFY_EQ(test_ptr, 3.0,
                      wlmaker_locate_pointer_scale_at(&s, 400, 0, 700));
    double c = wlmaker_locate_pointer_scale_at(&s, 800, 0, 700);
    double e = wlmaker_locate_pointer_scale_at(&s, 900, 0, 700);
    BS_TEST_VERIFY_TRUE(test_ptr, 3.0 > c && c > e && e >= 1.0);
    BS_TEST_VERIFY_TRUE(test_ptr,
                        0 > wlmaker_locate_pointer_scale_at(&s, 961, 0, 700));
}

/* == End of locate_pointer.c ============================================== */
