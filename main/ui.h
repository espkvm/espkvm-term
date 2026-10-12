/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screens and what they share: the key group, the on-screen keyboard, and
 * whether the Tab5 keyboard is attached.
 */
#pragma once

#include <stdbool.h>

#include "kvm_link.h"
#include "lvgl.h"
#include "net.h"
#include "tab5kbd.h"

typedef enum { UI_WIFI, UI_DEVICE, UI_VIEW, UI_TS } ui_screen_t;

/* LVGL lock held for all of these unless said otherwise. */
void ui_init(lv_group_t *keys);
void ui_show(ui_screen_t s);
ui_screen_t ui_current(void);
/* Schedules ui_show from any task (no lock needed). */
void ui_show_async(ui_screen_t s);

/* A text field joins the key group and calls up the on-screen keyboard. */
void ui_field(lv_obj_t *ta, lv_event_cb_t on_ready);
void ui_kb_hide(void);
void ui_hw_keyboard(bool present);

/* Each screen */
void ui_wifi_build(void);
void ui_device_build(void);
void ui_view_build(void);
void ui_ts_build(void);
/* The next device screen starts with this address in the field. */
void ui_device_prefill(const char *host);
/* News for the screens, from any task. */
void ui_wifi_on_state(net_state_t st, const char *info);
void ui_wifi_on_scan(const net_ap_t *aps, int n);
void ui_device_on_state(link_state_t st, const char *info);
void ui_view_on_msg(const uint8_t *data, size_t len);
/* A key from the Tab5 keyboard while the view is up (any task). */
void ui_view_key(const kbd_event_t *ev);
