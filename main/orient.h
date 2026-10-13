/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which way up the Tab5 is held, from its BMI270: the screen turns over when
 * it is held the other way up for a second. Landscape only - a 16:9 picture
 * has no use for portrait.
 */
#pragma once

#include <stdbool.h>

/* After the display is up; LVGL lock NOT held. */
void orient_start(void);
/* Turning by itself, kept across restarts. */
void orient_set_auto(bool on);
bool orient_auto(void);
/* The last reading, in g (for the TERMINAL screen). */
bool orient_accel(float *x, float *y, float *z);
