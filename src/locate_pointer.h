/* ========================================================================= */
/**
 * @file locate_pointer.h
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
#ifndef __LOCATE_POINTER_H__
#define __LOCATE_POINTER_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>

/** Forward declaration: Locating the pointer by shaking the mouse. */
typedef struct _wlmaker_locate_pointer_t wlmaker_locate_pointer_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Settings of locating the pointer. */
typedef struct {
    /** Whether shaking the mouse magnifies the pointer. */
    bool                      enabled;
    /** 0: low (5 reversals), 1: medium (4), 2: high (3). */
    int                       sensitivity;
    /** Largest magnification of the pointer, 1.5 to 8. */
    double                    max_scale;
    /** Time to grow to the largest size, in ms. */
    int                       grow_msec;
    /** Time from the start until it goes back, in ms. */
    int                       duration_msec;
    /** Time to go back to the regular size, in ms. */
    int                       return_msec;
} wlmaker_locate_pointer_settings_t;

/** The shake detector: Counts direction reversals of long, quick strokes. */
typedef struct {
    /** Per axis. */
    struct {
        /** Direction: -1, 0, +1. */
        int                   dir;
        /** Length of the current stroke, in pixels. */
        int                   seg;
        /** Start of the current stroke, in ms. */
        long                  seg_start;
        /** Reversals counted. */
        int                   reversals;
        /** Time of the first reversal, in ms. */
        long                  first_rev;
    } ax[2];
    /** Last position. */
    int                       last[2];
    /** Time of the last position, in ms. */
    long                      last_t;
    /** Whether there is a last position. */
    bool                      have_last;
    /** Minimum stroke length, in pixels. */
    int                       min_seg;
    /** Maximum stroke duration, in ms. */
    int                       max_seg_ms;
    /** Reversals needed. */
    int                       need;
    /** Time window for the reversals, in ms. */
    int                       window_ms;
    /** A pause longer than this starts over, in ms. */
    int                       idle_ms;
} wlmaker_shake_detector_t;

/**
 * Creates the handler. Watches the settings file (if configured) for
 * changes: Turning the feature on or off applies right away.
 *
 * @param server_ptr
 * @param config_dict_ptr     The 'LocatePointer' dictionary. May be NULL.
 *
 * @return The handler or NULL on error.
 */
wlmaker_locate_pointer_t *wlmaker_locate_pointer_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr);

/** Destroys the handler. */
void wlmaker_locate_pointer_destroy(wlmaker_locate_pointer_t *lp_ptr);

/**
 * Applies a new configuration (on reload).
 *
 * @param lp_ptr
 * @param config_dict_ptr     The 'LocatePointer' dictionary. May be NULL.
 *
 * @return true on success.
 */
bool wlmaker_locate_pointer_reconfigure(
    wlmaker_locate_pointer_t *lp_ptr,
    bspl_dict_t *config_dict_ptr);

/** Magnifies the pointer once, now (the `LocatePointer` action). */
void wlmaker_locate_pointer_show(wlmaker_locate_pointer_t *lp_ptr);

/**
 * Parses the `key=value` settings (as the X11 session's
 * big-wm-locate-pointer: enabled, sensitivity, max-scale, grow-ms,
 * duration-ms, return-ms). Unknown keys and invalid values are ignored.
 *
 * @param text_ptr
 * @param settings_ptr        Updated with the values found.
 */
void wlmaker_locate_pointer_parse_settings(
    const char *text_ptr,
    wlmaker_locate_pointer_settings_t *settings_ptr);

/** Sets up the detector for the sensitivity and the output's height. */
void wlmaker_shake_detector_setup(
    wlmaker_shake_detector_t *d_ptr,
    int sensitivity,
    int output_height);

/** Starts over (eg. a button was pressed). */
void wlmaker_shake_detector_reset(wlmaker_shake_detector_t *d_ptr);

/**
 * Feeds a position at time `t` (ms).
 *
 * @return true when a shake is recognized.
 */
bool wlmaker_shake_detector_sample(
    wlmaker_shake_detector_t *d_ptr,
    int x,
    int y,
    long t,
    bool button);

/**
 * The pointer's magnification at time `t`.
 *
 * @return The magnification, or a negative value once done.
 */
double wlmaker_locate_pointer_scale_at(
    const wlmaker_locate_pointer_settings_t *settings_ptr,
    long t,
    long start,
    long hold_until);

/** Unit test cases. */
extern const bs_test_set_t wlmaker_locate_pointer_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __LOCATE_POINTER_H__ */
/* == End of locate_pointer.h ============================================== */
