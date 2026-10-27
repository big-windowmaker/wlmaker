/* ========================================================================= */
/**
 * @file maintile.c
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

#include "maintile.h"

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>

/* == Declarations ========================================================= */

/** State of the Dock's main tile. */
struct _wlmdock_maintile_t {
    /** Derived from a @ref wlmtk_tile_t. */
    wlmtk_tile_t              super_tile;
    /** Original virtual method table of the element. */
    wlmtk_element_vmt_t       orig_element_vmt;
    /** The icon. NULL if there is none. */
    wlmtk_image_t             *image_ptr;
    /** Handler for a right-click. */
    wlmdock_maintile_menu_handler_t menu_handler;
    /** Argument for @ref wlmdock_maintile_t::menu_handler. */
    void                      *menu_handler_ud_ptr;
};

static void _wlmdock_maintile_element_destroy(wlmtk_element_t *element_ptr);
static bool _wlmdock_maintile_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);

/* == Data ================================================================= */

/** Extension of the element's virtual method table. */
static const wlmtk_element_vmt_t _wlmdock_maintile_element_vmt = {
    .destroy = _wlmdock_maintile_element_destroy,
    .pointer_button = _wlmdock_maintile_pointer_button,
};

#ifndef WLMDOCK_DEFAULT_ICON
/** Icon configured at build time. */
#define WLMDOCK_DEFAULT_ICON ""
#endif

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
char *wlmdock_resolve_icon(const char *icon_ptr, wlm_util_files_t *files_ptr)
{
    if (NULL == icon_ptr || '\0' == *icon_ptr) return NULL;
    if ('/' == icon_ptr[0] || '~' == icon_ptr[0]) {
        char *path_ptr = bs_file_resolve_path(icon_ptr, NULL);
        if (NULL != path_ptr && !bs_file_realpath_is(path_ptr, S_IFREG)) {
            free(path_ptr);
            path_ptr = NULL;
        }
        return path_ptr;
    }

    char *p = bs_strdupf("icons/%s", icon_ptr);
    if (NULL == p) return NULL;
    char *path_ptr = NULL;
    if (NULL != files_ptr) {
        path_ptr = wlm_util_files_xdg_data_find(files_ptr, p, S_IFREG);
    }
    free(p);
#ifdef WLMAKER_SOURCE_DIR
    if (NULL == path_ptr) {
        path_ptr = bs_strdupf(WLMAKER_SOURCE_DIR "/share/wlmaker/icons/%s",
                              icon_ptr);
        if (NULL != path_ptr && !bs_file_realpath_is(path_ptr, S_IFREG)) {
            free(path_ptr);
            path_ptr = NULL;
        }
    }
#endif
    return path_ptr;
}

/* ------------------------------------------------------------------------- */
wlmdock_maintile_t *wlmdock_maintile_create(
    const struct wlmtk_tile_style *style_ptr,
    const char *icon_ptr,
    wlm_util_files_t *files_ptr)
{
    wlmdock_maintile_t *maintile_ptr = logged_calloc(
        1, sizeof(*maintile_ptr));
    if (NULL == maintile_ptr) return NULL;
    if (!wlmtk_tile_init(&maintile_ptr->super_tile, style_ptr)) {
        free(maintile_ptr);
        return NULL;
    }
    maintile_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&maintile_ptr->super_tile),
        &_wlmdock_maintile_element_vmt);
    wlmtk_element_set_visible(
        wlmtk_tile_element(&maintile_ptr->super_tile), true);

    const char *candidates[] = {
        icon_ptr, WLMDOCK_DEFAULT_ICON, "dock-56x56.png" };
    char *path_ptr = NULL;
    for (size_t i = 0;
         NULL == path_ptr && i < sizeof(candidates) / sizeof(candidates[0]);
         ++i) {
        path_ptr = wlmdock_resolve_icon(candidates[i], files_ptr);
        if (NULL == path_ptr && NULL != candidates[i] &&
            '\0' != *candidates[i]) {
            bs_log(BS_WARNING, "Dock icon \"%s\" not found.", candidates[i]);
        }
    }
    if (NULL != path_ptr) {
        maintile_ptr->image_ptr = wlmtk_image_create_scaled(
            path_ptr,
            maintile_ptr->super_tile.style.content_size,
            maintile_ptr->super_tile.style.content_size);
        free(path_ptr);
    }
    if (NULL != maintile_ptr->image_ptr) {
        wlmtk_element_set_visible(
            wlmtk_image_element(maintile_ptr->image_ptr), true);
        wlmtk_tile_set_content(
            &maintile_ptr->super_tile,
            wlmtk_image_element(maintile_ptr->image_ptr));
    }
    return maintile_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmdock_maintile_destroy(wlmdock_maintile_t *maintile_ptr)
{
    if (NULL != maintile_ptr->image_ptr) {
        wlmtk_tile_set_content(&maintile_ptr->super_tile, NULL);
        wlmtk_image_destroy(maintile_ptr->image_ptr);
        maintile_ptr->image_ptr = NULL;
    }
    wlmtk_tile_fini(&maintile_ptr->super_tile);
    free(maintile_ptr);
}

/* ------------------------------------------------------------------------- */
wlmtk_tile_t *wlmdock_maintile_tile(wlmdock_maintile_t *maintile_ptr)
{
    return &maintile_ptr->super_tile;
}

/* ------------------------------------------------------------------------- */
wlmdock_maintile_t *wlmdock_maintile_from_tile(wlmtk_tile_t *tile_ptr)
{
    if (wlmtk_tile_element(tile_ptr)->vmt.destroy !=
        _wlmdock_maintile_element_destroy) return NULL;
    return BS_CONTAINER_OF(tile_ptr, wlmdock_maintile_t, super_tile);
}

/* ------------------------------------------------------------------------- */
void wlmdock_maintile_set_menu_handler(
    wlmdock_maintile_t *maintile_ptr,
    wlmdock_maintile_menu_handler_t handler,
    void *ud_ptr)
{
    maintile_ptr->menu_handler = handler;
    maintile_ptr->menu_handler_ud_ptr = ud_ptr;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::destroy. */
void _wlmdock_maintile_element_destroy(wlmtk_element_t *element_ptr)
{
    wlmdock_maintile_t *maintile_ptr = BS_CONTAINER_OF(
        element_ptr, wlmdock_maintile_t,
        super_tile.super_container.super_element);
    wlmdock_maintile_destroy(maintile_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_button: Right opens menu. */
bool _wlmdock_maintile_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmdock_maintile_t *maintile_ptr = BS_CONTAINER_OF(
        element_ptr, wlmdock_maintile_t,
        super_tile.super_container.super_element);
    if (BTN_RIGHT == button_event_ptr->button &&
        WLMTK_BUTTON_DOWN == button_event_ptr->type &&
        NULL != maintile_ptr->menu_handler) {
        maintile_ptr->menu_handler(maintile_ptr,
                                   maintile_ptr->menu_handler_ud_ptr);
    }
    return true;
}

/* == Unit tests =========================================================== */

static void test_create(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlmdock_maintile_test_cases[] = {
    { true, "create", test_create },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmdock_maintile_test_set = BS_TEST_SET(
    true, "maintile", _wlmdock_maintile_test_cases);

/** Test handler: Counts calls. */
static void _test_handler(__UNUSED__ wlmdock_maintile_t *m, void *ud_ptr)
{
    ++*(int*)ud_ptr;
}

/* ------------------------------------------------------------------------- */
/** Creates main tiles, with fallback icons; right button opens the menu. */
void test_create(bs_test_t *test_ptr)
{
    static const struct wlmtk_tile_style style = {
        .size = 64, .content_size = 48 };
    bs_test_setenv(test_ptr, "XDG_DATA_DIRS", WLMAKER_SOURCE_DIR "/share");
    wlm_util_files_t *files_ptr = wlm_util_files_create("wlmaker");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, files_ptr);

    char *p = wlmdock_resolve_icon("dock-56x56.png", files_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, p);
    if (NULL != p) free(p);
    BS_TEST_VERIFY_EQ(test_ptr, NULL,
                      wlmdock_resolve_icon("does-not-exist.png", files_ptr));

    // A missing icon falls back to the default one.
    wlmdock_maintile_t *m = wlmdock_maintile_create(
        &style, "does-not-exist.png", files_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, m);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, m->image_ptr);
    BS_TEST_VERIFY_EQ(
        test_ptr, m, wlmdock_maintile_from_tile(wlmdock_maintile_tile(m)));

    int calls = 0;
    wlmdock_maintile_set_menu_handler(m, _test_handler, &calls);
    wlmtk_button_event_t e = { .button = BTN_RIGHT, .type = WLMTK_BUTTON_DOWN };
    wlmtk_element_pointer_button(
        wlmtk_tile_element(wlmdock_maintile_tile(m)), &e);
    BS_TEST_VERIFY_EQ(test_ptr, 1, calls);
    e.button = BTN_LEFT;
    wlmtk_element_pointer_button(
        wlmtk_tile_element(wlmdock_maintile_tile(m)), &e);
    BS_TEST_VERIFY_EQ(test_ptr, 1, calls);

    wlmdock_maintile_destroy(m);
    wlm_util_files_destroy(files_ptr);
}

/* == End of maintile.c ==================================================== */
