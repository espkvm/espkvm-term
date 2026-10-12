/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Text mode: the target's screen as characters (a BIOS, a boot menu, a
 * console), drawn in a monospace font and scaled to fit. A few hundred bytes
 * per change where the picture is megabits.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

/* LVGL lock held. Hidden until shown. */
lv_obj_t *textview_create(lv_obj_t *parent);
void textview_detach(void);
void textview_show(bool on);
/* A message from the device ({"kind":"text",...}); any task. */
void textview_on_json(const char *json, size_t len);
/* The screen stopped being characters (it booted, or drew a picture). */
bool textview_gone(void);
/* Anything read yet. */
bool textview_has_text(void);
