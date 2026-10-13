/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The view: the target's screen, full size. Touch is the pointer (tap = left
 * click, hold = right click, drag = left drag), the Tab5 keyboard types. A tab
 * at the top opens the menu: the target's power buttons, Ctrl+Alt+Del, text
 * mode.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "kvm_link.h"
#include "textview.h"
#include "touch.h"
#include "power.h"
#include "theme.h"
#include "ui.h"
#include "video.h"
#include "src/misc/lv_area_private.h"

#define DRAG_PX 12
#define SCR_W 1280
#define SCR_H 720

static lv_obj_t *s_stats_slot; /* the FPS line's place in the menu */
static lv_obj_t *s_root, *s_img, *s_tab, *s_menu, *s_stats, *s_note, *s_ctrl, *s_toast, *s_text_btn;
static lv_obj_t *s_keybar, *s_zoomchip, *s_mode_btn, *s_keys_btn;
static lv_timer_t *s_timer, *s_over_timer;
static int64_t s_toast_until;

/* A button that does harm (hard off, reset) asks for a second tap. */
#define CONFIRM_MS 3000
static lv_obj_t *s_armed;
static int64_t s_armed_until;
static int s_control = CTRL_FREE;

/* Pointer gesture */
static lv_point_t s_down_at, s_last_at;
static bool s_dragging, s_long, s_moved;
static uint8_t s_tp_buttons; /* trackpad: what is held while the finger moves */

/* Two fingers: pinch zooms, a drag pans the zoomed picture or, unzoomed,
 * scrolls the target like a wheel. */
typedef enum { G_UNDECIDED, G_PINCH, G_PAN, G_SCROLL } gmode_t;
static bool s_two;
static gmode_t s_gmode;
static float s_g_d0, s_g_z0, s_g_cx0, s_g_cy0, s_g_sc0, s_g_fx, s_g_fy;
static lv_point_t s_g_m0, s_g_prev;
static int s_scroll_acc;
#define SCROLL_STEP_PX 40

/* Keyboard report: modifiers and up to six keys held; s_latched are the
 * on-screen Ctrl/Alt/Win/Shift, held for the next key. */
static uint8_t s_mods, s_keys[6], s_latched;
static portMUX_TYPE s_key_lock = portMUX_INITIALIZER_UNLOCKED;

/* Saved view choices */
static bool s_trackpad, s_keys_on;

static void send_mouse(uint8_t buttons, const lv_point_t *p)
{
    float u, v;
    video_to_target(p->x, p->y, &u, &v); /* off the picture: its nearest edge */
    video_stats_t vs;
    video_get_stats(&vs);
    if (vs.w <= 0 || vs.h <= 0) {
        return;
    }
    uint8_t msg[8];
    size_t n = kvm_msg_mouse_abs(msg, buttons, (uint16_t)(u * KVM_ABS_MAX), (uint16_t)(v * KVM_ABS_MAX), 0, 0);
    link_send(msg, n);
}

/* A relative move (the trackpad, the wheel). */
static void send_rel(uint8_t buttons, int dx, int dy, int wheel)
{
    uint8_t msg[8] = {WS_C2D_MOUSE_REL, buttons, (uint8_t)(dx & 0xff), (uint8_t)((dx >> 8) & 0xff),
                      (uint8_t)(dy & 0xff), (uint8_t)((dy >> 8) & 0xff), (uint8_t)(int8_t)wheel, 0};
    link_send(msg, sizeof(msg));
}

static void gesture_start(const touch_pts_t *t)
{
    s_two = true;
    s_gmode = G_UNDECIDED;
    s_scroll_acc = 0;
    s_g_m0 = s_g_prev = (lv_point_t){(t->p[0].x + t->p[1].x) / 2, (t->p[0].y + t->p[1].y) / 2};
    s_g_d0 = hypotf(t->p[0].x - t->p[1].x, t->p[0].y - t->p[1].y);
    if (s_g_d0 < 1) {
        s_g_d0 = 1;
    }
    video_get_zoom(&s_g_z0, &s_g_cx0, &s_g_cy0);
    s_g_sc0 = video_scale();
    /* The target's pixel under the fingers stays under them while zooming */
    video_stats_t vs;
    video_get_stats(&vs);
    float u = 0.5f, v = 0.5f;
    video_to_target(s_g_m0.x, s_g_m0.y, &u, &v);
    s_g_fx = u * vs.src_w;
    s_g_fy = v * vs.src_h;
    /* a drag the first finger started is let go */
    if (s_dragging || s_tp_buttons) {
        if (s_trackpad) {
            send_rel(0, 0, 0, 0);
        } else {
            send_mouse(0, &s_last_at);
        }
        s_dragging = false;
        s_tp_buttons = 0;
    }
}

static void gesture_move(const touch_pts_t *t)
{
    lv_point_t m = {(t->p[0].x + t->p[1].x) / 2, (t->p[0].y + t->p[1].y) / 2};
    float ratio = hypotf(t->p[0].x - t->p[1].x, t->p[0].y - t->p[1].y) / s_g_d0;
    if (s_gmode == G_UNDECIDED) {
        if (fabsf(ratio - 1) > 0.12f) {
            s_gmode = G_PINCH;
        } else if (LV_ABS(m.x - s_g_m0.x) > DRAG_PX || LV_ABS(m.y - s_g_m0.y) > DRAG_PX) {
            s_gmode = s_g_z0 > 1.01f ? G_PAN : G_SCROLL;
        }
    }
    float base = s_g_sc0 / s_g_z0; /* screen pixels per target pixel at zoom 1 */
    switch (s_gmode) {
    case G_PINCH: {
        float z = s_g_z0 * ratio;
        z = z < 1 ? 1 : z > 8 ? 8 : z;
        float sc = base * z;
        video_set_zoom(z, s_g_fx + (SCR_W / 2.0f - m.x) / sc, s_g_fy + (SCR_H / 2.0f - m.y) / sc);
        break;
    }
    case G_PAN:
        video_set_zoom(s_g_z0, s_g_cx0 - (m.x - s_g_m0.x) / s_g_sc0, s_g_cy0 - (m.y - s_g_m0.y) / s_g_sc0);
        break;
    case G_SCROLL:
        /* fingers up = the page moves up, as on a phone */
        s_scroll_acc += m.y - s_g_prev.y;
        while (LV_ABS(s_scroll_acc) >= SCROLL_STEP_PX) {
            int step = s_scroll_acc > 0 ? 1 : -1;
            send_rel(0, 0, 0, step);
            s_scroll_acc -= step * SCROLL_STEP_PX;
        }
        break;
    default:
        break;
    }
    s_g_prev = m;
}

/* Direct: the finger is the pointer (tap = click, hold = right click, drag =
 * left drag). Trackpad: the finger moves the pointer (tap = click, hold =
 * right click, hold then move = left drag). */
static void img_event(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_event_code_t code = lv_event_get_code(e);
    touch_pts_t t;
    touch_get(&t);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        if (t.n >= 2 && !s_two) {
            gesture_start(&t);
            return;
        }
        if (s_two) {
            if (t.n >= 2) {
                gesture_move(&t);
            }
            return; /* one finger left of two: nothing until both are off */
        }
    }
    switch (code) {
    case LV_EVENT_PRESSED:
        s_down_at = s_last_at = p;
        s_dragging = s_long = s_moved = false;
        s_tp_buttons = 0;
        if (!s_trackpad) {
            send_mouse(0, &p);
        }
        lv_obj_set_hidden(s_menu, true);
        break;
    case LV_EVENT_PRESSING:
        if (!s_moved && (LV_ABS(p.x - s_down_at.x) > DRAG_PX || LV_ABS(p.y - s_down_at.y) > DRAG_PX)) {
            s_moved = true;
        }
        if (s_trackpad) {
            if (s_moved) {
                if (s_long && !s_tp_buttons) {
                    s_tp_buttons = 1; /* held, then moved: a drag */
                }
                send_rel(s_tp_buttons, (p.x - s_last_at.x) * 3 / 2, (p.y - s_last_at.y) * 3 / 2, 0);
            }
        } else {
            if (s_moved && !s_dragging && !s_long) {
                s_dragging = true;
                send_mouse(1, &s_down_at); /* press where the finger landed */
            }
            if (s_dragging) {
                send_mouse(1, &p);
            }
        }
        s_last_at = p;
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!s_moved) {
            s_long = true;
            if (!s_trackpad) {
                send_mouse(2, &s_down_at);
                send_mouse(0, &s_down_at);
            }
        }
        break;
    case LV_EVENT_RELEASED:
        if (s_two) {
            s_two = false;
        } else if (s_trackpad) {
            if (s_tp_buttons) {
                send_rel(0, 0, 0, 0);
            } else if (!s_moved) {
                send_rel(s_long ? 2 : 1, 0, 0, 0);
                send_rel(0, 0, 0, 0);
            }
        } else if (s_dragging) {
            send_mouse(0, &p);
        } else if (!s_long && !s_moved) {
            send_mouse(1, &s_down_at);
            send_mouse(0, &s_down_at);
        }
        s_tp_buttons = 0;
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
    mods = s_mods | s_latched;
    taskEXIT_CRITICAL(&s_key_lock);
    size_t n = kvm_msg_keyboard(msg, mods, keys);
    link_send(msg, n);
}

static void latches_clear(void);

void ui_view_mouse(uint8_t buttons, int dx, int dy, int wheel)
{
    if (power_activity()) {
        return; /* it only woke the screen */
    }
    if (ui_current() == UI_VIEW) {
        send_rel(buttons, dx, dy, wheel);
    }
}

void ui_view_key(const kbd_event_t *ev)
{
    bool used_latch = false;
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
            used_latch = s_latched != 0;
        }
    }
    taskEXIT_CRITICAL(&s_key_lock);
    send_keys();
    if (used_latch) {
        /* the latched Ctrl/Alt went with this key: let them go */
        bsp_display_lock(0);
        latches_clear();
        bsp_display_unlock();
        send_keys();
    }
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

/* The keys a small keyboard lacks, in a bar at the bottom: Esc, F1-F12,
 * Tab, the arrows and the page keys tap through at once; Ctrl, Alt, Win and
 * Shift stay held (lit) for the next key. Plain buttons, not the textured
 * ones: the bar is taken as an overlay picture again on every latch. */
typedef struct {
    const char *t;
    uint8_t usage; /* 0 = a modifier, see mod */
    uint8_t mod;
} barkey_t;
static const barkey_t BAR_ROW1[] = {
    {"ESC", 0x29, 0}, {"F1", 0x3A, 0}, {"F2", 0x3B, 0}, {"F3", 0x3C, 0}, {"F4", 0x3D, 0}, {"F5", 0x3E, 0}, {"F6", 0x3F, 0},
    {"F7", 0x40, 0}, {"F8", 0x41, 0}, {"F9", 0x42, 0}, {"F10", 0x43, 0}, {"F11", 0x44, 0}, {"F12", 0x45, 0}, {"DEL", 0x4C, 0},
};
static const barkey_t BAR_ROW2[] = {
    {"TAB", 0x2B, 0}, {"CTRL", 0, 0x01}, {"ALT", 0, 0x04}, {"WIN", 0, 0x08}, {"SHIFT", 0, 0x02},
    {"HOME", 0x4A, 0}, {"END", 0x4D, 0}, {"PG" LV_SYMBOL_UP, 0x4B, 0}, {"PG" LV_SYMBOL_DOWN, 0x4E, 0},
    {LV_SYMBOL_LEFT, 0x50, 0}, {LV_SYMBOL_UP, 0x52, 0}, {LV_SYMBOL_DOWN, 0x51, 0}, {LV_SYMBOL_RIGHT, 0x4F, 0},
    {"ENTER", 0x28, 0},
};
static lv_obj_t *s_mod_btn[4];
static uint8_t s_mod_bit[4];

static void latches_clear(void)
{
    s_latched = 0;
    for (int i = 0; i < 4; i++) {
        if (s_mod_btn[i]) {
            lv_obj_remove_state(s_mod_btn[i], LV_STATE_CHECKED);
        }
    }
}

static void barkey_clicked(lv_event_t *e)
{
    const barkey_t *k = lv_event_get_user_data(e);
    if (!k->usage) {
        lv_obj_t *b = lv_event_get_current_target(e);
        s_latched ^= k->mod;
        if (s_latched & k->mod) {
            lv_obj_add_state(b, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(b, LV_STATE_CHECKED);
        }
        return;
    }
    uint8_t msg[8], keys[6] = {k->usage}, none[6] = {0};
    link_send(msg, kvm_msg_keyboard(msg, s_mods | s_latched, keys));
    link_send(msg, kvm_msg_keyboard(msg, s_mods, none));
    latches_clear();
}

static lv_obj_t *bar_button(lv_obj_t *parent, const barkey_t *k)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 84, 50);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TH_BLACK), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(TH_YELLOW), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TH_YELLOW), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(TH_YELLOW), LV_STATE_CHECKED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, k->t);
    lv_obj_set_style_text_font(l, &font_ui_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TH_YELLOW), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TH_BLACK), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(l, lv_color_hex(TH_BLACK), LV_STATE_CHECKED);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, barkey_clicked, LV_EVENT_CLICKED, (void *)k);
    return b;
}

static lv_obj_t *keybar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCR_W, LV_SIZE_CONTENT);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_pad_gap(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int m = 0;
    for (size_t i = 0; i < sizeof(BAR_ROW1) / sizeof(BAR_ROW1[0]); i++) {
        bar_button(bar, &BAR_ROW1[i]);
    }
    for (size_t i = 0; i < sizeof(BAR_ROW2) / sizeof(BAR_ROW2[0]); i++) {
        lv_obj_t *b = bar_button(bar, &BAR_ROW2[i]);
        if (!BAR_ROW2[i].usage && m < 4) {
            s_mod_btn[m] = b;
            s_mod_bit[m++] = BAR_ROW2[i].mod;
        }
    }
    return bar;
}

/* The view's own choices, kept across restarts */
static void prefs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("view", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "trackpad", &v) == ESP_OK) {
            s_trackpad = v;
        }
        if (nvs_get_u8(h, "keys", &v) == ESP_OK) {
            s_keys_on = v;
        }
        nvs_close(h);
    }
}

static void prefs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("view", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "trackpad", s_trackpad);
        nvs_set_u8(h, "keys", s_keys_on);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void mode_labels(void)
{
    if (s_mode_btn) {
        lv_label_set_text(lv_obj_get_child(s_mode_btn, 0),
                          s_trackpad ? LV_SYMBOL_GPS " TRACKPAD" : LV_SYMBOL_GPS " DIRECT");
    }
    if (s_keys_btn) {
        lv_label_set_text(lv_obj_get_child(s_keys_btn, 0),
                          s_keys_on ? LV_SYMBOL_KEYBOARD " HIDE KEYS" : LV_SYMBOL_KEYBOARD " KEYS");
    }
}

static void mode_clicked(lv_event_t *e)
{
    s_trackpad = !s_trackpad;
    prefs_save();
    mode_labels();
    toast(s_trackpad ? "TRACKPAD: THE FINGER MOVES THE POINTER" : "DIRECT: THE FINGER IS THE POINTER");
}

static void keys_clicked(lv_event_t *e)
{
    s_keys_on = !s_keys_on;
    prefs_save();
    mode_labels();
    lv_obj_set_hidden(s_keybar, !s_keys_on);
    latches_clear();
    lv_obj_set_hidden(s_menu, true);
}

static void bright_clicked(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    power_set_brightness(power_brightness() + step);
    char t[32];
    snprintf(t, sizeof(t), "BRIGHTNESS %d%%", power_brightness());
    toast(t);
}

static void sleep_clicked(lv_event_t *e)
{
    lv_obj_set_hidden(s_menu, true);
    power_sleep();
}

static void zoomchip_clicked(lv_event_t *e)
{
    video_set_zoom(1, 0, 0);
    lv_obj_set_hidden(s_zoomchip, true);
}

/* LVGL does not write the panel under the picture: hand the video pictures
 * of whatever shows over it. A picture is taken again only when its object
 * moved or LVGL redrew part of it; the FPS line is an overlay of its own, so
 * its updates do not mean taking the whole menu again. */
#define OVER_N 8
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
    static bool flipped;
    if (flipped != video_flipped()) {
        flipped = video_flipped();
        snaps_free(); /* the video dropped them: hand all in again */
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

    lv_obj_t *over[OVER_N] = {s_tab, s_keybar, s_zoomchip, s_menu, s_stats, s_note, s_ctrl, s_toast};
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
    float z, zx, zy;
    video_get_zoom(&z, &zx, &zy);
    bool zoomed = z > 1.01f && !link_text();
    if (zoomed) {
        char zt[32];
        snprintf(zt, sizeof(zt), LV_SYMBOL_EYE_OPEN " %.1fx  FIT", (double)z);
        if (strcmp(lv_label_get_text(lv_obj_get_child(s_zoomchip, 0)), zt)) {
            lv_label_set_text(lv_obj_get_child(s_zoomchip, 0), zt);
        }
    }
    if (lv_obj_is_hidden(s_zoomchip) == zoomed) {
        lv_obj_set_hidden(s_zoomchip, !zoomed);
    }
    power_state_t ps;
    power_get(&ps);
    if (!lv_obj_is_hidden(s_menu)) {
        char bat[40] = "";
        if (ps.present) {
            snprintf(bat, sizeof(bat), "   BATTERY %d%%%s", ps.percent, ps.charging ? " CHARGING" : "");
        }
        lv_label_set_text_fmt(s_stats, "%dx%d  %.1f FPS  DECODE %d MS%s", vs.src_w, vs.src_h, vs.fps,
                              vs.decode_ms, bat);
    }
    /* Once, when it gets low */
    static bool warned;
    if (ps.present && !ps.charging && ps.percent <= 10 && !warned) {
        warned = true;
        toast(LV_SYMBOL_BATTERY_EMPTY " BATTERY LOW - PLUG IN THE CHARGER");
    } else if (ps.charging || ps.percent > 15) {
        warned = false;
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
    s_keybar = s_zoomchip = s_mode_btn = s_keys_btn = NULL;
    for (int i = 0; i < 4; i++) {
        s_mod_btn[i] = NULL;
    }
    s_latched = 0;
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

    prefs_load();
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
    s_mode_btn = th_button(s_menu, "");
    lv_obj_set_style_text_font(s_mode_btn, &font_ui_26, 0);
    lv_obj_add_event_cb(s_mode_btn, mode_clicked, LV_EVENT_CLICKED, NULL);
    s_keys_btn = th_button(s_menu, "");
    lv_obj_set_style_text_font(s_keys_btn, &font_ui_26, 0);
    lv_obj_add_event_cb(s_keys_btn, keys_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dim = th_button(s_menu, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(dim, &font_ui_26, 0);
    lv_obj_add_event_cb(dim, bright_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    lv_obj_t *brt = th_button(s_menu, LV_SYMBOL_PLUS " LIGHT");
    lv_obj_set_style_text_font(brt, &font_ui_26, 0);
    lv_obj_add_event_cb(brt, bright_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)10);
    lv_obj_t *slp = th_button(s_menu, LV_SYMBOL_EYE_CLOSE " SLEEP");
    lv_obj_set_style_text_font(slp, &font_ui_26, 0);
    lv_obj_add_event_cb(slp, sleep_clicked, LV_EVENT_CLICKED, NULL);

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

    /* The keys a small keyboard lacks */
    s_keybar = keybar_create(s_root);
    lv_obj_set_hidden(s_keybar, !s_keys_on);

    /* Zoomed in: how far, and a tap back to the whole picture */
    s_zoomchip = th_button(s_root, "");
    lv_obj_set_style_text_font(s_zoomchip, &font_ui_20, 0);
    lv_obj_set_style_pad_ver(s_zoomchip, 6, 0);
    lv_obj_align(s_zoomchip, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_event_cb(s_zoomchip, zoomchip_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_zoomchip, true);

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

    mode_labels();
    set_text_mode(link_text());

    s_timer = lv_timer_create(tick, 500, NULL);
    tick(NULL);
    s_over_timer = lv_timer_create(overlay_tick, 50, NULL);
}
