/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * How config mode gives the config drive back to the host (#229).
 *
 * Every exit from config mode used to reboot the board on the spot, with the
 * host's `DESKHOP` volume still mounted. macOS 15.8's FSKit `msdos` teardown
 * does not always survive a mounted FAT disk vanishing under it: sometimes
 * `fskitd` crashes, sometimes `diskarbitrationd` wedges in the kernel and
 * nothing mounts again until the Mac reboots (#178). Two exits in five did it.
 *
 * So leaving config mode is now two steps. First the drive reports "no
 * medium" while the USB device stays enumerated, which a host that polls
 * handles as an ordinary media removal. Then, after a grace period, the board
 * reboots. A host that ejects the drive itself first is honoured: the medium
 * is gone from then on, and the later exit reboots at once because there is
 * nothing left for the host to tear down.
 *
 * What the hardware trial found (2026-09-21, board A, macOS 15.8, on #229):
 * the grace is real — a timeout exit vanished 302.5 s after enumeration —
 * but macOS never noticed it. It asks TEST UNIT READY when it first probes
 * the disk (an exit before the mount left it unmounted, every time) and
 * never again while the volume is mounted: in eight mounted exits the kernel
 * destroyed the device 15 ms *before* the unmount, as before. On the Mac the
 * eject is therefore the protection, and the user guide says so. The grace
 * stays for hosts that do poll (Windows and Linux check about once a second,
 * unverified here) and because it costs nothing.
 *
 * Pure C11: no SDK, no I/O, no clock of its own — the caller supplies the
 * time. tests/config_exit_test.c is the gate. Same split, for the same
 * reason, as fw_upgrade.c (#90).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * How long the medium is reported absent before the board reboots.
 *
 * Two seconds: above the once-a-second media poll Windows and Linux are
 * believed to run, and still feels immediate. macOS does not poll a mounted
 * volume at all (see above), so no value of this helps it. The knob, should
 * a host turn out to poll more slowly.
 */
#define CONFIG_EXIT_GRACE_US (2u * 1000000u)

/* The chord and the timeout request the exit from core1; core0 answers the
   host and asks whether to reboot. `volatile` on the two fields that cross,
   so the compiler keeps the time's store ahead of the flag's — a flag seen
   before its time would count the grace from stale memory and could reboot
   at once with the medium present, which is the bug this exists to fix. */
typedef struct {
    volatile bool exit_requested;      // Config mode is ending: the medium is withdrawn
    bool host_ejected;                 // The host ejected the drive itself
    volatile uint32_t requested_at_us; // When the exit was requested (#107: elapsed, not absolute)
} config_exit_t;

/* Config mode is ending — by the config page's Exit, the chord, the timeout,
   or a completed UF2 drop.
   The medium is absent from this call on. The first call's time stands: the
   timeout keeps asking every pass, and re-stamping would extend the grace
   forever. */
void config_exit_request(config_exit_t *e, uint32_t now_us);

/* The host ejected the drive (START STOP UNIT with load/eject set and start
   clear). The medium is absent from this call on, and an exit no longer
   needs the grace period. This does not end config mode by itself. */
void config_exit_host_ejected(config_exit_t *e);

/* What TEST UNIT READY answers: true until an exit or an eject withdraws the
   medium, and never true again before the reboot. */
bool config_exit_medium_present(const config_exit_t *e);

/* Whether the board should reboot now: an exit was requested, and either the
   host ejected the drive or the grace period has elapsed. False when nothing
   is ending, so a caller may ask unconditionally, once per pass. */
bool config_exit_reboot_now(const config_exit_t *e, uint32_t now_us);
