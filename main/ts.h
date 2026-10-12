/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tailscale, through microlink (a native client in C): the Tab5 joins the
 * tailnet, and a device is reached by its 100.x address or its tailnet name
 * from anywhere. The tunnel is an lwIP interface for 100.64.0.0/10, so the
 * link's HTTPS and WebSockets go through it unchanged.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enable;
    char key[96];   /* tskey-auth-... */
    char name[40];  /* this Tab5 on the tailnet; "" = microlink's default */
} ts_cfg_t;

typedef struct {
    bool running;
    bool connected;
    char ip[20];    /* "100.x.y.z" once given one */
    char self[64];  /* MagicDNS name, "" if off */
    int peers;
    const char *state; /* in words */
} ts_status_t;

typedef struct {
    char name[64];
    char ip[20];
    bool online, direct;
} ts_peer_t;

/* Loads the settings; joins once the network is up. */
void ts_init(void);
/* Wi-Fi got an address (or lost it). */
void ts_net_up(bool up);
void ts_get_cfg(ts_cfg_t *out);
/* Saves and applies (rejoins if anything changed). */
void ts_set_cfg(const ts_cfg_t *cfg);
void ts_status(ts_status_t *out);
/* Peers on the tailnet; returns how many were written. */
int ts_peers(ts_peer_t *out, int max);
/* A tailnet name ("kvm" or "kvm.tail1234.ts.net") to its address, or false. */
bool ts_resolve(const char *name, char ip[20]);
