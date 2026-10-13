/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A USB keyboard and mouse in the Tab5's USB-A port, read in the boot
 * protocol every keyboard and mouse speaks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tab5kbd.h"

/* A mouse report: buttons, movement, wheel (+ = away from you). */
typedef void (*usbhid_mouse_cb_t)(uint8_t buttons, int dx, int dy, int wheel);

/* Powers the port and starts the USB host. Keys come as the Tab5 keyboard's
 * events (on_key), with the ASCII filled in for text fields; on_presence
 * tells when a keyboard comes and goes. Callbacks run on the USB task. */
void usbhid_start(kbd_event_cb_t on_key, kbd_presence_cb_t on_presence, usbhid_mouse_cb_t on_mouse);
