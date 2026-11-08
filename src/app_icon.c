/* ========================================================================= */
/**
 * @file app_icon.c
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

#include "app_icon.h"

#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <librsvg/rsvg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

/* == Declarations ========================================================= */

/** A cached icon (or a miss). */
typedef struct {
    /** Node in @ref _cache. */
    bs_dllist_node_t          dlnode;
    /** "app_id\ninstance\nsize". */
    char                      *key_ptr;
    /** The icon, or NULL if there is none. */
    cairo_surface_t           *surface_ptr;
} _app_icon_entry_t;

/** Entries of the cache. */
static bs_dllist_t _cache;
/** At most this many entries: The oldest go first. */
static const size_t _cache_max = 96;

/** Desktop entries indexed by StartupWMClass (lower case) -> Icon. */
typedef struct {
    /** Node in @ref _wmclass_index. */
    bs_dllist_node_t          dlnode;
    /** StartupWMClass, lower case. */
    char                      *wmclass_ptr;
    /** Icon= of the entry. */
    char                      *icon_ptr;
} _wmclass_entry_t;

/** The index, built when first needed and at most once a minute. */
static bs_dllist_t _wmclass_index;
/** When the index was built (monotonic seconds), 0 if never. */
static time_t _wmclass_index_time;

static char *_desktop_icon(const char *name_ptr);
static char *_find_icon_file(const char *icon_ptr, int size);
static cairo_surface_t *_load(const char *path_ptr, int size);
static void _data_dirs(char dirs[][PATH_MAX], size_t *count_ptr, size_t max);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
cairo_surface_t *wlmaker_app_icon_get(
    const char *app_id_ptr,
    const char *instance_ptr,
    int size)
{
    if ((NULL == app_id_ptr || '\0' == *app_id_ptr) &&
        (NULL == instance_ptr || '\0' == *instance_ptr)) return NULL;
    char key[512];
    snprintf(key, sizeof(key), "%s\n%s\n%d",
             app_id_ptr ? app_id_ptr : "", instance_ptr ? instance_ptr : "",
             size);
    for (bs_dllist_node_t *d = _cache.head_ptr; NULL != d; d = d->next_ptr) {
        _app_icon_entry_t *e = BS_CONTAINER_OF(d, _app_icon_entry_t, dlnode);
        if (0 == strcmp(e->key_ptr, key)) return e->surface_ptr;
    }

    // The desktop entry's icon; without one, the names themselves.
    const char *names[] = { app_id_ptr, instance_ptr };
    char *icon_ptr = NULL;
    for (size_t i = 0; i < 2 && NULL == icon_ptr; ++i) {
        if (NULL != names[i] && '\0' != *names[i]) {
            icon_ptr = _desktop_icon(names[i]);
        }
    }
    char *path_ptr = NULL;
    if (NULL != icon_ptr) {
        path_ptr = _find_icon_file(icon_ptr, size);
        free(icon_ptr);
    }
    for (size_t i = 0; i < 2 && NULL == path_ptr; ++i) {
        if (NULL == names[i] || '\0' == *names[i]) continue;
        char lower[256];
        snprintf(lower, sizeof(lower), "%s", names[i]);
        for (char *c = lower; *c; ++c) *c = (char)tolower((unsigned char)*c);
        path_ptr = _find_icon_file(lower, size);
    }

    _app_icon_entry_t *e = logged_calloc(1, sizeof(_app_icon_entry_t));
    if (NULL == e) {
        free(path_ptr);
        return NULL;
    }
    e->key_ptr = logged_strdup(key);
    if (NULL != path_ptr) e->surface_ptr = _load(path_ptr, size);
    free(path_ptr);
    bs_dllist_push_front(&_cache, &e->dlnode);
    while (bs_dllist_size(&_cache) > _cache_max) {
        _app_icon_entry_t *o = BS_CONTAINER_OF(
            _cache.tail_ptr, _app_icon_entry_t, dlnode);
        bs_dllist_remove(&_cache, &o->dlnode);
        if (NULL != o->surface_ptr) cairo_surface_destroy(o->surface_ptr);
        free(o->key_ptr);
        free(o);
    }
    return e->surface_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_app_icon_flush(void)
{
    bs_dllist_node_t *d;
    while (NULL != (d = bs_dllist_pop_front(&_cache))) {
        _app_icon_entry_t *e = BS_CONTAINER_OF(d, _app_icon_entry_t, dlnode);
        if (NULL != e->surface_ptr) cairo_surface_destroy(e->surface_ptr);
        free(e->key_ptr);
        free(e);
    }
    while (NULL != (d = bs_dllist_pop_front(&_wmclass_index))) {
        _wmclass_entry_t *w = BS_CONTAINER_OF(d, _wmclass_entry_t, dlnode);
        free(w->wmclass_ptr);
        free(w->icon_ptr);
        free(w);
    }
    _wmclass_index_time = 0;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** XDG data directories: The user's first, then the system's. */
void _data_dirs(char dirs[][PATH_MAX], size_t *count_ptr, size_t max)
{
    *count_ptr = 0;
    const char *home_ptr = getenv("HOME");
    const char *dh_ptr = getenv("XDG_DATA_HOME");
    if (NULL != dh_ptr && '/' == *dh_ptr) {
        snprintf(dirs[(*count_ptr)++], PATH_MAX, "%s", dh_ptr);
    } else if (NULL != home_ptr) {
        snprintf(dirs[(*count_ptr)++], PATH_MAX, "%s/.local/share", home_ptr);
    }
    const char *dd_ptr = getenv("XDG_DATA_DIRS");
    if (NULL == dd_ptr || '\0' == *dd_ptr) dd_ptr = "/usr/local/share:/usr/share";
    while (*dd_ptr && *count_ptr < max) {
        size_t len = strcspn(dd_ptr, ":");
        if (0 < len && '/' == *dd_ptr && len < PATH_MAX) {
            snprintf(dirs[(*count_ptr)++], PATH_MAX, "%.*s", (int)len, dd_ptr);
        }
        dd_ptr += len;
        if (':' == *dd_ptr) ++dd_ptr;
    }
}

/* ------------------------------------------------------------------------- */
/** Value of `Icon=` (or `StartupWMClass=` with `key`) in a desktop entry. */
static char *_desktop_value(const char *path_ptr, const char *key_ptr)
{
    FILE *f = fopen(path_ptr, "r");
    if (NULL == f) return NULL;
    char line[1024];
    bool in_entry = false;
    char *value_ptr = NULL;
    size_t klen = strlen(key_ptr);
    while (NULL != fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if ('[' == line[0]) {
            in_entry = (0 == strcmp(line, "[Desktop Entry]"));
            continue;
        }
        if (in_entry && 0 == strncmp(line, key_ptr, klen) &&
            '=' == line[klen]) {
            const char *v = &line[klen + 1];
            while (' ' == *v) ++v;
            if ('\0' != *v) value_ptr = logged_strdup(v);
            break;
        }
    }
    fclose(f);
    return value_ptr;
}

/* ------------------------------------------------------------------------- */
/** Builds the StartupWMClass index of all desktop entries. */
static void _build_wmclass_index(void)
{
    char dirs[16][PATH_MAX];
    size_t n;
    _data_dirs(dirs, &n, 16);
    for (size_t i = 0; i < n; ++i) {
        char apps[PATH_MAX + 16];
        snprintf(apps, sizeof(apps), "%s/applications", dirs[i]);
        DIR *d = opendir(apps);
        if (NULL == d) continue;
        struct dirent *de;
        while (NULL != (de = readdir(d))) {
            size_t len = strlen(de->d_name);
            if (len < 9 || 0 != strcmp(de->d_name + len - 8, ".desktop")) {
                continue;
            }
            char path[PATH_MAX + 288];
            snprintf(path, sizeof(path), "%s/%s", apps, de->d_name);
            char *wmclass_ptr = _desktop_value(path, "StartupWMClass");
            if (NULL == wmclass_ptr) continue;
            char *icon_ptr = _desktop_value(path, "Icon");
            if (NULL == icon_ptr) {
                free(wmclass_ptr);
                continue;
            }
            for (char *c = wmclass_ptr; *c; ++c) {
                *c = (char)tolower((unsigned char)*c);
            }
            _wmclass_entry_t *w = logged_calloc(1, sizeof(_wmclass_entry_t));
            if (NULL == w) {
                free(wmclass_ptr);
                free(icon_ptr);
                continue;
            }
            w->wmclass_ptr = wmclass_ptr;
            w->icon_ptr = icon_ptr;
            bs_dllist_push_back(&_wmclass_index, &w->dlnode);
        }
        closedir(d);
    }
}

/* ------------------------------------------------------------------------- */
/** The `Icon=` of the desktop entry for `name_ptr` (app ID or WM_CLASS). */
char *_desktop_icon(const char *name_ptr)
{
    char dirs[16][PATH_MAX];
    size_t n;
    _data_dirs(dirs, &n, 16);
    char lower[256];
    snprintf(lower, sizeof(lower), "%s", name_ptr);
    for (char *c = lower; *c; ++c) *c = (char)tolower((unsigned char)*c);
    const char *candidates[] = { name_ptr, lower };
    for (size_t c = 0; c < 2; ++c) {
        for (size_t i = 0; i < n; ++i) {
            char path[PATH_MAX + 256];
            snprintf(path, sizeof(path), "%s/applications/%s.desktop",
                     dirs[i], candidates[c]);
            char *icon_ptr = _desktop_value(path, "Icon");
            if (NULL != icon_ptr) return icon_ptr;
        }
    }

    // X11 programs: The entry naming the class in StartupWMClass.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (0 == _wmclass_index_time || now.tv_sec - _wmclass_index_time > 60) {
        bs_dllist_node_t *d;
        while (NULL != (d = bs_dllist_pop_front(&_wmclass_index))) {
            _wmclass_entry_t *w = BS_CONTAINER_OF(d, _wmclass_entry_t, dlnode);
            free(w->wmclass_ptr);
            free(w->icon_ptr);
            free(w);
        }
        _build_wmclass_index();
        _wmclass_index_time = now.tv_sec ? now.tv_sec : 1;
    }
    for (bs_dllist_node_t *d = _wmclass_index.head_ptr; NULL != d;
         d = d->next_ptr) {
        _wmclass_entry_t *w = BS_CONTAINER_OF(d, _wmclass_entry_t, dlnode);
        if (0 == strcmp(w->wmclass_ptr, lower)) {
            return logged_strdup(w->icon_ptr);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Whether `path_ptr` is a regular file. */
static bool _is_file(const char *path_ptr)
{
    struct stat st;
    return 0 == stat(path_ptr, &st) && S_ISREG(st.st_mode);
}

/* ------------------------------------------------------------------------- */
/** The user's icon theme (GTK 3 settings.ini), or NULL. */
static char *_user_icon_theme(void)
{
    char path[PATH_MAX];
    const char *ch_ptr = getenv("XDG_CONFIG_HOME");
    const char *home_ptr = getenv("HOME");
    if (NULL != ch_ptr && '/' == *ch_ptr) {
        snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", ch_ptr);
    } else if (NULL != home_ptr) {
        snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini",
                 home_ptr);
    } else {
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (NULL == f) return NULL;
    char line[512];
    char *theme_ptr = NULL;
    while (NULL != fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *k = line;
        while (' ' == *k || '\t' == *k) ++k;
        if (0 != strncmp(k, "gtk-icon-theme-name", 19)) continue;
        char *v = strchr(k, '=');
        if (NULL == v) continue;
        ++v;
        while (' ' == *v || '"' == *v) ++v;
        v[strcspn(v, "\" ")] = '\0';
        if ('\0' != *v && NULL == strchr(v, '/')) theme_ptr = logged_strdup(v);
        break;
    }
    fclose(f);
    return theme_ptr;
}

/* ------------------------------------------------------------------------- */
/** Finds `icon_ptr` (a name or an absolute path) as a PNG or SVG file. */
char *_find_icon_file(const char *icon_ptr, int size)
{
    if ('/' == icon_ptr[0]) {
        return _is_file(icon_ptr) ? logged_strdup(icon_ptr) : NULL;
    }
    if (NULL != strchr(icon_ptr, '/')) return NULL;

    // Themes: The user's, the ones it inherits (one level), hicolor.
    char themes[6][128];
    size_t nthemes = 0;
    char *user_ptr = _user_icon_theme();
    if (NULL != user_ptr) {
        snprintf(themes[nthemes++], 128, "%s", user_ptr);
        free(user_ptr);
    }
    char dirs[16][PATH_MAX];
    size_t ndirs;
    _data_dirs(dirs, &ndirs, 16);
    if (0 < nthemes) {
        for (size_t i = 0; i < ndirs && nthemes < 5; ++i) {
            char path[PATH_MAX + 256];
            snprintf(path, sizeof(path), "%s/icons/%s/index.theme",
                     dirs[i], themes[0]);
            FILE *f = fopen(path, "r");
            if (NULL == f) continue;
            char line[512];
            while (NULL != fgets(line, sizeof(line), f)) {
                if (0 != strncmp(line, "Inherits=", 9)) continue;
                char *p = line + 9;
                p[strcspn(p, "\r\n")] = '\0';
                for (char *t = strtok(p, ","); NULL != t && nthemes < 5;
                     t = strtok(NULL, ",")) {
                    if (NULL == strchr(t, '/') && 0 != strcmp(t, "hicolor")) {
                        snprintf(themes[nthemes++], 128, "%s", t);
                    }
                }
                break;
            }
            fclose(f);
            break;
        }
    }
    snprintf(themes[nthemes++], 128, "hicolor");

    // Sizes: The asked one, the scalable, then larger and smaller ones.
    char sizes[10][16];
    size_t nsizes = 0;
    snprintf(sizes[nsizes++], 16, "%dx%d", size, size);
    snprintf(sizes[nsizes++], 16, "scalable");
    static const int others[] = { 96, 128, 256, 512, 48, 64, 32 };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        if (others[i] == size) continue;
        snprintf(sizes[nsizes++], 16, "%dx%d", others[i], others[i]);
    }

    static const char *exts[] = { "svg", "png" };
    for (size_t t = 0; t < nthemes; ++t) {
        for (size_t s = 0; s < nsizes; ++s) {
            for (size_t i = 0; i < ndirs; ++i) {
                for (size_t e = 0; e < 2; ++e) {
                    char path[PATH_MAX + 512];
                    snprintf(path, sizeof(path), "%s/icons/%s/%s/apps/%s.%s",
                             dirs[i], themes[t], sizes[s], icon_ptr, exts[e]);
                    if (_is_file(path)) return logged_strdup(path);
                }
            }
        }
    }
    for (size_t i = 0; i < ndirs; ++i) {
        for (size_t e = 0; e < 2; ++e) {
            char path[PATH_MAX + 512];
            snprintf(path, sizeof(path), "%s/pixmaps/%s.%s",
                     dirs[i], icon_ptr, exts[e]);
            if (_is_file(path)) return logged_strdup(path);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Loads a PNG or SVG file into a `size`x`size` surface, centered. */
cairo_surface_t *_load(const char *path_ptr, int size)
{
    cairo_surface_t *out_ptr = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    if (CAIRO_STATUS_SUCCESS != cairo_surface_status(out_ptr)) {
        cairo_surface_destroy(out_ptr);
        return NULL;
    }
    cairo_t *cr = cairo_create(out_ptr);
    bool ok = false;
    size_t len = strlen(path_ptr);
    if (4 < len && 0 == strcasecmp(path_ptr + len - 4, ".svg")) {
        GError *error_ptr = NULL;
        RsvgHandle *h = rsvg_handle_new_from_file(path_ptr, &error_ptr);
        if (NULL != h) {
            RsvgRectangle viewport = { .width = size, .height = size };
            ok = rsvg_handle_render_document(h, cr, &viewport, &error_ptr);
            g_object_unref(h);
        }
        if (NULL != error_ptr) {
            bs_log(BS_INFO, "Icon %s: %s", path_ptr, error_ptr->message);
            g_error_free(error_ptr);
        }
    } else {
        cairo_surface_t *png_ptr = cairo_image_surface_create_from_png(
            path_ptr);
        if (CAIRO_STATUS_SUCCESS == cairo_surface_status(png_ptr)) {
            int w = cairo_image_surface_get_width(png_ptr);
            int h = cairo_image_surface_get_height(png_ptr);
            if (0 < w && 0 < h) {
                double scale = (double)size / (w > h ? w : h);
                cairo_translate(cr, (size - w * scale) / 2.0,
                                (size - h * scale) / 2.0);
                cairo_scale(cr, scale, scale);
                cairo_set_source_surface(cr, png_ptr, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr),
                                         CAIRO_FILTER_GOOD);
                cairo_paint(cr);
                ok = true;
            }
        }
        cairo_surface_destroy(png_ptr);
    }
    cairo_destroy(cr);
    if (!ok) {
        cairo_surface_destroy(out_ptr);
        return NULL;
    }
    cairo_surface_flush(out_ptr);
    return out_ptr;
}

/* == Unit tests =========================================================== */

static void _app_icon_test(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _app_icon_test_cases[] = {
    { true, "lookup", _app_icon_test },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_app_icon_test_set = BS_TEST_SET(
    true, "app_icon", _app_icon_test_cases);

/* ------------------------------------------------------------------------- */
/** A desktop entry, a theme with an SVG icon; a WM_CLASS; a miss. */
void _app_icon_test(bs_test_t *test_ptr)
{
    char tmpl[] = "/tmp/wlm_app_icon_XXXXXX";
    char *root_ptr = mkdtemp(tmpl);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, root_ptr);
    char p[PATH_MAX + 128];
    snprintf(p, sizeof(p), "mkdir -p %s/share/applications "
             "%s/share/icons/hicolor/scalable/apps", root_ptr, root_ptr);
    BS_TEST_VERIFY_EQ_OR_RETURN(test_ptr, 0, system(p));
    snprintf(p, sizeof(p), "%s/share/applications/org.test.App.desktop",
             root_ptr);
    FILE *f = fopen(p, "w");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, f);
    fputs("[Desktop Entry]\nName=T\nIcon=test-icon\n"
          "StartupWMClass=TestClass\n", f);
    fclose(f);
    snprintf(p, sizeof(p),
             "%s/share/icons/hicolor/scalable/apps/test-icon.svg", root_ptr);
    f = fopen(p, "w");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, f);
    fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
          "<rect width=\"8\" height=\"8\" fill=\"#ff0000\"/></svg>", f);
    fclose(f);

    char *old_dirs = getenv("XDG_DATA_DIRS") ?
        logged_strdup(getenv("XDG_DATA_DIRS")) : NULL;
    char *old_home = getenv("XDG_DATA_HOME") ?
        logged_strdup(getenv("XDG_DATA_HOME")) : NULL;
    snprintf(p, sizeof(p), "%s/share", root_ptr);
    setenv("XDG_DATA_DIRS", p, 1);
    setenv("XDG_DATA_HOME", "/nonexistent", 1);
    wlmaker_app_icon_flush();

    cairo_surface_t *s = wlmaker_app_icon_get("org.test.App", NULL, 16);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, s);
    if (NULL != s) {
        uint32_t *px = (uint32_t*)cairo_image_surface_get_data(s);
        BS_TEST_VERIFY_EQ(test_ptr, 0xffff0000, px[8 * 16 + 8]);
    }
    // Cached: The same surface.
    BS_TEST_VERIFY_EQ(test_ptr, s, wlmaker_app_icon_get("org.test.App", NULL, 16));
    // X11: By StartupWMClass, any case.
    BS_TEST_VERIFY_NEQ(test_ptr, NULL,
                       wlmaker_app_icon_get("TESTCLASS", "testclass", 16));
    BS_TEST_VERIFY_EQ(test_ptr, NULL,
                      wlmaker_app_icon_get("no.such.app", NULL, 16));
    wlmaker_app_icon_flush();

    if (NULL != old_dirs) {
        setenv("XDG_DATA_DIRS", old_dirs, 1);
        free(old_dirs);
    } else {
        unsetenv("XDG_DATA_DIRS");
    }
    if (NULL != old_home) {
        setenv("XDG_DATA_HOME", old_home, 1);
        free(old_home);
    } else {
        unsetenv("XDG_DATA_HOME");
    }
    snprintf(p, sizeof(p), "rm -rf %s", root_ptr);
    BS_TEST_VERIFY_EQ(test_ptr, 0, system(p));
}

/* == End of app_icon.c ==================================================== */
