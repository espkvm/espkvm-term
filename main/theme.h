/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The look: heavy machinery. Safety yellow with worn paint, dark metal,
 * hazard stripes, condensed type.
 */
#pragma once

#include "lvgl.h"

#define TH_YELLOW 0xf2b600
#define TH_BLACK  0x111111
#define TH_TEXT   0xe8e2d0 /* off-white on metal */
#define TH_DIM    0x8d877a
#define TH_OK     0x58d46a
#define TH_BAD    0xff5a3c

LV_FONT_DECLARE(font_ui_20);
LV_FONT_DECLARE(font_ui_26);
LV_FONT_DECLARE(font_bold_34);
LV_FONT_DECLARE(font_stencil_44);

/* Call once, LVGL lock held. */
void theme_init(void);

/* Dark metal screen with the yellow header plate and a hazard strip. */
lv_obj_t *th_screen(const char *title, lv_obj_t **status_out);
/* A dark panel with a yellow rim and four bolts. */
lv_obj_t *th_panel(lv_obj_t *parent);
/* Worn yellow button with black condensed capitals. */
lv_obj_t *th_button(lv_obj_t *parent, const char *text);
/* A field label, small and yellow. */
lv_obj_t *th_label(lv_obj_t *parent, const char *text);
/* One-line text field: black, yellow rim, yellow text. */
lv_obj_t *th_field(lv_obj_t *parent);
/* A status lamp; th_lamp_set: 0 off, 1 amber, 2 green, 3 red. */
lv_obj_t *th_lamp(lv_obj_t *parent);
void th_lamp_set(lv_obj_t *lamp, int state);
/* Yellow square checkbox with a small label. */
void th_checkbox(lv_obj_t *cb);
/* Styles the on-screen keyboard. */
void th_keyboard(lv_obj_t *kb);
