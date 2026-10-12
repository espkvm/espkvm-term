/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "textview.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "theme.h"

static const char *TAG = "text";

#define SCR_W 1280
#define SCR_H 720
#define MAX_ROWS 80
#define MAX_COLS 256
#define MAX_RUNS 16

LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_mono_13);
LV_FONT_DECLARE(font_mono_9);
/* Largest first: the first that fits the grid is used. */
static const lv_font_t *const FONTS[] = {&font_mono_18, &font_mono_13, &font_mono_9};

/* The reading, as last applied. Touched with the display lock held. */
static char *s_row[MAX_ROWS];
static bool s_dirty[MAX_ROWS];
static int s_cols, s_rows;
static uint16_t s_runs[MAX_RUNS][3]; /* row, first column, length */
static int s_n_runs;
static bool s_gone, s_shape_changed, s_have;

/* The drawing */
static lv_obj_t *s_box, *s_grid, *s_label[MAX_ROWS], *s_hl_bg[MAX_RUNS], *s_hl_text[MAX_RUNS];
static const lv_font_t *s_font;
static int s_cw, s_lh;

static void set_row(int r, const char *text)
{
    if (r < 0 || r >= MAX_ROWS) {
        return;
    }
    if (s_row[r] && !strcmp(s_row[r], text)) {
        return;
    }
    free(s_row[r]);
    size_t n = strlen(text) + 1;
    s_row[r] = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (s_row[r]) {
        memcpy(s_row[r], text, n);
    }
    s_dirty[r] = true;
}

static void read_runs(const cJSON *hl)
{
    s_n_runs = 0;
    const cJSON *run;
    cJSON_ArrayForEach(run, hl)
    {
        if (s_n_runs >= MAX_RUNS || cJSON_GetArraySize(run) != 3) {
            continue;
        }
        for (int i = 0; i < 3; i++) {
            s_runs[s_n_runs][i] = (uint16_t)cJSON_GetArrayItem(run, i)->valueint;
        }
        s_n_runs++;
    }
}

/* Byte offset of column col in a UTF-8 row (or its end). */
static size_t col_offset(const char *s, int col)
{
    size_t o = 0;
    while (s[o] && col > 0) {
        o++;
        while ((s[o] & 0xC0) == 0x80) {
            o++;
        }
        col--;
    }
    return o;
}

/* Picks the font, sizes the grid, makes the row labels. */
static void layout(void)
{
    if (s_cols <= 0 || s_rows <= 0) {
        return;
    }
    s_font = FONTS[sizeof(FONTS) / sizeof(FONTS[0]) - 1];
    for (size_t i = 0; i < sizeof(FONTS) / sizeof(FONTS[0]); i++) {
        int cw = lv_font_get_glyph_width(FONTS[i], 'M', 0), lh = lv_font_get_line_height(FONTS[i]);
        if (cw * s_cols <= SCR_W && lh * s_rows <= SCR_H) {
            s_font = FONTS[i];
            break;
        }
    }
    s_cw = lv_font_get_glyph_width(s_font, 'M', 0);
    s_lh = lv_font_get_line_height(s_font);
    lv_obj_set_size(s_grid, s_cw * s_cols, s_lh * s_rows);
    lv_obj_center(s_grid);
    for (int r = 0; r < MAX_ROWS; r++) {
        if (r < s_rows && !s_label[r]) {
            s_label[r] = lv_label_create(s_grid);
            lv_label_set_long_mode(s_label[r], LV_LABEL_LONG_MODE_CLIP);
        }
        if (s_label[r]) {
            lv_obj_set_hidden(s_label[r], r >= s_rows);
            lv_obj_set_style_text_font(s_label[r], s_font, 0);
            lv_obj_set_size(s_label[r], s_cw * s_cols, s_lh);
            lv_obj_set_pos(s_label[r], 0, r * s_lh);
            s_dirty[r] = true;
        }
    }
    for (int i = 0; i < MAX_RUNS; i++) {
        if (s_hl_text[i]) {
            lv_obj_set_style_text_font(s_hl_text[i], s_font, 0);
        }
    }
    ESP_LOGI(TAG, "%dx%d, cell %dx%d", s_cols, s_rows, s_cw, s_lh);
}

/* The highlight: a light bar with the same characters over it, dark. */
static void draw_runs(void)
{
    for (int i = 0; i < MAX_RUNS; i++) {
        bool on = i < s_n_runs && s_runs[i][0] < s_rows && s_row[s_runs[i][0]];
        if (on && !s_hl_bg[i]) {
            s_hl_bg[i] = lv_obj_create(s_grid);
            lv_obj_remove_style_all(s_hl_bg[i]);
            lv_obj_set_style_bg_color(s_hl_bg[i], lv_color_hex(TH_TEXT), 0);
            lv_obj_set_style_bg_opa(s_hl_bg[i], LV_OPA_COVER, 0);
            s_hl_text[i] = lv_label_create(s_grid);
            lv_obj_set_style_text_color(s_hl_text[i], lv_color_hex(TH_BLACK), 0);
            lv_obj_set_style_text_font(s_hl_text[i], s_font, 0);
        }
        if (!s_hl_bg[i]) {
            continue;
        }
        lv_obj_set_hidden(s_hl_bg[i], !on);
        lv_obj_set_hidden(s_hl_text[i], !on);
        if (!on) {
            continue;
        }
        int r = s_runs[i][0], c = s_runs[i][1], n = s_runs[i][2];
        const char *row = s_row[r];
        size_t a = col_offset(row, c), b = a + col_offset(row + a, n);
        char seg[MAX_COLS * 3 + 1];
        size_t len = b - a < sizeof(seg) - 1 ? b - a : sizeof(seg) - 1;
        memcpy(seg, row + a, len);
        seg[len] = 0;
        lv_obj_set_pos(s_hl_bg[i], c * s_cw, r * s_lh);
        lv_obj_set_size(s_hl_bg[i], n * s_cw, s_lh);
        lv_label_set_text(s_hl_text[i], seg);
        lv_obj_set_pos(s_hl_text[i], c * s_cw, r * s_lh);
        lv_obj_move_foreground(s_hl_bg[i]);
        lv_obj_move_foreground(s_hl_text[i]);
    }
}

static void draw(void)
{
    if (!s_box) {
        return;
    }
    if (s_shape_changed) {
        s_shape_changed = false;
        layout();
    }
    for (int r = 0; r < s_rows && r < MAX_ROWS; r++) {
        if (s_dirty[r] && s_label[r]) {
            lv_label_set_text(s_label[r], s_row[r] ? s_row[r] : "");
            s_dirty[r] = false;
        }
    }
    draw_runs();
}

void textview_on_json(const char *json, size_t len)
{
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (!o) {
        return;
    }
    bsp_display_lock(0);
    if (cJSON_IsTrue(cJSON_GetObjectItem(o, "gone"))) {
        s_gone = true;
    } else if (cJSON_IsTrue(cJSON_GetObjectItem(o, "full"))) {
        int cols = cJSON_GetObjectItem(o, "cols") ? cJSON_GetObjectItem(o, "cols")->valueint : 0;
        int rows = cJSON_GetObjectItem(o, "rows") ? cJSON_GetObjectItem(o, "rows")->valueint : 0;
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(o, "text"));
        if (cols > 0 && cols <= MAX_COLS && rows > 0 && rows <= MAX_ROWS && text) {
            s_shape_changed |= cols != s_cols || rows != s_rows;
            s_cols = cols;
            s_rows = rows;
            const char *p = text;
            for (int r = 0; r < rows; r++) {
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p) : strlen(p);
                char line[MAX_COLS * 3 + 1];
                n = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
                memcpy(line, p, n);
                line[n] = 0;
                set_row(r, line);
                p = nl ? nl + 1 : p + strlen(p);
            }
            read_runs(cJSON_GetObjectItem(o, "highlight"));
            s_gone = false;
            s_have = true;
        }
    } else if (s_have) {
        /* Rows that changed against the reading held */
        const cJSON *pair;
        cJSON_ArrayForEach(pair, cJSON_GetObjectItem(o, "lines"))
        {
            const cJSON *i = cJSON_GetArrayItem(pair, 0);
            const char *t = cJSON_GetStringValue(cJSON_GetArrayItem(pair, 1));
            if (i && t && i->valueint < s_rows) {
                set_row(i->valueint, t);
            }
        }
        read_runs(cJSON_GetObjectItem(o, "highlight"));
        s_gone = false;
    }
    draw();
    bsp_display_unlock();
    cJSON_Delete(o);
}

lv_obj_t *textview_create(lv_obj_t *parent)
{
    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_box, lv_color_hex(0xc8c8c8), 0);
    /* Touches go through to the picture under it: the pointer still works. */
    lv_obj_set_clickable(s_box, false);
    s_grid = lv_obj_create(s_box);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_clickable(s_grid, false);
    memset(s_label, 0, sizeof(s_label));
    memset(s_hl_bg, 0, sizeof(s_hl_bg));
    memset(s_hl_text, 0, sizeof(s_hl_text));
    s_shape_changed = true;
    for (int r = 0; r < MAX_ROWS; r++) {
        s_dirty[r] = true;
    }
    lv_obj_set_hidden(s_box, true);
    draw();
    return s_box;
}

void textview_detach(void)
{
    s_box = s_grid = NULL;
}

void textview_show(bool on)
{
    if (s_box) {
        lv_obj_set_hidden(s_box, !on);
    }
    if (!on) {
        /* A new subscription starts with a whole reading. */
        s_have = false;
        s_gone = false;
    }
}

bool textview_gone(void)
{
    return s_gone;
}

bool textview_has_text(void)
{
    return s_have;
}
