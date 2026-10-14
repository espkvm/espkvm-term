/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The link to an ESP-KVM: trust its CA on first use, sign in, then the two
 * WebSockets - /ws for input and status, /video for frames.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_proto.h"

typedef enum {
    LINK_IDLE,
    LINK_WORKING,    /* info: what it is doing */
    LINK_TRUST,      /* first visit: info = the CA's name and fingerprint; call link_trust() */
    LINK_NEED_CODE,  /* two-factor is on: ask for the code, link_connect() again */
    LINK_FAILED,     /* info: why */
    LINK_ONLINE,     /* both sockets up */
} link_state_t;

typedef void (*link_state_cb_t)(link_state_t st, const char *info);
/* A whole video frame, header parsed. Called on the video socket's task; the
 * buffer is valid until the call returns. */
typedef void (*link_frame_cb_t)(const kvm_video_hdr_t *hdr, const uint8_t *data, size_t len);
/* A /ws message from the device. */
typedef void (*link_msg_cb_t)(const uint8_t *data, size_t len);

typedef struct {
    char host[64];
    char user[32];
    char pass[64];
} link_cfg_t;

void link_init(link_state_cb_t on_state, link_frame_cb_t on_frame, link_msg_cb_t on_msg);
/* The device used last, if any (host empty if none). */
void link_saved(link_cfg_t *out);

/* Saved devices, the last used first; each keeps its own trusted CA. */
#define LINK_MAX_DEVICES 8
int link_devices(link_cfg_t *out, int max);
void link_forget(const char *host);

/* Devices found on the network by mDNS. */
typedef struct {
    char name[64]; /* what the device calls itself */
    char host[64]; /* "name.local" */
} link_found_t;
#define LINK_MAX_FOUND 12
typedef void (*link_found_cb_t)(const link_found_t *found, int n);
/* Looks for a few seconds, then calls cb (on another task). */
void link_discover(link_found_cb_t cb);

/* A device action over HTTPS while signed in: "power/click", "power/hold",
 * "power/reset", "power/wake", "video/reconnect". cb on the link task. */
typedef void (*link_result_cb_t)(const char *action, bool ok, const char *error);
void link_action(const char *action, link_result_cb_t cb);

/* Text mode: /video carries the screen as characters, JSON messages
 * ({"kind":"text",...}) instead of frames. cb on the video socket's task. */
typedef void (*link_text_cb_t)(const char *json, size_t len);
void link_set_text_cb(link_text_cb_t cb);
void link_set_text(bool on);
bool link_text(void);
/* Connects in the background; code is the two-factor code or NULL. */
void link_connect(const link_cfg_t *cfg, const char *code);
/* Answer to LINK_TRUST. */
void link_trust(bool yes);
void link_disconnect(void);
/* Debug: frame acks on/off (takes effect on the next /video socket). */
void link_set_acks(bool on);
/* Debug: the pinned CA of the current device, or NULL. */
const char *link_ca(void);
/* Both sockets up right now. */
bool link_online(void);
/* Sends one /ws message (binary). False if not online. */
bool link_send(const uint8_t *msg, size_t len);
/* The screen is off (false): close /video so nothing is sent for nobody;
 * true opens it again. The link stays signed in either way. */
void link_video(bool on);
/* The sockets' state into the log. */
void link_debug(void);
/* Asks /video for a keyframe. */
void link_request_keyframe(void);
