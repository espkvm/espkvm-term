/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The view: the target's screen, full size. Touch is the pointer (tap = left
 * click, hold = right click, drag = left drag), the Tab5 keyboard types. A tab
 * at the top opens the menu: the target's power buttons, Ctrl+Alt+Del, text
 * mode.
 */
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "kvm_link.h"
#include "textview.h"
#include "theme.h"
#include "ui.h"
#include "video.h"
#include "src/misc/lv_area_private.h"

#define DRAG_PX 12

static lv_obj_t *s_stats_slot; /* the FPS line's place in the menu */
static lv_obj_t *s_root, *s_img, *s_tab, *s_menu, *s_stats, *s_note, *s_ctrl, *s_toast, *s_text_btn;
static lv_timer_t *s_timer, *s_over_timer;
static int64_t s_toast_until;

/* A button that does harm (hard off, reset) asks for a second tap. */
#define CONFIRM_MS 3000
static lv_obj_t *s_armed;
static int64_t s_armed_until;
static int s_control = CTRL_FREE;

/* Pointer gesture */
static lv_point_t s_down_at;
static bool s_dragging, s_long;

/* Keyboard report: modifiers and up to six keys held. */
static uint8_t s_mods, s_keys[6];
static portMUX_TYPE s_key_lock = portMUX_INITIALIZER_UNLOCKED;

static void send_mouse(uint8_t buttons, const lv_point_t *p)
{
    video_stats_t vs;
    video_get_stats(&vs);
    if (vs.w <= 0 || vs.h <= 0) {
        return;
    }
    int x = p->x - vs.x, y = p->y - vs.y;
    x = x < 0 ? 0 : x >= vs.w ? vs.w - 1 : x;
    y = y < 0 ? 0 : y >= vs.h ? vs.h - 1 : y;
    uint8_t msg[8];
    size_t n = kvm_msg_mouse_abs(msg, buttons, (uint16_t)((int64_t)x * KVM_ABS_MAX / (vs.w - 1)),
                                 (uint16_t)((int64_t)y * KVM_ABS_MAX / (vs.h - 1)), 0, 0);
    link_send(msg, n);
}

static void img_event(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_down_at = p;
        s_dragging = s_long = false;
        send_mouse(0, &p);
        lv_obj_set_hidden(s_menu, true);
        break;
    case LV_EVENT_PRESSING:
        if (!s_dragging && !s_long &&
            (LV_ABS(p.x - s_down_at.x) > DRAG_PX || LV_ABS(p.y - s_down_at.y) > DRAG_PX)) {
            s_dragging = true;
            send_mouse(1, &s_down_at); /* press where the finger landed */
        }
        if (s_dragging) {
            send_mouse(1, &p);
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!s_dragging) {
            s_long = true;
            send_mouse(2, &s_down_at);
            send_mouse(0, &s_down_at);
        }
        break;
    case LV_EVENT_RELEASED:
        if (s_dragging) {
            send_mouse(0, &p);
        } else if (!s_long) {
            send_mouse(1, &s_down_at);
            send_mouse(0, &s_down_at);
        }
        break;
    default:
        break;
    }
}

static void send_keys(void)
{
    uint8_t msg[8], keys[6], mods;
    taskENTER_CRITICAL(&s_key_lock);
    memcpy(keys, s_keys, 6);
    mods = s_mods;
    taskEXIT_CRITICAL(&s_key_lock);
    size_t n = kvm_msg_keyboard(msg, mods, keys);
    link_send(msg, n);
}

void ui_view_key(const kbd_event_t *ev)
{
    taskENTER_CRITICAL(&s_key_lock);
    /* While a key is down its own shift (for '!') goes with it; after, only what is held. */
    s_mods = ev->pressed ? ev->mods : ev->held;
    if (ev->usage) {
        int slot = -1;
        for (int i = 0; i < 6; i++) {
            if (s_keys[i] == ev->usage) {
                slot = i;
            }
        }
        if (ev->pressed && slot < 0) {
            for (int i = 0; i < 6; i++) {
                if (!s_keys[i]) {
                    s_keys[i] = ev->usage;
                    break;
                }
            }
        } else if (!ev->pressed && slot >= 0) {
            s_keys[slot] = 0;
        }
    }
    taskEXIT_CRITICAL(&s_key_lock);
    send_keys();
}

void ui_view_on_msg(const uint8_t *data, size_t len)
{
    if (len >= 2 && data[0] == WS_D2C_CONTROL) {
        bsp_display_lock(0);
        s_control = data[1];
        if (s_ctrl) {
            lv_obj_set_hidden(s_ctrl, s_control != CTRL_HELD_OTHER);
        }
        bsp_display_unlock();
    }
}

static void toast(const char *text)
{
    if (!s_toast) {
        return;
    }
    lv_label_set_text(s_toast, text);
    lv_obj_set_hidden(s_toast, false);
    s_toast_until = esp_timer_get_time() + 3000000;
}

/* LVGL does not write the panel under the picture: hand the video pictures
 * of whatever shows over it. A picture is taken again only when its object
 * moved or LVGL redrew part of it; the FPS line is an overlay of its own, so
 * its updates do not mean taking the whole menu again. */
#define OVER_N 6
static lv_draw_buf_t *s_snap[OVER_N];
static lv_area_t s_snap_at[OVER_N];

static void snaps_free(void)
{
    for (int i = 0; i < OVER_N; i++) {
        if (s_snap[i]) {
            lv_draw_buf_destroy(s_snap[i]);
            s_snap[i] = NULL;
        }
    }
}

static void overlay_tick(lv_timer_t *t)
{
    if (link_text()) {
        return;
    }
    lv_obj_set_hidden(s_stats, lv_obj_is_hidden(s_menu));
    if (!lv_obj_is_hidden(s_menu)) {
        lv_area_t slot;
        lv_obj_get_coords(s_stats_slot, &slot);
        lv_obj_set_pos(s_stats, slot.x1, slot.y1);
    }
    lv_area_t dirty;
    bool has_dirty = video_ui_dirty(&dirty);
    lv_area_t stats_at;
    lv_obj_get_coords(s_stats, &stats_at);
    lv_area_increase(&stats_at, 8, 8); /* LVGL redraws a little around the text */
    bool only_stats = has_dirty && lv_area_is_in(&dirty, &stats_at, 0);

    lv_obj_t *over[OVER_N] = {s_tab, s_menu, s_stats, s_note, s_ctrl, s_toast};
    lv_draw_buf_t *img[OVER_N];
    lv_area_t at[OVER_N];
    bool fresh[OVER_N];
    int n = 0;
    bool changed = false;
    for (int i = 0; i < OVER_N; i++) {
        if (!over[i] || lv_obj_is_hidden(over[i])) {
            changed |= s_snap[i] != NULL;
            if (s_snap[i]) {
                lv_draw_buf_destroy(s_snap[i]);
                s_snap[i] = NULL;
            }
            continue;
        }
        lv_area_t c;
        lv_obj_get_coords(over[i], &c);
        bool redo = !s_snap[i] || !lv_area_is_equal(&c, &s_snap_at[i]);
        if (has_dirty && over[i] != s_stats && only_stats) {
            /* the FPS line alone changed: not this one */
        } else if (has_dirty && lv_area_is_on(&dirty, &c)) {
            redo = true;
        }
        if (redo) {
            if (s_snap[i]) {
                lv_draw_buf_destroy(s_snap[i]);
            }
            s_snap[i] = lv_snapshot_take(over[i], LV_COLOR_FORMAT_ARGB8888);
            s_snap_at[i] = c;
            changed = true;
        }
        if (s_snap[i]) {
            img[n] = s_snap[i];
            fresh[n] = redo;
            at[n++] = c;
        }
    }
    if (changed) {
        video_set_overlays(img, at, fresh, n);
    }
}

static void set_text_mode(bool on)
{
    video_pause(on);
    video_own_panel(!on);
    snaps_free(); /* overlays again when the picture is back */
    link_set_text(on);
    textview_show(on);
    if (s_text_btn) {
        lv_label_set_text(lv_obj_get_child(s_text_btn, 0),
                          on ? LV_SYMBOL_IMAGE " PICTURE" : LV_SYMBOL_LIST " TEXT");
    }
}

static void tick(lv_timer_t *t)
{
    video_stats_t vs;
    video_get_stats(&vs);
    int64_t now = esp_timer_get_time();
    if (s_toast_until && now > s_toast_until) {
        s_toast_until = 0;
        lv_obj_set_hidden(s_toast, true);
    }
    if (s_armed && now > s_armed_until) {
        lv_obj_remove_state(s_armed, LV_STATE_CHECKED);
        s_armed = NULL;
    }
    if (link_text() && textview_gone()) {
        set_text_mode(false);
        toast("THE SCREEN IS A PICTURE NOW");
    }
    if (!lv_obj_is_hidden(s_menu)) {
        lv_label_set_text_fmt(s_stats, "%dx%d  %.1f FPS  DECODE %d MS", vs.src_w, vs.src_h, vs.fps,
                              vs.decode_ms);
    }
    if (!link_online()) {
        lv_label_set_text(s_note, "CONNECTION LOST - RECONNECTING...");
        lv_obj_set_hidden(s_note, false);
    } else if (link_text()) {
        lv_label_set_text(s_note, "READING THE SCREEN...");
        lv_obj_set_hidden(s_note, textview_has_text());
    } else if (vs.problem) {
        lv_label_set_text(s_note, vs.problem);
        lv_obj_set_hidden(s_note, false);
    } else if (vs.w == 0) {
        lv_label_set_text(s_note, "WAITING FOR THE PICTURE...");
        lv_obj_set_hidden(s_note, false);
    } else {
        lv_obj_set_hidden(s_note, true);
    }
}

static void tab_clicked(lv_event_t *e)
{
    lv_obj_set_hidden(s_menu, !lv_obj_is_hidden(s_menu));
    tick(NULL);
}

static void disconnect_clicked(lv_event_t *e)
{
    link_disconnect();
    ui_show(UI_DEVICE);
}

static void wifi_clicked(lv_event_t *e)
{
    ui_show(UI_WIFI);
}

static void takeover_clicked(lv_event_t *e)
{
    uint8_t m = WS_C2D_TAKEOVER;
    link_send(&m, 1);
}

static void keyframe_clicked(lv_event_t *e)
{
    link_request_keyframe();
}

static void text_clicked(lv_event_t *e)
{
    set_text_mode(!link_text());
    lv_obj_set_hidden(s_menu, true);
}

/* From the link task. */
static void action_done(const char *action, bool ok, const char *error)
{
    char msg[128];
    if (ok) {
        snprintf(msg, sizeof(msg), "DONE: %s", action);
    } else {
        snprintf(msg, sizeof(msg), "%s: %s", action, error ? error : "failed");
    }
    bsp_display_lock(0);
    toast(msg);
    bsp_display_unlock();
}

/* Hard off and reset ask for a second tap. */
static bool needs_confirm(const char *action)
{
    return !strcmp(action, "power/hold") || !strcmp(action, "power/reset");
}

/* user data: the action. */
static void action_clicked(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    const char *action = lv_event_get_user_data(e);
    if (needs_confirm(action) && s_armed != b) {
        if (s_armed) {
            lv_obj_remove_state(s_armed, LV_STATE_CHECKED);
        }
        s_armed = b;
        s_armed_until = esp_timer_get_time() + CONFIRM_MS * 1000LL;
        lv_obj_add_state(b, LV_STATE_CHECKED);
        toast("TAP AGAIN TO CONFIRM");
        return;
    }
    if (s_armed) {
        lv_obj_remove_state(s_armed, LV_STATE_CHECKED);
        s_armed = NULL;
    }
    toast("SENT...");
    link_action(action, action_done);
}

static void cad_clicked(lv_event_t *e)
{
    const uint8_t del[6] = {0x4C}, none[6] = {0};
    uint8_t msg[8];
    link_send(msg, kvm_msg_keyboard(msg, 0x01 | 0x04, del)); /* left Ctrl + left Alt */
    link_send(msg, kvm_msg_keyboard(msg, 0, none));
    toast("CTRL+ALT+DEL SENT");
}

static void root_deleted(lv_event_t *e)
{
    video_detach();
    textview_detach();
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_over_timer) {
        lv_timer_delete(s_over_timer);
        s_over_timer = NULL;
    }
    video_own_panel(false);
    snaps_free();
    s_stats_slot = NULL;
    s_root = s_img = s_tab = s_menu = s_stats = s_note = s_ctrl = s_toast = s_text_btn = s_armed = NULL;
}

void ui_view_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s_root, root_deleted, LV_EVENT_DELETE, NULL);

    s_img = video_create(s_root);
    lv_obj_set_clickable(s_img, true);
    lv_obj_add_event_cb(s_img, img_event, LV_EVENT_ALL, NULL);
    textview_create(s_root);

    s_note = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_note, &font_bold_34, 0);
    lv_obj_set_style_text_color(s_note, lv_color_hex(TH_YELLOW), 0);
    lv_obj_center(s_note);

    /* The tab: a small yellow plate at the top edge */
    lv_obj_t *tab = s_tab = th_button(s_root, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(tab, &font_ui_20, 0);
    lv_obj_set_style_pad_ver(tab, 0, 0);
    lv_obj_set_style_pad_hor(tab, 30, 0);
    lv_obj_set_style_radius(tab, 0, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_70, 0);
    lv_obj_align(tab, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_event_cb(tab, tab_clicked, LV_EVENT_CLICKED, NULL);

    /* The menu */
    s_menu = th_panel(s_root);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_90, 0);
    lv_obj_set_size(s_menu, 820, LV_SIZE_CONTENT);
    lv_obj_align(s_menu, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s_menu, 14, 0);
    s_stats_slot = th_label(s_menu, " ");
    lv_obj_set_width(s_stats_slot, lv_pct(100));
    const struct {
        const char *t;
        lv_event_cb_t cb;
    } items[] = {
        {LV_SYMBOL_REFRESH " KEYFRAME", keyframe_clicked},
        {LV_SYMBOL_WIFI " WI-FI", wifi_clicked},
        {LV_SYMBOL_CLOSE " DISCONNECT", disconnect_clicked},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *b = th_button(s_menu, items[i].t);
        lv_obj_set_style_text_font(b, &font_ui_26, 0);
        lv_obj_add_event_cb(b, items[i].cb, LV_EVENT_CLICKED, NULL);
    }
    s_text_btn = th_button(s_menu, "");
    lv_obj_set_style_text_font(s_text_btn, &font_ui_26, 0);
    lv_obj_add_event_cb(s_text_btn, text_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cad = th_button(s_menu, "CTRL+ALT+DEL");
    lv_obj_set_style_text_font(cad, &font_ui_26, 0);
    lv_obj_add_event_cb(cad, cad_clicked, LV_EVENT_CLICKED, NULL);

    /* The target's buttons */
    lv_obj_t *pl = th_label(s_menu, "TARGET");
    lv_obj_set_width(pl, lv_pct(100));
    const struct {
        const char *t, *action;
    } acts[] = {
        {LV_SYMBOL_POWER " POWER", "power/click"},
        {LV_SYMBOL_POWER " HARD OFF", "power/hold"},
        {LV_SYMBOL_REFRESH " RESET", "power/reset"},
        {LV_SYMBOL_CHARGE " WAKE (LAN)", "power/wake"},
        {LV_SYMBOL_REFRESH " RECONNECT HDMI", "video/reconnect"},
    };
    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        lv_obj_t *b = th_button(s_menu, acts[i].t);
        lv_obj_set_style_text_font(b, &font_ui_26, 0);
        if (needs_confirm(acts[i].action)) {
            lv_obj_set_style_bg_color(b, lv_color_hex(TH_BAD), LV_STATE_CHECKED);
            lv_obj_set_style_bg_image_opa(b, LV_OPA_TRANSP, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(b, action_clicked, LV_EVENT_CLICKED, (void *)acts[i].action);
    }
    lv_obj_set_hidden(s_menu, true);
    /* The FPS line floats over its slot (see overlay_tick) */
    s_stats = th_label(s_root, "");
    lv_obj_set_width(s_stats, lv_obj_get_style_width(s_menu, 0) - 2 * lv_obj_get_style_pad_left(s_menu, 0));
    lv_obj_set_hidden(s_stats, true);

    /* Someone else holds the keyboard and mouse */
    s_ctrl = th_button(s_root, "ANOTHER VIEWER HAS CONTROL  -  TAKE IT");
    lv_obj_set_style_text_font(s_ctrl, &font_ui_26, 0);
    lv_obj_align(s_ctrl, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(s_ctrl, takeover_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_ctrl, s_control != CTRL_HELD_OTHER);

    /* Short news: what a button did */
    s_toast = th_label(s_root, "");
    lv_obj_set_style_text_font(s_toast, &font_ui_26, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(TH_BLACK), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_toast, 12, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -90);
    lv_obj_set_hidden(s_toast, true);

    set_text_mode(link_text());

    s_timer = lv_timer_create(tick, 500, NULL);
    tick(NULL);
    s_over_timer = lv_timer_create(overlay_tick, 50, NULL);
}
