/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Surfaces drawn once at boot, full size, so nothing repeats: brushed dark
 * metal, worn safety-yellow paint, hazard stripes. RGB565 in PSRAM.
 */
#include "texgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "tex";

static uint32_t s_seed;

static inline uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s_seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline float rnd2(int x, int y)
{
    return (hash2(x, y) & 0xffff) / 65535.0f;
}

static inline float smooth(float t)
{
    return t * t * (3 - 2 * t);
}

/* Value noise, 0..1 */
static float vnoise(float x, float y)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = smooth(x - xi), fy = smooth(y - yi);
    float a = rnd2(xi, yi), b = rnd2(xi + 1, yi), c = rnd2(xi, yi + 1), d = rnd2(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/* Fractal noise over a few octaves, about 0..1 */
static float fbm(float x, float y, int oct)
{
    float v = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < oct; i++) {
        v += amp * vnoise(x, y);
        norm += amp;
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.5f;
    }
    return v / norm;
}

static uint32_t s_rng;
static inline float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (s_rng & 0xffffff) / 16777215.0f;
}

typedef struct {
    float *r, *g, *b;
    int w, h;
} fimg_t;

static bool fimg_new(fimg_t *f, int w, int h)
{
    size_t n = (size_t)w * h * sizeof(float);
    f->w = w;
    f->h = h;
    f->r = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    f->g = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    f->b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return f->r && f->g && f->b;
}

static void fimg_free(fimg_t *f)
{
    free(f->r);
    free(f->g);
    free(f->b);
}

/* Mix a colour into one pixel. */
static inline void blend(fimg_t *f, int x, int y, float r, float g, float b, float a)
{
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) {
        return;
    }
    size_t i = (size_t)y * f->w + x;
    f->r[i] += (r - f->r[i]) * a;
    f->g[i] += (g - f->g[i]) * a;
    f->b[i] += (b - f->b[i]) * a;
}

/* Scratches: thin wobbly strokes, mostly along one direction. */
static void scratches(fimg_t *f, int count, float len, float dir, float spread, float r, float g,
                      float b, float alpha)
{
    for (int n = 0; n < count; n++) {
        float x = frand() * f->w, y = frand() * f->h;
        float a = dir + (frand() - 0.5f) * 2 * spread;
        float L = (0.2f + frand() * 0.8f) * len;
        float s = (0.3f + frand() * 0.7f) * alpha;
        for (float t = 0; t < L; t += 0.7f) {
            a += (frand() - 0.5f) * 0.02f;
            float fade = sinf(3.14159f * t / L); /* thin at both ends */
            blend(f, (int)x, (int)y, r, g, b, s * fade);
            x += cosf(a) * 0.7f;
            y += sinf(a) * 0.7f;
        }
    }
}

/* A slow field sampled every G pixels and blended between: a full-screen
 * noise per pixel takes seconds on this CPU. */
#define G 8

static float *coarse(int w, int h, float sx, float sy, float ox, int oct)
{
    int cw = w / G + 2, ch = h / G + 2;
    float *c = heap_caps_malloc((size_t)cw * ch * sizeof(float), MALLOC_CAP_SPIRAM);
    if (c) {
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                c[y * cw + x] = fbm(x * G * sx + ox, y * G * sy, oct);
            }
        }
    }
    return c;
}

static inline float sample(const float *c, int w, int x, int y)
{
    int cw = w / G + 2, xi = x / G, yi = y / G;
    float fx = (x % G) / (float)G, fy = (y % G) / (float)G;
    const float *p = c + yi * cw + xi;
    float a = p[0] + (p[1] - p[0]) * fx, b = p[cw] + (p[cw + 1] - p[cw]) * fx;
    return a + (b - a) * fy;
}

static void metal(fimg_t *f)
{
    float *grime = coarse(f->w, f->h, 0.006f, 0.006f, 50, 4);
    float *rows = heap_caps_malloc(f->h * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!grime || !rows) {
        free(grime);
        free(rows);
        return;
    }
    for (int y = 0; y < f->h; y++) {
        rows[y] = fbm(0.5f, y * 0.15f, 4); /* brushing runs along x */
    }
    for (int y = 0; y < f->h; y++) {
        for (int x = 0; x < f->w; x++) {
            /* streaks: the row's value, drifting slowly along x */
            float brush = rows[y] * 0.6f + rows[(y + x / 160) % f->h] * 0.4f;
            float gr = sample(grime, f->w, x, y);
            float v = 0.82f + 0.32f * brush - 0.30f * fmaxf(gr - 0.5f, 0) + (rnd2(x, y) - 0.5f) * 0.10f;
            size_t i = (size_t)y * f->w + x;
            f->r[i] = 30 * v;
            f->g[i] = 31 * v;
            f->b[i] = 34 * v;
        }
    }
    free(grime);
    free(rows);
    scratches(f, 900, 90, 0.25f, 0.6f, 120, 120, 124, 0.35f);
    scratches(f, 120, 220, -0.6f, 0.3f, 140, 140, 145, 0.25f);
}

/* Worn yellow paint; mask, if set, says where paint is at all (else bare metal). */
static void yellow(fimg_t *f, const uint8_t *paint_mask, float wear)
{
    for (int y = 0; y < f->h; y++) {
        for (int x = 0; x < f->w; x++) {
            size_t i = (size_t)y * f->w + x;
            float mott = fbm(x * 0.02f, y * 0.02f, 4) * 0.6f + fbm(x * 0.004f, y * 0.004f, 3) * 0.4f;
            float grime = fbm(x * 0.01f + 100, y * 0.01f, 4);
            float v = 0.84f + 0.2f * mott - 0.35f * fmaxf(grime - 0.55f, 0);
            float r = 242 * v, g = 182 * v, b = 0;
            /* Chips: paint gone where ragged noise peaks; more near the edges. */
            float band = fminf(30.0f, f->h / 5.0f);
            float edge = 1.0f - fminf(fminf(x, f->w - 1 - x), fminf(y, f->h - 1 - y)) / band;
            float c = fbm(x * 0.03f + 300, y * 0.03f, 5) + fmaxf(edge, 0) * 0.15f + wear;
            bool painted = !paint_mask || paint_mask[i];
            if (c > 0.74f || !painted) {
                /* bare steel under the paint */
                float m = 0.75f + 0.5f * fbm(x * 0.05f, y * 0.05f, 2);
                r = 150 * m;
                g = 148 * m;
                b = 142 * m;
                if (!painted && c <= 0.74f) { /* black stripe paint */
                    r = g = b = 20 * (0.9f + 0.3f * mott);
                }
            } else if (c > 0.715f) { /* dark rim around a chip */
                r *= 0.6f;
                g *= 0.6f;
            }
            f->r[i] = r;
            f->g[i] = g;
            f->b[i] = b;
        }
    }
    scratches(f, f->w * f->h / 900, 60, 0.35f, 0.5f, 175, 170, 155, 0.45f);
    scratches(f, f->w * f->h / 2500, 40, 0.35f, 0.8f, 255, 240, 190, 0.3f);
}

static lv_draw_buf_t *to_buf(fimg_t *f)
{
    lv_draw_buf_t *db = lv_draw_buf_create(f->w, f->h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!db) {
        return NULL;
    }
    for (int y = 0; y < f->h; y++) {
        uint16_t *row = (uint16_t *)(db->data + (size_t)y * db->header.stride);
        for (int x = 0; x < f->w; x++) {
            size_t i = (size_t)y * f->w + x;
            int r = (int)f->r[i], g = (int)f->g[i], b = (int)f->b[i];
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    return db;
}

static lv_draw_buf_t *make(int w, int h, int kind)
{
    fimg_t f;
    lv_draw_buf_t *db = NULL;
    uint8_t *mask = NULL;
    if (!fimg_new(&f, w, h)) {
        goto out;
    }
    if (kind == 0) {
        metal(&f);
    } else if (kind == 1) {
        yellow(&f, NULL, 0);
    } else {
        mask = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
        if (!mask) {
            goto out;
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                mask[(size_t)y * w + x] = ((x + y) % 36) < 18;
            }
        }
        yellow(&f, mask, -0.02f);
    }
    db = to_buf(&f);
out:
    fimg_free(&f);
    free(mask);
    return db;
}

static lv_draw_buf_t *s_metal, *s_plate, *s_hazard;

void texgen_init(int w, int h)
{
    s_seed = esp_random();
    s_rng = esp_random() | 1;
    int64_t t = esp_log_timestamp();
    s_metal = make(w, h, 0);
    s_plate = make(w, 72, 1);
    s_hazard = make(w, 14, 2);
    ESP_LOGI(TAG, "surfaces drawn in %lu ms", (unsigned long)(esp_log_timestamp() - t));
}

const void *texgen_metal(void)
{
    return s_metal;
}

const void *texgen_plate(void)
{
    return s_plate;
}

const void *texgen_hazard(void)
{
    return s_hazard;
}
