/* ========================================================================= */
/**
 * @file window_attributes.c
 *
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

#include "window_attributes.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

static int _wlmaker_window_attributes_lookup(
    bspl_dict_t *attrs_dict_ptr,
    const char *class_ptr,
    const char *instance_ptr,
    const char *attribute_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
uint32_t wlmaker_window_attributes_properties(
    bspl_dict_t *config_dict_ptr,
    const char *class_ptr,
    const char *instance_ptr,
    bool fixed_size)
{
    uint32_t properties = WLMTK_WINDOW_PROPERTY_RESIZABLE |
        WLMTK_WINDOW_PROPERTY_ICONIFIABLE |
        WLMTK_WINDOW_PROPERTY_CLOSABLE;
    if (fixed_size) properties &= ~WLMTK_WINDOW_PROPERTY_RESIZABLE;

    bspl_dict_t *attrs_dict_ptr = NULL;
    if (NULL != config_dict_ptr) {
        attrs_dict_ptr = bspl_dict_get_dict(
            config_dict_ptr, "WindowAttributes");
    }
    if (NULL == attrs_dict_ptr) return properties;

    static const struct { const char *name_ptr; uint32_t property; } map[] = {
        { "NoResizebar", WLMTK_WINDOW_PROPERTY_RESIZABLE },
        { "NoMiniaturizeButton", WLMTK_WINDOW_PROPERTY_ICONIFIABLE },
        { "NoCloseButton", WLMTK_WINDOW_PROPERTY_CLOSABLE },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (1 == _wlmaker_window_attributes_lookup(
                attrs_dict_ptr, class_ptr, instance_ptr, map[i].name_ptr)) {
            properties &= ~map[i].property;
        }
    }
    return properties;
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Looks up `attribute_ptr` as Window Maker does: "instance.class", then
 * "instance", then "class", then "*".
 *
 * @return 1 if set to Yes, 0 if set to No, -1 if not set.
 */
int _wlmaker_window_attributes_lookup(
    bspl_dict_t *attrs_dict_ptr,
    const char *class_ptr,
    const char *instance_ptr,
    const char *attribute_ptr)
{
    char both[512] = "";
    if (NULL != class_ptr && NULL != instance_ptr) {
        snprintf(both, sizeof(both), "%s.%s", instance_ptr, class_ptr);
    }
    const char *keys[] = {
        both[0] ? both : NULL, instance_ptr, class_ptr, "*" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (NULL == keys[i] || 0 == keys[i][0]) continue;
        bspl_dict_t *d_ptr = bspl_dict_get_dict(attrs_dict_ptr, keys[i]);
        if (NULL == d_ptr) continue;
        const char *v_ptr = bspl_dict_get_string_value(d_ptr, attribute_ptr);
        if (NULL == v_ptr) continue;
        return (0 == strcasecmp(v_ptr, "Yes") ||
                0 == strcasecmp(v_ptr, "True")) ? 1 : 0;
    }
    return -1;
}

/* == Unit tests =========================================================== */

static void _wlmaker_window_attributes_test(bs_test_t *test_ptr);

static const bs_test_case_t _wlmaker_window_attributes_test_cases[] = {
    { true, "properties", _wlmaker_window_attributes_test },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_window_attributes_test_set = BS_TEST_SET(
    true, "window_attributes", _wlmaker_window_attributes_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests the lookup and the properties. */
void _wlmaker_window_attributes_test(bs_test_t *test_ptr)
{
    const uint32_t all = WLMTK_WINDOW_PROPERTY_RESIZABLE |
        WLMTK_WINDOW_PROPERTY_ICONIFIABLE | WLMTK_WINDOW_PROPERTY_CLOSABLE;
    BS_TEST_VERIFY_EQ(test_ptr, all, wlmaker_window_attributes_properties(
                          NULL, "a", "a", false));
    BS_TEST_VERIFY_EQ(
        test_ptr, all & ~WLMTK_WINDOW_PROPERTY_RESIZABLE,
        wlmaker_window_attributes_properties(NULL, "a", "a", true));

    bspl_object_t *o = bspl_create_object_from_plist_string(
        "{WindowAttributes = {"
        "\"*\" = {NoCloseButton = Yes;};"
        "About = {NoMiniaturizeButton = Yes; NoResizebar = Yes;};"
        "\"xterm.XTerm\" = {NoCloseButton = No;};"
        "};}");
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, o);
    bspl_dict_t *d = bspl_dict_from_object(o);
    BS_TEST_VERIFY_EQ(
        test_ptr, WLMTK_WINDOW_PROPERTY_RESIZABLE |
        WLMTK_WINDOW_PROPERTY_ICONIFIABLE,
        wlmaker_window_attributes_properties(d, "Other", "other", false));
    BS_TEST_VERIFY_EQ(
        test_ptr, 0,
        wlmaker_window_attributes_properties(d, "About", "About", false));
    BS_TEST_VERIFY_EQ(
        test_ptr, all,
        wlmaker_window_attributes_properties(d, "XTerm", "xterm", false));
    bspl_object_unref(o);
}

/* == End of window_attributes.c =========================================== */
