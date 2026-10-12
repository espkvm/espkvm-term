/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-KVM terminal for the M5Stack Tab5: an ESP-KVM's screen on the Tab5's
 * display, and the Tab5's touch and keyboard sent back to the target.
 *
 * Wi-Fi, then the device (sign in, trust its CA once), then its screen.
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "debug_link.h"
#include "kvm_link.h"
#include "lvgl.h"
#include "net.h"
#include "nvs_flash.h"
#include "tab5kbd.h"
#include "theme.h"
#include "ts.h"
#include "ui.h"
#include "textview.h"
#include "video.h"

static const char *TAG = "term";

/* EXT5V_EN: IO expander 0x43, P2 - 5 V to the side port the keyboard sits on. */
#define EXT5V_EN IO_EXPANDER_PIN_NUM_2

/* Keyboard events to LVGL: a keypad input device reads them from this queue. */
typedef struct {
    uint32_t key;
    bool pressed;
} key_msg_t;
static QueueHandle_t s_keys;

static uint32_t lv_key_of(const kbd_event_t *ev)
{
    switch (ev->usage) {
    case 0x28: return LV_KEY_ENTER;
    case 0x29: return LV_KEY_ESC;
    case 0x2A: return LV_KEY_BACKSPACE;
    case 0x2B: return LV_KEY_NEXT;
    case 0x4C: return LV_KEY_DEL;
    case 0x4F: return LV_KEY_RIGHT;
    case 0x50: return LV_KEY_LEFT;
    case 0x51: return LV_KEY_DOWN;
    case 0x52: return LV_KEY_UP;
    default: return (uint8_t)ev->ch;
    }
}

static void on_key(const kbd_event_t *ev)
{
    if (ui_current() == UI_VIEW) {
        ui_view_key(ev); /* typing goes to the target */
        return;
    }
    key_msg_t k = {.key = lv_key_of(ev), .pressed = ev->pressed};
    if (k.key) {
        xQueueSend(s_keys, &k, 0);
    }
}

/* A key from the debug link: press and release. */
static void inject_key(uint32_t key)
{
    key_msg_t k = {.key = key, .pressed = true};
    xQueueSend(s_keys, &k, 0);
    k.pressed = false;
    xQueueSend(s_keys, &k, 0);
}

static void on_kbd_presence(bool present)
{
    bsp_display_lock(0);
    ui_hw_keyboard(present);
    bsp_display_unlock();
}

/* The first time Wi-Fi is up, go to the saved device straight away. */
static void on_net_state(net_state_t st, const char *info)
{
    static bool tried;
    ui_wifi_on_state(st, info);
    ts_net_up(st == NET_CONNECTED);
    if (st == NET_CONNECTED && !tried) {
        tried = true;
        link_cfg_t saved;
        link_saved(&saved);
        if (saved.host[0] && saved.pass[0]) {
            link_connect(&saved, NULL);
            ui_show_async(UI_DEVICE);
        }
    }
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static key_msg_t last;
    key_msg_t k;
    if (xQueueReceive(s_keys, &k, 0) == pdTRUE) {
        last = k;
        data->continue_reading = uxQueueMessagesWaiting(s_keys) > 0;
    }
    data->key = last.key;
    data->state = last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The BSP aborts if the IO expander misses its first command, which
     * happens now and then right after a reset: wait until it answers. */
    if (bsp_i2c_init() == ESP_OK) {
        for (int i = 0; i < 50; i++) {
            if (i2c_master_probe(bsp_i2c_get_handle(), 0x43, 20) == ESP_OK &&
                i2c_master_probe(bsp_i2c_get_handle(), 0x44, 20) == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display did not start");
        return;
    }
    bsp_display_brightness_set(20); /* dim: easy on the eyes, cooler */

    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp) {
        esp_io_expander_set_dir(exp, EXT5V_EN, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(exp, EXT5V_EN, 1);
    }

    s_keys = xQueueCreate(32, sizeof(key_msg_t));

    bsp_display_lock(0);
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);
    theme_init();
    lv_group_t *keys = lv_group_create();
    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, keypad_read);
    lv_indev_set_group(kp, keys);
    link_init(ui_device_on_state, video_frame, ui_view_on_msg);
    link_set_text_cb(textview_on_json);
    ts_init();
    ui_init(keys);
    net_start(on_net_state, ui_wifi_on_scan); /* first: it loads the saved network */
    ui_show(UI_WIFI);
    bsp_display_unlock();

    kbd_start(on_key, on_kbd_presence);
    debug_link_start(inject_key);
    ESP_LOGI(TAG, "up");
}
