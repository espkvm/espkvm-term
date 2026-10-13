/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "touch.h"

#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "power.h"
#include "video.h"

static const char *TAG = "touch";

/* esp_lvgl_port's touch context starts with the panel's handle
 * (esp_lvgl_port_touch.c); the BSP does not hand it out. */
typedef struct {
    esp_lcd_touch_handle_t handle;
} port_touch_head_t;

static esp_lcd_touch_handle_t s_tp;
static touch_pts_t s_pts;
static bool s_swallow; /* the touch that woke the screen goes nowhere */

/* The panel is portrait and the UI turned 90 degrees: screen X is 1279 minus
 * the panel's y, screen Y is the panel's x. */
static lv_point_t to_screen(const esp_lcd_touch_point_data_t *d)
{
    if (video_flipped()) {
        return (lv_point_t){.x = d->y, .y = 719 - d->x}; /* upside down */
    }
    return (lv_point_t){.x = 1279 - d->y, .y = d->x};
}

/* Replaces the port's read: the same for LVGL (the first finger, in the
 * panel's own coordinates, LVGL turns it), and keeps the rest. */
static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_point_data_t d[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t n = 0;
    if (esp_lcd_touch_read_data(s_tp) == ESP_OK) {
        esp_lcd_touch_get_data(s_tp, d, &n, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    }
    if (n > 0 && power_activity()) {
        s_swallow = true;
    } else if (n == 0) {
        s_swallow = false;
    }
    if (s_swallow) {
        n = 0;
    }
    s_pts.n = n;
    for (int i = 0; i < n && i < 2; i++) {
        s_pts.p[i] = to_screen(&d[i]);
    }
    if (n > 0) {
        data->point.x = d[0].x;
        data->point.y = d[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void touch_init(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        const port_touch_head_t *ctx = lv_indev_get_driver_data(i);
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER && ctx && ctx->handle) {
            s_tp = ctx->handle;
            lv_indev_set_read_cb(i, read_cb);
            return;
        }
    }
    ESP_LOGW(TAG, "no touch panel: one finger only");
}

void touch_get(touch_pts_t *out)
{
    *out = s_pts;
}
