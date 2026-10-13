/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The battery (an INA226 on the system I2C bus measures it, the charger
 * IP2326 reports through the second IO expander) and the screen when nobody
 * is using it: dimmed after a while, off after longer when on battery.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    bool present;   /* a battery is in */
    bool charging;
    int percent;    /* 0..100, from the voltage */
    float volts;
} power_state_t;

/* After the display and the I2C bus are up. The screen starts at the saved
 * brightness (default_pct the first time). */
void power_start(int default_pct);
/* The screen's brightness, 5..100 %, kept across restarts. */
void power_set_brightness(int pct);
int power_brightness(void);
void power_get(power_state_t *out);
/* Someone touched, typed or moved the mouse (any task). Wakes the screen;
 * true if it was off, so the touch that woke it can be dropped. */
bool power_activity(void);
/* Screen off now (and no picture sent); a touch or a key wakes it. */
void power_sleep(void);
/* Cuts the Tab5's power through the charger's PWROFF line; the power button
 * turns it on again. With no battery the USB keeps it running. */
void power_off(void);
