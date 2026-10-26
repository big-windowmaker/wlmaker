/* ========================================================================= */
/**
 * @file apps_test.c
 *
 * Unit tests for the DockApps' helper modules.
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

#include <libbase/libbase.h>
#include <stddef.h>

#include "wlm_calendar.h"
#include "wlm_volume.h"

/** Unit tests. */
const bs_test_set_t *test_sets[] = {
    &wlm_calendar_test_set,
    &wlm_volume_test_set,
    NULL,
};

/** Main program, runs the unit tests. */
int main(int argc, const char **argv)
{
    const bs_test_param_t params = {};
    return bs_test_sets(test_sets, argc, argv, &params);
}

/* == End of apps_test.c =================================================== */
