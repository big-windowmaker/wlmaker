/* ========================================================================= */
/**
 * @file wlm_volume.c
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

#include "wlm_volume.h"

#include <stdbool.h>

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
unsigned wlm_volume_to_percent(uint32_t volume)
{
    return (unsigned)(((uint64_t)volume * 100 + WLM_VOLUME_NORM / 2) /
                      WLM_VOLUME_NORM);
}

/* ------------------------------------------------------------------------- */
uint32_t wlm_volume_from_percent(unsigned percent)
{
    return (uint32_t)(((uint64_t)percent * WLM_VOLUME_NORM + 50) / 100);
}

/* ------------------------------------------------------------------------- */
uint32_t wlm_volume_step(
    uint32_t volume,
    int steps,
    unsigned step_percent,
    unsigned max_percent)
{
    if (0 == steps || 0 == step_percent) return volume;
    long percent = wlm_volume_to_percent(volume);
    long step = step_percent;

    long new_percent;
    if (0 < steps) {
        if (percent >= (long)max_percent) return volume;
        // Down to the grid, then up.
        new_percent = (percent / step + steps) * step;
        new_percent = BS_MIN(new_percent, (long)max_percent);
    } else {
        // Up to the grid, then down.
        new_percent = ((percent + step - 1) / step + steps) * step;
        new_percent = BS_MAX(new_percent, 0L);
    }
    return wlm_volume_from_percent((unsigned)new_percent);
}

/* ------------------------------------------------------------------------- */
int wlm_volume_axis_steps(double *accumulator_ptr, int discrete, double value)
{
    if (0 != discrete) {
        *accumulator_ptr = 0;
        return -discrete;
    }
    *accumulator_ptr += value;
    int steps = (int)(*accumulator_ptr / WLM_VOLUME_AXIS_STEP_DISTANCE);
    *accumulator_ptr -= steps * WLM_VOLUME_AXIS_STEP_DISTANCE;
    return -steps;
}

/* == Unit tests =========================================================== */

static void test_percent(bs_test_t *test_ptr);
static void test_step(bs_test_t *test_ptr);
static void test_axis_steps(bs_test_t *test_ptr);

/** Test cases. */
static const bs_test_case_t _wlm_volume_test_cases[] = {
    { 1, "percent", test_percent },
    { 1, "step", test_step },
    { 1, "axis_steps", test_axis_steps },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlm_volume_test_set = BS_TEST_SET(
    true, "volume", _wlm_volume_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests conversion from and to percent. */
void test_percent(bs_test_t *test_ptr)
{
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlm_volume_to_percent(0));
    BS_TEST_VERIFY_EQ(test_ptr, 100, wlm_volume_to_percent(WLM_VOLUME_NORM));
    BS_TEST_VERIFY_EQ(test_ptr, 50, wlm_volume_to_percent(WLM_VOLUME_NORM / 2));
    BS_TEST_VERIFY_EQ(test_ptr, 150,
                      wlm_volume_to_percent(WLM_VOLUME_NORM * 3 / 2));
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlm_volume_from_percent(0));
    BS_TEST_VERIFY_EQ(test_ptr, WLM_VOLUME_NORM, wlm_volume_from_percent(100));
    // Round trip is exact for all percentages.
    for (unsigned p = 0; p <= 150; ++p) {
        BS_TEST_VERIFY_EQ(test_ptr, p,
                          wlm_volume_to_percent(wlm_volume_from_percent(p)));
    }
}

/* ------------------------------------------------------------------------- */
/** Tests @ref wlm_volume_step. */
void test_step(bs_test_t *test_ptr)
{
/** Steps from `_from` percent by `_steps` steps of 5%, up to 100%. */
#define STEP(_from, _steps) \
    wlm_volume_to_percent(wlm_volume_step( \
        wlm_volume_from_percent(_from), _steps, 5, 100))
    BS_TEST_VERIFY_EQ(test_ptr, 55, STEP(50, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 45, STEP(50, -1));
    BS_TEST_VERIFY_EQ(test_ptr, 50, STEP(47, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 45, STEP(47, -1));
    BS_TEST_VERIFY_EQ(test_ptr, 60, STEP(47, 3));
    BS_TEST_VERIFY_EQ(test_ptr, 35, STEP(47, -3));
    BS_TEST_VERIFY_EQ(test_ptr, 100, STEP(98, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 100, STEP(100, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 100, STEP(90, 10));
    BS_TEST_VERIFY_EQ(test_ptr, 0, STEP(3, -1));
    BS_TEST_VERIFY_EQ(test_ptr, 0, STEP(0, -1));
    BS_TEST_VERIFY_EQ(test_ptr, 0, STEP(20, -10));
    BS_TEST_VERIFY_EQ(test_ptr, 5, STEP(0, 1));
    // Above the maximum: Stepping up keeps it, stepping down lowers it.
    BS_TEST_VERIFY_EQ(test_ptr, 120, STEP(120, 1));
    BS_TEST_VERIFY_EQ(test_ptr, 115, STEP(120, -1));
    BS_TEST_VERIFY_EQ(test_ptr, 120, STEP(122, -1));
#undef STEP

    // No steps: Unchanged, also when not on a percent value.
    BS_TEST_VERIFY_EQ(test_ptr, 12345, wlm_volume_step(12345, 0, 5, 100));
    // A larger maximum.
    BS_TEST_VERIFY_EQ(
        test_ptr, wlm_volume_from_percent(150),
        wlm_volume_step(wlm_volume_from_percent(140), 3, 5, 150));
    // Other step size.
    BS_TEST_VERIFY_EQ(
        test_ptr, wlm_volume_from_percent(30),
        wlm_volume_step(wlm_volume_from_percent(25), 1, 10, 100));
}

/* ------------------------------------------------------------------------- */
/** Tests @ref wlm_volume_axis_steps. */
void test_axis_steps(bs_test_t *test_ptr)
{
    double acc = 0;
    // Wheel: One click up (negative) is one step up.
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlm_volume_axis_steps(&acc, -1, -15.0));
    BS_TEST_VERIFY_EQ(test_ptr, -2, wlm_volume_axis_steps(&acc, 2, 30.0));
    BS_TEST_VERIFY_EQ(test_ptr, 0, acc);

    // Touchpad: Accumulates.
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlm_volume_axis_steps(&acc, 0, -10.0));
    BS_TEST_VERIFY_EQ(test_ptr, 1, wlm_volume_axis_steps(&acc, 0, -10.0));
    BS_TEST_VERIFY_EQ(test_ptr, -5.0, acc);
    BS_TEST_VERIFY_EQ(test_ptr, 0, wlm_volume_axis_steps(&acc, 0, 10.0));
    BS_TEST_VERIFY_EQ(test_ptr, -2, wlm_volume_axis_steps(&acc, 0, 30.0));
    BS_TEST_VERIFY_EQ(test_ptr, 5.0, acc);
}

/* == End of wlm_volume.c ================================================== */
