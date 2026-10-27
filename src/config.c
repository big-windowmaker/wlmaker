/* ========================================================================= */
/**
 * @file config.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2023 Google LLC
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
 * Configurables for wlmaker. Currently, this file lists hardcoded entities,
 * and mainly serves as a catalog about which entities should be dynamically
 * configurable.
 */

#include "config.h"

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_keyboard.h>
#undef WLR_USE_UNSTABLE

#include "../share/theme.h"  // IWYU pragma: keep
#include "default_configuration.h"
#include "default_state.h"
#include "input/cursor.h"

/* == Declarations ========================================================= */

static bspl_object_t *_wlmaker_plist_load(
    const char *name_ptr,
    const char *fname_ptr,
    const uint8_t *default_data_ptr,
    size_t default_data_size);
static bspl_dict_t *_wlmaker_config_from_plist(const char *fname_ptr);

/* == Data ================================================================= */

/** Descriptor for decoding the "Clip" dictionary. */
static const bspl_desc_t _wlmaker_clip_style_desc[] = {
    BSPL_DESC_DICT(
        "Font", true, wlmaker_config_clip_style_t, font, font,
        wlmtk_style_font_desc),
    BSPL_DESC_ARGB32(
        "TextColor", true, wlmaker_config_clip_style_t,
        text_color, text_color, 0),
    BSPL_DESC_SENTINEL()
};

/** Descriptor for the task list's style. Declared in task_list.h; defined
 * here, so that loading the config does not require the task list (and,
 * through it, the server, XWayland and the input devices). */
const bspl_desc_t wlmaker_task_list_style_desc[] = {
    BSPL_DESC_CUSTOM(
        "Fill", true, struct wlmaker_task_list_style, fill, fill,
        wlmtk_style_decode_fill, NULL, NULL, NULL),
    BSPL_DESC_DICT(
        "Font", true, struct wlmaker_task_list_style, font, font,
        wlmtk_style_font_desc),
    BSPL_DESC_ARGB32(
        "TextColor", true, struct wlmaker_task_list_style,
        text_color, text_color, 0),
    BSPL_DESC_SENTINEL()
};

/** Desciptor for decoding the style information from a plist. */
const bspl_desc_t wlmaker_config_style_desc[] = {
    BSPL_DESC_ARGB32(
        "BackgroundColor", true, wlmaker_config_style_t,
        desktop.background_color, desktop.background_color, 0),
    BSPL_DESC_DICT(
        "Tile", true, wlmaker_config_style_t, tile, tile,
        wlmtk_tile_style_desc),
    BSPL_DESC_DICT(
        "Dock", true, wlmaker_config_style_t, dock, dock,
        wlmtk_dock_style_desc),
    BSPL_DESC_CUSTOM(
        "Window", true, wlmaker_config_style_t,
        window_style_ptr, has_window_style,
        wlmtk_window_style_decode, NULL,
        wlmtk_window_style_decode_init,
        wlmtk_window_style_decode_fini),
    BSPL_DESC_CUSTOM(
        "Menu", true, wlmaker_config_style_t,
        menu_style_ptr, has_menu_style,
        wlmtk_menu_style_decode, NULL,
        wlmtk_menu_style_decode_init,
        wlmtk_menu_style_decode_fini),
    BSPL_DESC_DICT(
        "TaskList", true, wlmaker_config_style_t, task_list, task_list,
        wlmaker_task_list_style_desc),
    BSPL_DESC_DICT(
        "Clip", true, wlmaker_config_style_t, clip, clip,
        _wlmaker_clip_style_desc),
    BSPL_DESC_DICT(
        "Cursor", true, wlmaker_config_style_t, cursor, cursor,
        wlmim_cursor_style_desc),
    BSPL_DESC_SENTINEL()
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bspl_object_t *wlmaker_config_object_load(
    wlm_util_files_t *files_ptr,
    const char *name_ptr,
    const char *arg_fname_ptr,
    const char *xdg_config_fname_ptr,
    const uint8_t *default_data_ptr,
    size_t default_data_size)
{
    char *fname_ptr = NULL;
    if (NULL == arg_fname_ptr && NULL != files_ptr) {
        fname_ptr = wlm_util_files_xdg_config_find(
            files_ptr, xdg_config_fname_ptr, S_IFREG);
    }

    bspl_object_t *object_ptr = _wlmaker_plist_load(
        name_ptr,
        NULL != arg_fname_ptr ? arg_fname_ptr : fname_ptr,
        default_data_ptr,
        default_data_size);
    if (NULL != fname_ptr) free(fname_ptr);
    return object_ptr;
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_config_load(
    wlm_util_files_t *files_ptr,
    const char *fname_ptr)
{
    return BS_ASSERT_NOTNULL(
        bspl_dict_from_object(
            wlmaker_config_object_load(
                files_ptr,
                "wlmaker config",
                fname_ptr,
                "Config.plist",
                embedded_binary_default_configuration_data,
                embedded_binary_default_configuration_size)));
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_state_load(
    wlm_util_files_t *files_ptr,
    const char *fname_ptr)
{
    return BS_ASSERT_NOTNULL(
        bspl_dict_from_object(
            wlmaker_config_object_load(
                files_ptr,
                "wlmaker state",
                fname_ptr,
                "State.plist",
                embedded_binary_default_state_data,
                embedded_binary_default_state_size)));
}

/* ------------------------------------------------------------------------- */
bspl_dict_t *wlmaker_state_load_defaults(
    wlm_util_files_t *files_ptr,
    const char *fname_ptr)
{
#ifndef WLMAKER_DEFAULT_STATE_FILE
#define WLMAKER_DEFAULT_STATE_FILE ""
#endif
    const char *candidates[] = { fname_ptr, WLMAKER_DEFAULT_STATE_FILE };
    char *found_ptr = NULL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (NULL != candidates[i] && '\0' != *candidates[i] &&
            bs_file_realpath_is(candidates[i], S_IFREG)) {
            found_ptr = logged_strdup(candidates[i]);
            break;
        }
    }
    if (NULL == found_ptr && NULL != files_ptr) {
        found_ptr = wlm_util_files_xdg_config_find_system(
            files_ptr, "State.plist", S_IFREG);
    }

    bspl_object_t *o = _wlmaker_plist_load(
        "default wlmaker state",
        found_ptr,
        embedded_binary_default_state_data,
        embedded_binary_default_state_size);
    if (NULL != found_ptr) free(found_ptr);
    bspl_dict_t *dict_ptr = bspl_dict_from_object(o);
    if (NULL == dict_ptr && NULL != o) bspl_object_unref(o);
    return dict_ptr;
}

/* ------------------------------------------------------------------------- */
char *wlmaker_state_fname(wlm_util_files_t *files_ptr, const char *fname_ptr)
{
    if (NULL != fname_ptr) return logged_strdup(fname_ptr);
    if (NULL == files_ptr) return NULL;
    return wlm_util_files_xdg_config_fname(files_ptr, "State.plist");
}

/** Arguments for @ref _wlmaker_state_copy_entry. */
typedef struct {
    /** Dict to copy into. */
    bspl_dict_t               *dest_dict_ptr;
    /** Dict with the values that replace the original ones. Or NULL. */
    bspl_dict_t               *replace_dict_ptr;
    /** A key that gets skipped. Or NULL. */
    const char                *skip_key_ptr;
} _wlmaker_state_copy_arg_t;

/** Copies an entry into a dict, unless it gets replaced or skipped. */
static bool _wlmaker_state_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *userdata_ptr)
{
    _wlmaker_state_copy_arg_t *arg_ptr = userdata_ptr;
    if (NULL != arg_ptr->skip_key_ptr &&
        0 == strcmp(key_ptr, arg_ptr->skip_key_ptr)) return true;
    if (NULL != arg_ptr->replace_dict_ptr &&
        NULL != bspl_dict_get(arg_ptr->replace_dict_ptr, key_ptr)) return true;
    return bspl_dict_add(arg_ptr->dest_dict_ptr, key_ptr, object_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_state_store_section(
    wlm_util_files_t *files_ptr,
    const char *fname_ptr,
    const char *section_ptr,
    bspl_dict_t *values_dict_ptr)
{
    bool rv = false;
    bspl_dict_t *state_dict_ptr = NULL, *new_state_dict_ptr = NULL;
    bspl_dict_t *new_section_dict_ptr = NULL;
    char *target_fname_ptr = wlmaker_state_fname(files_ptr, fname_ptr);
    if (NULL == target_fname_ptr) return false;

    bspl_object_t *o = wlmaker_config_object_load(
        files_ptr, "wlmaker state", fname_ptr, "State.plist",
        embedded_binary_default_state_data,
        embedded_binary_default_state_size);
    state_dict_ptr = bspl_dict_from_object(o);
    if (NULL == state_dict_ptr) {
        if (NULL != o) bspl_object_unref(o);
        goto done;
    }

    // The section: Its values, then all others it had.
    new_section_dict_ptr = bspl_dict_create();
    if (NULL == new_section_dict_ptr) goto done;
    _wlmaker_state_copy_arg_t arg = {
        .dest_dict_ptr = new_section_dict_ptr,
        .replace_dict_ptr = NULL,
        .skip_key_ptr = NULL };
    if (!bspl_dict_foreach(values_dict_ptr, _wlmaker_state_copy_entry, &arg)) {
        goto done;
    }
    bspl_dict_t *section_dict_ptr = bspl_dict_get_dict(
        state_dict_ptr, section_ptr);
    arg.replace_dict_ptr = values_dict_ptr;
    if (NULL != section_dict_ptr &&
        !bspl_dict_foreach(section_dict_ptr, _wlmaker_state_copy_entry, &arg)) {
        goto done;
    }

    // The state: All as it was, with the section replaced.
    new_state_dict_ptr = bspl_dict_create();
    if (NULL == new_state_dict_ptr ||
        !bspl_dict_add(new_state_dict_ptr, section_ptr,
                       bspl_object_from_dict(new_section_dict_ptr))) goto done;
    arg = (_wlmaker_state_copy_arg_t){
        .dest_dict_ptr = new_state_dict_ptr,
        .replace_dict_ptr = NULL,
        .skip_key_ptr = section_ptr };
    if (!bspl_dict_foreach(state_dict_ptr, _wlmaker_state_copy_entry, &arg)) {
        goto done;
    }

    bs_dynbuf_t dynbuf = {};
    if (!bs_dynbuf_init(&dynbuf, 4096, SIZE_MAX)) goto done;
    rv = bspl_object_write(bspl_object_from_dict(new_state_dict_ptr), &dynbuf) &&
        bs_dynbuf_write_file(&dynbuf, target_fname_ptr, S_IRUSR | S_IWUSR);
    bs_dynbuf_fini(&dynbuf);
    if (!rv) {
        bs_log(BS_WARNING, "Failed to store state in \"%s\"",
               target_fname_ptr);
    }

done:
    if (NULL != new_state_dict_ptr) bspl_dict_unref(new_state_dict_ptr);
    if (NULL != new_section_dict_ptr) bspl_dict_unref(new_section_dict_ptr);
    if (NULL != state_dict_ptr) bspl_dict_unref(state_dict_ptr);
    free(target_fname_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_theme_load(
    wlm_util_files_t *files_ptr,
    const char *arg_fname_ptr,
    wlmaker_config_style_t *style_ptr)
{
    char *fname_ptr = NULL;
    if (NULL != files_ptr) {
        fname_ptr = wlm_util_files_xdg_data_find(
            files_ptr, "Themes/Default.plist", S_IFREG);
    }

    bspl_object_t *o = _wlmaker_plist_load(
        "Theme",
        NULL != arg_fname_ptr ? arg_fname_ptr : fname_ptr,
        embedded_binary_theme_data,
        embedded_binary_theme_size);
    if (NULL != fname_ptr) free(fname_ptr);

    bspl_dict_t *style_dict_ptr = bspl_dict_from_object(o);
    if (NULL == style_dict_ptr) {
        bspl_object_unref(o);
        return NULL;
    }

    wlmaker_config_style_t tmp_style = {};
    bool rv = bspl_decode_dict(
        style_dict_ptr,
        wlmaker_config_style_desc,
        &tmp_style);
    bspl_dict_unref(style_dict_ptr);

    if (!rv || NULL == tmp_style.window_style_ptr) return false;

    bspl_decoded_destroy(wlmaker_config_style_desc, style_ptr);
    *style_ptr = tmp_style;
    return true;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Loads a plist object from the file or the default data. */
bspl_object_t *_wlmaker_plist_load(
    const char *name_ptr,
    const char *fname_ptr,
    const uint8_t *default_data_ptr,
    size_t default_data_size)
{
    if (NULL != fname_ptr) {
        bs_log(BS_INFO, "Loading %s plist from file \"%s\"",
               name_ptr, fname_ptr);
        bspl_object_t *object_ptr = bspl_create_object_from_plist_file(
            fname_ptr);
        if (NULL == object_ptr) {
            bs_log(BS_ERROR,
                   "Failed bspl_create_object_from_plist(\"%s\") for %s",
                   fname_ptr, name_ptr);
        }
        return object_ptr;
    }

    if (NULL == default_data_ptr) return NULL;

    bs_log(BS_INFO, "Using compiled-in data for %s plist.", name_ptr);
    return bspl_create_object_from_plist_data(
        default_data_ptr, default_data_size);
}

/* ------------------------------------------------------------------------- */
/** Loads a plist dict from fname_ptr. Returns NULL on error. */
bspl_dict_t *_wlmaker_config_from_plist(const char *fname_ptr)
{
    bspl_object_t *obj_ptr = bspl_create_object_from_plist_file(fname_ptr);
    if (NULL == obj_ptr) {
        bs_log(BS_ERROR, "Failed bspl_create_object_from_plist(%s)",
               fname_ptr);
        return NULL;
    }

    bspl_dict_t *dict_ptr = bspl_dict_from_object(obj_ptr);
    if (NULL == dict_ptr) {
        bs_log(BS_ERROR, "Not a plist dict in %s", fname_ptr);
        bspl_object_unref(obj_ptr);
        return NULL;
    }

    return dict_ptr;
}

/* == Unit tests =========================================================== */

static void test_embedded(bs_test_t *test_ptr);
static void test_file(bs_test_t *test_ptr);
static void test_style_file(bs_test_t *test_ptr);
static void test_state_store(bs_test_t *test_ptr);
static void test_state_defaults(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t wlmaker_config_test_cases[] = {
    { true, "embedded", test_embedded },
    { true, "file", test_file },
    { true, "style_file", test_style_file },
    { true, "state_store", test_state_store },
    { true, "state_defaults", test_state_defaults },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_config_test_set = BS_TEST_SET(
    true, "config", wlmaker_config_test_cases);

/* ------------------------------------------------------------------------- */
/** Verifies that the embedded config loads. */
// TODO(kaeser@gubbe.ch): For now, this just verifies that the configuration
// **parses**. There is no further verification of the dict contents. Would
// be great to extend this.
void test_embedded(bs_test_t *test_ptr)
{
    bspl_object_t *obj_ptr;

    obj_ptr = bspl_create_object_from_plist_data(
        embedded_binary_default_configuration_data,
        embedded_binary_default_configuration_size);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_from_object(obj_ptr));
    bspl_object_unref(obj_ptr);

    obj_ptr = bspl_create_object_from_plist_data(
        embedded_binary_default_state_data,
        embedded_binary_default_state_size);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_from_object(obj_ptr));
    bspl_object_unref(obj_ptr);

    obj_ptr = bspl_create_object_from_plist_data(
        embedded_binary_theme_data,
        embedded_binary_theme_size);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_from_object(obj_ptr));
    bspl_object_unref(obj_ptr);
}

/* ------------------------------------------------------------------------- */
/** Verifies that the (example) config files are loading. */
// TODO(kaeser@gubbe.ch): For now, this just verifies that the configuration
// file **parses**. There is no further verification of the dict contents.
// Would be great to extend this.
void test_file(bs_test_t *test_ptr)
{
    bspl_dict_t *dict_ptr;

#ifndef WLMAKER_SOURCE_DIR
#error "Missing definition of WLMAKER_SOURCE_DIR!"
#endif
    dict_ptr = _wlmaker_config_from_plist(
        WLMAKER_SOURCE_DIR "/etc/Config.plist");
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    bspl_dict_unref(dict_ptr);

    dict_ptr = _wlmaker_config_from_plist(
        WLMAKER_SOURCE_DIR "/etc/ExampleConfig.plist");
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    bspl_dict_unref(dict_ptr);

    dict_ptr = _wlmaker_config_from_plist(
        WLMAKER_SOURCE_DIR "/etc/HomeConfig.plist");
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    bspl_dict_unref(dict_ptr);

    dict_ptr = _wlmaker_config_from_plist(
        WLMAKER_SOURCE_DIR "/etc/State.plist");
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    bspl_dict_unref(dict_ptr);
}

/* ------------------------------------------------------------------------- */
/** Loads and decodes the style file. */
void test_style_file(bs_test_t *test_ptr)
{
    wlmaker_config_style_t cs = {};

#ifndef WLMAKER_SOURCE_DIR
#error "Missing definition of WLMAKER_SOURCE_DIR!"
#endif

    const char *f = WLMAKER_SOURCE_DIR "/share/Themes/Default.plist";
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_theme_load(NULL, f, &cs));
    bspl_decoded_destroy(wlmaker_config_style_desc, &cs);

    f = WLMAKER_SOURCE_DIR "/share/Themes/Debian.plist";
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_theme_load(NULL, f, &cs));
    bspl_decoded_destroy(wlmaker_config_style_desc, &cs);
}

/* ------------------------------------------------------------------------- */
/** Stores values into a section, keeping the rest of the state file. */
void test_state_store(bs_test_t *test_ptr)
{
    const char *fname_ptr = bs_test_temp_path(test_ptr, "State.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fname_ptr);
    bs_dynbuf_t dynbuf = {};
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr, bs_dynbuf_init(&dynbuf, 4096, SIZE_MAX));
    const char *initial = "{ Dock = { Edge = RIGHT; Anchor = TOP; "
        "Launchers = (); }; Workspaces = ( { Name = Main; } ); }";
    BS_TEST_VERIFY_TRUE(test_ptr, bs_dynbuf_append(
                            &dynbuf, initial, strlen(initial)));
    BS_TEST_VERIFY_TRUE(test_ptr, bs_dynbuf_write_file(
                            &dynbuf, fname_ptr, S_IRUSR | S_IWUSR));
    bs_dynbuf_fini(&dynbuf);

    bspl_object_t *v = bspl_create_object_from_plist_string(
        "{ Edge = LEFT; Margin = 3; }");
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_state_store_section(
                            NULL, fname_ptr, "Dock", bspl_dict_from_object(v)));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_state_store_section(
                            NULL, fname_ptr, "Clip", bspl_dict_from_object(v)));
    bspl_object_unref(v);

    bspl_dict_t *d = bspl_dict_from_object(
        bspl_create_object_from_plist_file(fname_ptr));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    bspl_dict_t *dock_ptr = bspl_dict_get_dict(d, "Dock");
    BS_TEST_VERIFY_STREQ(
        test_ptr, "LEFT", bspl_dict_get_string_value(dock_ptr, "Edge"));
    BS_TEST_VERIFY_STREQ(
        test_ptr, "TOP", bspl_dict_get_string_value(dock_ptr, "Anchor"));
    BS_TEST_VERIFY_STREQ(
        test_ptr, "3", bspl_dict_get_string_value(dock_ptr, "Margin"));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_get_array(dock_ptr, "Launchers"));
    BS_TEST_VERIFY_STREQ(test_ptr, "LEFT", bspl_dict_get_string_value(
                             bspl_dict_get_dict(d, "Clip"), "Edge"));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_get_array(d, "Workspaces"));
    bspl_dict_unref(d);
    unlink(fname_ptr);
    char *old_ptr = bs_strdupf("%s.old", fname_ptr);
    if (NULL != old_ptr) {
        unlink(old_ptr);
        free(old_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Loads defaults: From the given file, otherwise compiled-in. */
void test_state_defaults(bs_test_t *test_ptr)
{
    bspl_dict_t *d = wlmaker_state_load_defaults(
        NULL, WLMAKER_SOURCE_DIR "/etc/State.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_get_array(
                           bspl_dict_get_dict(d, "Dock"), "Launchers"));
    bspl_dict_unref(d);

    // A missing file: Falls back (here, to the compiled-in default).
    d = wlmaker_state_load_defaults(NULL, "/nonexistent/State.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_get_dict(d, "Dock"));
    bspl_dict_unref(d);
}

/* == End of config.c ====================================================== */
