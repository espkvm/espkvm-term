/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frames from the device onto the screen: JPEG in the hardware decoder, then
 * the PPA scales and turns it straight into the panel's frame buffer, past
 * LVGL. If that buffer cannot be had, an LVGL image shows it instead.
 */
#pragma once

#include "kvm_link.h"
#include "lvgl.h"

typedef struct {
    int src_w, src_h;     /* the target's picture */
    int x, y, w, h;       /* where it lands on the 1280x720 screen */
    float fps;            /* drawn */
    float in_fps;         /* arrived */
    int decode_ms;
    int64_t shown_at;     /* esp_timer time of the last frame drawn */
    const char *problem;  /* why nothing shows, or NULL */
} video_stats_t;

/* Creates the image on parent (full size). LVGL lock held. */
lv_obj_t *video_create(lv_obj_t *parent);
/* The image is going away: stop drawing into it. LVGL lock held. */
void video_detach(void);
/* The picture takes the panel (on) or gives it back to LVGL. While taken,
 * LVGL renders but writes nothing; what it shows over the picture must be
 * handed in with video_set_overlays. LVGL lock held. */
void video_own_panel(bool on);
/* LVGL drew something since the last call, within *at: take the overlays
 * there again. LVGL lock held. */
bool video_ui_dirty(lv_area_t *at);
/* Pictures of the objects drawn over the frames (ARGB8888 snapshots, at =
 * each object's screen coordinates, fresh = the picture changed since the
 * last call; same place and not fresh = kept as it was). Copied. LVGL lock held. */
void video_set_overlays(lv_draw_buf_t *const *img, const lv_area_t *at, const bool *fresh, int n);
/* The panel's buffer while the picture owns it, else NULL (for screenshots). */
uint16_t *video_owned_panel(void);
/* Text mode covers the picture: frames still in flight are not drawn. */
void video_pause(bool on);
/* From the link's video task. */
void video_frame(const kvm_video_hdr_t *hdr, const uint8_t *data, size_t len);
void video_get_stats(video_stats_t *out);
/* Zoom: 1 = the whole picture; above 1 a part of it, centred on (cx, cy) in
 * the target's pixels. Up to 8. */
void video_set_zoom(float zoom, float cx, float cy);
void video_get_zoom(float *zoom, float *cx, float *cy);
/* A screen point as a place on the target's picture, 0..1 each way (clamped);
 * false if the point is off the picture or there is none yet. */
bool video_to_target(int x, int y, float *u, float *v);
/* The Tab5 is upside down: the picture and the overlays are turned the
 * other way (LVGL is turned by the caller). */
void video_set_flip(bool flip);
bool video_flipped(void);
/* Screen pixels per target pixel as drawn now. */
float video_scale(void);
