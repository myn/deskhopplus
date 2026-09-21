/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * How config mode gives the config drive back to the host (#229).
 * See config_exit.h for why this is not inside the tasks and callbacks.
 */

#include "config_exit.h"

void config_exit_request(config_exit_t *e, uint32_t now_us) {
    if (e->exit_requested)
        return;

    /* The time lands before the flag; config_exit.h says why the order holds. */
    e->requested_at_us = now_us;
    e->exit_requested  = true;
}

void config_exit_host_ejected(config_exit_t *e) {
    e->host_ejected = true;
}

bool config_exit_medium_present(const config_exit_t *e) {
    return !e->exit_requested && !e->host_ejected;
}

bool config_exit_reboot_now(const config_exit_t *e, uint32_t now_us) {
    if (!e->exit_requested)
        return false;

    /* Unsigned 32-bit subtraction, so the microsecond counter wrapping every
       71 minutes gives the right answer rather than a spurious reboot. */
    return e->host_ejected || (uint32_t)(now_us - e->requested_at_us) >= CONFIG_EXIT_GRACE_US;
}
