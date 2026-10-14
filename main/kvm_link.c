/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_link.h"

#include "lwip/sockets.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "ts.h"

static const char *TAG = "link";

#define NVS_NS "kvm"
#define CA_MAX 4096
#define FRAME_MAX (1024 * 1024)

typedef enum {
    CMD_CONNECT,
    CMD_TRUST,
    CMD_DISTRUST,
    CMD_DISCONNECT,
    CMD_FORGET,
    CMD_ACTION,
    CMD_TEXT,
    CMD_VIDEO,
} cmd_kind_t;
typedef struct {
    cmd_kind_t kind;
    link_cfg_t cfg; /* CMD_FORGET: cfg.host */
    char code[12];
    char action[24];
    link_result_cb_t cb;
    bool on;
} cmd_t;

static QueueHandle_t s_cmds;
static link_state_cb_t s_on_state;
static link_frame_cb_t s_on_frame;
static link_msg_cb_t s_on_msg;
static link_text_cb_t s_on_text;

/* Saved devices, the last used first. */
static link_cfg_t s_list[LINK_MAX_DEVICES];
static int s_list_n;
static SemaphoreHandle_t s_list_mu;

static link_cfg_t s_cfg;
static char s_addr[64]; /* s_cfg.host, with a .local name looked up */
static char s_code[12];
static char *s_ca;        /* pinned CA, PEM */
static char *s_new_ca;    /* fetched, waiting for the user's yes */
static char s_cookie[96]; /* "kvm_session=..." */
static esp_websocket_client_handle_t s_ws, s_video;
static volatile bool s_ws_up, s_video_up;
/* Text mode, and which /video socket is the live one: events from one being
 * replaced (picture <-> text) are ignored. */
static volatile bool s_text;
static volatile int s_video_gen = 1;

/* Reconnects are ours, not the client's: a device that restarted has
 * forgotten the session, so the cookie must be fetched again. */
#define RETRY_MIN_MS 2000
#define RETRY_MAX_MS 30000
#define OPEN_WAIT_MS 20000
#define WS_SILENT_MS 10000
static bool s_session;              /* signed in, keep the link up */
static volatile int64_t s_loop_at; /* the link task's last turn */
/* Latency, for the `link` debug line: /ws ping round trips, and how much later
 * than the quickest one each frame arrives (its pts is the device's clock). */
static int64_t s_ping_at;
static bool s_acks = true;
static bool s_dev_acks; /* the device said it takes acks (WS_D2C_VIDEO_ACKS) */
static QueueHandle_t s_ack_q;
static SemaphoreHandle_t s_video_mu; /* ack_task's send vs. the socket going away */

static void ack_task(void *arg)
{
    uint32_t seq;
    for (;;) {
        if (xQueueReceive(s_ack_q, &seq, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s_video_mu, portMAX_DELAY);
        if (s_video && s_video_up) {
            uint8_t ack[5] = {WS_C2D_VIDEO_ACK, (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16),
                              (uint8_t)(seq >> 24)};
            esp_websocket_client_send_bin(s_video, (const char *)ack, sizeof(ack), pdMS_TO_TICKS(300));
        }
        xSemaphoreGive(s_video_mu);
    }
}
static int s_rtt_min = 1 << 30, s_rtt_max, s_rtt_sum, s_rtt_n;
static bool s_lag_base_set;
static uint16_t s_lag_base;
static int s_lag_floor = 1 << 30, s_lag_max, s_lag_sum, s_lag_n;
static volatile bool s_broken;      /* a socket dropped */
static volatile bool s_video_off;   /* the screen is off: no /video, and that is fine */
static volatile bool s_video_lost;  /* only /video dropped: open it again, no new sign-in */
static int64_t s_opened_at, s_retry_at;
static volatile int64_t s_ws_heard; /* last frame on /ws */
static int s_retry_ms = RETRY_MIN_MS;

/* Video frame reassembly (a frame arrives in several pieces). */
static uint8_t *s_frame;
static size_t s_frame_len;

static void state(link_state_t st, const char *info)
{
    s_on_state(st, info ? info : "");
}

/* ---- NVS ---- */

/* Each device's CA under a key made from its name (NVS keys are 15 chars). */
static void ca_key(const char *host, char key[16])
{
    uint32_t h = 2166136261u;
    for (const char *p = host; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    snprintf(key, 16, "ca_%08lx", (unsigned long)h);
}

static char *load_ca(const char *host)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    char key[16], *ca = NULL;
    ca_key(host, key);
    size_t n = 0;
    if (nvs_get_str(h, key, NULL, &n) == ESP_OK && n > 0 && n <= CA_MAX) {
        ca = malloc(n);
        if (ca && nvs_get_str(h, key, ca, &n) != ESP_OK) {
            free(ca);
            ca = NULL;
        }
    }
    nvs_close(h);
    return ca;
}

static void save_list(nvs_handle_t h)
{
    nvs_set_blob(h, "list", s_list, s_list_n * sizeof(link_cfg_t));
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_list);
    if (nvs_get_blob(h, "list", s_list, &n) == ESP_OK) {
        s_list_n = n / sizeof(link_cfg_t);
    } else {
        /* One device, from before the list: move it in. */
        link_cfg_t c = {0};
        n = sizeof(c.host);
        if (nvs_get_str(h, "host", c.host, &n) == ESP_OK && c.host[0]) {
            n = sizeof(c.user);
            nvs_get_str(h, "user", c.user, &n);
            n = sizeof(c.pass);
            nvs_get_str(h, "pass", c.pass, &n);
            s_list[0] = c;
            s_list_n = 1;
            n = 0;
            if (nvs_get_str(h, "ca", NULL, &n) == ESP_OK && n > 0 && n <= CA_MAX) {
                char *ca = malloc(n), key[16];
                if (ca && nvs_get_str(h, "ca", ca, &n) == ESP_OK) {
                    ca_key(c.host, key);
                    nvs_set_str(h, key, ca);
                }
                free(ca);
            }
            save_list(h);
        }
        nvs_erase_key(h, "host");
        nvs_erase_key(h, "user");
        nvs_erase_key(h, "pass");
        nvs_erase_key(h, "ca");
        nvs_commit(h);
    }
    nvs_close(h);
    if (s_list_n) {
        s_cfg = s_list[0];
        s_ca = load_ca(s_cfg.host);
    }
}

/* After a good sign-in: this device to the top of the list, its CA kept. */
static void save(void)
{
    xSemaphoreTake(s_list_mu, portMAX_DELAY);
    int at = s_list_n < LINK_MAX_DEVICES ? s_list_n : LINK_MAX_DEVICES - 1;
    for (int i = 0; i < s_list_n; i++) {
        if (!strcmp(s_list[i].host, s_cfg.host)) {
            at = i;
            break;
        }
    }
    if (at == s_list_n) {
        s_list_n++;
    }
    memmove(&s_list[1], &s_list[0], at * sizeof(link_cfg_t));
    s_list[0] = s_cfg;
    xSemaphoreGive(s_list_mu);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    save_list(h);
    char key[16];
    ca_key(s_cfg.host, key);
    if (s_ca) {
        nvs_set_str(h, key, s_ca);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void forget(const char *host)
{
    xSemaphoreTake(s_list_mu, portMAX_DELAY);
    for (int i = 0; i < s_list_n; i++) {
        if (!strcmp(s_list[i].host, host)) {
            memmove(&s_list[i], &s_list[i + 1], (s_list_n - i - 1) * sizeof(link_cfg_t));
            s_list_n--;
            break;
        }
    }
    xSemaphoreGive(s_list_mu);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    save_list(h);
    char key[16];
    ca_key(host, key);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    if (!strcmp(host, s_cfg.host)) {
        free(s_ca);
        s_ca = NULL;
    }
}

void link_saved(link_cfg_t *out)
{
    xSemaphoreTake(s_list_mu, portMAX_DELAY);
    if (s_list_n) {
        *out = s_list[0];
    } else {
        memset(out, 0, sizeof(*out));
    }
    xSemaphoreGive(s_list_mu);
}

int link_devices(link_cfg_t *out, int max)
{
    xSemaphoreTake(s_list_mu, portMAX_DELAY);
    int n = s_list_n < max ? s_list_n : max;
    memcpy(out, s_list, n * sizeof(link_cfg_t));
    xSemaphoreGive(s_list_mu);
    return n;
}

/* ---- HTTPS ---- */

typedef struct {
    char *body;
    size_t len, cap;
} resp_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    resp_t *r = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Set-Cookie") &&
        !strncmp(e->header_value, "kvm_session=", 12)) {
        const char *end = strchr(e->header_value, ';');
        size_t n = end ? (size_t)(end - e->header_value) : strlen(e->header_value);
        if (n < sizeof(s_cookie)) {
            memcpy(s_cookie, e->header_value, n);
            s_cookie[n] = 0;
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA && r && r->body) {
        size_t n = e->data_len;
        if (r->len + n >= r->cap) {
            n = r->cap - 1 - r->len;
        }
        memcpy(r->body + r->len, e->data, n);
        r->len += n;
        r->body[r->len] = 0;
    }
    return ESP_OK;
}

/* One request. ca NULL = no check (only for fetching the CA itself). */
static int request(const char *path, const char *post, const char *ca, resp_t *r)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s%s", s_addr, path);
    esp_http_client_config_t c = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .cert_pem = ca,
        .skip_cert_common_name_check = true, /* the CA is pinned; names vary (IP, mDNS) */
        .event_handler = http_event,
        .user_data = r,
        .timeout_ms = 8000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        return -1;
    }
    if (post) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "X-ESP-KVM", "1");
        esp_http_client_set_post_field(h, post, strlen(post));
    }
    if (s_cookie[0]) {
        esp_http_client_set_header(h, "Cookie", s_cookie);
    }
    esp_err_t err = esp_http_client_perform(h);
    int status = err == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", path, esp_err_to_name(err));
    }
    esp_http_client_cleanup(h);
    return status;
}

/* "Name, AB:CD:..." of a PEM certificate, for the trust question. */
static bool describe(const char *pem, char *out, size_t outlen)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool ok = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0;
    if (ok) {
        /* "CN=espkvm CA, O=..." -> "espkvm CA" */
        char dn[160], name[96] = "?";
        if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.subject) > 0) {
            const char *cn = strstr(dn, "CN=");
            if (cn) {
                strlcpy(name, cn + 3, sizeof(name));
                char *comma = strchr(name, ',');
                if (comma) {
                    *comma = 0;
                }
            }
        }
        uint8_t h[32];
        size_t hl = 0;
        psa_hash_compute(PSA_ALG_SHA_256, crt.raw.p, crt.raw.len, h, sizeof(h), &hl);
        int o = snprintf(out, outlen, "%s\n", name);
        for (int i = 0; i < 12 && o < (int)outlen - 4; i++) {
            o += snprintf(out + o, outlen - o, "%02X%s", h[i], i < 11 ? ":" : "");
        }
    }
    mbedtls_x509_crt_free(&crt);
    return ok;
}

/* ---- WebSockets ---- */

/* The WebSocket client hides its socket, so turn Nagle off on every TCP socket
 * once one connects. With it on, a small message (an ack, a ping) waited for
 * the ACK of the one before, which the device delays: 400+ ms round trips. */
static void nodelay_all(void)
{
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        int type = 0;
        socklen_t len = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_STREAM) {
            const int one = 1;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *d = data;
    int gen = (int)(intptr_t)arg; /* 0 = /ws, else which /video */
    bool video = gen != 0;
    if (video && gen != s_video_gen) {
        return; /* a socket being replaced */
    }
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        nodelay_all();
        if (video) {
            s_video_up = true;
            s_dev_acks = false;
            uint8_t sub = s_text ? WS_SUBSCRIBE_TEXT : 1; /* 1 = the picture */
            esp_websocket_client_send_bin(s_video, (const char *)&sub, 1, pdMS_TO_TICKS(1000));
        } else {
            s_ws_up = true;
            s_ws_heard = esp_timer_get_time();
            uint8_t ping = WS_C2D_PING;
            esp_websocket_client_send_bin(s_ws, (const char *)&ping, 1, pdMS_TO_TICKS(1000));
        }
        if (s_ws_up && (s_video_up || s_video_off)) {
            s_retry_ms = RETRY_MIN_MS;
            state(LINK_ONLINE, s_cfg.host);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "%s dropped", video ? "/video" : "/ws");
        if (video) {
            s_video_up = false;
        } else {
            s_ws_up = false;
        }
        if (video && s_ws_up) {
            s_video_lost = true;
        } else if (!s_broken) {
            s_broken = true;
            state(LINK_WORKING, "reconnecting");
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!video) {
            s_ws_heard = esp_timer_get_time();
        }
        if (video && s_text) {
            /* JSON readings; text frames, maybe in pieces */
            if ((d->op_code != 0x1 && d->op_code != 0x0) || d->payload_len >= FRAME_MAX) {
                break;
            }
            if (d->payload_offset == 0) {
                s_frame_len = 0;
            }
            memcpy(s_frame + d->payload_offset, d->data_ptr, d->data_len);
            s_frame_len = d->payload_offset + d->data_len;
            if (s_frame_len == (size_t)d->payload_len && s_on_text) {
                s_frame[s_frame_len] = 0;
                s_on_text((const char *)s_frame, s_frame_len);
            }
            break;
        }
        if (d->op_code != 0x2 && d->op_code != 0x0) {
            break; /* text, ping, pong */
        }
        if (!video) {
            if (d->payload_offset == 0 && d->data_len == d->payload_len) {
                if (d->data_len >= 1 && (uint8_t)d->data_ptr[0] == WS_D2C_PONG && s_ping_at) {
                    int rtt = (int)((esp_timer_get_time() - s_ping_at) / 1000);
                    s_ping_at = 0;
                    s_rtt_min = rtt < s_rtt_min ? rtt : s_rtt_min;
                    s_rtt_max = rtt > s_rtt_max ? rtt : s_rtt_max;
                    s_rtt_sum += rtt;
                    s_rtt_n++;
                }
                s_on_msg((const uint8_t *)d->data_ptr, d->data_len);
            }
            break;
        }
        if (d->payload_len > FRAME_MAX) {
            break;
        }
        if (d->payload_len == 1 && d->data_len == 1 && (uint8_t)d->data_ptr[0] == WS_D2C_VIDEO_ACKS) {
            s_dev_acks = true; /* older firmware reads an ack as a keyframe request */
            break;
        }
        if (d->payload_offset == 0) {
            s_frame_len = 0;
        }
        memcpy(s_frame + d->payload_offset, d->data_ptr, d->data_len);
        s_frame_len = d->payload_offset + d->data_len;
        if (s_frame_len == (size_t)d->payload_len) {
            kvm_video_hdr_t hdr;
            if (kvm_video_hdr_read(s_frame, s_frame_len, &hdr)) {
                uint16_t d16 = (uint16_t)((uint16_t)(esp_timer_get_time() / 1000) - hdr.pts_ms);
                if (!s_lag_base_set) {
                    s_lag_base = d16;
                    s_lag_base_set = true;
                }
                int rel = (int16_t)(uint16_t)(d16 - s_lag_base);
                s_lag_floor = rel < s_lag_floor ? rel : s_lag_floor;
                s_lag_max = rel > s_lag_max ? rel : s_lag_max;
                s_lag_sum += rel;
                s_lag_n++;
                /* Ack it: the device then keeps only VIDEO_ACK_WINDOW frames
                 * on the way, not a full TCP buffer of old ones. Sent by
                 * ack_task: a send from in here can miss the client's lock. */
                if (s_acks && s_dev_acks && s_ack_q) {
                    uint32_t seq = hdr.seq;
                    xQueueSend(s_ack_q, &seq, 0);
                }
                s_on_frame(&hdr, s_frame + VIDEO_HDR_LEN, s_frame_len - VIDEO_HDR_LEN);
            }
        }
        break;
    default:
        break;
    }
}

/* A live /video socket is closed properly first: destroying one that is
 * still streaming hung the link task (seen going to sleep). */
static void video_close(void)
{
    if (!s_video) {
        return;
    }
    esp_websocket_client_close(s_video, pdMS_TO_TICKS(1000));
    xSemaphoreTake(s_video_mu, portMAX_DELAY);
    esp_websocket_client_destroy(s_video);
    s_video = NULL;
    xSemaphoreGive(s_video_mu);
}

static esp_websocket_client_handle_t ws_open(const char *path, bool video)
{
    char uri[128], headers[128];
    snprintf(uri, sizeof(uri), "wss://%s%s", s_addr, path);
    snprintf(headers, sizeof(headers), "Cookie: %s\r\n", s_cookie);
    esp_websocket_client_config_t c = {
        .uri = uri,
        .cert_pem = s_ca,
        .skip_cert_common_name_check = true,
        .headers = headers,
        .buffer_size = video ? 16384 : 1024,
        .task_stack = 6144,
        .task_prio = video ? 6 : 7,
        .network_timeout_ms = 8000,
        .disable_auto_reconnect = true,
        /* A silent connection is cut within seconds somewhere on the way (seen
         * on Wi-Fi, -0x50 on the device); /ws also gets 0x06 from the keepalive. */
        .ping_interval_sec = 3,
        /* A peer gone without a word leaves the client waiting for the rest
         * of a frame forever (it times out with 0 and tries again); the
         * keepalive turns that into an error. */
        .keep_alive_enable = true,
        .keep_alive_idle = 3,
        .keep_alive_interval = 2,
        .keep_alive_count = 3,
    };
    esp_websocket_client_handle_t h = esp_websocket_client_init(&c);
    if (!h) {
        return NULL;
    }
    esp_websocket_register_events(h, WEBSOCKET_EVENT_ANY, ws_event,
                                  (void *)(intptr_t)(video ? s_video_gen : 0));
    if (esp_websocket_client_start(h) != ESP_OK) {
        esp_websocket_client_destroy(h);
        return NULL;
    }
    return h;
}

static void close_all(void)
{
    if (s_ws) {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_video) {
        xSemaphoreTake(s_video_mu, portMAX_DELAY);
        esp_websocket_client_destroy(s_video);
        s_video = NULL;
        xSemaphoreGive(s_video_mu);
    }
    s_ws_up = s_video_up = false;
    s_broken = false;
}

/* Picture <-> text: a new /video socket asks for the other. */
static void reopen_video(void)
{
    if (!s_video) {
        return; /* not connected: the next sign-in subscribes the right way */
    }
    s_video_gen++;
    s_video_up = false;
    video_close();
    s_opened_at = esp_timer_get_time();
    s_video = ws_open("/video", true);
    if (!s_video) {
        s_broken = true;
    }
}

/* POST /api/v1/<action>; the device answers {"error":"..."} when it says no. */
static void do_action(const char *action, link_result_cb_t cb)
{
    char path[48], buf[160] = "";
    snprintf(path, sizeof(path), "/api/v1/%s", action);
    resp_t r = {.body = buf, .cap = sizeof(buf)};
    int st = s_session && s_ca ? request(path, "{}", s_ca, &r) : -1;
    const char *err = NULL;
    char msg[96] = "";
    if (st < 0) {
        err = "no answer";
    } else if (st != 200) {
        const char *e = strstr(buf, "\"error\":\"");
        if (e) {
            strlcpy(msg, e + 9, sizeof(msg));
            char *q = strchr(msg, '"');
            if (q) {
                *q = 0;
            }
            err = msg;
        } else {
            snprintf(msg, sizeof(msg), "HTTP %d", st);
            err = msg;
        }
    }
    if (cb) {
        cb(action, err == NULL, err);
    }
}

/* A JSON string value: quotes and backslashes escaped, control bytes dropped. */
static void json_esc(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if (*in == '"' || *in == '\\') {
            out[o++] = '\\';
        } else if ((unsigned char)*in < 0x20) {
            continue;
        }
        out[o++] = *in;
    }
    out[o] = 0;
}

/* ---- the steps ---- */

/* "name.local" by mDNS; a tailnet name ("kvm", "kvm.tail1234.ts.net") from
 * the tailnet's peers; anything else goes to the normal resolver. */
static bool resolve(void)
{
    strlcpy(s_addr, s_cfg.host, sizeof(s_addr));
    size_t n = strlen(s_cfg.host);
    bool dotless = !strchr(s_cfg.host, '.');
    if (dotless || (n > 7 && !strcasecmp(s_cfg.host + n - 7, ".ts.net"))) {
        char ip[20];
        if (ts_resolve(s_cfg.host, ip)) {
            strlcpy(s_addr, ip, sizeof(s_addr));
            ESP_LOGI(TAG, "%s is %s on the tailnet", s_cfg.host, s_addr);
        }
        return true;
    }
    if (n <= 6 || strcasecmp(s_cfg.host + n - 6, ".local")) {
        return true;
    }
    char name[64];
    strlcpy(name, s_cfg.host, n - 6 + 1 < sizeof(name) ? n - 6 + 1 : sizeof(name));
    esp_ip4_addr_t ip = {0};
    if (mdns_query_a(name, 3000, &ip) != ESP_OK) {
        ESP_LOGW(TAG, "%s: no mDNS answer", s_cfg.host);
        return false;
    }
    snprintf(s_addr, sizeof(s_addr), IPSTR, IP2STR(&ip));
    ESP_LOGI(TAG, "%s is %s", s_cfg.host, s_addr);
    return true;
}

/* False when it failed in a way worth retrying (no answer). */
static bool sign_in(void)
{
    char body[300], buf[256], user[70], pass[130];
    json_esc(user, sizeof(user), s_cfg.user);
    json_esc(pass, sizeof(pass), s_cfg.pass);
    resp_t r = {.body = buf, .cap = sizeof(buf)};
    buf[0] = 0;
    s_cookie[0] = 0;
    state(LINK_WORKING, "signing in");
    if (!resolve()) {
        state(LINK_FAILED, "name not found");
        return false;
    }
    if (s_code[0]) {
        snprintf(body, sizeof(body), "{\"user\":\"%s\",\"password\":\"%s\",\"code\":\"%s\"}", user, pass,
                 s_code);
    } else {
        snprintf(body, sizeof(body), "{\"user\":\"%s\",\"password\":\"%s\"}", user, pass);
    }
    int st = request("/api/v1/auth/login", body, s_ca, &r);
    if (st < 0) {
        /* A TLS failure with a pinned CA: the device may have a new one. */
        state(LINK_FAILED, "no answer (or its certificate changed)");
        return false;
    }
    if (st == 401 && strstr(buf, "needCode")) {
        state(LINK_NEED_CODE, s_code[0] ? "wrong code" : "enter the two-factor code");
        s_session = false;
        return true;
    }
    if (st != 200 || !s_cookie[0]) {
        state(LINK_FAILED, st == 401 ? "wrong user or password" : "sign-in refused");
        s_session = false;
        return true;
    }
    save();
    s_code[0] = 0; /* used up */
    s_session = true;
    state(LINK_WORKING, "opening the screen");
    s_opened_at = esp_timer_get_time();
    s_ws = ws_open("/ws", false);
    s_video = s_video_off ? NULL : ws_open("/video", true);
    if (!s_ws || (!s_video && !s_video_off)) {
        close_all();
        state(LINK_FAILED, "could not open the sockets");
        return false;
    }
    return true;
}

/* A dropped socket, or sockets that never opened: sign in again, slower each time. */
static void watch(void)
{
    if (!s_session) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_video_lost && s_ws_up && !s_broken) {
        /* The session is fine; only the picture socket went. Too many drops
         * in a row: start over the slow way. */
        static int64_t first;
        static int n;
        s_video_lost = false;
        if (now - first > 10000000) {
            first = now;
            n = 0;
        }
        if (++n <= 4) {
            ESP_LOGI(TAG, "/video again");
            reopen_video();
            return;
        }
        s_broken = true;
    }
    s_video_lost = false;
    /* /ws answers the ping every 2.5 s: silence means it is gone */
    if (s_ws_up && !s_broken && now - s_ws_heard > WS_SILENT_MS * 1000LL) {
        ESP_LOGI(TAG, "/ws silent");
        s_broken = true;
    }
    bool stuck = s_ws && !(s_ws_up && (s_video_up || s_video_off)) && now - s_opened_at > OPEN_WAIT_MS * 1000LL;
    if (s_ws && !s_broken && !stuck) {
        return;
    }
    if (s_ws) { /* first noticed: close now, retry after a pause */
        close_all();
        s_retry_at = now + s_retry_ms * 1000LL;
        ESP_LOGI(TAG, "link lost, signing in again in %d s", s_retry_ms / 1000);
        state(LINK_WORKING, "reconnecting");
        return;
    }
    if (now < s_retry_at) {
        return;
    }
    s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    if (!sign_in() && s_session) {
        s_retry_at = esp_timer_get_time() + s_retry_ms * 1000LL;
    }
}

static void connect_step(void)
{
    close_all();
    s_session = false;
    s_retry_ms = RETRY_MIN_MS;
    if (s_ca) {
        s_session = true; /* a first attempt with no answer is retried too */
        if (!sign_in() && s_session) {
            s_retry_at = esp_timer_get_time() + s_retry_ms * 1000LL;
        }
        return;
    }
    /* First visit: fetch the device's CA without checking it, and ask. */
    state(LINK_WORKING, "fetching the certificate");
    if (!resolve()) {
        state(LINK_FAILED, "name not found");
        return;
    }
    free(s_new_ca);
    s_new_ca = calloc(1, CA_MAX);
    resp_t r = {.body = s_new_ca, .cap = CA_MAX};
    int st = s_new_ca ? request("/cert.pem", NULL, NULL, &r) : -1;
    char desc[160];
    if (st != 200 || !describe(s_new_ca, desc, sizeof(desc))) {
        free(s_new_ca);
        s_new_ca = NULL;
        state(LINK_FAILED, st < 0 ? "device not answering" : "no certificate there");
        return;
    }
    state(LINK_TRUST, desc);
}

static void link_task(void *arg)
{
    cmd_t c;
    for (;;) {
        s_loop_at = esp_timer_get_time();
        /* The console's keepalive: a ping on /ws every 2.5 s, answered with the status. */
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(500)) != pdTRUE) {
            watch();
            int64_t now = esp_timer_get_time();
            static int64_t last_ping;
            if (s_ws_up && now - last_ping >= 2500000) {
                last_ping = now;
                uint8_t ping = WS_C2D_PING;
                s_ping_at = esp_timer_get_time();
                esp_websocket_client_send_bin(s_ws, (const char *)&ping, 1, pdMS_TO_TICKS(500));
            }
            continue;
        }
        switch (c.kind) {
        case CMD_CONNECT:
            if (strcmp(c.cfg.host, s_cfg.host) || !s_ca) { /* this device's own CA, if trusted */
                free(s_ca);
                s_ca = load_ca(c.cfg.host);
            }
            s_cfg = c.cfg;
            strlcpy(s_code, c.code, sizeof(s_code));
            connect_step();
            break;
        case CMD_TRUST:
            if (s_new_ca) {
                free(s_ca);
                s_ca = s_new_ca;
                s_new_ca = NULL;
                s_session = true;
                if (!sign_in() && s_session) {
                    s_retry_at = esp_timer_get_time() + s_retry_ms * 1000LL;
                }
            }
            break;
        case CMD_DISTRUST:
            free(s_new_ca);
            s_new_ca = NULL;
            state(LINK_IDLE, NULL);
            break;
        case CMD_DISCONNECT:
            close_all();
            s_session = false;
            state(LINK_IDLE, NULL);
            break;
        case CMD_FORGET:
            forget(c.cfg.host);
            break;
        case CMD_ACTION:
            do_action(c.action, c.cb);
            break;
        case CMD_TEXT:
            if (c.on != s_text) {
                s_text = c.on;
                reopen_video();
            }
            break;
        case CMD_VIDEO:
            /* Off: the socket goes, nothing arrives; on: a new one, signed in already */
            if (!c.on && !s_video_off) {
                s_video_off = true;
                s_video_gen++;
                s_video_up = false;
                video_close();
            } else if (c.on && s_video_off) {
                s_video_off = false;
                if (s_ws) {
                    s_video_gen++;
                    s_opened_at = esp_timer_get_time();
                    s_video = ws_open("/video", true);
                    if (!s_video) {
                        s_broken = true;
                    }
                }
            }
            break;
        }
    }
}

void link_init(link_state_cb_t on_state, link_frame_cb_t on_frame, link_msg_cb_t on_msg)
{
    s_on_state = on_state;
    s_on_frame = on_frame;
    s_on_msg = on_msg;
    s_frame = heap_caps_malloc(FRAME_MAX, MALLOC_CAP_SPIRAM);
    s_cmds = xQueueCreate(6, sizeof(cmd_t));
    s_list_mu = xSemaphoreCreateMutex();
    load();
    s_ack_q = xQueueCreate(8, sizeof(uint32_t));
    s_video_mu = xSemaphoreCreateMutex();
    xTaskCreate(ack_task, "ack", 3072, NULL, 6, NULL);
    xTaskCreate(link_task, "link", 8192, NULL, 5, NULL);
}

void link_connect(const link_cfg_t *cfg, const char *code)
{
    cmd_t c = {.kind = CMD_CONNECT, .cfg = *cfg};
    if (code) {
        strlcpy(c.code, code, sizeof(c.code));
    }
    xQueueSend(s_cmds, &c, 0);
}

void link_trust(bool yes)
{
    cmd_t c = {.kind = yes ? CMD_TRUST : CMD_DISTRUST};
    xQueueSend(s_cmds, &c, 0);
}

void link_disconnect(void)
{
    cmd_t c = {.kind = CMD_DISCONNECT};
    xQueueSend(s_cmds, &c, 0);
}

bool link_online(void)
{
    return s_ws_up && (s_video_up || s_video_off);
}

void link_video(bool on)
{
    cmd_t c = {.kind = CMD_VIDEO, .on = on};
    xQueueSend(s_cmds, &c, 0);
}

bool link_send(const uint8_t *msg, size_t len)
{
    if (!s_ws_up) {
        return false;
    }
    return esp_websocket_client_send_bin(s_ws, (const char *)msg, len, pdMS_TO_TICKS(200)) >= 0;
}

void link_debug(void)
{
    ESP_LOGI(TAG, "%s (%s): ws %s/%s, video %s/%s%s, acks %s, gen %d, session %d, broken %d, task %lld ms ago",
             s_cfg.host, s_addr,
             s_ws ? "open" : "none",
             s_ws_up ? "up" : "down", s_video ? "open" : "none", s_video_up ? "up" : "down",
             s_video_off ? " (off)" : "", !s_acks ? "off" : s_dev_acks ? "on" : "not offered", s_video_gen, s_session, s_broken,
             (long long)((esp_timer_get_time() - s_loop_at) / 1000));
    /* Frame delay is relative to the quickest frame seen since the link came up
     * (the clocks differ); the quickest is about half the shortest round trip. */
    if (s_rtt_n || s_lag_n) {
        ESP_LOGI(TAG, "ping rtt min/avg/max %d/%d/%d ms (%d); frames later than the quickest: avg %d, max %d ms (%d)",
                 s_rtt_n ? s_rtt_min : -1, s_rtt_n ? s_rtt_sum / s_rtt_n : -1, s_rtt_max, s_rtt_n,
                 s_lag_n ? s_lag_sum / s_lag_n - s_lag_floor : -1, s_lag_n ? s_lag_max - s_lag_floor : -1, s_lag_n);
        s_rtt_min = 1 << 30;
        s_rtt_max = s_rtt_sum = s_rtt_n = 0;
        s_lag_max = s_lag_sum = s_lag_n = 0;
        s_lag_max = -(1 << 30);
    }
}

void link_request_keyframe(void)
{
    if (s_video_up) {
        uint8_t b = 1;
        esp_websocket_client_send_bin(s_video, (const char *)&b, 1, pdMS_TO_TICKS(200));
    }
}

void link_forget(const char *host)
{
    cmd_t c = {.kind = CMD_FORGET};
    strlcpy(c.cfg.host, host, sizeof(c.cfg.host));
    xQueueSend(s_cmds, &c, 0);
}

void link_action(const char *action, link_result_cb_t cb)
{
    cmd_t c = {.kind = CMD_ACTION, .cb = cb};
    strlcpy(c.action, action, sizeof(c.action));
    if (xQueueSend(s_cmds, &c, 0) != pdTRUE && cb) {
        cb(action, false, "busy");
    }
}

void link_set_text_cb(link_text_cb_t cb)
{
    s_on_text = cb;
}

void link_set_text(bool on)
{
    cmd_t c = {.kind = CMD_TEXT, .on = on};
    xQueueSend(s_cmds, &c, 0);
}

bool link_text(void)
{
    return s_text;
}

/* ---- finding devices ---- */

static link_found_cb_t s_found_cb;

/* Anything on the network may offer HTTPS (routers, printers): an ESP-KVM
 * is the one whose sign-in endpoint answers with its own JSON. No CA is
 * checked here; nothing is sent but the GET. */
static bool is_espkvm(const esp_ip_addr_t *ip)
{
    char url[64], body[256];
    if (ip->type != ESP_IPADDR_TYPE_V4) {
        return false;
    }
    snprintf(url, sizeof(url), "https://" IPSTR "/api/v1/auth/session", IP2STR(&ip->u_addr.ip4));
    esp_http_client_config_t c = {
        .url = url,
        .skip_cert_common_name_check = true,
        .timeout_ms = 3000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        return false;
    }
    int len = 0;
    if (esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0 &&
        esp_http_client_get_status_code(h) == 200) {
        len = esp_http_client_read(h, body, sizeof(body) - 1);
    }
    esp_http_client_cleanup(h);
    body[len > 0 ? len : 0] = 0;
    return strstr(body, "\"authenticated\"") && strstr(body, "\"required\"");
}

static void discover_task(void *arg)
{
    static link_found_t found[LINK_MAX_FOUND];
    int n = 0;
    mdns_result_t *res = NULL;
    /* A device with HTTPS on; this client speaks nothing else. */
    if (mdns_query_ptr("_https", "_tcp", 3000, LINK_MAX_FOUND, &res) == ESP_OK) {
        for (mdns_result_t *r = res; r && n < LINK_MAX_FOUND; r = r->next) {
            if (!r->hostname) {
                continue;
            }
            bool dup = false;
            char host[64];
            snprintf(host, sizeof(host), "%s.local", r->hostname);
            for (int i = 0; i < n; i++) {
                dup |= !strcmp(found[i].host, host);
            }
            if (dup || !r->addr || !is_espkvm(&r->addr->addr)) {
                continue;
            }
            strlcpy(found[n].host, host, sizeof(found[n].host));
            strlcpy(found[n].name, r->instance_name ? r->instance_name : r->hostname,
                    sizeof(found[n].name));
            n++;
        }
        mdns_query_results_free(res);
    }
    ESP_LOGI(TAG, "mDNS: %d found", n);
    s_found_cb(found, n);
    s_found_cb = NULL;
    vTaskDelete(NULL);
}

void link_discover(link_found_cb_t cb)
{
    if (s_found_cb) {
        return; /* already looking */
    }
    s_found_cb = cb;
    if (xTaskCreate(discover_task, "discover", 4096, NULL, 4, NULL) != pdPASS) {
        s_found_cb = NULL;
        cb(NULL, 0);
    }
}

void link_set_acks(bool on)
{
    s_acks = on;
}

const char *link_ca(void)
{
    return s_ca;
}
