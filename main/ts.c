/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * microlink's start and stop each take seconds, so they run on a task of
 * their own; the screens only read the status.
 */
#include "ts.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "microlink.h"
#include "nvs.h"

static const char *TAG = "ts";

#define NVS_NS "ts"

static SemaphoreHandle_t s_mu, s_wake;
static microlink_t *s_ml;
static volatile bool s_net;
/* microlink keeps the config's pointers: these must outlive the session. */
static ts_cfg_t s_cfg, s_running_cfg;

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t en = 0;
    nvs_get_u8(h, "enable", &en);
    s_cfg.enable = en;
    size_t n = sizeof(s_cfg.key);
    nvs_get_str(h, "key", s_cfg.key, &n);
    n = sizeof(s_cfg.name);
    nvs_get_str(h, "name", s_cfg.name, &n);
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "enable", s_cfg.enable);
    nvs_set_str(h, "key", s_cfg.key);
    nvs_set_str(h, "name", s_cfg.name);
    nvs_commit(h);
    nvs_close(h);
}

static void stop(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    microlink_t *ml = s_ml;
    s_ml = NULL;
    xSemaphoreGive(s_mu);
    if (ml) {
        microlink_stop(ml);
        microlink_destroy(ml);
        ESP_LOGI(TAG, "left the tailnet");
    }
}

static void reconcile(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    ts_cfg_t want = s_cfg;
    bool running = s_ml != NULL;
    xSemaphoreGive(s_mu);
    bool on = want.enable && want.key[0] && s_net;
    if (running && on && !memcmp(&want, &s_running_cfg, sizeof(want))) {
        return;
    }
    stop();
    if (!on) {
        return;
    }
    s_running_cfg = want;
    microlink_config_t c = {
        .auth_key = s_running_cfg.key,
        .device_name = s_running_cfg.name[0] ? s_running_cfg.name : NULL,
        .enable_derp = true,
        /* Direct paths: the picture is megabits, a relay is not enough. This
         * client has no capture to keep fed, so the probing costs nothing. */
        .enable_stun = true,
        .enable_disco = true,
        .ctrl_tls = true,
    };
    microlink_t *ml = microlink_init(&c);
    if (!ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        return;
    }
    esp_err_t err = microlink_start(ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start: %s", esp_err_to_name(err));
        microlink_destroy(ml);
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_ml = ml;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "joining the tailnet as '%s'", c.device_name ? c.device_name : "(default)");
}

static void ts_task(void *arg)
{
    for (;;) {
        /* A settings change, or a periodic retry after a failed start */
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(15000));
        reconcile();
    }
}

void ts_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    load();
    xTaskCreate(ts_task, "ts", 6144, NULL, 4, NULL);
}

void ts_net_up(bool up)
{
    s_net = up;
    xSemaphoreGive(s_wake);
}

void ts_get_cfg(ts_cfg_t *out)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mu);
}

void ts_set_cfg(const ts_cfg_t *cfg)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_mu);
    save();
    xSemaphoreGive(s_wake);
}

static const char *state_words(microlink_state_t st)
{
    switch (st) {
    case ML_STATE_WIFI_WAIT: return "waiting for the network";
    case ML_STATE_CONNECTING: return "connecting";
    case ML_STATE_REGISTERING: return "registering";
    case ML_STATE_CONNECTED: return "connected";
    case ML_STATE_RECONNECTING: return "reconnecting";
    case ML_STATE_ERROR: return "error (check the key)";
    default: return "idle";
    }
}

void ts_status(ts_status_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!s_ml) {
        out->state = !s_cfg.enable ? "off" : !s_cfg.key[0] ? "no key" : !s_net ? "waiting for Wi-Fi"
                                                                                : "starting";
    } else {
        out->running = true;
        out->connected = microlink_is_connected(s_ml);
        out->state = state_words(microlink_get_state(s_ml));
        uint32_t ip = microlink_get_vpn_ip(s_ml);
        if (ip) {
            microlink_ip_to_str(ip, out->ip);
        }
        strlcpy(out->self, microlink_get_self_name(s_ml), sizeof(out->self));
        out->peers = microlink_get_peer_count(s_ml);
    }
    xSemaphoreGive(s_mu);
}

int ts_peers(ts_peer_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_ml) {
        int total = microlink_get_peer_count(s_ml);
        for (int i = 0; i < total && n < max; i++) {
            microlink_peer_info_t p;
            if (microlink_get_peer_info(s_ml, i, &p) != ESP_OK) {
                continue;
            }
            strlcpy(out[n].name, p.hostname, sizeof(out[n].name));
            microlink_ip_to_str(p.vpn_ip, out[n].ip);
            out[n].online = p.online;
            out[n].direct = p.direct_path;
            n++;
        }
    }
    xSemaphoreGive(s_mu);
    return n;
}

bool ts_resolve(const char *name, char ip[20])
{
    uint32_t a = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_ml && microlink_is_connected(s_ml)) {
        a = microlink_resolve(s_ml, name);
    }
    xSemaphoreGive(s_mu);
    if (a) {
        microlink_ip_to_str(a, ip);
    }
    return a != 0;
}
