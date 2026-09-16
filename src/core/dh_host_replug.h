/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * When to emulate a cable pull on the board's own USB host (#102).
 *
 * A warm reboot never drops VBUS to the USB-A port, so the keyboard receiver
 * stays attached and configured while the host stack starts from nothing.
 * About one boot in three it is never enumerated: the port reads attached,
 * no device mounts, and the keyboard is dead until someone pulls the cable.
 * The pull always works, so this decides when the firmware should do the
 * same thing itself. The root cause is still open on #102; this is what
 * makes it survivable, and each pull is counted in the trace so the hunt
 * has a number.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A good enumeration mounts within ~0.6 s of the attach (TinyUSB's 50 ms
   reset, 450 ms debounce, then the descriptors); its three retries add up to
   0.3 s more. Two seconds is past all of that with margin. */
#define DH_HOST_REPLUG_TIMEOUT_US 2000000u
/* ponytail: bounded so a device that can never mount is not pulled forever;
   a real unplug restores the budget. Raise it if a slow device needs more. */
#define DH_HOST_REPLUG_MAX_ATTEMPTS 5u

/* Zero is a valid start: the port was last seen healthy at boot. */
typedef struct {
    uint32_t since_us; /* when the port was last seen healthy, or last pulled */
    uint8_t attempts;  /* pulls since the last mount or unplug */
} dh_host_replug;

/* Call every pass with the port's line state and whether any device is
   mounted. True exactly when a pull is due; the caller performs it. */
static inline bool dh_host_replug_due(dh_host_replug *r, bool attached, bool mounted,
                                      uint32_t now_us) {
    if (!attached || mounted) {
        r->since_us = now_us;
        r->attempts = 0;
        return false;
    }
    if (r->attempts >= DH_HOST_REPLUG_MAX_ATTEMPTS ||
        (uint32_t)(now_us - r->since_us) < DH_HOST_REPLUG_TIMEOUT_US)
        return false;
    r->since_us = now_us;
    r->attempts++;
    return true;
}
