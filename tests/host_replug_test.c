/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include <stdio.h>
#include <stdlib.h>
#include "dh_host_replug.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

/*
 * The board's USB host sometimes never enumerates a receiver that stayed
 * attached across a warm reboot (#102). A cable pull always fixes it, so the
 * firmware emulates one when a device is present and nothing has mounted for
 * DH_HOST_REPLUG_TIMEOUT_US. This is the decision, with the clock supplied.
 */
int main(void) {
    const uint32_t T = DH_HOST_REPLUG_TIMEOUT_US;
    dh_host_replug r = {0};

    /* Nothing plugged in: never, however long it stays empty. */
    CHECK(!dh_host_replug_due(&r, false, false, 10 * T));

    /* A healthy port: attached and mounted keeps the clock fresh. */
    CHECK(!dh_host_replug_due(&r, true, true, 11 * T));
    CHECK(!dh_host_replug_due(&r, true, false, 11 * T + T - 1));

    /* Attached, nothing mounted, for a whole timeout: pull the cable once. */
    CHECK(dh_host_replug_due(&r, true, false, 12 * T));
    CHECK(r.attempts == 1);
    /* ...and not again until another whole timeout has passed. */
    CHECK(!dh_host_replug_due(&r, true, false, 12 * T + 1));
    CHECK(!dh_host_replug_due(&r, true, false, 13 * T - 1));
    CHECK(dh_host_replug_due(&r, true, false, 13 * T));

    /* A mount ends the run and forgives the attempts. */
    CHECK(!dh_host_replug_due(&r, true, true, 13 * T + 1));
    CHECK(r.attempts == 0);

    /* A device that never mounts is pulled a bounded number of times. */
    uint32_t now = 20 * T;
    unsigned pulls = 0;
    for (unsigned i = 0; i < 2 * DH_HOST_REPLUG_MAX_ATTEMPTS; ++i, now += T)
        if (dh_host_replug_due(&r, true, false, now)) pulls++;
    CHECK(pulls == DH_HOST_REPLUG_MAX_ATTEMPTS);

    /* A real unplug resets the budget. */
    CHECK(!dh_host_replug_due(&r, false, false, now));
    CHECK(r.attempts == 0);
    CHECK(dh_host_replug_due(&r, true, false, now + T));

    /* The clock wraps and the arithmetic does not care. */
    r = (dh_host_replug){.since_us = UINT32_MAX - T / 2};
    CHECK(!dh_host_replug_due(&r, true, false, UINT32_MAX));
    CHECK(dh_host_replug_due(&r, true, false, T / 2));

    printf("host replug tests passed\n");
    return 0;
}
