/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every finger on the screen, not just the first: LVGL is handed one point,
 * the view reads both for pinch, pan and two-finger scroll.
 */
#pragma once

#include "lvgl.h"

typedef struct {
    int n;            /* fingers down */
    lv_point_t p[2];  /* the first two, in screen coordinates (1280x720) */
} touch_pts_t;

/* Takes over the touch panel's LVGL read. After bsp_display_start, LVGL lock held. */
void touch_init(void);
/* The fingers as of LVGL's last read (LVGL task). */
void touch_get(touch_pts_t *out);
