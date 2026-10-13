/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "orient.h"

#include "bmi270.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "video.h"

static const char *TAG = "orient";

/* Held landscape, gravity runs along the panel's short side: this axis (y,
 * the long side, is tilting left and right). The sign tells which way up. */
#define UP_AXIS(x, y, z) (x)
#define UPRIGHT_SIGN     1.0f
#define TILT_G             0.55f /* far enough from flat to mean it */
#define HOLD_READS         5     /* a second at 5 readings a second */

static bmi270_handle_t *s_imu;
static bool s_auto = true;
static volatile bool s_have;
static float s_x, s_y, s_z;

static void turn(bool flip)
{
    bsp_display_lock(0);
    bsp_display_rotate(lv_display_get_default(), flip ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
    video_set_flip(flip);
    lv_obj_invalidate(lv_screen_active());
    bsp_display_unlock();
    ESP_LOGI(TAG, "%s", flip ? "upside down" : "upright");
}

static void orient_task(void *arg)
{
    int votes = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        float x, y, z;
        if (bmi270_get_acce_data(s_imu, &x, &y, &z) != ESP_OK) {
            continue;
        }
        s_x = x;
        s_y = y;
        s_z = z;
        s_have = true;
        if (!s_auto) {
            votes = 0;
            continue;
        }
        float g = UP_AXIS(x, y, z) * UPRIGHT_SIGN;
        bool want_flip = video_flipped();
        if (g > TILT_G) {
            want_flip = false;
        } else if (g < -TILT_G) {
            want_flip = true;
        }
        /* lying flat, or barely tilted: leave it as it is */
        votes = want_flip != video_flipped() ? votes + 1 : 0;
        if (votes >= HOLD_READS) {
            votes = 0;
            turn(want_flip);
        }
    }
}

void orient_set_auto(bool on)
{
    s_auto = on;
    nvs_handle_t h;
    if (nvs_open("term", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "autorot", on);
        nvs_commit(h);
        nvs_close(h);
    }
    if (!on && video_flipped()) {
        turn(false); /* off: back to the usual way up */
    }
}

bool orient_auto(void)
{
    return s_auto;
}

bool orient_accel(float *x, float *y, float *z)
{
    *x = s_x;
    *y = s_y;
    *z = s_z;
    return s_have;
}

void orient_start(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("term", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "autorot", &v) == ESP_OK) {
            s_auto = v;
        }
        nvs_close(h);
    }
    bmi270_driver_config_t cfg = {
        .addr = BMI270_I2C_ADDRESS_L,
        .interface = BMI270_USE_I2C,
        .i2c_bus = bsp_i2c_get_handle(),
    };
    bmi270_config_t run = {
        .acce_odr = BMI270_ACC_ODR_25_HZ,
        .acce_range = BMI270_ACC_RANGE_2_G,
        .gyro_odr = BMI270_GYR_ODR_25_HZ,
        .gyro_range = BMI270_GYR_RANGE_250_DPS,
    };
    if (!cfg.i2c_bus || bmi270_create(&cfg, &s_imu) != ESP_OK || bmi270_start(s_imu, &run) != ESP_OK) {
        ESP_LOGW(TAG, "no BMI270: the screen will not turn by itself");
        return;
    }
    xTaskCreateWithCaps(orient_task, "orient", 3072, NULL, 2, NULL, MALLOC_CAP_SPIRAM); /* no flash access */
}
