/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "kvm_link.h"
#include "nvs.h"

static const char *TAG = "power";

#define INA226_ADDR   0x41
#define INA226_SHUNT  0x01 /* 2.5 uV a bit, signed */
#define INA226_BUS    0x02 /* 1.25 mV a bit */
#define EXP2_ADDR     0x44 /* PI4IOE5V6408 #2 */
#define EXP_INPUT_REG 0x0F
#define EXP2_CHG_STAT (1 << 6) /* the charger's status line, low while charging */
#define EXP2_PWROFF   (1 << 4) /* a pulse here switches the Tab5 off */
#define EXP_DIR_REG   0x03     /* 1 = output */
#define EXP_OUT_REG   0x05

#define DIM_AFTER_MS (2 * 60 * 1000)
#define OFF_AFTER_MS (10 * 60 * 1000)
#define DIM_LEVEL    5

static i2c_master_dev_handle_t s_ina, s_exp2;
static power_state_t s_state;
static int s_bright;
static volatile int64_t s_last_use;
static volatile int s_level; /* 0 normal, 1 dimmed, 2 off */

static bool read16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    if (!dev || i2c_master_transmit_receive(dev, &reg, 1, b, 2, 50) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)(b[0] << 8 | b[1]);
    return true;
}

/* A two-cell pack: per cell 4.2 V full, about 3.3 V empty, roughly linear
 * in between except at the ends. */
static int percent_of(float volts)
{
    static const float cell[] = {3.30f, 3.55f, 3.68f, 3.75f, 3.82f, 3.90f, 4.00f, 4.10f, 4.20f};
    float v = volts / 2;
    if (v <= cell[0]) {
        return 0;
    }
    for (int i = 1; i < 9; i++) {
        if (v <= cell[i]) {
            return (int)(12.5f * (i - 1 + (v - cell[i - 1]) / (cell[i] - cell[i - 1])));
        }
    }
    return 100;
}

static void measure(void)
{
    uint16_t bus;
    if (!read16(s_ina, INA226_BUS, &bus)) {
        s_state.present = false;
        return;
    }
    float v = bus * 0.00125f;
    s_state.volts = v;
    s_state.present = v > 5.5f && v < 9.0f; /* a 2S pack, else no battery */
    s_state.percent = s_state.present ? percent_of(v) : 0;
    uint8_t reg = EXP_INPUT_REG, in = 0xff;
    if (s_exp2 && i2c_master_transmit_receive(s_exp2, &reg, 1, &in, 1, 50) == ESP_OK) {
        s_state.charging = s_state.present && !(in & EXP2_CHG_STAT);
    }
}

static void set_level(int level)
{
    if (level == s_level) {
        return;
    }
    if (level == 2) {
        bsp_display_backlight_off();
        link_video(false);
    } else {
        if (s_level == 2) {
            link_video(true);
            link_request_keyframe();
        }
        bsp_display_brightness_set(level == 1 ? DIM_LEVEL : s_bright);
    }
    ESP_LOGI(TAG, "screen %s", level == 0 ? "on" : level == 1 ? "dimmed" : "off");
    s_level = level;
}

static void power_task(void *arg)
{
    int n = 0;
    for (;;) {
        if (n++ % 10 == 0) {
            measure(); /* every 5 s */
        }
        int64_t idle_ms = (esp_timer_get_time() - s_last_use) / 1000;
        int want = 0;
        if (idle_ms > OFF_AFTER_MS && s_state.present && !s_state.charging) {
            want = 2; /* off only on battery: on the charger it may be a wall display */
        } else if (idle_ms > DIM_AFTER_MS) {
            want = 1;
        }
        if (want > s_level) {
            set_level(want);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void power_sleep(void)
{
    s_last_use = esp_timer_get_time() - (int64_t)OFF_AFTER_MS * 1000;
    set_level(2);
}

/* Read-modify-write on the second expander: only the PWROFF bit changes, the
 * C6's and the USB port's power lines on the same chip stay as they are. */
static bool exp2_rmw(uint8_t reg, uint8_t set, uint8_t clear)
{
    uint8_t v;
    if (!s_exp2 || i2c_master_transmit_receive(s_exp2, &reg, 1, &v, 1, 50) != ESP_OK) {
        return false;
    }
    uint8_t w[2] = {reg, (uint8_t)((v | set) & ~clear)};
    return i2c_master_transmit(s_exp2, w, 2, 50) == ESP_OK;
}

void power_off(void)
{
    ESP_LOGI(TAG, "switching off");
    bsp_display_backlight_off();
    exp2_rmw(EXP_OUT_REG, 0, EXP2_PWROFF);
    exp2_rmw(EXP_DIR_REG, EXP2_PWROFF, 0);
    exp2_rmw(EXP_OUT_REG, EXP2_PWROFF, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    exp2_rmw(EXP_OUT_REG, 0, EXP2_PWROFF);
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* still here: the USB holds the power up */
    ESP_LOGW(TAG, "still powered (USB): sleeping instead");
    power_sleep();
}

void power_set_brightness(int pct)
{
    pct = pct < 5 ? 5 : pct > 100 ? 100 : pct;
    s_bright = pct;
    if (s_level == 0) {
        bsp_display_brightness_set(pct);
    }
    nvs_handle_t h;
    if (nvs_open("term", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", (uint8_t)pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

int power_brightness(void)
{
    return s_bright;
}

bool power_activity(void)
{
    s_last_use = esp_timer_get_time();
    bool was_off = s_level == 2;
    if (s_level != 0) {
        set_level(0);
    }
    return was_off;
}

void power_get(power_state_t *out)
{
    *out = s_state;
}

void power_start(int default_pct)
{
    s_bright = default_pct;
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("term", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= 5 && v <= 100) {
            s_bright = v;
        }
        nvs_close(h);
    }
    bsp_display_brightness_set(s_bright);
    s_last_use = esp_timer_get_time();
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t ina = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = INA226_ADDR,
                               .scl_speed_hz = 400000};
    i2c_device_config_t exp = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = EXP2_ADDR,
                               .scl_speed_hz = 400000};
    if (!bus || i2c_master_bus_add_device(bus, &ina, &s_ina) != ESP_OK) {
        s_ina = NULL;
    }
    if (bus && i2c_master_bus_add_device(bus, &exp, &s_exp2) != ESP_OK) {
        s_exp2 = NULL;
    }
    measure();
    ESP_LOGI(TAG, "battery %s, %.2f V, %d%%%s", s_state.present ? "in" : "not found", s_state.volts,
             s_state.percent, s_state.charging ? ", charging" : "");
    xTaskCreateWithCaps(power_task, "power", 3072, NULL, 2, NULL, MALLOC_CAP_SPIRAM); /* no flash access */
}
