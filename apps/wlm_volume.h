/* ========================================================================= */
/**
 * @file wlm_volume.h
 *
 * Volume computations for the mixer DockApp (`wlmmixer`). Independent of the
 * sound server's library, to permit testing.
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
#ifndef __WLM_VOLUME_H__
#define __WLM_VOLUME_H__

#include <libbase/libbase.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** The volume at 100%. Same value as PulseAudio's `PA_VOLUME_NORM`. */
#define WLM_VOLUME_NORM UINT32_C(0x10000)

/** Scroll distance that counts as one step, for continuous axis sources. */
#define WLM_VOLUME_AXIS_STEP_DISTANCE 15.0

/**
 * Converts a volume to percent, rounded to the nearest integer.
 *
 * @param volume              Linear volume, @ref WLM_VOLUME_NORM is 100%.
 *
 * @return The volume in percent.
 */
unsigned wlm_volume_to_percent(uint32_t volume);

/**
 * Converts percent to volume.
 *
 * @param percent
 *
 * @return The volume, where @ref WLM_VOLUME_NORM is 100%.
 */
uint32_t wlm_volume_from_percent(unsigned percent);

/**
 * Computes the volume after `steps` steps up (positive) or down (negative).
 *
 * The result is aligned to multiples of `step_percent`: From 47%, one step of
 * 5% up yields 50%, and one step down yields 45%. Stepping up does not exceed
 * `max_percent`, and does not lower a volume that is already above. Stepping
 * down does not go below 0%.
 *
 * @param volume              Current volume.
 * @param steps               Number of steps. Positive values increase.
 * @param step_percent        Size of a step, in percent. Must be > 0.
 * @param max_percent         Upper limit when stepping up, in percent.
 *
 * @return The new volume.
 */
uint32_t wlm_volume_step(
    uint32_t volume,
    int steps,
    unsigned step_percent,
    unsigned max_percent);

/**
 * Converts pointer axis motion into volume steps.
 *
 * Discrete steps (eg. mouse wheel clicks) are used if provided. Otherwise
 * (eg. touchpad scrolling), the continuous `value` is accumulated in
 * `*accumulator_ptr`, and a step is taken for each
 * @ref WLM_VOLUME_AXIS_STEP_DISTANCE of motion. Scrolling up (negative axis
 * values) yields positive steps, ie. increases the volume.
 *
 * @param accumulator_ptr     Accumulated continuous motion. Initialize to 0.
 * @param discrete            Discrete steps of the axis event, or 0.
 * @param value               Continuous value of the axis event.
 *
 * @return Number of volume steps.
 */
int wlm_volume_axis_steps(double *accumulator_ptr, int discrete, double value);

/** Unit tests. */
/**
 * Converts a position along the volume meter into a volume in percent: The
 * meter spans `length` from `start`, for 0% to 100%. Positions before or
 * beyond it give 0% and 100%.
 *
 * @param position            Eg. the pointer's x coordinate.
 * @param start               Where the meter starts (0%).
 * @param length              Length of the meter (up to 100%).
 *
 * @return The volume, in percent, from 0 to 100.
 */
unsigned wlm_volume_percent_at(double position, double start, double length);

/**
 * Converts a volume into the fraction of the meter to illuminate.
 *
 * @param volume              Eg. 0x10000 for 100%.
 *
 * @return A value from 0.0 (nothing) to 1.0 (everything, at 100% or more).
 */
double wlm_volume_meter_fraction(uint32_t volume);

extern const bs_test_set_t wlm_volume_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLM_VOLUME_H__ */
/* == End of wlm_volume.h ================================================== */
