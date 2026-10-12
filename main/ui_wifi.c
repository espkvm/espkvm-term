/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Wi-Fi screen: networks around, name and password, connect.
 */
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "net.h"
#include "theme.h"
#include "ui.h"

/* NULL while the screen is not shown. */
static lv_obj_t *s_root, *s_list, *s_ssid, *s_pass, *s_status, *s_lamp, *s_next;

/* The last network news, kept for when the screen is built again. */
static net_state_t s_state = NET_OFF;
static char s_info[64] = "starting Wi-Fi";
static net_ap_t s_aps[NET_MAX_APS];
static int s_n_aps = -1;

static void list_note(const char *text)
{
    lv_obj_t *l = lv_label_create(s_list);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(TH_DIM), 0);
    lv_obj_set_style_pad_all(l, 8, 0);
}

static void connect(void)
{
    const char *ssid = lv_textarea_get_text(s_ssid);
    if (!ssid[0]) {
        lv_group_focus_obj(s_ssid);
        return;
    }
    ui_kb_hide();
    net_connect(ssid, lv_textarea_get_text(s_pass));
}

static void ssid_ready(lv_event_t *e)
{
    lv_group_focus_obj(s_pass);
}

static void pass_ready(lv_event_t *e)
{
    connect();
}

static void ap_clicked(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    lv_textarea_set_text(s_ssid, lv_label_get_text(lv_obj_get_child(row, 1)));
    lv_textarea_set_text(s_pass, "");
    if (lv_obj_get_user_data(row)) { /* open network */
        connect();
    } else {
        lv_group_focus_obj(s_pass);
    }
}

static void scan_clicked(lv_event_t *e)
{
    lv_obj_clean(s_list);
    list_note("Scanning...");
    net_scan();
}

static void connect_clicked(lv_event_t *e)
{
    connect();
}

static void next_clicked(lv_event_t *e)
{
    ui_show(UI_DEVICE);
}

static void show_pass_changed(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    lv_textarea_set_password_mode(s_pass, !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

/* Signal as four bars, lit by strength. */
static void bars(lv_obj_t *parent, int rssi)
{
    int lit = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 40, 26);
    lv_obj_set_clickable(box, false);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(box);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 7, 8 + i * 6);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 10, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i < lit ? TH_YELLOW : 0x3a3833), 0);
        lv_obj_set_clickable(b, false);
    }
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    if (s_n_aps < 0) {
        list_note("Waiting for the radio...");
        return;
    }
    if (!s_n_aps) {
        list_note("No networks found");
    }
    for (int i = 0; i < s_n_aps; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 50);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x3a3833), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(TH_YELLOW), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, lv_color_hex(TH_BLACK), LV_STATE_PRESSED);
        lv_obj_set_clickable(row, true);
        bars(row, s_aps[i].rssi);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_aps[i].ssid);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_user_data(row, (void *)s_aps[i].open);
        if (s_aps[i].open) {
            lv_obj_t *tag = th_label(row, "OPEN");
            lv_obj_set_style_text_color(tag, lv_color_hex(TH_DIM), 0);
        }
        lv_obj_add_event_cb(row, ap_clicked, LV_EVENT_CLICKED, NULL);
    }
}

static void show_state(void)
{
    char text[96];
    int lamp = 0;
    switch (s_state) {
    case NET_OFF:
        snprintf(text, sizeof(text), "%s", s_info);
        break;
    case NET_IDLE:
        snprintf(text, sizeof(text), "PICK A NETWORK");
        break;
    case NET_CONNECTING:
        snprintf(text, sizeof(text), "JOINING %s", s_info);
        lamp = 1;
        break;
    case NET_CONNECTED:
        snprintf(text, sizeof(text), "ONLINE  %s", s_info);
        lamp = 2;
        break;
    case NET_FAILED:
        snprintf(text, sizeof(text), "FAILED: %s", s_info);
        lamp = 3;
        break;
    }
    lv_label_set_text(s_status, text);
    th_lamp_set(s_lamp, lamp);
    lv_obj_set_hidden(s_next, s_state != NET_CONNECTED);
}

void ui_wifi_on_scan(const net_ap_t *aps, int n)
{
    bsp_display_lock(0);
    memcpy(s_aps, aps, n * sizeof(*aps));
    s_n_aps = n;
    if (s_root) {
        fill_list();
    }
    bsp_display_unlock();
}

void ui_wifi_on_state(net_state_t st, const char *info)
{
    bsp_display_lock(0);
    s_state = st;
    strlcpy(s_info, info, sizeof(s_info));
    if (s_root) {
        show_state();
    }
    bsp_display_unlock();
}

static void root_deleted(lv_event_t *e)
{
    s_root = s_list = s_ssid = s_pass = s_status = s_lamp = s_next = NULL;
}

void ui_wifi_build(void)
{
    lv_obj_t *plate_status;
    lv_obj_t *body = th_screen("WI-FI LINK", &plate_status);
    s_root = body;
    lv_obj_add_event_cb(body, root_deleted, LV_EVENT_DELETE, NULL);
    lv_label_set_text(plate_status, "ESP-KVM");
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 20, 0);

    /* Left: networks around */
    lv_obj_t *left = th_panel(body);
    lv_obj_set_height(left, lv_pct(100));
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);
    lv_obj_t *head = lv_obj_create(left);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *hl = th_label(head, "NETWORKS IN RANGE");
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *scan = th_button(head, LV_SYMBOL_REFRESH " SCAN");
    lv_obj_set_style_text_font(scan, &font_ui_26, 0);
    lv_obj_set_style_pad_ver(scan, 4, 0);
    lv_obj_set_style_pad_hor(scan, 14, 0);
    lv_obj_align(scan, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(scan, scan_clicked, LV_EVENT_CLICKED, NULL);
    s_list = lv_obj_create(left);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(TH_YELLOW), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_70, LV_PART_SCROLLBAR);

    /* Right: status and the form */
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
    lv_obj_set_style_pad_bottom(st_row, 10, 0);
    s_lamp = th_lamp(st_row);
    s_status = lv_label_create(st_row);
    lv_obj_set_style_text_font(s_status, &font_bold_34, 0);
    lv_obj_set_flex_grow(s_status, 1);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);

    th_label(right, "NETWORK NAME");
    s_ssid = th_field(right);
    lv_textarea_set_max_length(s_ssid, 32);
    ui_field(s_ssid, ssid_ready);
    th_label(right, "PASSWORD");
    s_pass = th_field(right);
    lv_textarea_set_max_length(s_pass, 64);
    lv_textarea_set_password_mode(s_pass, true);
    ui_field(s_pass, pass_ready);

    lv_obj_t *show = lv_checkbox_create(right);
    lv_checkbox_set_text(show, "SHOW PASSWORD");
    th_checkbox(show);
    lv_obj_add_event_cb(show, show_pass_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *spacer = lv_obj_create(right);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_t *row = lv_obj_create(right);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *btn = th_button(row, "CONNECT");
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_event_cb(btn, connect_clicked, LV_EVENT_CLICKED, NULL);
    s_next = th_button(row, "DEVICE " LV_SYMBOL_RIGHT);
    lv_obj_add_event_cb(s_next, next_clicked, LV_EVENT_CLICKED, NULL);

    lv_textarea_set_text(s_ssid, net_saved_ssid());
    fill_list();
    show_state();
}
