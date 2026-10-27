/* ========================================================================= */
/**
 * @file dockapp_prefs.c
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

#include "dockapp_prefs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "action.h"
#include "i18n.h"

/* == Declarations ========================================================= */

/** State of the dock-app's preferences. */
struct _wlmaker_dockapp_prefs_t {
    /** All of `Dockapps.plist`. */
    bspl_dict_t               *schema_dict_ptr;
    /** The dock-app's dict in it. */
    bspl_dict_t               *app_dict_ptr;
    /** The values stored in the preferences file. Maybe empty. */
    bspl_dict_t               *values_dict_ptr;
    /** Path of the preferences file. */
    char                      *fname_ptr;
};

/** Argument of a preference's item: What to store where. */
typedef struct {
    /** Path of the preferences file. */
    char                      *fname_ptr;
    /** The key. */
    char                      *key_ptr;
    /** The value. */
    char                      *value_ptr;
} _wlmaker_dockapp_prefs_store_arg_t;

/** Argument of an action's item: What to run. */
typedef struct {
    /** Runs the command. */
    wlmaker_server_t          *server_ptr;
    /** The command. */
    char                      *command_ptr;
} _wlmaker_dockapp_prefs_action_arg_t;

static bspl_dict_t *_wlmaker_dockapp_prefs_find(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    const char *key_ptr);
static bspl_array_t *_wlmaker_dockapp_prefs_choices(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    bspl_dict_t *pref_dict_ptr);
static bool _wlmaker_dockapp_prefs_add_option(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *menu_ptr,
    const char *title_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    const char *key_ptr,
    const char *value_ptr);
static bool _wlmaker_dockapp_prefs_add_choice(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *menu_ptr,
    const char *key_ptr,
    const char *current_ptr,
    const char *value_ptr,
    const char *title_ptr);
static void _wlmaker_dockapp_prefs_store_callback(void *ud_ptr);
static void _wlmaker_dockapp_prefs_store_arg_free(void *ud_ptr);
static void _wlmaker_dockapp_prefs_action_callback(void *ud_ptr);
static void _wlmaker_dockapp_prefs_action_arg_free(void *ud_ptr);
static int _wlmaker_dockapp_prefs_strcmp(const void *a_ptr, const void *b_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_dockapp_prefs_t *wlmaker_dockapp_prefs_load(
    wlm_util_files_t *files_ptr,
    const char *schema_fname_ptr,
    const char *prefs_fname_ptr,
    const char *name_ptr)
{
    char *found_ptr = NULL;
    if (NULL == schema_fname_ptr && NULL != files_ptr) {
        found_ptr = wlm_util_files_xdg_data_find(
            files_ptr, "Dockapps.plist", S_IFREG);
        schema_fname_ptr = found_ptr;
    }
    if (NULL == schema_fname_ptr || NULL == name_ptr) return NULL;

    bspl_object_t *o = bspl_create_object_from_plist_file(schema_fname_ptr);
    if (NULL != found_ptr) free(found_ptr);
    bspl_dict_t *schema_dict_ptr = bspl_dict_from_object(o);
    if (NULL == schema_dict_ptr) {
        if (NULL != o) bspl_object_unref(o);
        return NULL;
    }
    bspl_dict_t *app_dict_ptr = bspl_dict_get_dict(
        bspl_dict_get_dict(schema_dict_ptr, "Dockapps"), name_ptr);
    if (NULL == app_dict_ptr) {
        bspl_dict_unref(schema_dict_ptr);
        return NULL;
    }

    wlmaker_dockapp_prefs_t *prefs_ptr = logged_calloc(1, sizeof(*prefs_ptr));
    if (NULL == prefs_ptr) {
        bspl_dict_unref(schema_dict_ptr);
        return NULL;
    }
    prefs_ptr->schema_dict_ptr = schema_dict_ptr;
    prefs_ptr->app_dict_ptr = app_dict_ptr;

    if (NULL != prefs_fname_ptr) {
        prefs_ptr->fname_ptr = logged_strdup(prefs_fname_ptr);
    } else if (NULL != files_ptr) {
        char *n = bs_strdupf("%s.plist", name_ptr);
        if (NULL != n) {
            prefs_ptr->fname_ptr = wlm_util_files_xdg_config_fname(
                files_ptr, n);
            free(n);
        }
    }
    if (NULL == prefs_ptr->fname_ptr) {
        wlmaker_dockapp_prefs_destroy(prefs_ptr);
        return NULL;
    }

    struct stat st;
    if (0 == stat(prefs_ptr->fname_ptr, &st)) {
        o = bspl_create_object_from_plist_file(prefs_ptr->fname_ptr);
        prefs_ptr->values_dict_ptr = bspl_dict_from_object(o);
        if (NULL == prefs_ptr->values_dict_ptr && NULL != o) {
            bspl_object_unref(o);
        }
    }
    if (NULL == prefs_ptr->values_dict_ptr) {
        prefs_ptr->values_dict_ptr = bspl_dict_create();
    }
    if (NULL == prefs_ptr->values_dict_ptr) {
        wlmaker_dockapp_prefs_destroy(prefs_ptr);
        return NULL;
    }
    return prefs_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dockapp_prefs_destroy(wlmaker_dockapp_prefs_t *prefs_ptr)
{
    if (NULL != prefs_ptr->values_dict_ptr) {
        bspl_dict_unref(prefs_ptr->values_dict_ptr);
    }
    if (NULL != prefs_ptr->schema_dict_ptr) {
        bspl_dict_unref(prefs_ptr->schema_dict_ptr);
    }
    if (NULL != prefs_ptr->fname_ptr) free(prefs_ptr->fname_ptr);
    free(prefs_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapp_prefs_add_actions(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_server_t *server_ptr,
    wlmaker_context_menu_t *context_menu_ptr)
{
    bspl_array_t *actions_ptr = bspl_dict_get_array(
        prefs_ptr->app_dict_ptr, "Actions");
    if (NULL == actions_ptr) return true;

    for (size_t i = 0; i < bspl_array_size(actions_ptr); ++i) {
        bspl_dict_t *a_ptr = bspl_dict_from_object(
            bspl_array_at(actions_ptr, i));
        const char *title_ptr = bspl_dict_get_string_value(a_ptr, "Title");
        const char *cmd_ptr = bspl_dict_get_string_value(a_ptr, "Command");
        if (NULL == title_ptr || NULL == cmd_ptr) continue;

        char *expanded_ptr = wlmaker_dockapp_prefs_expand(prefs_ptr, cmd_ptr);
        if (NULL == expanded_ptr) return false;
        // Commands that end up empty (eg. 'Nothing' chosen): Left out.
        if (strspn(expanded_ptr, " \t") == strlen(expanded_ptr)) {
            free(expanded_ptr);
            continue;
        }
        _wlmaker_dockapp_prefs_action_arg_t *arg_ptr = logged_calloc(
            1, sizeof(*arg_ptr));
        if (NULL == arg_ptr) {
            free(expanded_ptr);
            return false;
        }
        arg_ptr->server_ptr = server_ptr;
        arg_ptr->command_ptr = expanded_ptr;
        if (!wlmaker_context_menu_add_option(
                context_menu_ptr, NULL, wlmaker_i18n(title_ptr),
                WLMTK_MENU_ITEM_INDICATOR_NONE, false,
                _wlmaker_dockapp_prefs_action_callback, arg_ptr,
                _wlmaker_dockapp_prefs_action_arg_free)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapp_prefs_add_preferences(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_context_menu_t *context_menu_ptr)
{
    bspl_array_t *list_ptr = bspl_dict_get_array(
        prefs_ptr->app_dict_ptr, "Preferences");
    if (NULL == list_ptr || 0 == bspl_array_size(list_ptr)) return true;

    wlmtk_menu_t *menu_ptr = wlmaker_context_menu_add_submenu(
        context_menu_ptr, NULL, wlmaker_i18n("Preferences"));
    if (NULL == menu_ptr) return false;

    for (size_t i = 0; i < bspl_array_size(list_ptr); ++i) {
        bspl_dict_t *p_ptr = bspl_dict_from_object(bspl_array_at(list_ptr, i));
        const char *key_ptr = bspl_dict_get_string_value(p_ptr, "Key");
        const char *title_ptr = bspl_dict_get_string_value(p_ptr, "Title");
        if (NULL == key_ptr || NULL == title_ptr) continue;
        const char *current_ptr = wlmaker_dockapp_prefs_value(
            prefs_ptr, key_ptr);
        const char *type_ptr = bspl_dict_get_string_value(p_ptr, "Type");

        if (NULL != type_ptr && 0 == strcmp(type_ptr, "Bool")) {
            bool on = false;
            if (NULL != current_ptr) {
                on = (0 == strcasecmp(current_ptr, "Yes") ||
                      0 == strcasecmp(current_ptr, "True") ||
                      0 == strcasecmp(current_ptr, "On") ||
                      0 == strcasecmp(current_ptr, "Enabled"));
            }
            if (!_wlmaker_dockapp_prefs_add_option(
                    prefs_ptr, context_menu_ptr, menu_ptr,
                    wlmaker_i18n(title_ptr),
                    WLMTK_MENU_ITEM_INDICATOR_CHECK, on,
                    key_ptr, on ? "No" : "Yes")) return false;
            continue;
        }

        // A choice: A submenu, with a radio button per value.
        wlmtk_menu_t *sub_ptr = wlmaker_context_menu_add_submenu(
            context_menu_ptr, menu_ptr, wlmaker_i18n(title_ptr));
        if (NULL == sub_ptr) return false;
        bspl_array_t *choices_ptr = _wlmaker_dockapp_prefs_choices(
            prefs_ptr, p_ptr);
        for (size_t c = 0;
             NULL != choices_ptr && c < bspl_array_size(choices_ptr); ++c) {
            bspl_array_t *pair_ptr = bspl_array_from_object(
                bspl_array_at(choices_ptr, c));
            if (NULL == pair_ptr || 2 > bspl_array_size(pair_ptr)) continue;
            if (!_wlmaker_dockapp_prefs_add_choice(
                    prefs_ptr, context_menu_ptr, sub_ptr, key_ptr,
                    current_ptr,
                    bspl_array_string_value_at(pair_ptr, 0),
                    wlmaker_i18n(bspl_array_string_value_at(pair_ptr, 1)))) {
                return false;
            }
        }

        // Entries of a directory, eg. the network interfaces.
        const char *dir_ptr = bspl_dict_get_string_value(
            p_ptr, "ChoicesFromDirectory");
        if (NULL == dir_ptr) continue;
        bspl_array_t *exclude_ptr = bspl_dict_get_array(p_ptr, "Exclude");
        DIR *d = opendir(dir_ptr);
        if (NULL == d) continue;
        char *names[64];
        size_t n = 0;
        for (struct dirent *e = readdir(d); NULL != e && n < 64;
             e = readdir(d)) {
            if ('.' == e->d_name[0]) continue;
            bool excluded = false;
            for (size_t x = 0;
                 NULL != exclude_ptr && x < bspl_array_size(exclude_ptr);
                 ++x) {
                const char *v = bspl_array_string_value_at(exclude_ptr, x);
                if (NULL != v && 0 == strcmp(v, e->d_name)) excluded = true;
            }
            if (excluded) continue;
            names[n] = logged_strdup(e->d_name);
            if (NULL != names[n]) ++n;
        }
        closedir(d);
        qsort(names, n, sizeof(names[0]), _wlmaker_dockapp_prefs_strcmp);
        bool rv = true;
        for (size_t k = 0; k < n; ++k) {
            rv = rv && _wlmaker_dockapp_prefs_add_choice(
                prefs_ptr, context_menu_ptr, sub_ptr, key_ptr,
                current_ptr, names[k], names[k]);
            free(names[k]);
        }
        if (!rv) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
const char *wlmaker_dockapp_prefs_value(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    const char *key_ptr)
{
    const char *v = bspl_dict_get_string_value(
        prefs_ptr->values_dict_ptr, key_ptr);
    if (NULL != v) return v;
    return bspl_dict_get_string_value(
        _wlmaker_dockapp_prefs_find(prefs_ptr, key_ptr), "Default");
}

/* ------------------------------------------------------------------------- */
char *wlmaker_dockapp_prefs_expand(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    const char *command_ptr)
{
    bs_dynbuf_t dynbuf = {};
    if (!bs_dynbuf_init(&dynbuf, 256, 65536)) return NULL;
    bool rv = true;
    for (const char *p = command_ptr; rv && '\0' != *p; ) {
        const char *end_ptr;
        if ('$' == p[0] && '{' == p[1] &&
            NULL != (end_ptr = strchr(p + 2, '}'))) {
            char key[64];
            size_t len = BS_MIN((size_t)(end_ptr - p - 2), sizeof(key) - 1);
            memcpy(key, p + 2, len);
            key[len] = '\0';
            const char *v = wlmaker_dockapp_prefs_value(prefs_ptr, key);
            if (NULL != v) rv = bs_dynbuf_append(&dynbuf, v, strlen(v));
            p = end_ptr + 1;
        } else {
            rv = bs_dynbuf_append_char(&dynbuf, *p++);
        }
    }
    rv = rv && bs_dynbuf_append_char(&dynbuf, '\0');
    char *s = rv ? logged_strdup(dynbuf.data_ptr) : NULL;
    bs_dynbuf_fini(&dynbuf);
    return s;
}

/** Arguments for @ref _wlmaker_dockapp_prefs_copy_entry. */
typedef struct {
    /** Copy to. */
    bspl_dict_t               *dict_ptr;
    /** Key to skip. */
    const char                *skip_key_ptr;
} _wlmaker_dockapp_prefs_copy_arg_t;

/** Copies an entry into a dict, except for the skipped key. */
static bool _wlmaker_dockapp_prefs_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *ud_ptr)
{
    _wlmaker_dockapp_prefs_copy_arg_t *arg_ptr = ud_ptr;
    if (0 == strcmp(key_ptr, arg_ptr->skip_key_ptr)) return true;
    return bspl_dict_add(arg_ptr->dict_ptr, key_ptr, object_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_dockapp_prefs_store(
    const char *fname_ptr,
    const char *key_ptr,
    const char *value_ptr)
{
    bspl_dict_t *old_dict_ptr = NULL;
    struct stat st;
    if (0 == stat(fname_ptr, &st)) {
        bspl_object_t *o = bspl_create_object_from_plist_file(fname_ptr);
        old_dict_ptr = bspl_dict_from_object(o);
        if (NULL == old_dict_ptr && NULL != o) bspl_object_unref(o);
    }

    bool rv = false;
    bspl_dict_t *dict_ptr = bspl_dict_create();
    bspl_string_t *s_ptr = bspl_string_create(value_ptr);
    if (NULL == dict_ptr || NULL == s_ptr) goto done;
    _wlmaker_dockapp_prefs_copy_arg_t arg = {
        .dict_ptr = dict_ptr, .skip_key_ptr = key_ptr };
    if (NULL != old_dict_ptr &&
        !bspl_dict_foreach(old_dict_ptr, _wlmaker_dockapp_prefs_copy_entry,
                           &arg)) goto done;
    if (!bspl_dict_add(dict_ptr, key_ptr, bspl_object_from_string(s_ptr))) {
        goto done;
    }

    bs_dynbuf_t dynbuf = {};
    if (!bs_dynbuf_init(&dynbuf, 1024, SIZE_MAX)) goto done;
    rv = bspl_object_write(bspl_object_from_dict(dict_ptr), &dynbuf) &&
        bs_dynbuf_append_char(&dynbuf, '\n') &&
        bs_dynbuf_write_file(&dynbuf, fname_ptr, S_IRUSR | S_IWUSR);
    bs_dynbuf_fini(&dynbuf);
    if (!rv) bs_log(BS_WARNING, "Failed to store \"%s\"", fname_ptr);

done:
    if (NULL != s_ptr) bspl_string_unref(s_ptr);
    if (NULL != dict_ptr) bspl_dict_unref(dict_ptr);
    if (NULL != old_dict_ptr) bspl_dict_unref(old_dict_ptr);
    return rv;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** @return The schema's dict of preference `key_ptr`, or NULL. */
bspl_dict_t *_wlmaker_dockapp_prefs_find(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    const char *key_ptr)
{
    bspl_array_t *list_ptr = bspl_dict_get_array(
        prefs_ptr->app_dict_ptr, "Preferences");
    for (size_t i = 0; NULL != list_ptr && i < bspl_array_size(list_ptr);
         ++i) {
        bspl_dict_t *p_ptr = bspl_dict_from_object(bspl_array_at(list_ptr, i));
        const char *k = bspl_dict_get_string_value(p_ptr, "Key");
        if (NULL != k && 0 == strcmp(k, key_ptr)) return p_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** @return The preference's choices: Its own, or the named shared list. */
bspl_array_t *_wlmaker_dockapp_prefs_choices(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    bspl_dict_t *pref_dict_ptr)
{
    bspl_object_t *o = bspl_dict_get(pref_dict_ptr, "Choices");
    bspl_array_t *a_ptr = bspl_array_from_object(o);
    if (NULL != a_ptr) return a_ptr;
    const char *name_ptr = bspl_string_value_from_object(o);
    if (NULL == name_ptr) return NULL;
    return bspl_dict_get_array(
        bspl_dict_get_dict(prefs_ptr->schema_dict_ptr, "Choices"), name_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds a radio button for `value_ptr`: Checked if it is the current. */
bool _wlmaker_dockapp_prefs_add_choice(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *menu_ptr,
    const char *key_ptr,
    const char *current_ptr,
    const char *value_ptr,
    const char *title_ptr)
{
    if (NULL == value_ptr || NULL == title_ptr) return true;
    return _wlmaker_dockapp_prefs_add_option(
        prefs_ptr, context_menu_ptr, menu_ptr, title_ptr,
        WLMTK_MENU_ITEM_INDICATOR_RADIO,
        NULL != current_ptr && 0 == strcmp(current_ptr, value_ptr),
        key_ptr, value_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds an item that stores `value_ptr` for `key_ptr` when triggered. */
bool _wlmaker_dockapp_prefs_add_option(
    wlmaker_dockapp_prefs_t *prefs_ptr,
    wlmaker_context_menu_t *context_menu_ptr,
    wlmtk_menu_t *menu_ptr,
    const char *title_ptr,
    wlmtk_menu_item_indicator_t indicator,
    bool checked,
    const char *key_ptr,
    const char *value_ptr)
{
    _wlmaker_dockapp_prefs_store_arg_t *arg_ptr = logged_calloc(
        1, sizeof(*arg_ptr));
    if (NULL == arg_ptr) return false;
    arg_ptr->fname_ptr = logged_strdup(prefs_ptr->fname_ptr);
    arg_ptr->key_ptr = logged_strdup(key_ptr);
    arg_ptr->value_ptr = logged_strdup(value_ptr);
    if (NULL == arg_ptr->fname_ptr || NULL == arg_ptr->key_ptr ||
        NULL == arg_ptr->value_ptr) {
        _wlmaker_dockapp_prefs_store_arg_free(arg_ptr);
        return false;
    }
    return wlmaker_context_menu_add_option(
        context_menu_ptr, menu_ptr, title_ptr, indicator, checked,
        _wlmaker_dockapp_prefs_store_callback, arg_ptr,
        _wlmaker_dockapp_prefs_store_arg_free);
}

/* ------------------------------------------------------------------------- */
/** Item callback: Stores the preference. The dock-app sees the change. */
void _wlmaker_dockapp_prefs_store_callback(void *ud_ptr)
{
    _wlmaker_dockapp_prefs_store_arg_t *arg_ptr = ud_ptr;
    wlmaker_dockapp_prefs_store(
        arg_ptr->fname_ptr, arg_ptr->key_ptr, arg_ptr->value_ptr);
}

/* ------------------------------------------------------------------------- */
/** Releases a @ref _wlmaker_dockapp_prefs_store_arg_t. */
void _wlmaker_dockapp_prefs_store_arg_free(void *ud_ptr)
{
    _wlmaker_dockapp_prefs_store_arg_t *arg_ptr = ud_ptr;
    if (NULL != arg_ptr->fname_ptr) free(arg_ptr->fname_ptr);
    if (NULL != arg_ptr->key_ptr) free(arg_ptr->key_ptr);
    if (NULL != arg_ptr->value_ptr) free(arg_ptr->value_ptr);
    free(arg_ptr);
}

/* ------------------------------------------------------------------------- */
/** Item callback: Runs the action's command. */
void _wlmaker_dockapp_prefs_action_callback(void *ud_ptr)
{
    _wlmaker_dockapp_prefs_action_arg_t *arg_ptr = ud_ptr;
    if (NULL == arg_ptr->server_ptr) return;
    wlmaker_action_execute(
        arg_ptr->server_ptr, WLMAKER_ACTION_SHELL_EXECUTE,
        arg_ptr->command_ptr);
}

/* ------------------------------------------------------------------------- */
/** Releases a @ref _wlmaker_dockapp_prefs_action_arg_t. */
void _wlmaker_dockapp_prefs_action_arg_free(void *ud_ptr)
{
    _wlmaker_dockapp_prefs_action_arg_t *arg_ptr = ud_ptr;
    free(arg_ptr->command_ptr);
    free(arg_ptr);
}

/* ------------------------------------------------------------------------- */
/** Compares strings, for qsort(). */
int _wlmaker_dockapp_prefs_strcmp(const void *a_ptr, const void *b_ptr)
{
    return strcmp(*(char *const*)a_ptr, *(char *const*)b_ptr);
}

/* == Unit tests =========================================================== */

static void test_load_expand(bs_test_t *test_ptr);
static void test_store(bs_test_t *test_ptr);
static void test_menu(bs_test_t *test_ptr);
static void test_installed(bs_test_t *test_ptr);

static const bs_test_case_t _wlmaker_dockapp_prefs_test_cases[] = {
    { true, "load_expand", test_load_expand },
    { true, "store", test_store },
    { true, "menu", test_menu },
    { true, "installed", test_installed },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dockapp_prefs_test_set = BS_TEST_SET(
    true, "dockapp_prefs", _wlmaker_dockapp_prefs_test_cases);

/** A schema, for the tests. */
static const char _test_schema[] =
    "{ Choices = { Theme = ((Default, \"Default Colors\"), (Amber, Amber)); };"
    "  Dockapps = { app = {"
    "    Actions = ( { Title = Open; Command = \"run ${Cmd} --x\"; },"
    "                { Title = Never; Command = \"${Empty}\"; } );"
    "    Preferences = ("
    "      { Key = Theme; Title = Theme; Choices = Theme; Default = Default; },"
    "      { Key = Seconds; Title = Seconds; Type = Bool; Default = No; },"
    "      { Key = Cmd; Title = Command; Default = cal;"
    "        Choices = ((cal, Cal), (date, Date)); },"
    "      { Key = Empty; Title = E; Default = \"\"; Choices = ((\"\", No)); },"
    "      { Key = Dir; Title = Dir; Default = All; Choices = ((All, All));"
    "        ChoicesFromDirectory = \"%s\"; Exclude = (lo); }"
    "    ); }; }; }";

/** Writes `content_ptr` into the file at `path_ptr`. */
static bool _test_write(const char *path_ptr, const char *content_ptr)
{
    FILE *f = fopen(path_ptr, "w");
    if (NULL == f) return false;
    bool rv = 0 <= fputs(content_ptr, f);
    return 0 == fclose(f) && rv;
}

/** Writes the test schema, with the directory `dir_ptr`. */
static bool _test_write_schema(const char *path_ptr, const char *dir_ptr)
{
    char buf[sizeof(_test_schema) + 256];
    snprintf(buf, sizeof(buf), _test_schema, dir_ptr);
    return _test_write(path_ptr, buf);
}

/* ------------------------------------------------------------------------- */
/** Loads values and defaults, and expands commands. */
void test_load_expand(bs_test_t *test_ptr)
{
    const char *schema_ptr = bs_test_temp_path(test_ptr, "Dockapps.plist");
    const char *values_ptr = bs_test_temp_path(test_ptr, "app.plist");
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr, _test_write_schema(schema_ptr, "/nonexistent"));

    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlmaker_dockapp_prefs_load(
                          NULL, schema_ptr, values_ptr, "other"));

    // No preferences file: The defaults.
    wlmaker_dockapp_prefs_t *p = wlmaker_dockapp_prefs_load(
        NULL, schema_ptr, values_ptr, "app");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
    BS_TEST_VERIFY_STREQ(test_ptr, "No",
                         wlmaker_dockapp_prefs_value(p, "Seconds"));
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlmaker_dockapp_prefs_value(p, "X"));
    char *c = wlmaker_dockapp_prefs_expand(p, "run ${Cmd} --x ${X}$");
    BS_TEST_VERIFY_STREQ(test_ptr, "run cal --x $", c);
    free(c);
    wlmaker_dockapp_prefs_destroy(p);

    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr, _test_write(values_ptr, "{ Cmd = date; Seconds = Yes; }"));
    p = wlmaker_dockapp_prefs_load(NULL, schema_ptr, values_ptr, "app");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
    BS_TEST_VERIFY_STREQ(test_ptr, "Yes",
                         wlmaker_dockapp_prefs_value(p, "Seconds"));
    c = wlmaker_dockapp_prefs_expand(p, "run ${Cmd}");
    BS_TEST_VERIFY_STREQ(test_ptr, "run date", c);
    free(c);
    wlmaker_dockapp_prefs_destroy(p);

    unlink(values_ptr);
    unlink(schema_ptr);
}

/* ------------------------------------------------------------------------- */
/** Stores values, keeping the others. */
void test_store(bs_test_t *test_ptr)
{
    const char *values_ptr = bs_test_temp_path(test_ptr, "app.plist");
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_store(
                            values_ptr, "A", "1"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_store(
                            values_ptr, "B", "x y"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_store(
                            values_ptr, "A", "2"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_store(
                            values_ptr, "TZ", "Asia/Tokyo"));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_store(
                            values_ptr, "Cmd", "sh -c 'cal; read x' \"$@\""));

    bspl_object_t *o = bspl_create_object_from_plist_file(values_ptr);
    bspl_dict_t *d = bspl_dict_from_object(o);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, d);
    BS_TEST_VERIFY_STREQ(test_ptr, "2", bspl_dict_get_string_value(d, "A"));
    BS_TEST_VERIFY_STREQ(test_ptr, "x y", bspl_dict_get_string_value(d, "B"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Asia/Tokyo",
                         bspl_dict_get_string_value(d, "TZ"));
    BS_TEST_VERIFY_STREQ(test_ptr, "sh -c 'cal; read x' \"$@\"",
                         bspl_dict_get_string_value(d, "Cmd"));
    bspl_dict_unref(d);

    // The file is replaced; a backup may be left: Clean up both.
    unlink(values_ptr);
    char *b = bs_strdupf("%s.old", values_ptr);
    if (NULL != b) {
        unlink(b);
        free(b);
    }
}

/* ------------------------------------------------------------------------- */
/** Builds the menu, and stores a choice. */
void test_menu(bs_test_t *test_ptr)
{
    const char *schema_ptr = bs_test_temp_path(test_ptr, "Dockapps.plist");
    const char *values_ptr = bs_test_temp_path(test_ptr, "app.plist");
    const char *dir_ptr = bs_test_temp_path(test_ptr, "dir");
    BS_TEST_VERIFY_EQ_OR_RETURN(test_ptr, 0, mkdir(dir_ptr, 0700));
    const char *eth_ptr = bs_test_temp_path(test_ptr, "dir/eth0");
    const char *lo_ptr = bs_test_temp_path(test_ptr, "dir/lo");
    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, _test_write(eth_ptr, ""));
    BS_TEST_VERIFY_TRUE_OR_RETURN(test_ptr, _test_write(lo_ptr, ""));
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr, _test_write_schema(schema_ptr, dir_ptr));
    BS_TEST_VERIFY_TRUE_OR_RETURN(
        test_ptr, _test_write(values_ptr, "{ Theme = Amber; }"));

    wlmaker_config_style_t style = {
        .window_style_ptr = wlmtk_window_style_create(),
        .menu_style_ptr = wlmtk_menu_style_create()
    };
    wlmaker_server_t server = { .style_ptr = &style };
    int owner;
    wlmaker_context_menu_t *cm_ptr = wlmaker_context_menu_create(
        &server, &owner, "app");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, cm_ptr);
    wlmaker_dockapp_prefs_t *p = wlmaker_dockapp_prefs_load(
        NULL, schema_ptr, values_ptr, "app");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);

    // The 'Never' action expands to nothing: Left out.
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_add_actions(
                            p, NULL, cm_ptr));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmaker_dockapp_prefs_add_preferences(
                            p, cm_ptr));
    wlmtk_menu_t *m = wlmaker_context_menu_menu(cm_ptr);
    BS_TEST_VERIFY_EQ_OR_RETURN(test_ptr, 2, wlmtk_menu_items_size(m));
    wlmtk_menu_t *prefs_m = wlmtk_menu_item_get_submenu(
        wlmtk_menu_item_at(m, 1));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, prefs_m);
    BS_TEST_VERIFY_EQ(test_ptr, 5, wlmtk_menu_items_size(prefs_m));

    // Theme: Amber is checked.
    wlmtk_menu_t *theme_m = wlmtk_menu_item_get_submenu(
        wlmtk_menu_item_at(prefs_m, 0));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, theme_m);
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_menu_items_size(theme_m));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_item_checked(
                             wlmtk_menu_item_at(theme_m, 0)));
    BS_TEST_VERIFY_TRUE(test_ptr, wlmtk_menu_item_checked(
                            wlmtk_menu_item_at(theme_m, 1)));
    // Seconds: An unchecked check box.
    BS_TEST_VERIFY_EQ(test_ptr, NULL, wlmtk_menu_item_get_submenu(
                          wlmtk_menu_item_at(prefs_m, 1)));
    BS_TEST_VERIFY_FALSE(test_ptr, wlmtk_menu_item_checked(
                             wlmtk_menu_item_at(prefs_m, 1)));
    // From the directory: All, and eth0. Not lo.
    wlmtk_menu_t *dir_m = wlmtk_menu_item_get_submenu(
        wlmtk_menu_item_at(prefs_m, 4));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dir_m);
    BS_TEST_VERIFY_EQ(test_ptr, 2, wlmtk_menu_items_size(dir_m));

    // Checking 'Seconds', and choosing 'eth0', stores them.
    wlmtk_menu_item_trigger(wlmtk_menu_item_at(prefs_m, 1));
    wlmtk_menu_item_trigger(wlmtk_menu_item_at(dir_m, 1));
    wlmaker_dockapp_prefs_destroy(p);
    p = wlmaker_dockapp_prefs_load(NULL, schema_ptr, values_ptr, "app");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, p);
    BS_TEST_VERIFY_STREQ(test_ptr, "Yes",
                         wlmaker_dockapp_prefs_value(p, "Seconds"));
    BS_TEST_VERIFY_STREQ(test_ptr, "eth0",
                         wlmaker_dockapp_prefs_value(p, "Dir"));
    BS_TEST_VERIFY_STREQ(test_ptr, "Amber",
                         wlmaker_dockapp_prefs_value(p, "Theme"));
    wlmaker_dockapp_prefs_destroy(p);

    wlmaker_context_menu_destroy(cm_ptr);
    wlmtk_window_style_ref_release(
        wlmtk_window_style_to_ref(style.window_style_ptr));
    wlmtk_menu_style_ref_release(wlmtk_menu_style_to_ref(style.menu_style_ptr));
    unlink(eth_ptr);
    unlink(lo_ptr);
    rmdir(dir_ptr);
    unlink(schema_ptr);
    unlink(values_ptr);
    char *b = bs_strdupf("%s.old", values_ptr);
    if (NULL != b) {
        unlink(b);
        free(b);
    }
}

/* ------------------------------------------------------------------------- */
/** The installed `Dockapps.plist` parses, and has the dock-apps. */
void test_installed(bs_test_t *test_ptr)
{
    const char *fname_ptr = bs_test_data_path(
        test_ptr, "../../share/wlmaker/Dockapps.plist");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, fname_ptr);
    static const char *names[] = {
        "wlmcalclock", "wlmcpugraph", "wlmmemgraph", "wlmnetgraph",
        "wlmmixer", "wlmclock", NULL };
    for (const char **n = names; NULL != *n; ++n) {
        wlmaker_dockapp_prefs_t *p = wlmaker_dockapp_prefs_load(
            NULL, fname_ptr, "/nonexistent/x.plist", *n);
        BS_TEST_VERIFY_NEQ(test_ptr, NULL, p);
        if (NULL == p) continue;
        BS_TEST_VERIFY_STREQ(test_ptr, "Default",
                             wlmaker_dockapp_prefs_value(p, "Theme"));
        wlmaker_dockapp_prefs_destroy(p);
    }
}

/* == End of dockapp_prefs.c =============================================== */
