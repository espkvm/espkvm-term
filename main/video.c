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
#include "freertos/idf_additions.h"
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
static size_t s_in_cap, s_dec_cap;

/* Two decoded frames: the decoder fills one while the other is drawn, so a
 * decode and a draw run at the same time. s_shown is the one the screen was
 * last drawn from (the decoder keeps off it), s_ready one decoded and not
 * drawn yet, -1 = none. */
typedef struct {
    int pw, ph, w, h; /* padded and real size */
} frame_t;
static uint8_t *s_dec_buf[2];
static frame_t s_frm[2];
static int s_shown = -1, s_ready = -1;
static SemaphoreHandle_t s_frm_mu;
static TaskHandle_t s_draw_task;
static void draw_task(void *arg);

/* What part of the frame is on screen, and where: zoom 1 = all of it. */
typedef struct {
    int sx, sy, sw, sh; /* in the frame */
    int ox, oy, ow, oh; /* on the 1280x720 screen */
    float sc;
} view_t;
static float s_zoom = 1, s_zcx, s_zcy; /* draw lock */
/* The Tab5 upside down: the UI turned 270 degrees on the panel instead of 90 */
static volatile bool s_flip;
static view_t s_view;                  /* the last one drawn */

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
#define OVL_MAX 8
static overlay_t s_ovl[OVL_MAX];
static int s_ovl_n;

static ppa_client_handle_t s_blend;
static bool s_own;                    /* LVGL is kept off the panel */
static bool s_ui_dirty;               /* LVGL drew something meanwhile... */
static lv_area_t s_ui_dirty_at;       /* ...there (screen coordinates) */
static lv_display_flush_cb_t s_lv_flush;
static volatile bool s_redraw;        /* draw the last frame again (overlays, zoom) */
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
static int s_clears, s_dec_n;

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
    s_dec_buf[0] = jpeg_alloc_decoder_mem(DEC_MAX, &out_cfg, &s_dec_cap);
    s_dec_buf[1] = jpeg_alloc_decoder_mem(DEC_MAX, &out_cfg, &s_dec_cap);
    ppa_client_config_t pcfg = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_config_t bcfg = {.oper_type = PPA_OPERATION_BLEND};
    if (!s_in[0] || !s_in[1] || !s_dec_buf[0] || !s_dec_buf[1] || ppa_register_client(&pcfg, &s_ppa) != ESP_OK ||
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
    s_frm_mu = xSemaphoreCreateMutex();
    return s_in_mu && s_frm_mu &&
           /* Stacks in PSRAM: neither task touches the flash */
           xTaskCreateWithCaps(draw_task, "draw", 4096, NULL, 5, &s_draw_task, MALLOC_CAP_SPIRAM) == pdPASS &&
           xTaskCreateWithCaps(decode_task, "video", 4096, NULL, 5, &s_task, MALLOC_CAP_SPIRAM) == pdPASS;
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
    if (s_frm_mu) {
        xSemaphoreTake(s_frm_mu, portMAX_DELAY);
        s_shown = s_ready = -1;
        xSemaphoreGive(s_frm_mu);
    }
    s_zoom = 1;
    s_view = (view_t){0};
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
        if (s_draw_task) {
            xTaskNotifyGive(s_draw_task);
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
        /* Screen (x, y) is panel (y, SCR_W - 1 - x), or (SCR_H - 1 - y, x) flipped */
        int px = s_flip ? SCR_H - 1 - y2 : y1, py = s_flip ? x1 : SCR_W - 1 - x2;
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
                    if (s_flip) {
                        uint32_t *dst = o->px + (size_t)(tx - x1) * w + (y2 - y);
                        for (int x = tx; x <= ex; x++) {
                            *dst = row[x];
                            dst += w;
                        }
                    } else {
                        uint32_t *dst = o->px + (size_t)(x2 - tx) * w + (y - y1);
                        for (int x = tx; x <= ex; x++) {
                            *dst = row[x];
                            dst -= w;
                        }
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
    if (moved && !(s_shown >= 0 && s_stats.w == SCR_W && s_stats.h == SCR_H)) {
        s_gen++;
    }
    s_redraw = true;
    xSemaphoreGive(s_draw_mu);
    if (s_draw_task) {
        xTaskNotifyGive(s_draw_task);
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

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Which part of the frame shows, at the zoom asked for. Draw lock held. */
static view_t view_of(const frame_t *f)
{
    view_t v;
    float base = fit_scale(f->w, f->h);
    v.sc = floorf(base * s_zoom * 16) / 16;
    v.sc = v.sc < base ? base : v.sc > 8 ? 8 : v.sc;
    v.sw = LV_MIN(f->w, (int)(SCR_W / v.sc));
    v.sh = LV_MIN(f->h, (int)(SCR_H / v.sc));
    if (s_zoom <= 1) {
        s_zcx = f->w / 2.0f;
        s_zcy = f->h / 2.0f;
    }
    v.sx = clampi((int)(s_zcx - v.sw / 2.0f), 0, f->w - v.sw);
    v.sy = clampi((int)(s_zcy - v.sh / 2.0f), 0, f->h - v.sh);
    /* the centre as it ended up, so a pan past the edge does not pile up */
    s_zcx = v.sx + v.sw / 2.0f;
    s_zcy = v.sy + v.sh / 2.0f;
    v.ow = (int)(v.sw * v.sc);
    v.oh = (int)(v.sh * v.sc);
    v.ox = (SCR_W - v.ow) / 2;
    v.oy = (SCR_H - v.oh) / 2;
    return v;
}

/* A frame into the hidden panel buffer, scaled and turned in one pass.
 * The UI's (x, y) is the panel's (y, SCR_W - 1 - x), as esp_lvgl_port turns it. */
static esp_err_t draw_frame(const uint8_t *buf, const frame_t *f, const view_t *v)
{
    ppa_srm_oper_config_t op = {
        .in = {.buffer = buf, .pic_w = f->pw, .pic_h = f->ph, .block_w = v->sw, .block_h = v->sh,
               .block_offset_x = v->sx, .block_offset_y = v->sy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = s_panel, .buffer_size = PANEL_W * PANEL_H * 2, .pic_w = PANEL_W,
                .pic_h = PANEL_H,
                .block_offset_x = s_flip ? PANEL_W - v->oy - v->oh : v->oy,
                .block_offset_y = s_flip ? v->ox : SCR_W - v->ox - v->ow,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = s_flip ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = v->sc,
        .scale_y = v->sc,
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
 * frame, the overlays; then shown from the next refresh on, so nothing
 * half-drawn is ever seen. Draw lock held; buf NULL = no frame yet. */
static esp_err_t compose(const uint8_t *buf, const frame_t *f)
{
    int back = s_front ^ 1;
    s_panel = s_pfb[back];
    view_t v = {0};
    if (buf) {
        v = view_of(f);
        if (v.ox != s_view.ox || v.oy != s_view.oy || v.ow != s_view.ow || v.oh != s_view.oh) {
            s_gen++; /* the picture's edges moved: clear the borders */
        }
    }
    /* The buffer just hidden may still be read out by the last refresh */
    int64_t wait = s_flip_us + 17000 - esp_timer_get_time();
    if (wait > 0) {
        s_wait_us += wait;
        vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000));
    }
    if (s_fb_gen[back] != s_gen || !buf) {
        s_clears++;
        memset(s_panel, 0, PANEL_W * PANEL_H * 2);
        esp_cache_msync(s_panel, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_fb_gen[back] = s_gen;
    }
    esp_err_t err = ESP_OK;
    int64_t t0 = esp_timer_get_time();
    if (buf) {
        err = draw_frame(buf, f, &v);
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
        if (buf) {
            s_view = v;
        }
    }
    return err;
}

/* Without the panel's own buffers: into an LVGL image, unturned. */
static esp_err_t compose_lvgl(const uint8_t *buf, const frame_t *f)
{
    view_t v = view_of(f);
    uint16_t *fb = s_fb[s_back];
    if (v.ox != s_view.ox || v.oy != s_view.oy || v.ow != s_view.ow || v.oh != s_view.oh) {
        memset(s_fb[0], 0, SCR_W * SCR_H * 2); /* the edges moved: clear the borders */
        memset(s_fb[1], 0, SCR_W * SCR_H * 2);
    }
    ppa_srm_oper_config_t op = {
        .in = {.buffer = buf, .pic_w = f->pw, .pic_h = f->ph, .block_w = v.sw, .block_h = v.sh,
               .block_offset_x = v.sx, .block_offset_y = v.sy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = fb, .buffer_size = SCR_W * SCR_H * 2, .pic_w = SCR_W, .pic_h = SCR_H,
                .block_offset_x = v.ox, .block_offset_y = v.oy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = v.sc,
        .scale_y = v.sc,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) {
        return err;
    }
    s_view = v;
    bsp_display_lock(0);
    if (s_img) {
        lv_image_set_src(s_img, &s_dbuf[s_back]);
        lv_obj_invalidate(s_img);
    }
    bsp_display_unlock();
    s_back ^= 1;
    return ESP_OK;
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
        if (len && s_img && !s_paused) {
            decode_one(s_in[s_work], len);
        }
    }
}

/* Into the buffer the screen is not drawn from, then handed to the draw task. */
static void decode_one(const uint8_t *in, size_t len)
{
    int64_t t0 = esp_timer_get_time();
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(in, len, &info) != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_frm_mu, portMAX_DELAY);
    int target = s_shown == 0 ? 1 : 0;
    if (s_ready == target) {
        s_ready = -1; /* not drawn in time: this frame replaces it */
    }
    xSemaphoreGive(s_frm_mu);
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_dec, &dcfg, in, len, s_dec_buf[target], s_dec_cap, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode %ux%u, %u bytes: %s", (unsigned)info.width, (unsigned)info.height,
                 (unsigned)len, esp_err_to_name(err));
        s_stats.problem = "JPEG decode failed";
        return;
    }
    s_dec_us += esp_timer_get_time() - t0;
    s_dec_n++;
    xSemaphoreTake(s_frm_mu, portMAX_DELAY);
    /* The decoder pads to whole 16-pixel blocks. */
    s_frm[target] = (frame_t){(info.width + 15) & ~15, (info.height + 15) & ~15, info.width, info.height};
    s_ready = target;
    xSemaphoreGive(s_frm_mu);
    xTaskNotifyGive(s_draw_task);
}

static void count_frame(void)
{
    int64_t t1 = esp_timer_get_time();
    s_stats.shown_at = t1;
    s_fps_n++;
    if (t1 - s_fps_t0 >= 1000000) {
        s_stats.fps = s_fps_n * 1e6f / (t1 - s_fps_t0);
        s_stats.in_fps = s_in_n * 1e6f / (t1 - s_fps_t0);
        s_stats.decode_ms = s_dec_n ? (int)(s_dec_us / 1000 / s_dec_n) : 0;
        ESP_LOGD(TAG, "in %d frames %u KB, shown %d, decode %d ms, compose %d ms (scale %d, overlays %d, wait %d, clears %d)",
                 s_in_n, (unsigned)(s_in_bytes / 1024), s_fps_n, s_stats.decode_ms,
                 (int)(s_scale_us / 1000 / s_fps_n), (int)(s_srm_us / 1000 / s_fps_n),
                 (int)(s_blend_us / 1000 / s_fps_n), (int)(s_wait_us / 1000 / s_fps_n), s_clears);
        s_srm_us = s_blend_us = s_wait_us = 0;
        s_clears = 0;
        s_in_n = 0;
        s_in_bytes = 0;
        s_dec_us = s_scale_us = 0;
        s_dec_n = 0;
        s_fps_t0 = t1;
        s_fps_n = 0;
    }
}

/* Draws the newest decoded frame, or the last one again when the overlays or
 * the zoom changed. Runs while the next frame decodes. */
static void draw_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_frm_mu, portMAX_DELAY);
        bool fresh = s_ready >= 0;
        if (fresh) {
            s_shown = s_ready;
            s_ready = -1;
        }
        int idx = s_shown;
        frame_t f = idx >= 0 ? s_frm[idx] : (frame_t){0};
        xSemaphoreGive(s_frm_mu);
        bool again = s_redraw;
        s_redraw = false;
        if (!fresh && !again) {
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = ESP_OK;
        xSemaphoreTake(s_draw_mu, portMAX_DELAY);
        if (s_img && !s_paused) {
            if (s_panel && s_own) {
                err = compose(idx >= 0 ? s_dec_buf[idx] : NULL, &f);
            } else if (!s_panel && idx >= 0) {
                err = compose_lvgl(s_dec_buf[idx], &f);
            }
        }
        view_t v = s_view;
        xSemaphoreGive(s_draw_mu);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw: %s", esp_err_to_name(err));
            s_stats.problem = "scaling failed";
            continue;
        }
        if (!fresh || idx < 0) {
            continue;
        }
        s_scale_us += esp_timer_get_time() - t0;
        s_stats.problem = NULL;
        s_stats.src_w = f.w;
        s_stats.src_h = f.h;
        s_stats.x = v.ox;
        s_stats.y = v.oy;
        s_stats.w = v.ow;
        s_stats.h = v.oh;
        count_frame();
    }
}

void video_set_flip(bool flip)
{
    if (!s_draw_mu) {
        s_flip = flip;
        return;
    }
    xSemaphoreTake(s_draw_mu, portMAX_DELAY);
    s_flip = flip;
    s_ovl_n = 0; /* turned the old way: the UI hands them in again */
    s_gen++;
    s_redraw = true;
    xSemaphoreGive(s_draw_mu);
    xTaskNotifyGive(s_draw_task);
}

bool video_flipped(void)
{
    return s_flip;
}

void video_set_zoom(float zoom, float cx, float cy)
{
    if (!s_draw_mu) {
        return;
    }
    xSemaphoreTake(s_draw_mu, portMAX_DELAY);
    s_zoom = zoom < 1 ? 1 : zoom > 8 ? 8 : zoom;
    s_zcx = cx;
    s_zcy = cy;
    s_redraw = true;
    xSemaphoreGive(s_draw_mu);
    xTaskNotifyGive(s_draw_task);
}

void video_get_zoom(float *zoom, float *cx, float *cy)
{
    *zoom = s_zoom;
    *cx = s_zcx;
    *cy = s_zcy;
}

bool video_to_target(int x, int y, float *u, float *v)
{
    view_t w = s_view;
    if (w.ow <= 0 || w.oh <= 0 || s_stats.src_w <= 0) {
        return false;
    }
    float fx = w.sx + (x - w.ox) / w.sc;
    float fy = w.sy + (y - w.oy) / w.sc;
    *u = fx / s_stats.src_w;
    *v = fy / s_stats.src_h;
    *u = *u < 0 ? 0 : *u > 1 ? 1 : *u;
    *v = *v < 0 ? 0 : *v > 1 ? 1 : *v;
    return x >= w.ox && x < w.ox + w.ow && y >= w.oy && y < w.oy + w.oh;
}

float video_scale(void)
{
    return s_view.sc > 0 ? s_view.sc : 1;
}

void video_get_stats(video_stats_t *out)
{
    *out = s_stats;
}
