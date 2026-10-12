/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

/* Screenshots and taps over the USB serial port, for development. */
void debug_link_start(void (*inject_key)(uint32_t key));
