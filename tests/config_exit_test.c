/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * How config mode gives the config drive back to the host (#229).
 *
 * The defect this guards is on the host, not the board: a FAT volume still
 * mounted when its device vanishes can crash macOS's FSKit driver or wedge
 * its disk mounter until the Mac reboots (#178). The board's part is to
 * withdraw the medium first and reboot only after the host has had time to
 * notice — and to honour a host that ejects the drive itself.
 *
 * Checked only through what the host and the reboot path see: is the medium
 * present, and is it time to reboot. Style follows fw_upgrade_test.c.
 */

#include <stdio.h>

#include "config_exit.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

#define SECONDS(n) ((uint32_t)((n) * 1000000u))

int main(void) {
    /* Config mode with nothing ending: the drive is there and the
       board stays up, however long the caller keeps asking. */
    {
        config_exit_t e = {0};
        CHECK(config_exit_medium_present(&e), "idle", "medium present");
        CHECK(!config_exit_reboot_now(&e, SECONDS(3600)), "idle", "no reboot");
    }

    /* An exit withdraws the medium at once and reboots only once the grace
       period has elapsed — that gap is what gives the host its clean
       media removal. */
    {
        config_exit_t e = {0};
        config_exit_request(&e, SECONDS(10));

        CHECK(!config_exit_medium_present(&e), "exit", "medium absent at once");
        CHECK(!config_exit_reboot_now(&e, SECONDS(10)), "exit", "no reboot at once");
        CHECK(!config_exit_reboot_now(&e, SECONDS(10) + CONFIG_EXIT_GRACE_US - 1),
              "exit", "no reboot one tick short of the grace");
        CHECK(config_exit_reboot_now(&e, SECONDS(10) + CONFIG_EXIT_GRACE_US),
              "exit", "reboot on the grace");
    }

    /* The host ejected the drive itself, so the medium is gone and nothing is
       left for it to tear down — the exit that follows reboots at once, with
       no grace to sit through for nothing. */
    {
        config_exit_t e = {0};
        config_exit_host_ejected(&e);

        CHECK(!config_exit_medium_present(&e), "eject-first", "medium absent after the eject");
        CHECK(!config_exit_reboot_now(&e, SECONDS(3600)), "eject-first", "an eject alone never reboots");

        config_exit_request(&e, SECONDS(3601));
        CHECK(config_exit_reboot_now(&e, SECONDS(3601)), "eject-first", "the exit reboots at once");
    }

    /* An eject that lands during the grace ends it: the host has just said it
       is finished with the drive. */
    {
        config_exit_t e = {0};
        config_exit_request(&e, SECONDS(10));
        CHECK(!config_exit_reboot_now(&e, SECONDS(11)), "eject-during", "still in the grace");

        config_exit_host_ejected(&e);
        CHECK(config_exit_reboot_now(&e, SECONDS(11)), "eject-during", "reboot at once on the eject");
    }

    /* Once withdrawn the medium stays gone, so the host never sees the drive
       flicker back between the removal and the reboot — an eject that lands
       in the grace included. */
    {
        config_exit_t e = {0};
        config_exit_request(&e, SECONDS(10));
        config_exit_host_ejected(&e);
        CHECK(!config_exit_medium_present(&e), "gone", "absent after an eject too");
    }

    /* The timeout keeps asking for the exit on every pass it is past due. If
       each ask restarted the grace, the board would never reboot. */
    {
        config_exit_t e = {0};
        config_exit_request(&e, SECONDS(10));
        config_exit_request(&e, SECONDS(11));

        CHECK(config_exit_reboot_now(&e, SECONDS(10) + CONFIG_EXIT_GRACE_US),
              "repeat", "the first request's grace stands");
    }

    /* The microsecond counter wraps every 71 minutes. A grace that straddles
       the wrap is still the same two seconds, not a spurious reboot on the
       spot and not a wait of seventy-one minutes. */
    {
        config_exit_t e = {0};
        uint32_t just_before_wrap = UINT32_MAX - SECONDS(1) + 1;
        config_exit_request(&e, just_before_wrap);

        CHECK(!config_exit_reboot_now(&e, just_before_wrap), "wrap", "no reboot at once");
        CHECK(!config_exit_reboot_now(&e, SECONDS(1) - 1), "wrap", "no reboot one tick short, past the wrap");
        CHECK(config_exit_reboot_now(&e, SECONDS(1)), "wrap", "reboot on the grace, past the wrap");
    }

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("config_exit_test: all passed\n");

    return failures ? 1 : 0;
}
