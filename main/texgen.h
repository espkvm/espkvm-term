/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"

/* Draws the surfaces for a w x h screen. New random wear on every boot. */
void texgen_init(int w, int h);
/* Image sources (lv_draw_buf_t); NULL if out of memory. */
const void *texgen_metal(void);
const void *texgen_plate(void);
const void *texgen_hazard(void);
