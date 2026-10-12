/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi through the Tab5's ESP32-C6 (esp-hosted over SDIO).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_OFF,         /* the C6 is not up (yet, or it failed) */
    NET_IDLE,        /* up, no network chosen */
    NET_CONNECTING,
    NET_CONNECTED,   /* has an address */
    NET_FAILED,      /* wrong password or no such network */
} net_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_ap_t;

#define NET_MAX_APS 20

/* Called from the network task; take the LVGL lock before touching the UI. */
typedef void (*net_state_cb_t)(net_state_t state, const char *info);
typedef void (*net_scan_cb_t)(const net_ap_t *aps, int n);

/* Powers the C6 and brings Wi-Fi up in the background; joins the saved network. */
void net_start(net_state_cb_t on_state, net_scan_cb_t on_scan);
void net_scan(void);
/* Joins and, once it works, remembers the network. */
void net_connect(const char *ssid, const char *pass);
/* The remembered network's name, or "" */
const char *net_saved_ssid(void);
