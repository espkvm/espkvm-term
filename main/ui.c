/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui.h"

#include "theme.h"

static lv_group_t *s_keys;
static lv_obj_t *s_kb;
static bool s_hw_kb;
static ui_screen_t s_cur = UI_WIFI;

void ui_init(lv_group_t *keys)
{
    s_keys = keys;
    s_kb = lv_keyboard_create(lv_layer_top());
    th_keyboard(s_kb);
    lv_obj_set_size(s_kb, lv_pct(100), lv_pct(48));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_kb, true);
}

ui_screen_t ui_current(void)
{
    return s_cur;
}

void ui_show(ui_screen_t s)
{
    ui_kb_hide();
    s_cur = s;
    switch (s) {
    case UI_WIFI:
        ui_wifi_build();
        break;
    case UI_DEVICE:
        ui_device_build();
        break;
    case UI_VIEW:
        ui_view_build();
        break;
    case UI_TS:
        ui_ts_build();
        break;
    case UI_UPDATE:
        ui_update_build();
        break;
    }
}

static void show_cb(void *arg)
{
    ui_show((ui_screen_t)(intptr_t)arg);
}

void ui_show_async(ui_screen_t s)
{
    lv_async_call(show_cb, (void *)(intptr_t)s);
}

void ui_kb_hide(void)
{
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_set_hidden(s_kb, true);
    }
}

static void field_event(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_cb_t on_ready = (lv_event_cb_t)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_FOCUSED:
        if (!s_hw_kb) {
            lv_keyboard_set_textarea(s_kb, ta);
            lv_obj_set_hidden(s_kb, false);
        }
        break;
    case LV_EVENT_READY: /* Enter, or the keyboard's OK */
        if (on_ready) {
            on_ready(e);
        }
        break;
    case LV_EVENT_CANCEL:
        ui_kb_hide();
        break;
    default:
        break;
    }
}

void ui_field(lv_obj_t *ta, lv_event_cb_t on_ready)
{
    lv_obj_add_event_cb(ta, field_event, LV_EVENT_ALL, (void *)on_ready);
    lv_group_add_obj(s_keys, ta);
}

void ui_hw_keyboard(bool present)
{
    s_hw_kb = present;
    if (present) {
        ui_kb_hide();
    }
}
