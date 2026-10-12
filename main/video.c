/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "video.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_area_private.h"

static const char *TAG = "video";

#define SCR_W 1280
#define SCR_H 720
#define IN_MAX (512 * 1024)
#define DEC_MAX (1920 * 1088 * 2)
#define ALIGN 128 /* L2 cache line: what the PPA wants for PSRAM */

static jpeg_decoder_handle_t s_dec;
static ppa_client_handle_t s_ppa;
static uint8_t *s_dec_out;
static size_t s_in_cap, s_dec_cap;

/* Two input buffers: the network fills one while the decoder works on the
 * other, so receiving never waits for a decode. A newer frame replaces one
 * not yet started. */
static uint8_t *s_in[2];
static int s_work;               /* the decoder's buffer */
static size_t s_pending_len;     /* bytes waiting in s_in[s_work ^ 1], 0 = none */
static SemaphoreHandle_t s_in_mu;
static TaskHandle_t s_task;
static void decode_task(void *arg);

/* The panel's own frame buffers, 720x1280 portrait; the UI is turned 90
 * degrees on them. s_panel is the hidden one being drawn; NULL = the LVGL
 * image way. */
static uint16_t *s_panel, *s_pfb[2];
static int s_front;
static esp_lcd_panel_handle_t s_panel_h;
static int64_t s_flip_us;
/* Bumped when the black under the overlays must be laid again; each buffer
 * remembers the one it was cleared for. */
static unsigned s_gen, s_fb_gen[2];
#define PANEL_W SCR_H
#define PANEL_H SCR_W
static volatile bool s_paused;

/* While the picture is up, LVGL does not write the panel: what it draws over
 * the picture (buttons, notes) comes here as pictures of their own, turned
 * to the panel and laid over every frame by the PPA. */
typedef struct {
    int x, y, w, h; /* block in the panel */
    uint32_t *px;   /* ARGB8888, w x h */
    size_t cap;
} overlay_t;
#define OVL_MAX 6
static overlay_t s_ovl[OVL_MAX];
static int s_ovl_n;

static ppa_client_handle_t s_blend;
static bool s_own;                    /* LVGL is kept off the panel */
static bool s_ui_dirty;               /* LVGL drew something meanwhile... */
static lv_area_t s_ui_dirty_at;       /* ...there (screen coordinates) */
static lv_display_flush_cb_t s_lv_flush;
static volatile bool s_redraw;        /* lay the overlays again over the last frame */
static struct {
    int pw, ph, w, h, ox, oy;
    float sc;
    bool ok;
} s_last;
/* Held while a frame is written, so detaching waits for it. */
static SemaphoreHandle_t s_draw_mu;

/* Two screen-size buffers: the PPA writes one while LVGL shows the other. */
static uint16_t *s_fb[2];
static lv_draw_buf_t s_dbuf[2];
static int s_back;
static lv_obj_t *s_img;

static video_stats_t s_stats;
static int64_t s_fps_t0;
static int s_fps_n;
static int s_in_n;
static size_t s_in_bytes;
static int64_t s_dec_us, s_scale_us, s_srm_us, s_blend_us, s_wait_us;
static int s_clears;

/*
 * The panel handle is not handed out by the BSP; esp_lvgl_port keeps it in
 * its display context, which starts like this (esp_lvgl_port_disp.c).
 */
typedef struct {
    int disp_type;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} port_ctx_head_t;

/* The panel's two buffers: one shown, the other being put together. */
static bool panel_fbs(void)
{
    lv_display_t *d = lv_display_get_default();
    const port_ctx_head_t *ctx = d ? lv_display_get_driver_data(d) : NULL;
    void *fb0 = NULL, *fb1 = NULL;
    if (!ctx || !ctx->panel_handle ||
        esp_lcd_dpi_panel_get_frame_buffer(ctx->panel_handle, 2, &fb0, &fb1) != ESP_OK || !fb1) {
        return false;
    }
    s_panel_h = ctx->panel_handle;
    s_pfb[0] = fb0;
    s_pfb[1] = fb1;
    s_front = 0; /* LVGL starts on the first */
    s_panel = s_pfb[1];
    return true;
}

static bool setup(void)
{
    jpeg_decode_engine_cfg_t ecfg = {.timeout_ms = 100};
    if (jpeg_new_decoder_engine(&ecfg, &s_dec) != ESP_OK) {
        return false;
    }
    jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    s_in[0] = jpeg_alloc_decoder_mem(IN_MAX, &in_cfg, &s_in_cap);
    s_in[1] = jpeg_alloc_decoder_mem(IN_MAX, &in_cfg, &s_in_cap);
    s_dec_out = jpeg_alloc_decoder_mem(DEC_MAX, &out_cfg, &s_dec_cap);
    ppa_client_config_t pcfg = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_config_t bcfg = {.oper_type = PPA_OPERATION_BLEND};
    if (!s_in[0] || !s_in[1] || !s_dec_out || ppa_register_client(&pcfg, &s_ppa) != ESP_OK ||
        ppa_register_client(&bcfg, &s_blend) != ESP_OK) {
        return false;
    }
    s_draw_mu = xSemaphoreCreateMutex();
    panel_fbs();
    ESP_LOGI(TAG, "%s", s_panel ? "frames go straight to the panel" : "frames go through LVGL");
    size_t fb_size = SCR_W * SCR_H * 2;
    for (int i = 0; i < 2 && !s_panel; i++) {
        s_fb[i] = heap_caps_aligned_calloc(ALIGN, 1, fb_size, MALLOC_CAP_SPIRAM);
        if (!s_fb[i]) {
            return false;
        }
        lv_draw_buf_init(&s_dbuf[i], SCR_W, SCR_H, LV_COLOR_FORMAT_RGB565, SCR_W * 2, s_fb[i], fb_size);
    }
    s_in_mu = xSemaphoreCreateMutex();
    return s_in_mu && xTaskCreate(decode_task, "video", 4096, NULL, 5, &s_task) == pdPASS;
}

lv_obj_t *video_create(lv_obj_t *parent)
{
    if (!s_dec && !setup()) {
        ESP_LOGE(TAG, "no memory for video");
        s_stats.problem = "no memory for video";
    }
    if (s_panel) {
        /* Nothing to draw: an empty object for the touches */
        s_img = lv_obj_create(parent);
        lv_obj_remove_style_all(s_img);
    } else {
        s_img = lv_image_create(parent);
    }
    lv_obj_set_size(s_img, SCR_W, SCR_H);
    lv_obj_align(s_img, LV_ALIGN_TOP_LEFT, 0, 0);
    s_stats.w = s_stats.h = 0; /* the first frame clears the borders */
    if (s_fb[0] && !s_panel) {
        lv_image_set_src(s_img, &s_dbuf[0]);
        s_back = 1;
    }
    return s_img;
}

void video_detach(void)
{
    if (s_draw_mu) {
        xSemaphoreTake(s_draw_mu, portMAX_DELAY);
    }
    s_img = NULL;
    s_stats.w = s_stats.h = 0;
    s_last.ok = false;
    if (s_draw_mu) {
        xSemaphoreGive(s_draw_mu);
    }
}

/* LVGL's flush while the picture owns the panel: nothing is written, the
 * UI only learns that its overlays must be taken again. */
static void flush_owned(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    if (s_ui_dirty) {
        lv_area_join(&s_ui_dirty_at, &s_ui_dirty_at, area);
    } else {
        s_ui_dirty_at = *area;
    }
    s_ui_dirty = true;
    lv_display_flush_ready(d);
}

void video_own_panel(bool on)
{
    lv_display_t *d = lv_display_get_default();
    if (!s_panel || !d || on == s_own) {
        return;
    }
    s_own = on;
    if (on) {
        s_lv_flush = d->flush_cb;
        lv_display_set_flush_cb(d, flush_owned);
        s_gen++;
        s_redraw = true;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    } else {
        xSemaphoreTake(s_draw_mu, portMAX_DELAY); /* no frame half-way into the panel */
        lv_display_set_flush_cb(d, s_lv_flush);
        s_ovl_n = 0;
        xSemaphoreGive(s_draw_mu);
        lv_obj_invalidate(lv_screen_active());
    }
}

bool video_ui_dirty(lv_area_t *at)
{
    bool d = s_ui_dirty;
    *at = s_ui_dirty_at;
    s_ui_dirty = false;
    return d;
}

uint16_t *video_owned_panel(void)
{
    return s_own ? s_pfb[s_front] : NULL;
}

void video_set_overlays(lv_draw_buf_t *const *img, const lv_area_t *at, const bool *fresh, int n)
{
    if (!s_panel) {
        return;
    }
    xSemaphoreTake(s_draw_mu, portMAX_DELAY);
    int k = 0;
    bool moved = false;
    for (int i = 0; i < n && k < OVL_MAX; i++) {
        const lv_draw_buf_t *b = img[i];
        if (!b) {
            continue;
        }
        /* The picture spans the object plus what it draws outside (shadow) */
        int bw = b->header.w, bh = b->header.h;
        int sx0 = at[i].x1 - (bw - lv_area_get_width(&at[i])) / 2;
        int sy0 = at[i].y1 - (bh - lv_area_get_height(&at[i])) / 2;
        int x1 = LV_MAX(sx0, 0), y1 = LV_MAX(sy0, 0);
        int x2 = LV_MIN(sx0 + bw, SCR_W) - 1, y2 = LV_MIN(sy0 + bh, SCR_H) - 1;
        if (x2 < x1 || y2 < y1) {
            continue;
        }
        overlay_t *o = &s_ovl[k];
        int w = y2 - y1 + 1, h = x2 - x1 + 1; /* turned */
        size_t need = ((size_t)w * h * 4 + ALIGN - 1) & ~(size_t)(ALIGN - 1);
        if (o->cap < need) {
            heap_caps_free(o->px);
            o->px = heap_caps_aligned_alloc(ALIGN, need, MALLOC_CAP_SPIRAM);
            o->cap = o->px ? need : 0;
            if (!o->px) {
                continue;
            }
        }
        /* Screen (x, y) is panel (y, SCR_W - 1 - x) */
        int px = y1, py = SCR_W - 1 - x2;
        bool same = k < s_ovl_n && o->x == px && o->y == py && o->w == w && o->h == h;
        moved |= !same;
        if (same && !fresh[i]) {
            k++; /* already turned */
            continue;
        }
        o->x = px;
        o->y = py;
        o->w = w;
        o->h = h;
        /* Turned in 16x16 tiles, so reads and writes both stay in the cache */
        uint32_t stride = b->header.stride / 4;
        const uint32_t *src = (const uint32_t *)b->data;
        for (int ty = y1; ty <= y2; ty += 16) {
            for (int tx = x1; tx <= x2; tx += 16) {
                int ey = LV_MIN(ty + 15, y2), ex = LV_MIN(tx + 15, x2);
                for (int y = ty; y <= ey; y++) {
                    const uint32_t *row = src + (size_t)(y - sy0) * stride - sx0;
                    uint32_t *dst = o->px + (size_t)(x2 - tx) * w + (y - y1);
                    for (int x = tx; x <= ex; x++) {
                        *dst = row[x];
                        dst -= w;
                    }
                }
            }
        }
        k++;
    }
    moved |= k != s_ovl_n;
    s_ovl_n = k;
    /* Old overlays may lie over black borders; a picture over the whole
     * screen hides them anyway. */
    if (moved && !(s_last.ok && s_stats.w == SCR_W && s_stats.h == SCR_H)) {
        s_gen++;
    }
    s_redraw = true;
    xSemaphoreGive(s_draw_mu);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void video_pause(bool on)
{
    s_paused = on;
}

/* The largest PPA scale (steps of 1/16) that fits the screen. */
static float fit_scale(int w, int h)
{
    float s = fminf((float)SCR_W / w, (float)SCR_H / h);
    return floorf(s * 16) / 16;
}

/* The last frame into the hidden panel buffer, scaled and turned in one pass.
 * The UI's (x, y) is the panel's (y, SCR_W - 1 - x), as esp_lvgl_port turns it. */
static esp_err_t draw_frame(void)
{
    int ow = (int)(s_last.w * s_last.sc);
    ppa_srm_oper_config_t op = {
        .in = {.buffer = s_dec_out, .pic_w = s_last.pw, .pic_h = s_last.ph, .block_w = s_last.w,
               .block_h = s_last.h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = s_panel, .buffer_size = PANEL_W * PANEL_H * 2, .pic_w = PANEL_W,
                .pic_h = PANEL_H, .block_offset_x = s_last.oy, .block_offset_y = SCR_W - s_last.ox - ow,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = s_last.sc,
        .scale_y = s_last.sc,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

/* The overlays over whatever is in the panel. Draw lock held. */
static esp_err_t draw_overlays(void)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < s_ovl_n && err == ESP_OK; i++) {
        const overlay_t *o = &s_ovl[i];
        ppa_blend_oper_config_t op = {
            .in_bg = {.buffer = s_panel, .pic_w = PANEL_W, .pic_h = PANEL_H, .block_w = o->w,
                      .block_h = o->h, .block_offset_x = o->x, .block_offset_y = o->y,
                      .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
            .in_fg = {.buffer = o->px, .pic_w = o->w, .pic_h = o->h, .block_w = o->w, .block_h = o->h,
                      .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888},
            .out = {.buffer = s_panel, .buffer_size = PANEL_W * PANEL_H * 2, .pic_w = PANEL_W,
                    .pic_h = PANEL_H, .block_offset_x = o->x, .block_offset_y = o->y,
                    .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
            .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
            .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        err = ppa_do_blend(s_blend, &op);
    }
    return err;
}

/* The screen put together in the hidden buffer: black where needed, the
 * last frame, the overlays; then shown from the next refresh on, so nothing
 * half-drawn is ever seen. Draw lock held. */
static esp_err_t compose(void)
{
    int back = s_front ^ 1;
    s_panel = s_pfb[back];
    /* The buffer just hidden may still be read out by the last refresh */
    int64_t wait = s_flip_us + 20000 - esp_timer_get_time();
    if (wait > 0) {
        s_wait_us += wait;
        vTaskDelay(pdMS_TO_TICKS(wait / 1000) + 1);
    }
    if (s_fb_gen[back] != s_gen || !s_last.ok) {
        s_clears++;
        memset(s_panel, 0, PANEL_W * PANEL_H * 2);
        esp_cache_msync(s_panel, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_fb_gen[back] = s_gen;
    }
    esp_err_t err = ESP_OK;
    int64_t t0 = esp_timer_get_time();
    if (s_last.ok) {
        err = draw_frame();
    }
    int64_t t1 = esp_timer_get_time();
    if (err == ESP_OK) {
        err = draw_overlays();
    }
    s_srm_us += t1 - t0;
    s_blend_us += esp_timer_get_time() - t1;
    if (err == ESP_OK) {
        esp_lcd_panel_draw_bitmap(s_panel_h, 0, 0, PANEL_W, PANEL_H, s_panel);
        s_front = back;
        s_flip_us = esp_timer_get_time();
    }
    return err;
}

/* The overlays changed: the last frame (or black) under the new ones. */
static void redraw(void)
{
    xSemaphoreTake(s_draw_mu, portMAX_DELAY);
    if (s_own && !s_paused) {
        esp_err_t err = compose();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "redraw: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_draw_mu);
}

void video_frame(const kvm_video_hdr_t *hdr, const uint8_t *data, size_t len)
{
    s_in_n++;
    s_in_bytes += len;
    if (!s_task || !s_img) {
        return;
    }
    if (hdr->type != VIDEO_TYPE_JPEG) {
        s_stats.problem = "the device sends H.264; set it to MJPEG";
        return;
    }
    if (len > s_in_cap) {
        s_stats.problem = "frame too big";
        return;
    }
    xSemaphoreTake(s_in_mu, portMAX_DELAY);
    memcpy(s_in[s_work ^ 1], data, len);
    s_pending_len = len;
    xSemaphoreGive(s_in_mu);
    xTaskNotifyGive(s_task);
}

static void decode_one(const uint8_t *in, size_t len);

static void decode_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_in_mu, portMAX_DELAY);
        size_t len = s_pending_len;
        s_pending_len = 0;
        if (len) {
            s_work ^= 1;
        }
        xSemaphoreGive(s_in_mu);
        bool again = s_redraw;
        s_redraw = false;
        if (len && s_img && !s_paused) {
            decode_one(s_in[s_work], len);
        } else if (again && s_panel) {
            redraw();
        }
    }
}

static void decode_one(const uint8_t *in, size_t len)
{
    int64_t t0 = esp_timer_get_time();
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(in, len, &info) != ESP_OK) {
        return;
    }
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_dec, &dcfg, in, len, s_dec_out, s_dec_cap, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode %ux%u, %u bytes: %s", (unsigned)info.width, (unsigned)info.height,
                 (unsigned)len, esp_err_to_name(err));
        s_stats.problem = "JPEG decode failed";
        return;
    }
    int64_t td = esp_timer_get_time();
    /* The decoder pads to whole 16-pixel blocks. */
    int pw = (info.width + 15) & ~15, ph = (info.height + 15) & ~15;
    float sc = fit_scale(info.width, info.height);
    int ow = (int)(info.width * sc), oh = (int)(info.height * sc);
    int ox = (SCR_W - ow) / 2, oy = (SCR_H - oh) / 2;

    bool fresh = ow != s_stats.w || oh != s_stats.h;
    int64_t ts;
    if (s_panel) {
        /* Scaled and turned in one pass, into the hidden panel buffer */
        xSemaphoreTake(s_draw_mu, portMAX_DELAY);
        if (!s_img || s_paused || !s_own) {
            xSemaphoreGive(s_draw_mu);
            return;
        }
        if (fresh) {
            s_gen++; /* new size: clear the borders */
        }
        s_last = (typeof(s_last)){pw, ph, info.width, info.height, ox, oy, sc, true};
        esp_err_t err = compose();
        xSemaphoreGive(s_draw_mu);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA: %s", esp_err_to_name(err));
            s_stats.problem = "scaling failed";
            return;
        }
        ts = esp_timer_get_time();
    } else {
        uint16_t *fb = s_fb[s_back];
        if (fresh) {
            memset(fb, 0, SCR_W * SCR_H * 2); /* new size: clear the borders */
            memset(s_fb[s_back ^ 1], 0, SCR_W * SCR_H * 2);
        }
        ppa_srm_oper_config_t op = {
            .in = {.buffer = s_dec_out, .pic_w = pw, .pic_h = ph, .block_w = info.width,
                   .block_h = info.height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out = {.buffer = fb, .buffer_size = SCR_W * SCR_H * 2, .pic_w = SCR_W, .pic_h = SCR_H,
                    .block_offset_x = ox, .block_offset_y = oy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = sc,
            .scale_y = sc,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK) {
            s_stats.problem = "scaling failed";
            return;
        }
        ts = esp_timer_get_time();
        bsp_display_lock(0);
        if (s_img) {
            lv_image_set_src(s_img, &s_dbuf[s_back]);
            lv_obj_invalidate(s_img);
        }
        bsp_display_unlock();
        s_back ^= 1;
    }

    int64_t t1 = esp_timer_get_time();
    s_stats.problem = NULL;
    s_stats.src_w = info.width;
    s_stats.src_h = info.height;
    s_stats.x = ox;
    s_stats.y = oy;
    s_stats.w = ow;
    s_stats.h = oh;
    s_stats.decode_ms = (int)((t1 - t0) / 1000);
    s_fps_n++;
    s_dec_us += td - t0;
    s_scale_us += ts - td;
    if (t1 - s_fps_t0 >= 1000000) {
        s_stats.fps = s_fps_n * 1e6f / (t1 - s_fps_t0);
        ESP_LOGD(TAG, "in %d frames %u KB, shown %d, decode %d ms, compose %d ms (scale %d, overlays %d, wait %d, clears %d)",
                 s_in_n, (unsigned)(s_in_bytes / 1024), s_fps_n, (int)(s_dec_us / 1000 / s_fps_n),
                 (int)(s_scale_us / 1000 / s_fps_n), (int)(s_srm_us / 1000 / s_fps_n),
                 (int)(s_blend_us / 1000 / s_fps_n), (int)(s_wait_us / 1000 / s_fps_n), s_clears);
        s_srm_us = s_blend_us = s_wait_us = 0;
        s_clears = 0;
        s_in_n = 0;
        s_in_bytes = 0;
        s_dec_us = s_scale_us = 0;
        s_fps_t0 = t1;
        s_fps_n = 0;
    }
}

void video_get_stats(video_stats_t *out)
{
    *out = s_stats;
}
