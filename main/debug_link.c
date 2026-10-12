/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Development aid over the USB serial port: look at the screen and touch it
 * from a computer. Lines in:
 *   shot [1|2]  -> "SHOT <w> <h>\n" + w*h RGB565 (1:1 or half size) + "\nENDSHOT\n"
 *   tap <x> <y> -> a touch at screen coordinates (landscape)
 *   key <code>  -> one key (LVGL key code or a character) to the focused field
 */
#include "debug_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/usb_serial_jtag.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "video.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dbg";

static lv_indev_t *s_ptr;
static volatile int s_tap_x, s_tap_y, s_tap_frames;
static void (*s_key)(uint32_t key);

static void ptr_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* LVGL turns pointer input with the display (90 degrees: X = 1279 - y,
     * Y = x), so hand it the panel's own coordinates. */
    data->point.x = s_tap_y;
    data->point.y = 1279 - s_tap_x;
    if (s_tap_frames > 0) {
        s_tap_frames--;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void out(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        int w = usb_serial_jtag_write_bytes(p, n > 512 ? 512 : n, pdMS_TO_TICKS(1000));
        if (w <= 0) {
            return;
        }
        p += w;
        n -= w;
    }
}

/* The picture owns the panel: send what the panel shows (turned back). */
static void shot_panel(const uint16_t *fb, int div)
{
    /* The PPA wrote it: drop what the CPU cache still holds */
    esp_cache_msync((void *)fb, 720 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    int w = 1280 / div, h = 720 / div;
    uint16_t *row = malloc(w * 2);
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "SHOT %d %d\n", w, h);
    esp_log_level_set("*", ESP_LOG_NONE);
    out(hdr, n);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            row[x] = fb[(size_t)(1279 - x * div) * 720 + y * div]; /* screen (x, y) = panel (y, 1279 - x) */
        }
        out(row, w * 2);
    }
    out("\nENDSHOT\n", 9);
    esp_log_level_set("*", ESP_LOG_INFO);
    free(row);
}

static void shot(int div)
{
    const uint16_t *fb = video_owned_panel();
    if (fb) {
        shot_panel(fb, div);
        return;
    }
    bsp_display_lock(0);
    lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    bsp_display_unlock();
    if (!img) {
        out("SHOT 0 0\nENDSHOT\n", 17);
        return;
    }
    int w = img->header.w / div, h = img->header.h / div;
    uint16_t *row = malloc(w * 2);
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "SHOT %d %d\n", w, h);
    esp_log_level_set("*", ESP_LOG_NONE);
    out(hdr, n);
    for (int y = 0; y < h; y++) {
        const uint16_t *src = (const uint16_t *)(img->data + (size_t)(y * div) * img->header.stride);
        const uint32_t *tsrc = top ? (const uint32_t *)(top->data + (size_t)(y * div) * top->header.stride)
                                   : NULL;
        for (int x = 0; x < w; x++) {
            uint16_t px = src[x * div];
            /* The top layer (on-screen keyboard) is drawn over, where opaque. */
            if (tsrc && (tsrc[x * div] >> 24) > 0x80) {
                uint32_t c = tsrc[x * div];
                px = ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
            }
            row[x] = px;
        }
        out(row, w * 2);
    }
    out("\nENDSHOT\n", 9);
    esp_log_level_set("*", ESP_LOG_INFO);
    free(row);
    lv_draw_buf_destroy(img);
    if (top) {
        lv_draw_buf_destroy(top);
    }
}

static void handle(char *line)
{
    int x, y;
    unsigned k;
    if (!strncmp(line, "shot", 4)) {
        shot(line[4] == ' ' && line[5] == '1' ? 1 : 2);
    } else if (sscanf(line, "tap %d %d", &x, &y) == 2) {
        s_tap_x = x;
        s_tap_y = y;
        s_tap_frames = 3;
    } else if (sscanf(line, "key %u", &k) == 1 && s_key) {
        s_key(k);
    } else if (!strncmp(line, "chr ", 4) && s_key) {
        for (const char *c = line + 4; *c; c++) {
            s_key((uint8_t)*c);
        }
    } else if (!strcmp(line, "heap")) {
        ESP_LOGI(TAG, "internal free %u largest %u min %u, dma free %u largest %u, psram free %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (line[0]) {
        ESP_LOGW(TAG, "unknown: %s", line);
    }
}

static void link_task(void *arg)
{
    char line[96];
    size_t len = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            line[len] = 0;
            handle(line);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

void debug_link_start(void (*inject_key)(uint32_t key))
{
    s_key = inject_key;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no USB serial driver");
        return;
    }
    bsp_display_lock(0);
    s_ptr = lv_indev_create();
    lv_indev_set_type(s_ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_ptr, ptr_read);
    bsp_display_unlock();
    /* The snapshot renders the screen on this stack. */
    xTaskCreate(link_task, "dbg", 16384, NULL, 3, NULL);
}
