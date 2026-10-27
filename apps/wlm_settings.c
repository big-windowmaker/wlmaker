/* ========================================================================= */
/**
 * @file wlm_settings.c
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

#include "wlm_settings.h"

#include <errno.h>
#include <libbase/plist.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* == Declarations ========================================================= */

/** State of the preferences. */
struct _wlm_settings_t {
    /** Directory holding the file. */
    char                      *dir_ptr;
    /** Name of the file, in the directory. */
    char                      *fname_ptr;
    /** Full path of the file. */
    char                      *path_ptr;
    /** The values. Never NULL after creation. */
    bspl_dict_t               *dict_ptr;
    /** inotify descriptor, or -1. */
    int                       inotify_fd;
    /** The theme returned by @ref wlm_settings_theme. */
    wlm_theme_t               theme;
};

/** The themes. Colors left 0 come from the DockApp's own theme. */
static const wlm_theme_t _wlm_themes[] = {
    {
        // Window Maker's classic look: Green LCD.
        .name_ptr = "Green",
        .background = 0xff071407, .led = 0xff52ff52, .off = 0xff0f3a12,
        .accent = 0xffff5a3c, .paper = 0xffdfeed6, .ink = 0xff0c240c,
        .graph_low = 0xff0a4a12, .graph_high = 0xff6cff6c,
        .graph_line = 0xffb4ffb4,
    },
    {
        .name_ptr = "Amber",
        .background = 0xff140c00, .led = 0xffffb020, .off = 0xff3c2800,
        .accent = 0xffff5a28, .paper = 0xfff2e4c4, .ink = 0xff2c1c00,
        .graph_low = 0xff503400, .graph_high = 0xffffc848,
        .graph_line = 0xffffe0a0,
    },
    {
        .name_ptr = "Blue",
        .background = 0xff060e1a, .led = 0xff5cb8ff, .off = 0xff0e2a46,
        .accent = 0xffff5c5c, .paper = 0xffdce8f6, .ink = 0xff0c1c30,
        .graph_low = 0xff0c2c5c, .graph_high = 0xff78c8ff,
        .graph_line = 0xffc0e4ff,
    },
    {
        // NeXT: Dark on light grey.
        .name_ptr = "NeXT",
        .background = 0xffaaaaaa, .led = 0xff000000, .off = 0xff9a9a9a,
        .accent = 0xff555555, .paper = 0xffdddddd, .ink = 0xff000000,
        .graph_low = 0xff707070, .graph_high = 0xff000000,
        .graph_line = 0xff000000,
    },
    {
        .name_ptr = "HighContrast",
        .background = 0xff000000, .led = 0xffffffff, .off = 0xff2a2a2a,
        .accent = 0xffffff00, .paper = 0xffffffff, .ink = 0xff000000,
        .graph_low = 0xff909090, .graph_high = 0xffffffff,
        .graph_line = 0xffffff00,
    },
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlm_settings_t *wlm_settings_create(const char *app_name_ptr,
                                    const char *dir_ptr)
{
    wlm_settings_t *settings_ptr = logged_calloc(1, sizeof(*settings_ptr));
    if (NULL == settings_ptr) return NULL;
    settings_ptr->inotify_fd = -1;

    if (NULL != dir_ptr) {
        settings_ptr->dir_ptr = logged_strdup(dir_ptr);
    } else {
        const char *config_ptr = getenv("XDG_CONFIG_HOME");
        if (NULL != config_ptr && '/' == *config_ptr) {
            settings_ptr->dir_ptr = bs_strdupf("%s/wlmaker", config_ptr);
        } else {
            const char *home_ptr = getenv("HOME");
            settings_ptr->dir_ptr = bs_strdupf(
                "%s/.config/wlmaker", NULL != home_ptr ? home_ptr : "");
        }
    }
    settings_ptr->fname_ptr = bs_strdupf("%s.plist", app_name_ptr);
    if (NULL == settings_ptr->dir_ptr || NULL == settings_ptr->fname_ptr) {
        wlm_settings_destroy(settings_ptr);
        return NULL;
    }
    settings_ptr->path_ptr = bs_strdupf(
        "%s/%s", settings_ptr->dir_ptr, settings_ptr->fname_ptr);
    if (NULL == settings_ptr->path_ptr) {
        wlm_settings_destroy(settings_ptr);
        return NULL;
    }

    if (!wlm_settings_reload(settings_ptr) &&
        NULL == settings_ptr->dict_ptr) {
        settings_ptr->dict_ptr = bspl_dict_create();
    }
    if (NULL == settings_ptr->dict_ptr) {
        wlm_settings_destroy(settings_ptr);
        return NULL;
    }
    return settings_ptr;
}

/* ------------------------------------------------------------------------- */
void wlm_settings_destroy(wlm_settings_t *settings_ptr)
{
    if (0 <= settings_ptr->inotify_fd) {
        close(settings_ptr->inotify_fd);
        settings_ptr->inotify_fd = -1;
    }
    if (NULL != settings_ptr->dict_ptr) bspl_dict_unref(settings_ptr->dict_ptr);
    if (NULL != settings_ptr->path_ptr) free(settings_ptr->path_ptr);
    if (NULL != settings_ptr->fname_ptr) free(settings_ptr->fname_ptr);
    if (NULL != settings_ptr->dir_ptr) free(settings_ptr->dir_ptr);
    free(settings_ptr);
}

/* ------------------------------------------------------------------------- */
const char *wlm_settings_path(wlm_settings_t *settings_ptr)
{
    return settings_ptr->path_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlm_settings_reload(wlm_settings_t *settings_ptr)
{
    bspl_dict_t *dict_ptr = NULL;
    struct stat st;
    bool rv = true;
    if (0 == stat(settings_ptr->path_ptr, &st)) {
        bspl_object_t *o = bspl_create_object_from_plist_file(
            settings_ptr->path_ptr);
        dict_ptr = bspl_dict_from_object(o);
        if (NULL == dict_ptr) {
            bs_log(BS_WARNING, "Not a dict of preferences: \"%s\"",
                   settings_ptr->path_ptr);
            if (NULL != o) bspl_object_unref(o);
            rv = false;
        }
    }
    // A missing file, or one with errors: The defaults.
    if (NULL == dict_ptr) dict_ptr = bspl_dict_create();
    if (NULL == dict_ptr) return false;

    if (NULL != settings_ptr->dict_ptr) bspl_dict_unref(settings_ptr->dict_ptr);
    settings_ptr->dict_ptr = dict_ptr;
    return rv;
}

/* ------------------------------------------------------------------------- */
int wlm_settings_watch(wlm_settings_t *settings_ptr)
{
    if (0 <= settings_ptr->inotify_fd) return settings_ptr->inotify_fd;

    // The directory must exist, to watch it. The file gets replaced (by a
    // rename) when written: Watch the directory, not the file.
    struct stat st;
    if (0 != stat(settings_ptr->dir_ptr, &st) &&
        !bs_file_mkdir_p(settings_ptr->dir_ptr, S_IRWXU)) {
        bs_log(BS_WARNING, "Failed to create \"%s\"", settings_ptr->dir_ptr);
        return -1;
    }
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (0 > fd) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed inotify_init1()");
        return -1;
    }
    if (0 > inotify_add_watch(
            fd, settings_ptr->dir_ptr,
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM)) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed inotify_add_watch(%d, %s)",
               fd, settings_ptr->dir_ptr);
        close(fd);
        return -1;
    }
    settings_ptr->inotify_fd = fd;
    return fd;
}

/* ------------------------------------------------------------------------- */
bool wlm_settings_handle_fd(wlm_settings_t *settings_ptr)
{
    if (0 > settings_ptr->inotify_fd) return false;
    bool changed = false;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t len = read(settings_ptr->inotify_fd, buf, sizeof(buf));
        if (0 >= len) break;
        for (char *p = buf; p < buf + len; ) {
            const struct inotify_event *e = (const struct inotify_event*)p;
            if (0 < e->len && 0 == strcmp(e->name, settings_ptr->fname_ptr)) {
                changed = true;
            }
            p += sizeof(struct inotify_event) + e->len;
        }
    }
    if (changed) wlm_settings_reload(settings_ptr);
    return changed;
}

/* ------------------------------------------------------------------------- */
const char *wlm_settings_string(wlm_settings_t *settings_ptr,
                                const char *key_ptr,
                                const char *default_ptr)
{
    const char *v = bspl_dict_get_string_value(settings_ptr->dict_ptr, key_ptr);
    return NULL != v ? v : default_ptr;
}

/* ------------------------------------------------------------------------- */
bool wlm_settings_bool(wlm_settings_t *settings_ptr,
                       const char *key_ptr,
                       bool default_value)
{
    static const char *const t[] = { "Yes", "True", "On", "Enabled", NULL };
    static const char *const f[] = { "No", "False", "Off", "Disabled", NULL };
    const char *v = wlm_settings_string(settings_ptr, key_ptr, NULL);
    if (NULL == v) return default_value;
    for (int i = 0; NULL != t[i]; ++i) {
        if (0 == strcasecmp(v, t[i])) return true;
        if (0 == strcasecmp(v, f[i])) return false;
    }
    return default_value;
}

/* ------------------------------------------------------------------------- */
double wlm_settings_number(wlm_settings_t *settings_ptr,
                           const char *key_ptr,
                           double default_value,
                           double min,
                           double max)
{
    const char *v = wlm_settings_string(settings_ptr, key_ptr, NULL);
    if (NULL == v || '\0' == *v) return default_value;
    char *end_ptr;
    errno = 0;
    double d = strtod(v, &end_ptr);
    if (0 != errno || '\0' != *end_ptr || d != d) return default_value;
    return BS_MAX(min, BS_MIN(max, d));
}

/* ------------------------------------------------------------------------- */
int wlm_settings_choice(wlm_settings_t *settings_ptr,
                        const char *key_ptr,
                        const char *const *names_ptr,
                        int default_index)
{
    const char *v = wlm_settings_string(settings_ptr, key_ptr, NULL);
    if (NULL == v) return default_index;
    for (int i = 0; NULL != names_ptr[i]; ++i) {
        if (0 == strcasecmp(v, names_ptr[i])) return i;
    }
    return default_index;
}

/* ------------------------------------------------------------------------- */
const wlm_theme_t *wlm_theme_find(const char *name_ptr)
{
    if (NULL == name_ptr) return NULL;
    for (size_t i = 0; i < sizeof(_wlm_themes) / sizeof(_wlm_themes[0]); ++i) {
        if (0 == strcasecmp(name_ptr, _wlm_themes[i].name_ptr)) {
            return &_wlm_themes[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
const wlm_theme_t *wlm_settings_theme(wlm_settings_t *settings_ptr,
                                      const wlm_theme_t *default_theme_ptr)
{
    settings_ptr->theme = *default_theme_ptr;
    const wlm_theme_t *t = wlm_theme_find(
        wlm_settings_string(settings_ptr, "Theme", NULL));
    if (NULL == t) return &settings_ptr->theme;

    wlm_theme_t *d = &settings_ptr->theme;
    d->name_ptr = t->name_ptr;
#define _WLM_THEME_SET(_f) if (0 != t->_f) d->_f = t->_f
    _WLM_THEME_SET(background);
    _WLM_THEME_SET(led);
    _WLM_THEME_SET(off);
    _WLM_THEME_SET(accent);
    _WLM_THEME_SET(paper);
    _WLM_THEME_SET(ink);
    _WLM_THEME_SET(graph_low);
    _WLM_THEME_SET(graph_high);
    _WLM_THEME_SET(graph_line);
#undef _WLM_THEME_SET
    return d;
}

/* ------------------------------------------------------------------------- */
uint32_t wlm_theme_blend(uint32_t from, uint32_t to, uint32_t i)
{
    i = BS_MIN(i, 255U);
    uint32_t c = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t a = (from >> shift) & 0xff, b = (to >> shift) & 0xff;
        c |= ((a * (255 - i) + b * i + 127) / 255) << shift;
    }
    return c;
}

/* ------------------------------------------------------------------------- */
bool wlm_settings_spawn(const char *command_ptr)
{
    if (NULL == command_ptr || '\0' == *command_ptr) return true;

    posix_spawnattr_t attr;
    if (0 != posix_spawnattr_init(&attr)) return false;
    sigset_t sigset;
    sigemptyset(&sigset);
    posix_spawnattr_setsigmask(&attr, &sigset);
    sigaddset(&sigset, SIGCHLD);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGPIPE);
    sigaddset(&sigset, SIGTERM);
    posix_spawnattr_setsigdefault(&attr, &sigset);
    // A process group of its own: Keeps it running when we are stopped.
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(
        &attr,
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

    char *const argv[] = { "/bin/sh", "-c", (char*)command_ptr, NULL };
    extern char **environ;
    pid_t pid;
    int rv = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (0 != rv) {
        bs_log(BS_WARNING, "Failed to start \"%s\": %s",
               command_ptr, strerror(rv));
        return false;
    }
    bs_log(BS_INFO, "Started \"%s\", pid %d", command_ptr, pid);
    return true;
}

/* == Unit tests =========================================================== */

static void test_values(bs_test_t *test_ptr);
static void test_watch(bs_test_t *test_ptr);
static void test_theme(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlm_settings_test_cases[] = {
    { true, "values", test_values },
    { true, "watch", test_watch },
    { true, "theme", test_theme },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlm_settings_test_set = BS_TEST_SET(
    true, "settings", _wlm_settings_test_cases);

/** Writes `content_ptr` into the file at `path_ptr`. */
static bool _test_write(const char *path_ptr, const char *content_ptr)
{
    FILE *f = fopen(path_ptr, "w");
    if (NULL == f) return false;
    bool rv = 0 <= fputs(content_ptr, f);
    return 0 == fclose(f) && rv;
}

/* ------------------------------------------------------------------------- */
/** Reads values, and falls back to defaults. */
void test_values(bs_test_t *test_ptr)
{
    const char *dir_ptr = bs_test_temp_path(test_ptr, NULL);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dir_ptr);

    // No file: Defaults.
    wlm_settings_t *s = wlm_settings_create("app", dir_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, s);
    BS_TEST_VERIFY_STREQ(test_ptr, "d", wlm_settings_string(s, "K", "d"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlm_settings_bool(s, "B", true));

    const char *path_ptr = bs_test_temp_path(test_ptr, "app.plist");
    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, _test_write(
        path_ptr, "{ K = v; B = No; C = on; N = \"2.5\"; X = abc; "
        "E = Analog; Big = 99; }"));
    BS_TEST_VERIFY_STREQ(test_ptr, path_ptr, wlm_settings_path(s));
    BS_TEST_VERIFY_TRUE(test_ptr, wlm_settings_reload(s));
    BS_TEST_VERIFY_STREQ(test_ptr, "v", wlm_settings_string(s, "K", "d"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlm_settings_bool(s, "B", true));
    BS_TEST_VERIFY_TRUE(test_ptr, wlm_settings_bool(s, "C", false));
    BS_TEST_VERIFY_TRUE(test_ptr, wlm_settings_bool(s, "X", true));
    BS_TEST_VERIFY_TRUE(test_ptr, 2.5 == wlm_settings_number(s, "N", 1, 0, 10));
    BS_TEST_VERIFY_TRUE(test_ptr, 1 == wlm_settings_number(s, "X", 1, 0, 10));
    BS_TEST_VERIFY_TRUE(test_ptr, 10 == wlm_settings_number(s, "Big", 1, 0, 10));
    static const char *const styles[] = { "Digital", "Analog", NULL };
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlm_settings_choice(s, "E", styles, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlm_settings_choice(s, "X", styles, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlm_settings_choice(s, "None", styles, 0));

    // A broken file: Defaults again.
    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, _test_write(path_ptr, "{ K = "));
    BS_TEST_VERIFY_FALSE(test_ptr, wlm_settings_reload(s));
    BS_TEST_VERIFY_STREQ(test_ptr, "d", wlm_settings_string(s, "K", "d"));

    wlm_settings_destroy(s);
    unlink(path_ptr);
}

/* ------------------------------------------------------------------------- */
/** Changes to the file are seen through the watch descriptor. */
void test_watch(bs_test_t *test_ptr)
{
    const char *dir_ptr = bs_test_temp_path(test_ptr, NULL);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dir_ptr);
    wlm_settings_t *s = wlm_settings_create("app", dir_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, s);
    BS_TEST_VERIFY_TRUE(test_ptr, 0 <= wlm_settings_watch(s));
    BS_TEST_VERIFY_FALSE(test_ptr, wlm_settings_handle_fd(s));

    // Another file in the directory: No change.
    const char *other_ptr = bs_test_temp_path(test_ptr, "other.plist");
    BS_TEST_VERIFY_TRUE(test_ptr, _test_write(other_ptr, "{ K = x; }"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlm_settings_handle_fd(s));

    // Written by a rename, as wlmaker does: A change, re-read.
    const char *tmp_ptr = bs_test_temp_path(test_ptr, "tmp");
    BS_TEST_VERIFY_TRUE(test_ptr, _test_write(tmp_ptr, "{ K = w; }"));
    BS_TEST_VERIFY_EQ(test_ptr, 0, rename(tmp_ptr, wlm_settings_path(s)));
    BS_TEST_VERIFY_TRUE(test_ptr, wlm_settings_handle_fd(s));
    BS_TEST_VERIFY_STREQ(test_ptr, "w", wlm_settings_string(s, "K", "d"));
    BS_TEST_VERIFY_FALSE(test_ptr, wlm_settings_handle_fd(s));

    // Deleted: Defaults.
    unlink(wlm_settings_path(s));
    BS_TEST_VERIFY_TRUE(test_ptr, wlm_settings_handle_fd(s));
    BS_TEST_VERIFY_STREQ(test_ptr, "d", wlm_settings_string(s, "K", "d"));

    wlm_settings_destroy(s);
    unlink(other_ptr);
}

/* ------------------------------------------------------------------------- */
/** Themes, and colors falling back to the DockApp's own. */
void test_theme(bs_test_t *test_ptr)
{
    const char *dir_ptr = bs_test_temp_path(test_ptr, NULL);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dir_ptr);
    const char *path_ptr = bs_test_temp_path(test_ptr, "app.plist");
    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, _test_write(
        path_ptr, "{ Theme = Amber; }"));
    wlm_settings_t *s = wlm_settings_create("app", dir_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, s);

    const wlm_theme_t own = { .name_ptr = "Default", .led = 0xff112233,
                              .background = 0xff000001 };
    const wlm_theme_t *t = wlm_settings_theme(s, &own);
    BS_TEST_VERIFY_STREQ(test_ptr, "Amber", t->name_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, wlm_theme_find("amber")->led, t->led);

    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, _test_write(
        path_ptr, "{ Theme = Unknown; }"));
    wlm_settings_reload(s);
    t = wlm_settings_theme(s, &own);
    BS_TEST_VERIFY_EQ(test_ptr, 0xff112233, t->led);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlm_theme_find("Default"));

    BS_TEST_VERIFY_EQ(test_ptr, 0xff000000,
                      wlm_theme_blend(0xff000000, 0xffffffff, 0));
    BS_TEST_VERIFY_EQ(test_ptr, 0xffffffff,
                      wlm_theme_blend(0xff000000, 0xffffffff, 255));
    BS_TEST_VERIFY_EQ(test_ptr, 0xff808080,
                      wlm_theme_blend(0xff000000, 0xffffffff, 128));

    wlm_settings_destroy(s);
    unlink(path_ptr);
}

/* == End of wlm_settings.c ================================================ */
