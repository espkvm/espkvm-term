/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The M5Stack Tab5 keyboard (A164): I2C 0x6D on Ext.Port1 (SDA G0, SCL G1,
 * INT G50). Read in matrix mode, so every key has a press and a release.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* HID modifier bits (keyboard page). */
#define KBD_MOD_LCTRL  0x01
#define KBD_MOD_LSHIFT 0x02
#define KBD_MOD_LALT   0x04

typedef struct {
    uint8_t usage;   /* HID usage, 0 for Sym/Aa and the modifier keys */
    uint8_t mods;    /* HID modifiers for this key: held ones plus any it needs (Shift for '!') */
    uint8_t held;    /* just the modifier keys held down */
    char ch;         /* ASCII for text fields, 0 if none */
    bool pressed;
} kbd_event_t;

typedef void (*kbd_event_cb_t)(const kbd_event_t *ev);
typedef void (*kbd_presence_cb_t)(bool present);

/* Starts the polling task. Plugging and unplugging is noticed within a second. */
void kbd_start(kbd_event_cb_t on_event, kbd_presence_cb_t on_presence);
bool kbd_present(void);
