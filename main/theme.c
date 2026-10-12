/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "theme.h"

#include "texgen.h"

LV_IMAGE_DECLARE(tex_yellow);

static lv_style_t s_screen, s_plate, s_panel, s_btn, s_btn_pr, s_field, s_field_focus,
    s_cursor, s_label, s_scroll, s_bolt;
static bool s_init;

static void tiled(lv_style_t *st, const lv_image_dsc_t *img)
{
    lv_style_set_bg_image_src(st, img);
    lv_style_set_bg_image_tiled(st, true);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
}

/* A full-size surface from texgen; plain colour if it could not be drawn. */
static void surface(lv_style_t *st, const void *img)
{
    if (img) {
        lv_style_set_bg_image_src(st, img);
    }
    lv_style_set_bg_opa(st, LV_OPA_COVER);
}

void theme_init(void)
{
    if (s_init) {
        return;
    }
    s_init = true;
    texgen_init(1280, 720);

    lv_style_init(&s_screen);
    surface(&s_screen, texgen_metal());
    lv_style_set_bg_color(&s_screen, lv_color_hex(0x1e1f21));
    lv_style_set_text_color(&s_screen, lv_color_hex(TH_TEXT));
    lv_style_set_text_font(&s_screen, &font_ui_26);

    lv_style_init(&s_plate);
    surface(&s_plate, texgen_plate());
    lv_style_set_bg_color(&s_plate, lv_color_hex(TH_YELLOW));
    lv_style_set_text_color(&s_plate, lv_color_hex(TH_BLACK));
    lv_style_set_radius(&s_plate, 0);
    lv_style_set_border_width(&s_plate, 0);

    lv_style_init(&s_panel);
    lv_style_set_bg_color(&s_panel, lv_color_hex(0x161718));
    lv_style_set_bg_opa(&s_panel, LV_OPA_80);
    lv_style_set_border_color(&s_panel, lv_color_hex(TH_YELLOW));
    lv_style_set_border_width(&s_panel, 3);
    lv_style_set_border_opa(&s_panel, LV_OPA_70);
    lv_style_set_radius(&s_panel, 4);
    lv_style_set_pad_all(&s_panel, 18);
    lv_style_set_shadow_width(&s_panel, 0);

    lv_style_init(&s_bolt);
    lv_style_set_bg_color(&s_bolt, lv_color_hex(0x6d6a63));
    lv_style_set_bg_grad_color(&s_bolt, lv_color_hex(0x2a2927));
    lv_style_set_bg_grad_dir(&s_bolt, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_bolt, LV_OPA_COVER);
    lv_style_set_radius(&s_bolt, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_bolt, 1);
    lv_style_set_border_color(&s_bolt, lv_color_hex(0x0a0a0a));

    lv_style_init(&s_btn);
    tiled(&s_btn, &tex_yellow);
    lv_style_set_bg_color(&s_btn, lv_color_hex(TH_YELLOW));
    lv_style_set_text_color(&s_btn, lv_color_hex(TH_BLACK));
    lv_style_set_text_font(&s_btn, &font_bold_34);
    lv_style_set_radius(&s_btn, 3);
    lv_style_set_border_width(&s_btn, 2);
    lv_style_set_border_color(&s_btn, lv_color_hex(0x000000));
    lv_style_set_border_opa(&s_btn, LV_OPA_50);
    lv_style_set_shadow_width(&s_btn, 0);
    lv_style_set_pad_hor(&s_btn, 26);
    lv_style_set_pad_ver(&s_btn, 10);

    lv_style_init(&s_btn_pr);
    lv_style_set_bg_image_recolor(&s_btn_pr, lv_color_hex(0x000000));
    lv_style_set_bg_image_recolor_opa(&s_btn_pr, LV_OPA_30);
    lv_style_set_translate_y(&s_btn_pr, 2);

    lv_style_init(&s_field);
    lv_style_set_bg_color(&s_field, lv_color_hex(0x0b0b0b));
    lv_style_set_bg_opa(&s_field, LV_OPA_COVER);
    lv_style_set_text_color(&s_field, lv_color_hex(TH_YELLOW));
    lv_style_set_text_font(&s_field, &font_ui_26);
    lv_style_set_border_color(&s_field, lv_color_hex(0x55524a));
    lv_style_set_border_width(&s_field, 2);
    lv_style_set_radius(&s_field, 2);
    lv_style_set_pad_hor(&s_field, 12);
    lv_style_set_pad_ver(&s_field, 8);

    lv_style_init(&s_field_focus);
    lv_style_set_border_color(&s_field_focus, lv_color_hex(TH_YELLOW));
    lv_style_set_outline_width(&s_field_focus, 0);

    lv_style_init(&s_cursor);
    lv_style_set_border_color(&s_cursor, lv_color_hex(TH_YELLOW));
    lv_style_set_border_width(&s_cursor, 3);
    lv_style_set_border_side(&s_cursor, LV_BORDER_SIDE_LEFT);
    lv_style_set_bg_opa(&s_cursor, LV_OPA_TRANSP);

    lv_style_init(&s_label);
    lv_style_set_text_color(&s_label, lv_color_hex(TH_YELLOW));
    lv_style_set_text_font(&s_label, &font_ui_20);
    lv_style_set_text_letter_space(&s_label, 2);

    lv_style_init(&s_scroll);
    lv_style_set_bg_color(&s_scroll, lv_color_hex(TH_YELLOW));
    lv_style_set_bg_opa(&s_scroll, LV_OPA_70);
    lv_style_set_width(&s_scroll, 6);
    lv_style_set_radius(&s_scroll, 0);
}

static void bolts(lv_obj_t *panel)
{
    static const lv_align_t corners[] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT,
                                         LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(panel);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &s_bolt, 0);
        lv_obj_set_size(b, 10, 10);
        lv_obj_set_ignore_layout(b, true);
        lv_obj_set_floating(b, true);
        lv_obj_set_clickable(b, false);
        int dx = (i & 1) ? 10 : -10, dy = (i & 2) ? 10 : -10;
        lv_obj_align(b, corners[i], dx, dy);
    }
}

lv_obj_t *th_screen(const char *title, lv_obj_t **status_out)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &s_screen, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);

    lv_obj_t *plate = lv_obj_create(scr);
    lv_obj_remove_style_all(plate);
    lv_obj_add_style(plate, &s_plate, 0);
    lv_obj_set_size(plate, lv_pct(100), 72);
    lv_obj_set_style_pad_hor(plate, 24, 0);
    lv_obj_set_scrollable(plate, false);
    lv_obj_t *t = lv_label_create(plate);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &font_stencil_44, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 2);
    if (status_out) {
        lv_obj_t *st = lv_label_create(plate);
        lv_obj_set_style_text_font(st, &font_bold_34, 0);
        lv_label_set_text(st, "");
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
        *status_out = st;
    }

    lv_obj_t *strip = lv_obj_create(scr);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, lv_pct(100), 14);
    if (texgen_hazard()) {
        lv_obj_set_style_bg_image_src(strip, texgen_hazard(), 0);
    }
    lv_obj_set_style_bg_color(strip, lv_color_hex(TH_YELLOW), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 20, 0);
    return body;
}

lv_obj_t *th_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &s_panel, 0);
    lv_obj_add_style(p, &s_scroll, LV_PART_SCROLLBAR);
    bolts(p);
    return p;
}

lv_obj_t *th_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_btn, 0);
    lv_obj_add_style(b, &s_btn_pr, LV_STATE_PRESSED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *th_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &s_label, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *th_field(lv_obj_t *parent)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_remove_style_all(ta);
    lv_obj_add_style(ta, &s_field, 0);
    lv_obj_add_style(ta, &s_field_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(ta, &s_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(ta, lv_color_hex(0x5a564c), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, lv_pct(100));
    return ta;
}

lv_obj_t *th_lamp(lv_obj_t *parent)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, 22, 22);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l, 3, 0);
    lv_obj_set_style_border_color(l, lv_color_hex(0x000000), 0);
    th_lamp_set(l, 0);
    return l;
}

void th_lamp_set(lv_obj_t *lamp, int state)
{
    static const uint32_t col[] = {0x3a3833, 0xffb000, TH_OK, TH_BAD};
    lv_color_t c = lv_color_hex(col[state & 3]);
    lv_obj_set_style_bg_color(lamp, c, 0);
    lv_obj_set_style_shadow_color(lamp, c, 0);
    lv_obj_set_style_shadow_width(lamp, state ? 18 : 0, 0);
    lv_obj_set_style_shadow_spread(lamp, state ? 2 : 0, 0);
}

void th_keyboard(lv_obj_t *kb)
{
    if (texgen_metal()) {
        lv_obj_set_style_bg_image_src(kb, texgen_metal(), 0);
    }
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1e1f21), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(kb, lv_color_hex(TH_YELLOW), 0);
    lv_obj_set_style_border_width(kb, 4, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 8, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x2b2b2a), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(TH_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &font_ui_26, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 3, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(0x000000), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    /* Control keys (Enter, Backspace, ...) are yellow. */
    lv_obj_set_style_bg_color(kb, lv_color_hex(TH_YELLOW), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(TH_BLACK), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(TH_YELLOW), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(TH_BLACK), LV_PART_ITEMS | LV_STATE_PRESSED);
}

void th_checkbox(lv_obj_t *cb)
{
    lv_obj_set_style_text_font(cb, &font_ui_20, 0);
    lv_obj_set_style_text_color(cb, lv_color_hex(TH_DIM), 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(TH_YELLOW), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x0b0b0b), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cb, lv_color_hex(TH_YELLOW), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(cb, lv_color_hex(TH_BLACK), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(cb, 2, LV_PART_INDICATOR);
    lv_obj_set_style_pad_top(cb, 8, 0);
}
