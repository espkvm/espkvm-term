/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tab5 keyboard. Registers and the key table come from M5Stack's driver
 * (M5Unit-KEYBOARD, unit_Tab5Keyboard, MIT: see LICENSE-M5Unit-KEYBOARD.txt).
 */
#include "tab5kbd.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kbd";

#define KBD_ADDR     0x6D
#define KBD_SDA      GPIO_NUM_0
#define KBD_SCL      GPIO_NUM_1
#define KBD_INT      GPIO_NUM_50
#define KBD_I2C_HZ   400000
#define KBD_TIMEOUT  20

#define REG_INT_STAT  0x01
#define REG_EVENT_NUM 0x02
#define REG_MODE      0x10
#define REG_KEY_EVENT 0x20
#define REG_FW        0xFE

#define ROWS 5
#define COLS 14

/* Modifier keys on the matrix: no usage of their own. */
#define IS_SYM(r, c)  ((r) == 3 && (c) == 0)
#define IS_AA(r, c)   ((r) == 3 && (c) == 1)
#define IS_CTRL(r, c) ((r) == 4 && (c) == 0)
#define IS_ALT(r, c)  ((r) == 4 && (c) == 1)

typedef struct {
    uint8_t usage, mods;
} hid_map_t;

/* US layout, as the keyboard's own HID mode reports it. */
static const hid_map_t MAP_BASE[ROWS][COLS] = {
    /* Esc 1 2 3 4 5 6 7 8 9 0 - + Del */
    {{0x29, 0}, {0x1E, 0}, {0x1F, 0}, {0x20, 0}, {0x21, 0}, {0x22, 0}, {0x23, 0},
     {0x24, 0}, {0x25, 0}, {0x26, 0}, {0x27, 0}, {0x2D, 0}, {0x2E, 2}, {0x4C, 0}},
    /* ` ! @ # $ % ^ & * ( ) [ ] \ */
    {{0x35, 0}, {0x1E, 2}, {0x1F, 2}, {0x20, 2}, {0x21, 2}, {0x22, 2}, {0x23, 2},
     {0x24, 2}, {0x25, 2}, {0x26, 2}, {0x27, 2}, {0x2F, 0}, {0x30, 0}, {0x31, 0}},
    /* Tab q w e r t y u i o p ; ' Backspace */
    {{0x2B, 0}, {0x14, 0}, {0x1A, 0}, {0x08, 0}, {0x15, 0}, {0x17, 0}, {0x1C, 0},
     {0x18, 0}, {0x0C, 0}, {0x12, 0}, {0x13, 0}, {0x33, 0}, {0x34, 0}, {0x2A, 0}},
    /* Sym Aa a s d f g h j k l Up _ Enter */
    {{0, 0}, {0, 0}, {0x04, 0}, {0x16, 0}, {0x07, 0}, {0x09, 0}, {0x0A, 0},
     {0x0B, 0}, {0x0D, 0}, {0x0E, 0}, {0x0F, 0}, {0x52, 0}, {0x2D, 2}, {0x28, 0}},
    /* Ctrl Alt z x c v b n m . Left Down Right Space */
    {{0, 0}, {0, 0}, {0x1D, 0}, {0x1B, 0}, {0x06, 0}, {0x19, 0}, {0x05, 0},
     {0x11, 0}, {0x10, 0}, {0x37, 0}, {0x50, 0}, {0x51, 0}, {0x4F, 0}, {0x2C, 0}},
};

/* Sym layer: only the keys that change. */
static hid_map_t sym_map(int r, int c)
{
    static const struct {
        uint8_t r, c;
        hid_map_t m;
    } SYM[] = {
        {1, 0, {0x35, 2}},  {1, 1, {0x38, 2}},  {1, 8, {0x38, 0}}, /* ~ ? / */
        {1, 9, {0x36, 2}},  {1, 10, {0x37, 2}},                    /* < > */
        {1, 11, {0x2F, 2}}, {1, 12, {0x30, 2}}, {1, 13, {0x31, 2}}, /* { } | */
        {2, 11, {0x33, 2}}, {2, 12, {0x34, 2}},                     /* : " */
        {3, 12, {0x2E, 0}}, {4, 9, {0x36, 0}},                      /* = , */
    };
    for (size_t i = 0; i < sizeof(SYM) / sizeof(SYM[0]); i++) {
        if (SYM[i].r == r && SYM[i].c == c) {
            return SYM[i].m;
        }
    }
    return MAP_BASE[r][c];
}

static char usage_to_char(uint8_t u, bool shift)
{
    static const char NUM[] = "1234567890", NUM_S[] = "!@#$%^&*()";
    static const char PUNCT[] = " -=[]\\#;'`,./", PUNCT_S[] = " _+{}|~:\"~<>?";
    if (u >= 0x04 && u <= 0x1D) {
        return (char)((shift ? 'A' : 'a') + (u - 0x04));
    }
    if (u >= 0x1E && u <= 0x27) {
        return (shift ? NUM_S : NUM)[u - 0x1E];
    }
    if (u >= 0x2C && u <= 0x38) {
        return (shift ? PUNCT_S : PUNCT)[u - 0x2C];
    }
    return 0;
}

static i2c_master_dev_handle_t s_dev;
static volatile bool s_present;
static kbd_event_cb_t s_on_event;
static kbd_presence_cb_t s_on_presence;
static bool s_sym, s_aa, s_ctrl, s_alt;
/* What each key sent on press, so the release matches even if Sym changed. */
static hid_map_t s_down[ROWS][COLS];

bool kbd_present(void)
{
    return s_present;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, KBD_TIMEOUT);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, sizeof(b), KBD_TIMEOUT);
}

static void key(int r, int c, bool pressed)
{
    if (IS_SYM(r, c) || IS_AA(r, c) || IS_CTRL(r, c) || IS_ALT(r, c)) {
        if (IS_SYM(r, c)) s_sym = pressed;
        if (IS_AA(r, c)) s_aa = pressed;
        if (IS_CTRL(r, c)) s_ctrl = pressed;
        if (IS_ALT(r, c)) s_alt = pressed;
        kbd_event_t ev = {.pressed = pressed};
        ev.mods = ev.held = (s_ctrl ? KBD_MOD_LCTRL : 0) | (s_alt ? KBD_MOD_LALT : 0) |
                            (s_aa ? KBD_MOD_LSHIFT : 0);
        s_on_event(&ev);
        return;
    }
    hid_map_t m;
    if (pressed) {
        m = s_sym ? sym_map(r, c) : MAP_BASE[r][c];
        s_down[r][c] = m;
    } else {
        m = s_down[r][c];
    }
    kbd_event_t ev = {.usage = m.usage, .pressed = pressed};
    ev.held = (s_aa ? KBD_MOD_LSHIFT : 0) | (s_ctrl ? KBD_MOD_LCTRL : 0) | (s_alt ? KBD_MOD_LALT : 0);
    ev.mods = m.mods | ev.held;
    if (!s_ctrl && !s_alt) {
        ev.ch = usage_to_char(m.usage, ev.mods & KBD_MOD_LSHIFT);
    }
    s_on_event(&ev);
}

static bool attach(void)
{
    uint8_t fw = 0;
    if (reg_read(REG_FW, &fw, 1) != ESP_OK || reg_write(REG_MODE, 0) != ESP_OK) {
        return false;
    }
    reg_write(REG_INT_STAT, 0);
    reg_write(REG_EVENT_NUM, 0);
    s_sym = s_aa = s_ctrl = s_alt = false;
    ESP_LOGI(TAG, "keyboard attached, firmware 0x%02x", fw);
    return true;
}

/* Reads every queued event. False when the keyboard stopped answering. */
static bool drain(void)
{
    uint8_t n = 0;
    if (reg_read(REG_EVENT_NUM, &n, 1) != ESP_OK) {
        return false;
    }
    while (n--) {
        uint8_t raw;
        if (reg_read(REG_KEY_EVENT, &raw, 1) != ESP_OK) {
            return false;
        }
        if (raw == 0xFF) {
            break;
        }
        int r = (raw >> 4) & 0x07, c = raw & 0x0F;
        if (r < ROWS && c < COLS) {
            key(r, c, raw & 0x80);
        }
    }
    reg_write(REG_INT_STAT, 0);
    return true;
}

static void kbd_task(void *arg)
{
    for (;;) {
        if (!s_present) {
            if (i2c_master_probe((i2c_master_bus_handle_t)arg, KBD_ADDR, KBD_TIMEOUT) == ESP_OK &&
                attach()) {
                s_present = true;
                s_on_presence(true);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        /* INT is low while events wait; a slow poll covers a missed edge. */
        static int idle;
        if (gpio_get_level(KBD_INT) == 0 || ++idle >= 50) {
            idle = 0;
            if (!drain()) {
                ESP_LOGI(TAG, "keyboard detached");
                s_present = false;
                s_on_presence(false);
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void kbd_start(kbd_event_cb_t on_event, kbd_presence_cb_t on_presence)
{
    s_on_event = on_event;
    s_on_presence = on_presence;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = KBD_SDA,
        .scl_io_num = KBD_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "no I2C bus for the keyboard");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KBD_ADDR,
        .scl_speed_hz = KBD_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    gpio_config_t io = {
        .pin_bit_mask = BIT64(KBD_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    xTaskCreate(kbd_task, "kbd", 3072, bus, 5, NULL);
}
