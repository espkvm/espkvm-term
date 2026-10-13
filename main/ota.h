/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Updating the terminal itself from its latest GitHub release. Not when it
 * runs from M5 Launcher: there the launcher owns the flash layout, so it
 * updates the app.
 */
#pragma once

#include <stdbool.h>

#define OTA_REPO  "espkvm/espkvm-term"
#define OTA_ASSET "espkvm-term.bin"

typedef enum {
    OTA_IDLE,
    OTA_UNSUPPORTED, /* running from a launcher */
    OTA_CHECKING,
    OTA_LATEST,      /* this is the newest */
    OTA_AVAILABLE,   /* latest[] is newer */
    OTA_INSTALLING,  /* percent */
    OTA_DONE,        /* restarting */
    OTA_FAILED,      /* error[] */
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current[32];
    char latest[32];
    int percent;
    char error[96];
} ota_info_t;

/* At start: what is running, and tells the bootloader this image works once
 * the network is up (call ota_good then). */
void ota_init(void);
void ota_good(void);
/* Both run in the background; follow with ota_get. */
void ota_check(void);
void ota_install(void);
void ota_get(ota_info_t *out);
