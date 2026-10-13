/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The terminal itself: its version, the battery, and an update from the
 * latest GitHub release.
 */
#include <stdio.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "orient.h"
#include "ota.h"
#include "power.h"
#include "theme.h"
#include "ui.h"

static lv_obj_t *s_version, *s_status, *s_battery, *s_check, *s_install, *s_bar, *s_tilt;
static lv_timer_t *s_timer;

static void back_clicked(lv_event_t *e)
{
    ui_show(UI_DEVICE);
}

static void check_clicked(lv_event_t *e)
{
    ota_check();
}

static void install_clicked(lv_event_t *e)
{
    ota_install();
}

static void bright_changed(lv_event_t *e)
{
    power_set_brightness(lv_slider_get_value(lv_event_get_target(e)));
}

static void sleep_clicked(lv_event_t *e)
{
    power_sleep();
}

/* Off asks for a second tap within three seconds */
static int64_t s_off_armed;
static void off_clicked(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (now - s_off_armed > 3000000) {
        s_off_armed = now;
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), "TAP AGAIN");
        return;
    }
    power_off();
}

static void autorot_changed(lv_event_t *e)
{
    orient_set_auto(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void tick(lv_timer_t *t)
{
    ota_info_t o;
    ota_get(&o);
    char s[160];
    switch (o.state) {
    case OTA_UNSUPPORTED:
        snprintf(s, sizeof(s), "Started from M5 Launcher: update the app there.");
        break;
    case OTA_CHECKING:
        snprintf(s, sizeof(s), "Asking GitHub...");
        break;
    case OTA_LATEST:
        snprintf(s, sizeof(s), "This is the latest release.");
        break;
    case OTA_AVAILABLE:
        snprintf(s, sizeof(s), "%s is out.", o.latest);
        break;
    case OTA_INSTALLING:
        snprintf(s, sizeof(s), "Installing %s: %d%%", o.latest, o.percent);
        break;
    case OTA_DONE:
        snprintf(s, sizeof(s), "Installed. Restarting...");
        break;
    case OTA_FAILED:
        snprintf(s, sizeof(s), "%s", o.error);
        break;
    default:
        snprintf(s, sizeof(s), "Updates come from github.com/" OTA_REPO ".");
        break;
    }
    lv_label_set_text(s_status, s);
    lv_obj_set_style_text_color(s_status, lv_color_hex(o.state == OTA_FAILED ? TH_BAD : TH_TEXT), 0);
    lv_obj_set_hidden(s_install, o.state != OTA_AVAILABLE);
    lv_obj_set_hidden(s_check, o.state == OTA_UNSUPPORTED || o.state == OTA_INSTALLING || o.state == OTA_DONE);
    lv_obj_set_hidden(s_bar, o.state != OTA_INSTALLING && o.state != OTA_DONE);
    lv_bar_set_value(s_bar, o.state == OTA_DONE ? 100 : o.percent, LV_ANIM_OFF);

    power_state_t p;
    power_get(&p);
    if (p.present) {
        snprintf(s, sizeof(s), "BATTERY  %d%%  (%.2f V)%s", p.percent, (double)p.volts, p.charging ? "  CHARGING" : "");
    } else {
        snprintf(s, sizeof(s), "NO BATTERY - RUNNING FROM USB");
    }
    lv_label_set_text(s_battery, s);
    float ax, ay, az;
    if (orient_accel(&ax, &ay, &az)) {
        snprintf(s, sizeof(s), "TILT  X %+.2f  Y %+.2f  Z %+.2f G", (double)ax, (double)ay, (double)az);
        lv_label_set_text(s_tilt, s);
    }
}

static void root_deleted(lv_event_t *e)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

void ui_update_build(void)
{
    lv_obj_t *plate_status;
    lv_obj_t *body = th_screen("TERMINAL", &plate_status);
    lv_obj_add_event_cb(body, root_deleted, LV_EVENT_DELETE, NULL);
    lv_label_set_text(plate_status, "ESP-KVM");
    lv_obj_t *p = th_panel(body);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 14, 0);

    th_label(p, "VERSION");
    s_version = lv_label_create(p);
    lv_obj_set_style_text_font(s_version, &font_bold_34, 0);
    lv_obj_set_style_text_color(s_version, lv_color_hex(TH_TEXT), 0);
    ota_info_t o;
    ota_get(&o);
    lv_label_set_text(s_version, o.current);

    s_status = lv_label_create(p);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_status, &font_ui_26, 0);

    s_bar = lv_bar_create(p);
    lv_obj_set_size(s_bar, lv_pct(100), 18);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(TH_BLACK), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(TH_YELLOW), LV_PART_INDICATOR);

    th_label(p, "SCREEN BRIGHTNESS");
    lv_obj_t *sl = lv_slider_create(p);
    lv_obj_set_size(sl, lv_pct(60), 20);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, power_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(TH_BLACK), 0);
    lv_obj_set_style_bg_color(sl, lv_color_hex(TH_YELLOW), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(TH_YELLOW), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *ar = lv_checkbox_create(p);
    lv_checkbox_set_text(ar, "TURN THE SCREEN OVER WHEN THE TAB5 IS UPSIDE DOWN");
    th_checkbox(ar);
    if (orient_auto()) {
        lv_obj_add_state(ar, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ar, autorot_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_tilt = lv_label_create(p);
    lv_obj_set_style_text_font(s_tilt, &font_ui_20, 0);
    lv_obj_set_style_text_color(s_tilt, lv_color_hex(TH_DIM), 0);
    lv_label_set_text(s_tilt, "");

    th_label(p, "POWER");
    s_battery = lv_label_create(p);
    lv_obj_set_style_text_font(s_battery, &font_ui_26, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(TH_TEXT), 0);

    lv_obj_t *spacer = lv_obj_create(p);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *back = th_button(row, LV_SYMBOL_LEFT " DEVICE");
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    s_check = th_button(row, LV_SYMBOL_REFRESH " CHECK FOR AN UPDATE");
    lv_obj_add_event_cb(s_check, check_clicked, LV_EVENT_CLICKED, NULL);
    s_install = th_button(row, LV_SYMBOL_DOWNLOAD " INSTALL");
    lv_obj_add_event_cb(s_install, install_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gap = lv_obj_create(row);
    lv_obj_remove_style_all(gap);
    lv_obj_set_flex_grow(gap, 1);
    lv_obj_t *slp = th_button(row, LV_SYMBOL_EYE_CLOSE " SLEEP");
    lv_obj_add_event_cb(slp, sleep_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *off = th_button(row, LV_SYMBOL_POWER " SWITCH OFF");
    lv_obj_set_style_bg_color(off, lv_color_hex(TH_BAD), 0);
    lv_obj_set_style_bg_image_opa(off, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(off, off_clicked, LV_EVENT_CLICKED, NULL);

    tick(NULL);
    s_timer = lv_timer_create(tick, 300, NULL);
}
