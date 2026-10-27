/* ========================================================================= */
/**
 * @file wlm_settings.h
 *
 * Preferences of a DockApp: Read from `${XDG_CONFIG_HOME}/wlmaker/<app>.plist`
 * (eg. `~/.config/wlmaker/wlmcalclock.plist`), a dict of values. The file is
 * written by wlmaker, from the dock-app's menu (see `Dockapps.plist`), or by
 * hand. The DockApp watches it, and applies changes right away.
 *
 * Also holds the color themes the DockApps share, and a helper to run the
 * commands configured for clicks.
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
#ifndef __WLM_SETTINGS_H__
#define __WLM_SETTINGS_H__

#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Forward declaration: Preferences of a DockApp. */
typedef struct _wlm_settings_t wlm_settings_t;

/**
 * Colors of a theme. The DockApps draw their LCD-like displays with these.
 */
typedef struct {
    /** Name, as used for the `Theme` preference. */
    const char                *name_ptr;
    /** Background of the display. */
    uint32_t                  background;
    /** An illuminated element (LED, segment, hand, text). */
    uint32_t                  led;
    /** An element that is turned off. */
    uint32_t                  off;
    /** Highlights: Eg. 'muted', the calendar's band. */
    uint32_t                  accent;
    /** The calendar's paper. */
    uint32_t                  paper;
    /** Text on the paper. */
    uint32_t                  ink;
    /** Graphs: Color for low activity. 0 for the graph's own colors. */
    uint32_t                  graph_low;
    /** Graphs: Color for high activity. */
    uint32_t                  graph_high;
    /** Graphs: The line on top of the graph. */
    uint32_t                  graph_line;
} wlm_theme_t;

/**
 * Creates the preferences for `app_name_ptr`, and loads them.
 *
 * @param app_name_ptr        Eg. "wlmcalclock".
 * @param dir_ptr             Directory of the file. NULL for
 *                            `${XDG_CONFIG_HOME}/wlmaker`, or
 *                            `${HOME}/.config/wlmaker`.
 *
 * @return The preferences, or NULL on error. A missing file is no error: All
 *     values are the defaults then.
 */
wlm_settings_t *wlm_settings_create(const char *app_name_ptr,
                                    const char *dir_ptr);

/** Destroys the preferences. */
void wlm_settings_destroy(wlm_settings_t *settings_ptr);

/** @return Path of the file. */
const char *wlm_settings_path(wlm_settings_t *settings_ptr);

/** Re-reads the file. @return true if it was read, or does not exist. */
bool wlm_settings_reload(wlm_settings_t *settings_ptr);

/**
 * Starts watching the file for changes, through inotify.
 *
 * @return A file descriptor that turns readable on changes, for a main loop.
 *     Pass to @ref wlm_settings_handle_fd then. Or -1 on error.
 */
int wlm_settings_watch(wlm_settings_t *settings_ptr);

/**
 * Consumes the events of the descriptor from @ref wlm_settings_watch. If the
 * file was changed: Re-reads it.
 *
 * @return true if the file changed, and was re-read.
 */
bool wlm_settings_handle_fd(wlm_settings_t *settings_ptr);

/** @return The string value for `key_ptr`, or `default_ptr`. */
const char *wlm_settings_string(wlm_settings_t *settings_ptr,
                                const char *key_ptr,
                                const char *default_ptr);

/**
 * @return The boolean value for `key_ptr` (Yes, No, True, False, On, Off,
 *     Enabled, Disabled), or `default_value` if unset or not a boolean.
 */
bool wlm_settings_bool(wlm_settings_t *settings_ptr,
                       const char *key_ptr,
                       bool default_value);

/**
 * @return The numeric value for `key_ptr` within [`min`, `max`], or
 *     `default_value` if unset or not a number.
 */
double wlm_settings_number(wlm_settings_t *settings_ptr,
                           const char *key_ptr,
                           double default_value,
                           double min,
                           double max);

/**
 * Looks up the value for `key_ptr` in a list of names.
 *
 * @param settings_ptr
 * @param key_ptr
 * @param names_ptr           NULL-terminated array of names.
 * @param default_index       Returned if unset, or not in `names_ptr`.
 *
 * @return Index of the value's name in `names_ptr`.
 */
int wlm_settings_choice(wlm_settings_t *settings_ptr,
                        const char *key_ptr,
                        const char *const *names_ptr,
                        int default_index);

/**
 * Returns the theme named by the `Theme` preference.
 *
 * @param settings_ptr
 * @param default_theme_ptr   The DockApp's own colors: For "Default", for
 *                            unknown names, and for colors left 0.
 *
 * @return The theme. Valid until the next call. Colors that the theme leaves
 *     open come from `default_theme_ptr`.
 */
const wlm_theme_t *wlm_settings_theme(wlm_settings_t *settings_ptr,
                                      const wlm_theme_t *default_theme_ptr);

/** @return The theme named `name_ptr`, or NULL. "Default" is not one. */
const wlm_theme_t *wlm_theme_find(const char *name_ptr);

/**
 * Starts `command_ptr` through `/bin/sh -c`, in its own process group, with
 * default signal handling. Does nothing for NULL or an empty command. From
 * then on, SIGCHLD is ignored: The processes are reaped automatically.
 *
 * @return true if started (or nothing to start).
 */
bool wlm_settings_spawn(const char *command_ptr);

/**
 * Computes an ARGB color between `from` and `to`.
 *
 * @param from
 * @param to
 * @param i                   From 0 (`from`) to 255 (`to`).
 */
uint32_t wlm_theme_blend(uint32_t from, uint32_t to, uint32_t i);

/** Unit test cases. */
extern const bs_test_set_t wlm_settings_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLM_SETTINGS_H__ */
/* == End of wlm_settings.h ================================================ */
