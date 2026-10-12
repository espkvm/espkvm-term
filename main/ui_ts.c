/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Tailscale screen: on or off, the auth key, this Tab5's name on the
 * tailnet; on the right what it is doing and the peers. Touching a peer opens
 * the device screen with its address filled in.
 */
#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "ts.h"
#include "ui.h"

#define MAX_PEERS 16

static lv_obj_t *s_root, *s_enable, *s_key, *s_name, *s_status, *s_lamp, *s_info, *s_list;
static lv_timer_t *s_timer;
static ts_peer_t s_peers[MAX_PEERS];
static int s_n_peers = -1;

static void save(void)
{
    ts_cfg_t c = {0};
    c.enable = lv_obj_has_state(s_enable, LV_STATE_CHECKED);
    strlcpy(c.key, lv_textarea_get_text(s_key), sizeof(c.key));
    strlcpy(c.name, lv_textarea_get_text(s_name), sizeof(c.name));
    ui_kb_hide();
    ts_set_cfg(&c);
}

static void key_ready(lv_event_t *e)
{
    lv_group_focus_obj(s_name);
}

static void name_ready(lv_event_t *e)
{
    save();
}

static void save_clicked(lv_event_t *e)
{
    save();
}

static void back_clicked(lv_event_t *e)
{
    ui_show(UI_DEVICE);
}

static void peer_clicked(lv_event_t *e)
{
    const ts_peer_t *p = lv_event_get_user_data(e);
    ui_device_prefill(p->ip);
    ui_show(UI_DEVICE);
}

static void fill_peers(void)
{
    lv_obj_clean(s_list);
    if (s_n_peers <= 0) {
        th_label(s_list, s_n_peers < 0 ? "" : "No peers yet");
        return;
    }
    for (int i = 0; i < s_n_peers; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x3a3833), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(TH_YELLOW), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, lv_color_hex(TH_BLACK), LV_STATE_PRESSED);
        lv_obj_set_clickable(row, true);
        lv_obj_t *lamp = th_lamp(row);
        th_lamp_set(lamp, s_peers[i].online ? 2 : 0);
        lv_obj_t *n = lv_label_create(row);
        lv_label_set_text(n, s_peers[i].name);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(n, 1);
        lv_obj_t *ip = lv_label_create(row);
        lv_label_set_text_fmt(ip, "%s%s", s_peers[i].ip, s_peers[i].direct ? "" : "  RELAY");
        lv_obj_set_style_text_font(ip, &font_ui_20, 0);
        lv_obj_set_style_text_color(ip, lv_color_hex(TH_DIM), 0);
        lv_obj_add_event_cb(row, peer_clicked, LV_EVENT_CLICKED, &s_peers[i]);
    }
}

static void tick(lv_timer_t *t)
{
    ts_status_t st;
    ts_status(&st);
    char text[64];
    snprintf(text, sizeof(text), "%s", st.state);
    for (char *p = text; *p; p++) {
        *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    }
    lv_label_set_text(s_status, text);
    th_lamp_set(s_lamp, st.connected ? 2 : st.running ? 1 : 0);
    if (st.ip[0]) {
        lv_label_set_text_fmt(s_info, "%s\n%s", st.ip, st.self);
    } else {
        lv_label_set_text(s_info, "");
    }
    static ts_peer_t now[MAX_PEERS];
    int n = st.connected ? ts_peers(now, MAX_PEERS) : 0;
    if (n != s_n_peers || memcmp(now, s_peers, n * sizeof(now[0]))) {
        memcpy(s_peers, now, n * sizeof(now[0]));
        s_n_peers = n;
        fill_peers();
    }
}

static void root_deleted(lv_event_t *e)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_root = NULL;
}

void ui_ts_build(void)
{
    lv_obj_t *plate_status;
    lv_obj_t *body = th_screen("TAILSCALE", &plate_status);
    s_root = body;
    lv_obj_add_event_cb(body, root_deleted, LV_EVENT_DELETE, NULL);
    lv_label_set_text(plate_status, "ESP-KVM");
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 20, 0);

    /* Left: the settings */
    lv_obj_t *left = th_panel(body);
    lv_obj_set_height(left, lv_pct(100));
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);
    s_enable = lv_checkbox_create(left);
    lv_checkbox_set_text(s_enable, "JOIN THE TAILNET");
    th_checkbox(s_enable);
    th_label(left, "AUTH KEY  (TSKEY-AUTH-...)");
    s_key = th_field(left);
    lv_textarea_set_max_length(s_key, sizeof(((ts_cfg_t *)0)->key) - 1);
    ui_field(s_key, key_ready);
    th_label(left, "NAME ON THE TAILNET  (EMPTY = DEFAULT)");
    s_name = th_field(left);
    lv_textarea_set_max_length(s_name, sizeof(((ts_cfg_t *)0)->name) - 1);
    ui_field(s_name, name_ready);
    lv_obj_t *hint = lv_label_create(left);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(TH_DIM), 0);
    lv_obj_set_style_text_font(hint, &font_ui_20, 0);
    lv_label_set_text(hint, "Make a key in the Tailscale admin console, Settings - Keys. "
                            "With it on, a device's address can be its 100.x address or "
                            "its tailnet name.");
    lv_obj_t *spacer = lv_obj_create(left);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_t *row = lv_obj_create(left);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *back = th_button(row, LV_SYMBOL_LEFT " DEVICE");
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn = th_button(row, "SAVE");
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_event_cb(btn, save_clicked, LV_EVENT_CLICKED, NULL);

    /* Right: state and peers */
    lv_obj_t *right = th_panel(body);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_set_width(right, 520);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, 0);
    lv_obj_t *st_row = lv_obj_create(right);
    lv_obj_remove_style_all(st_row);
    lv_obj_set_size(st_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st_row, 12, 0);
    s_lamp = th_lamp(st_row);
    s_status = lv_label_create(st_row);
    lv_obj_set_style_text_font(s_status, &font_bold_34, 0);
    lv_obj_set_flex_grow(s_status, 1);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    s_info = lv_label_create(right);
    lv_obj_set_style_text_color(s_info, lv_color_hex(TH_DIM), 0);
    th_label(right, "PEERS");
    s_list = lv_obj_create(right);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    ts_cfg_t c;
    ts_get_cfg(&c);
    lv_obj_set_state(s_enable, LV_STATE_CHECKED, c.enable);
    lv_textarea_set_text(s_key, c.key);
    lv_textarea_set_text(s_name, c.name);
    s_n_peers = -1;
    s_timer = lv_timer_create(tick, 1000, NULL);
    tick(NULL);
}
