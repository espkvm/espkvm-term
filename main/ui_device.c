/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device screen: where the ESP-KVM is and how to sign in. First visit
 * asks whether to trust the device's certificate. On the left, the devices
 * used before and the ones found on the network.
 */
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "kvm_link.h"
#include "theme.h"
#include "ui.h"

static lv_obj_t *s_root, *s_host, *s_user, *s_pass, *s_code, *s_code_lbl, *s_status, *s_lamp,
    *s_trust, *s_trust_text, *s_list;

/* What mDNS found last; -1 = not looked yet, -2 = looking. */
static link_found_t s_found[LINK_MAX_FOUND];
static int s_n_found = -1;
static char s_prefill[64];

void ui_device_prefill(const char *host)
{
    strlcpy(s_prefill, host, sizeof(s_prefill));
}

static link_state_t s_state = LINK_IDLE;
static char s_info[160];

static void read_form(link_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->host, lv_textarea_get_text(s_host), sizeof(c->host));
    strlcpy(c->user, lv_textarea_get_text(s_user), sizeof(c->user));
    strlcpy(c->pass, lv_textarea_get_text(s_pass), sizeof(c->pass));
}

static void connect(void)
{
    link_cfg_t c;
    read_form(&c);
    if (!c.host[0]) {
        lv_group_focus_obj(s_host);
        return;
    }
    ui_kb_hide();
    const char *code = lv_obj_is_hidden(s_code) ? NULL : lv_textarea_get_text(s_code);
    link_connect(&c, code);
}

static void next_field(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (ta == s_host) {
        lv_group_focus_obj(s_user);
    } else if (ta == s_user) {
        lv_group_focus_obj(s_pass);
    } else {
        connect();
    }
}

static void connect_clicked(lv_event_t *e)
{
    connect();
}

/* ---- the device list ---- */

static void fill_list(void);

static lv_obj_t *list_row(const char *name, const char *sub)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x3a3833), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(TH_YELLOW), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(row, lv_color_hex(TH_BLACK), LV_STATE_PRESSED);
    lv_obj_set_clickable(row, true);
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(col, false);
    lv_obj_t *n = lv_label_create(col);
    lv_label_set_text(n, name);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(n, lv_pct(100));
    if (sub && sub[0] && strcmp(sub, name)) {
        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, sub);
        lv_obj_set_style_text_font(l, &font_ui_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(TH_DIM), 0);
    }
    return row;
}

static void list_note(const char *text)
{
    lv_obj_t *l = th_label(s_list, text);
    lv_obj_set_style_pad_top(l, 10, 0);
}

static void saved_clicked(lv_event_t *e)
{
    link_cfg_t *c = lv_event_get_user_data(e);
    lv_textarea_set_text(s_host, c->host);
    lv_textarea_set_text(s_user, c->user);
    lv_textarea_set_text(s_pass, c->pass);
    lv_textarea_set_text(s_code, "");
    connect();
}

static void forget_clicked(lv_event_t *e)
{
    link_cfg_t *c = lv_event_get_user_data(e);
    link_forget(c->host);
    lv_obj_delete_async(lv_obj_get_parent(lv_event_get_current_target(e)));
}

static void found_clicked(lv_event_t *e)
{
    const link_found_t *f = lv_event_get_user_data(e);
    lv_textarea_set_text(s_host, f->host);
    if (!lv_textarea_get_text(s_user)[0]) {
        lv_textarea_set_text(s_user, "admin");
    }
    lv_textarea_set_text(s_pass, "");
    lv_group_focus_obj(s_pass);
}

/* From the discovery task. */
static void on_found(const link_found_t *found, int n)
{
    bsp_display_lock(0);
    s_n_found = n;
    if (n) {
        memcpy(s_found, found, n * sizeof(*found));
    }
    if (s_root) {
        fill_list();
    }
    bsp_display_unlock();
}

static void find_clicked(lv_event_t *e)
{
    s_n_found = -2;
    fill_list();
    link_discover(on_found);
}

static void free_user_data(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    static link_cfg_t saved[LINK_MAX_DEVICES];
    int n = link_devices(saved, LINK_MAX_DEVICES);
    th_label(s_list, "USED BEFORE");
    if (!n) {
        list_note("None yet");
    }
    for (int i = 0; i < n; i++) {
        /* Each row keeps its own copy: the list may change under it. */
        link_cfg_t *c = malloc(sizeof(*c));
        if (!c) {
            break;
        }
        *c = saved[i];
        lv_obj_t *row = list_row(c->host, c->user);
        lv_obj_add_event_cb(row, saved_clicked, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(row, free_user_data, LV_EVENT_DELETE, c);
        lv_obj_t *x = th_button(row, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_font(x, &font_ui_20, 0);
        lv_obj_set_style_pad_hor(x, 12, 0);
        lv_obj_set_style_pad_ver(x, 6, 0);
        lv_obj_add_event_cb(x, forget_clicked, LV_EVENT_CLICKED, c);
    }
    lv_obj_t *fl = th_label(s_list, "ON THIS NETWORK");
    lv_obj_set_style_pad_top(fl, 16, 0);
    if (s_n_found == -2) {
        list_note("Looking...");
    } else if (s_n_found == 0) {
        list_note("Nothing found. HTTPS must be on.");
    }
    for (int i = 0; i < s_n_found; i++) {
        lv_obj_t *row = list_row(s_found[i].name, s_found[i].host);
        lv_obj_add_event_cb(row, found_clicked, LV_EVENT_CLICKED, &s_found[i]);
    }
}

static void back_clicked(lv_event_t *e)
{
    ui_show(UI_WIFI);
}

static void term_clicked(lv_event_t *e)
{
    ui_show(UI_UPDATE);
}

static void ts_clicked(lv_event_t *e)
{
    ui_show(UI_TS);
}

static void trust_clicked(lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_hidden(s_trust, true);
    link_trust(yes);
}

static void show_state(void)
{
    char text[200];
    int lamp = 0;
    switch (s_state) {
    case LINK_IDLE:
        snprintf(text, sizeof(text), "NOT CONNECTED");
        break;
    case LINK_WORKING:
        snprintf(text, sizeof(text), "%s...", s_info);
        lamp = 1;
        break;
    case LINK_TRUST:
        snprintf(text, sizeof(text), "NEW DEVICE");
        lamp = 1;
        break;
    case LINK_NEED_CODE:
        snprintf(text, sizeof(text), "%s", s_info);
        lamp = 1;
        break;
    case LINK_FAILED:
        snprintf(text, sizeof(text), "FAILED: %s", s_info);
        lamp = 3;
        break;
    case LINK_ONLINE:
        snprintf(text, sizeof(text), "ONLINE");
        lamp = 2;
        break;
    }
    lv_label_set_text(s_status, text);
    th_lamp_set(s_lamp, lamp);
    bool code = s_state == LINK_NEED_CODE;
    lv_obj_set_hidden(s_code, !code && lv_textarea_get_text(s_code)[0] == 0);
    lv_obj_set_hidden(s_code_lbl, lv_obj_is_hidden(s_code));
    if (code) {
        lv_textarea_set_text(s_code, "");
        lv_group_focus_obj(s_code);
    }
    lv_obj_set_hidden(s_trust, s_state != LINK_TRUST);
    if (s_state == LINK_TRUST) {
        lv_label_set_text_fmt(s_trust_text,
                              "First visit. The device's certificate:\n\n%s\n\n"
                              "Trust it only if this is your device on your own network. "
                              "It is kept and checked on every connect from now on.",
                              s_info);
    }
}

void ui_device_on_state(link_state_t st, const char *info)
{
    bsp_display_lock(0);
    s_state = st;
    strlcpy(s_info, info, sizeof(s_info));
    if (s_root) {
        show_state();
    }
    if (st == LINK_ONLINE && ui_current() != UI_VIEW) {
        ui_show_async(UI_VIEW);
    }
    bsp_display_unlock();
}

static void root_deleted(lv_event_t *e)
{
    s_root = s_list = NULL;
}

static lv_obj_t *field(lv_obj_t *parent, const char *label, int max, lv_obj_t **lbl_out)
{
    lv_obj_t *l = th_label(parent, label);
    if (lbl_out) {
        *lbl_out = l;
    }
    lv_obj_t *ta = th_field(parent);
    lv_textarea_set_max_length(ta, max);
    ui_field(ta, next_field);
    return ta;
}

void ui_device_build(void)
{
    lv_obj_t *plate_status;
    lv_obj_t *body = th_screen("DEVICE", &plate_status);
    s_root = body;
    lv_obj_add_event_cb(body, root_deleted, LV_EVENT_DELETE, NULL);
    lv_label_set_text(plate_status, "ESP-KVM");
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 20, 0);

    /* Far left: the devices */
    lv_obj_t *devs = th_panel(body);
    lv_obj_set_height(devs, lv_pct(100));
    lv_obj_set_width(devs, 380);
    lv_obj_set_flex_flow(devs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(devs, 10, 0);
    s_list = lv_obj_create(devs);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_t *find = th_button(devs, LV_SYMBOL_REFRESH "  FIND");
    lv_obj_set_width(find, lv_pct(100));
    lv_obj_add_event_cb(find, find_clicked, LV_EVENT_CLICKED, NULL);

    /* Left: the form */
    lv_obj_t *left = th_panel(body);
    lv_obj_set_height(left, lv_pct(100));
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);
    s_host = field(left, "ADDRESS  (IP OR NAME)", 63, NULL);
    lv_textarea_set_placeholder_text(s_host, "192.168.1.50, kvm.local, 100.x");
    s_user = field(left, "USER", 31, NULL);
    s_pass = field(left, "PASSWORD", 63, NULL);
    lv_textarea_set_password_mode(s_pass, true);
    s_code = field(left, "TWO-FACTOR CODE", 8, &s_code_lbl);
    lv_textarea_set_accepted_chars(s_code, "0123456789");

    /* Right: status, buttons */
    lv_obj_t *right = th_panel(body);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_set_width(right, 360);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 12, 0);
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
    lv_obj_t *hint = lv_label_create(right);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(TH_DIM), 0);
    lv_obj_set_style_text_font(hint, &font_ui_20, 0);
    lv_label_set_text(hint, "The same address and sign-in as the web console. "
                            "Touch the picture to move the pointer; the Tab5 keyboard types on the target.");
    lv_obj_t *spacer = lv_obj_create(right);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_t *btn = th_button(right, "CONNECT  " LV_SYMBOL_RIGHT);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_add_event_cb(btn, connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nav = lv_obj_create(right);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 10, 0);
    lv_obj_t *back = th_button(nav, LV_SYMBOL_LEFT " WI-FI");
    lv_obj_set_style_text_font(back, &font_ui_20, 0);
    lv_obj_set_flex_grow(back, 1);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ts = th_button(nav, "TAILSCALE");
    lv_obj_set_style_text_font(ts, &font_ui_20, 0);
    lv_obj_set_flex_grow(ts, 1);
    lv_obj_add_event_cb(ts, ts_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *term = th_button(nav, "TERMINAL");
    lv_obj_set_style_text_font(term, &font_ui_20, 0);
    lv_obj_set_flex_grow(term, 1);
    lv_obj_add_event_cb(term, term_clicked, LV_EVENT_CLICKED, NULL);

    /* The trust question, over everything */
    s_trust = th_panel(lv_screen_active());
    lv_obj_set_ignore_layout(s_trust, true); /* the screen is a flex column */
    lv_obj_set_floating(s_trust, true);
    lv_obj_set_size(s_trust, 760, 440);
    lv_obj_center(s_trust);
    lv_obj_set_style_bg_opa(s_trust, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_trust, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_trust, 14, 0);
    lv_obj_t *tt = lv_label_create(s_trust);
    lv_obj_set_style_text_font(tt, &font_bold_34, 0);
    lv_obj_set_style_text_color(tt, lv_color_hex(TH_YELLOW), 0);
    lv_label_set_text(tt, LV_SYMBOL_WARNING "  TRUST THIS DEVICE?");
    s_trust_text = lv_label_create(s_trust);
    lv_obj_set_width(s_trust_text, lv_pct(100));
    lv_label_set_long_mode(s_trust_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(s_trust_text, 1);
    lv_obj_t *brow = lv_obj_create(s_trust);
    lv_obj_remove_style_all(brow);
    lv_obj_set_size(brow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(brow, 16, 0);
    lv_obj_t *no = th_button(brow, "CANCEL");
    lv_obj_set_flex_grow(no, 1);
    lv_obj_add_event_cb(no, trust_clicked, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *yes = th_button(brow, "TRUST");
    lv_obj_set_flex_grow(yes, 1);
    lv_obj_add_event_cb(yes, trust_clicked, LV_EVENT_CLICKED, (void *)1);

    link_cfg_t saved;
    link_saved(&saved);
    lv_textarea_set_text(s_host, saved.host);
    lv_textarea_set_text(s_user, saved.user[0] ? saved.user : "admin");
    lv_textarea_set_text(s_pass, saved.pass);
    if (s_prefill[0]) { /* a tailnet peer was picked */
        lv_textarea_set_text(s_host, s_prefill);
        lv_textarea_set_text(s_pass, "");
        s_prefill[0] = 0;
    }
    fill_list();
    if (s_n_found == -1) {
        find_clicked(NULL); /* look once, on the first visit */
    }
    show_state();
}
