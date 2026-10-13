/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbhid.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

static const char *TAG = "usbhid";

static kbd_event_cb_t s_on_key;
static kbd_presence_cb_t s_on_presence;
static usbhid_mouse_cb_t s_on_mouse;
static QueueHandle_t s_q;
static int s_keyboards;

/* The driver's callbacks only queue: opening and reading happen on our task. */
typedef struct {
    hid_host_device_handle_t dev;
    bool driver;  /* a new device, else an interface event */
    int event;
} hid_msg_t;

static void driver_cb(hid_host_device_handle_t dev, const hid_host_driver_event_t event, void *arg)
{
    hid_msg_t m = {.dev = dev, .driver = true, .event = event};
    xQueueSend(s_q, &m, 0);
}

static void iface_cb(hid_host_device_handle_t dev, const hid_host_interface_event_t event, void *arg)
{
    hid_msg_t m = {.dev = dev, .driver = false, .event = event};
    xQueueSend(s_q, &m, 0);
}

/* US layout: what a usage types, unshifted and shifted. */
static char ascii_of(uint8_t usage, bool shift)
{
    if (usage >= 0x04 && usage <= 0x1D) {
        return (shift ? 'A' : 'a') + usage - 0x04;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char d[] = "1234567890", s[] = "!@#$%^&*()";
        return (shift ? s : d)[usage - 0x1E];
    }
    static const struct {
        uint8_t u;
        char n, s;
    } t[] = {
        {0x2C, ' ', ' '}, {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'},
        {0x31, '\\', '|'}, {0x33, ';', ':'}, {0x34, '\'', '"'}, {0x35, '`', '~'}, {0x36, ',', '<'},
        {0x37, '.', '>'}, {0x38, '/', '?'},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].u == usage) {
            return shift ? t[i].s : t[i].n;
        }
    }
    return 0;
}

/* A boot keyboard report into press and release events, the way the Tab5
 * keyboard sends them. */
static void keyboard_report(const uint8_t *r, size_t len)
{
    static uint8_t prev_mods, prev[6];
    if (len < 8) {
        return;
    }
    uint8_t mods = r[0];
    const uint8_t *keys = r + 2;
    if (keys[0] == 0x01) {
        return; /* rollover error: too many keys at once */
    }
    bool shift = mods & 0x22;
    for (int i = 0; i < 6; i++) { /* released */
        if (prev[i] && !memchr(keys, prev[i], 6)) {
            kbd_event_t ev = {.usage = prev[i], .mods = mods, .held = mods, .pressed = false};
            s_on_key(&ev);
        }
    }
    if (mods != prev_mods) { /* a modifier alone changed */
        kbd_event_t ev = {.usage = 0, .mods = mods, .held = mods, .pressed = (mods & ~prev_mods) != 0};
        s_on_key(&ev);
    }
    for (int i = 0; i < 6; i++) { /* pressed */
        if (keys[i] && !memchr(prev, keys[i], 6)) {
            kbd_event_t ev = {.usage = keys[i], .mods = mods, .held = mods,
                              .ch = (mods & 0x11) ? 0 : ascii_of(keys[i], shift), .pressed = true};
            s_on_key(&ev);
        }
    }
    prev_mods = mods;
    memcpy(prev, keys, 6);
}

static void mouse_report(const uint8_t *r, size_t len)
{
    if (len < 3 || !s_on_mouse) {
        return;
    }
    /* Boot mice send no wheel, most still put it in a fourth byte */
    s_on_mouse(r[0] & 0x07, (int8_t)r[1], (int8_t)r[2], len >= 4 ? (int8_t)r[3] : 0);
}

static void open_device(hid_host_device_handle_t dev)
{
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(dev, &p) != ESP_OK ||
        (p.proto != HID_PROTOCOL_KEYBOARD && p.proto != HID_PROTOCOL_MOUSE)) {
        return; /* not a keyboard or a mouse */
    }
    hid_host_device_config_t cfg = {.callback = iface_cb, .callback_arg = NULL};
    if (hid_host_device_open(dev, &cfg) != ESP_OK) {
        return;
    }
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            hid_class_request_set_idle(dev, 0, 0);
        }
    }
    if (hid_host_device_start(dev) != ESP_OK) {
        hid_host_device_close(dev);
        return;
    }
    ESP_LOGI(TAG, "%s attached", p.proto == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse");
    if (p.proto == HID_PROTOCOL_KEYBOARD && s_keyboards++ == 0 && s_on_presence) {
        s_on_presence(true);
    }
}

static void hid_task(void *arg)
{
    hid_msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (m.driver) {
            if (m.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
                open_device(m.dev);
            }
            continue;
        }
        hid_host_dev_params_t p;
        if (hid_host_device_get_params(m.dev, &p) != ESP_OK) {
            continue;
        }
        if (m.event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
            uint8_t r[64];
            size_t len = 0;
            if (hid_host_device_get_raw_input_report_data(m.dev, r, sizeof(r), &len) == ESP_OK) {
                if (p.proto == HID_PROTOCOL_KEYBOARD) {
                    keyboard_report(r, len);
                } else {
                    mouse_report(r, len);
                }
            }
        } else if (m.event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
            ESP_LOGI(TAG, "%s gone", p.proto == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse");
            hid_host_device_close(m.dev);
            if (p.proto == HID_PROTOCOL_KEYBOARD) {
                static const uint8_t none[8] = {0};
                keyboard_report(none, sizeof(none)); /* let go of whatever was held */
                if (--s_keyboards == 0 && s_on_presence) {
                    s_on_presence(false);
                }
            }
        }
    }
}

void usbhid_start(kbd_event_cb_t on_key, kbd_presence_cb_t on_presence, usbhid_mouse_cb_t on_mouse)
{
    s_on_key = on_key;
    s_on_presence = on_presence;
    s_on_mouse = on_mouse;
    s_q = xQueueCreate(32, sizeof(hid_msg_t));
    if (!s_q || bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true) != ESP_OK) {
        ESP_LOGW(TAG, "no USB host");
        return;
    }
    const hid_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = driver_cb,
        .callback_arg = NULL,
    };
    if (hid_host_install(&cfg) != ESP_OK ||
        xTaskCreateWithCaps(hid_task, "usbhid", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "HID host did not start");
    }
}
