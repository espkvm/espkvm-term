/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "net";

#define NVS_NS "net"
#define RETRIES 3
#define KNOWN_RETRIES 5 /* then look around for another saved network */

static net_state_cb_t s_on_state;
static net_scan_cb_t s_on_scan;
static bool s_up;
/* Saved networks, the last one joined first. Slot 0 keeps the NVS keys of
 * the single network saved before. */
typedef struct {
    char ssid[33], pass[65];
} saved_t;
static saved_t s_saved[NET_SAVED_MAX];
static int s_n_saved;
static SemaphoreHandle_t s_saved_mu;
/* The next scan picks the strongest saved network and joins it. */
static bool s_pick;
static esp_timer_handle_t s_rescan;
/* The network being joined; saved to NVS only once it gives an address. */
static char s_ssid[33], s_pass[65];
static bool s_unsaved;
static int s_retries;
/* A scan asked for while joining is refused or silently dropped, so until one
 * has come back it is run again once the join settles. */
static bool s_scan_pending, s_scanned;

static void state(net_state_t st, const char *info)
{
    s_on_state(st, info ? info : "");
}

static void slot_keys(int i, char *ssid_key, char *pass_key)
{
    if (i == 0) {
        strcpy(ssid_key, "ssid");
        strcpy(pass_key, "pass");
    } else {
        sprintf(ssid_key, "ssid%d", i);
        sprintf(pass_key, "pass%d", i);
    }
}

static int find_saved(const char *ssid)
{
    for (int i = 0; i < s_n_saved; i++) {
        if (!strcmp(s_saved[i].ssid, ssid)) {
            return i;
        }
    }
    return -1;
}

static void load_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < NET_SAVED_MAX; i++) {
        char sk[8], pk[8];
        slot_keys(i, sk, pk);
        saved_t *e = &s_saved[s_n_saved];
        size_t n = sizeof(e->ssid);
        if (nvs_get_str(h, sk, e->ssid, &n) != ESP_OK || !e->ssid[0]) {
            continue;
        }
        n = sizeof(e->pass);
        if (nvs_get_str(h, pk, e->pass, &n) != ESP_OK) {
            e->pass[0] = 0;
        }
        s_n_saved++;
    }
    nvs_close(h);
}

/* Lock held. */
static void store_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < NET_SAVED_MAX; i++) {
        char sk[8], pk[8];
        slot_keys(i, sk, pk);
        if (i < s_n_saved) {
            nvs_set_str(h, sk, s_saved[i].ssid);
            nvs_set_str(h, pk, s_saved[i].pass);
        } else {
            nvs_erase_key(h, sk);
            nvs_erase_key(h, pk);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

/* The network just joined goes first; the oldest drops off the end. */
static void save(void)
{
    xSemaphoreTake(s_saved_mu, portMAX_DELAY);
    int i = find_saved(s_ssid);
    if (i < 0) {
        i = s_n_saved < NET_SAVED_MAX ? s_n_saved++ : NET_SAVED_MAX - 1;
    }
    memmove(&s_saved[1], &s_saved[0], i * sizeof(saved_t));
    strlcpy(s_saved[0].ssid, s_ssid, sizeof(s_saved[0].ssid));
    strlcpy(s_saved[0].pass, s_pass, sizeof(s_saved[0].pass));
    store_saved();
    xSemaphoreGive(s_saved_mu);
}

const char *net_saved_ssid(void)
{
    return s_n_saved ? s_saved[0].ssid : "";
}

bool net_is_saved(const char *ssid)
{
    xSemaphoreTake(s_saved_mu, portMAX_DELAY);
    bool yes = find_saved(ssid) >= 0;
    xSemaphoreGive(s_saved_mu);
    return yes;
}

void net_forget(const char *ssid)
{
    xSemaphoreTake(s_saved_mu, portMAX_DELAY);
    int i = find_saved(ssid);
    if (i >= 0) {
        memmove(&s_saved[i], &s_saved[i + 1], (s_n_saved - i - 1) * sizeof(saved_t));
        s_n_saved--;
        store_saved();
    }
    xSemaphoreGive(s_saved_mu);
}

static void join(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    s_retries = 0;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    state(NET_CONNECTING, s_ssid);
}

static void pick(const net_ap_t *aps, int n);

static int ap_cmp(const void *a, const void *b)
{
    return ((const net_ap_t *)b)->rssi - ((const net_ap_t *)a)->rssi;
}

static void scan_done(void)
{
    s_scanned = true;
    s_scan_pending = false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    net_ap_t *aps = calloc(NET_MAX_APS, sizeof(*aps));
    int count = 0;
    if (recs && aps && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (int i = 0; i < n && count < NET_MAX_APS; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (!ssid[0]) {
                continue; /* hidden: typed by hand */
            }
            int j;
            for (j = 0; j < count && strcmp(aps[j].ssid, ssid); j++) {
            }
            if (j < count) {
                if (recs[i].rssi > aps[j].rssi) {
                    aps[j].rssi = recs[i].rssi;
                }
                continue;
            }
            strlcpy(aps[count].ssid, ssid, sizeof(aps[count].ssid));
            aps[count].rssi = recs[i].rssi;
            aps[count].open = recs[i].authmode == WIFI_AUTH_OPEN;
            count++;
        }
        qsort(aps, count, sizeof(*aps), ap_cmp);
    } else {
        esp_wifi_clear_ap_list();
    }
    s_on_scan(aps, count);
    if (s_pick) {
        s_pick = false;
        pick(aps, count);
    }
    free(recs);
    free(aps);
}

/* The strongest saved network in range; none seen (or hidden): the last one
 * joined, and a fresh look after a while. */
static void pick(const net_ap_t *aps, int n)
{
    xSemaphoreTake(s_saved_mu, portMAX_DELAY);
    int best = -1;
    for (int i = 0; i < n && best < 0; i++) {
        best = find_saved(aps[i].ssid);
    }
    if (best < 0 && s_n_saved) {
        best = 0;
    }
    if (best >= 0) {
        strlcpy(s_ssid, s_saved[best].ssid, sizeof(s_ssid));
        strlcpy(s_pass, s_saved[best].pass, sizeof(s_pass));
    }
    xSemaphoreGive(s_saved_mu);
    if (best < 0) {
        state(NET_IDLE, NULL);
        return;
    }
    s_unsaved = false;
    join();
}

static void rescan(void *arg)
{
    s_pick = true;
    net_scan();
    if (s_scan_pending) { /* the radio is busy: once more in a moment */
        esp_timer_start_once(s_rescan, 2000000);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (!s_ssid[0]) {
            return;
        }
        /* A known network that drops out: try it a while, then look around
         * for the strongest saved one. A new one: give up soon. */
        if (!s_unsaved && ++s_retries >= KNOWN_RETRIES) {
            s_ssid[0] = 0;
            state(NET_CONNECTING, "looking for a saved network");
            esp_timer_start_once(s_rescan, 2000000);
            return;
        }
        if (!s_unsaved || s_retries++ < RETRIES - 1) {
            vTaskDelay(pdMS_TO_TICKS(s_unsaved ? 500 : 2000));
            esp_wifi_connect();
            state(NET_CONNECTING, s_ssid);
            return;
        }
        char why[48];
        snprintf(why, sizeof(why), "reason %d", d->reason);
        if (d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            strcpy(why, "wrong password?");
        } else if (d->reason == WIFI_REASON_NO_AP_FOUND) {
            strcpy(why, "network not found");
        }
        s_ssid[0] = 0;
        state(NET_FAILED, why);
        if (s_scan_pending || !s_scanned) {
            net_scan();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        if (s_unsaved) {
            save();
            s_unsaved = false;
        }
        s_retries = 0;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "%s: %s", s_ssid, ip);
        state(NET_CONNECTED, ip);
        if (s_scan_pending || !s_scanned) {
            net_scan();
        }
    }
}

static void net_task(void *arg)
{
    /* C6 power is on the second IO expander; esp-hosted resets it on GPIO15. */
    bsp_feature_enable(BSP_FEATURE_WIFI, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    int herr = esp_hosted_init();
    if (herr == 0) {
        herr = esp_hosted_connect_to_slave();
    }
    if (herr != 0) {
        ESP_LOGE(TAG, "esp-hosted did not come up (%d)", herr);
        state(NET_OFF, "Wi-Fi module not answering");
        vTaskDelete(NULL);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    mdns_init(); /* to find "name.local" devices */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
        state(NET_OFF, "Wi-Fi did not start");
        vTaskDelete(NULL);
    }
    s_up = true;

    /* The name the router lists this Tab5 under (DHCP option 12), told apart
     * from another Tab5 by the end of the MAC */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char name[24];
    snprintf(name, sizeof(name), "espkvm-term-%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(sta, name);
    ESP_LOGI(TAG, "named %s", name);

    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK) {
        ESP_LOGI(TAG, "C6 esp-hosted %lu.%lu.%lu", (unsigned long)ver.major1,
                 (unsigned long)ver.minor1, (unsigned long)ver.patch1);
    }

    const esp_timer_create_args_t ta = {.callback = rescan, .name = "rescan"};
    esp_timer_create(&ta, &s_rescan);
    if (s_n_saved) {
        state(NET_CONNECTING, "looking for a saved network");
        s_pick = true;
    } else {
        state(NET_IDLE, NULL);
    }
    net_scan();
    vTaskDelete(NULL);
}

void net_start(net_state_cb_t on_state, net_scan_cb_t on_scan)
{
    s_on_state = on_state;
    s_on_scan = on_scan;
    s_saved_mu = xSemaphoreCreateMutex();
    load_saved();
    state(NET_OFF, "starting Wi-Fi");
    xTaskCreate(net_task, "net", 6144, NULL, 5, NULL);
}

void net_scan(void)
{
    if (!s_up) {
        return;
    }
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    s_scan_pending = err == ESP_ERR_WIFI_STATE;
    if (err != ESP_OK && !s_scan_pending) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    }
}

void net_connect(const char *ssid, const char *pass)
{
    if (!s_up || !ssid[0]) {
        return;
    }
    s_ssid[0] = 0; /* the disconnect below must not retry the old one */
    s_pick = false;
    esp_timer_stop(s_rescan);
    esp_wifi_disconnect();
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    /* No password typed for a saved network: the saved one */
    xSemaphoreTake(s_saved_mu, portMAX_DELAY);
    int i = find_saved(ssid);
    if (i >= 0 && !pass[0]) {
        strlcpy(s_pass, s_saved[i].pass, sizeof(s_pass));
    }
    xSemaphoreGive(s_saved_mu);
    s_unsaved = true;
    join();
}
