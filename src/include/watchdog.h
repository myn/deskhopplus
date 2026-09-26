/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 * Modified by Derek Reynolds, 2026, for deskhopplus.
 */
#pragma once

#include <hardware/watchdog.h>

#define WATCHDOG_TIMEOUT        500                     // In milliseconds => needs to be reset at least every 200ms
#define WATCHDOG_PAUSE_ON_DEBUG 1                       // When using a debugger, disable watchdog
#define CORE1_HANG_TIMEOUT_US   WATCHDOG_TIMEOUT * 1000 // Convert to microseconds

#define MAGIC_WORD_1 0xdeadf00f // When these are set, we'll boot to configuration mode
/* Set by the config chord and consumed on the next normal-mode boot: a
   pairing window is owed (#46). Pairing still waits until Exit, so the flag
   has to survive a boot it is not consumed on. It lives in
   scratch[3] for that reason: the SDK's own watchdog_enable() overwrites
   scratch[4] on every boot, and watchdog_reboot() writes 5, 6 and 7, so a
   flag in any of those is erased before the boot that would have used it. */
#define MAGIC_WORD_PAIR 0x9a17c0de
/* Set on every reboot the firmware means — the chord's watchdog-timeout
   path and reboot() alike — and read and cleared by cursor_trace_boot, so a
   boot the watchdog caused without it is a hang (#102). scratch[2] because
   0-2 are the ones nothing else writes. */
#define MAGIC_WORD_REBOOT 0x0b00dead
#define MAGIC_WORD_2 0x00c0ffee
/* Set when the peer board asked this board to link its helper: enter config
   mode, then leave it immediately on the next boot. scratch[0] because 0-2
   are the ones nothing else writes, and 3, 5 and 6 already carry the pairing
   window and the config-mode flags. Consumed by is_config_mode_active. */
#define MAGIC_WORD_LINK_HELPER 0x11ac0ffe
