/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ota";

#define API_URL  "https://api.github.com/repos/" OTA_REPO "/releases/latest"
#define JSON_MAX (64 * 1024)

static ota_info_t s_info;
static char s_url[512];
static volatile bool s_busy;

void ota_init(void)
{
    snprintf(s_info.current, sizeof(s_info.current), "%s", esp_app_get_description()->version);
    /* Our own table has two app slots and no factory app; under M5 Launcher
     * the launcher is the factory app and the slots are its business. */
    bool ours = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL) &&
                !esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    s_info.state = ours ? OTA_IDLE : OTA_UNSUPPORTED;
}

void ota_good(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "this image works: keeping it");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static void fail(const char *why)
{
    snprintf(s_info.error, sizeof(s_info.error), "%s", why);
    s_info.state = OTA_FAILED;
    ESP_LOGW(TAG, "%s", why);
}

/* The latest release's tag and the URL of its app image. */
static void check_task(void *arg)
{
    char *buf = heap_caps_malloc(JSON_MAX, MALLOC_CAP_SPIRAM);
    esp_http_client_config_t cfg = {
        .url = API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = buf ? esp_http_client_init(&cfg) : NULL;
    int len = 0, status = 0;
    if (c) {
        esp_http_client_set_header(c, "User-Agent", "espkvm-term");
        esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
        if (esp_http_client_open(c, 0) == ESP_OK) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            int n;
            while (len < JSON_MAX - 1 && (n = esp_http_client_read(c, buf + len, JSON_MAX - 1 - len)) > 0) {
                len += n;
            }
        }
        esp_http_client_cleanup(c);
    }
    cJSON *j = NULL;
    if (status == 200) {
        buf[len] = 0;
        j = cJSON_Parse(buf);
    }
    const cJSON *tag = cJSON_GetObjectItem(j, "tag_name");
    s_url[0] = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "assets"))
    {
        const cJSON *name = cJSON_GetObjectItem(a, "name"), *url = cJSON_GetObjectItem(a, "browser_download_url");
        if (cJSON_IsString(name) && cJSON_IsString(url) && !strcmp(name->valuestring, OTA_ASSET)) {
            snprintf(s_url, sizeof(s_url), "%s", url->valuestring);
        }
    }
    if (!buf) {
        fail("no memory");
    } else if (status == 404) {
        fail("no release yet");
    } else if (status != 200) {
        char why[48];
        snprintf(why, sizeof(why), status ? "GitHub answered %d" : "GitHub not reachable", status);
        fail(why);
    } else if (!cJSON_IsString(tag) || !s_url[0]) {
        fail("the release has no " OTA_ASSET);
    } else {
        snprintf(s_info.latest, sizeof(s_info.latest), "%s", tag->valuestring);
        s_info.state = strcmp(s_info.latest, s_info.current) ? OTA_AVAILABLE : OTA_LATEST;
        ESP_LOGI(TAG, "running %s, latest %s", s_info.current, s_info.latest);
    }
    cJSON_Delete(j);
    free(buf);
    s_busy = false;
    vTaskDelete(NULL);
}

static void install_task(void *arg)
{
    esp_http_client_config_t http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048, /* GitHub redirects to a long signed URL */
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        int size = esp_https_ota_get_image_size(h), got = esp_https_ota_get_image_len_read(h);
        if (size > 0) {
            s_info.percent = (int)((int64_t)got * 100 / size);
        }
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        err = ESP_OK;
    }
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        h = NULL;
        if (err == ESP_OK) {
            s_info.state = OTA_DONE;
            ESP_LOGI(TAG, "installed %s, restarting", s_info.latest);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
    }
    if (h) {
        esp_https_ota_abort(h);
    }
    char why[96];
    snprintf(why, sizeof(why), "install failed: %s", esp_err_to_name(err));
    fail(why);
    s_busy = false;
    vTaskDelete(NULL);
}

void ota_check(void)
{
    if (s_busy || s_info.state == OTA_UNSUPPORTED) {
        return;
    }
    s_busy = true;
    s_info.state = OTA_CHECKING;
    if (xTaskCreate(check_task, "ota_check", 8192, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        fail("no memory");
    }
}

void ota_install(void)
{
    if (s_busy || s_info.state != OTA_AVAILABLE) {
        return;
    }
    s_busy = true;
    s_info.percent = 0;
    s_info.state = OTA_INSTALLING;
    if (xTaskCreate(install_task, "ota", 8192, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        fail("no memory");
    }
}

void ota_get(ota_info_t *out)
{
    *out = s_info;
}
