/*
 * Whether a firmware upgrade being received is still alive (#90).
 * See fw_upgrade.h for why this is not inside the tasks and handlers.
 */

#include "fw_upgrade.h"

void fw_upgrade_progress(fw_upgrade_state_t *fw, uint32_t now_us) {
    fw->progressed_at_us = now_us;
}

bool fw_upgrade_stalled(const fw_upgrade_state_t *fw, uint32_t now_us) {
    if (!fw->upgrade_in_progress)
        return false;

    /* Unsigned 32-bit subtraction, so the microsecond counter wrapping every
       71 minutes gives the right answer rather than a spurious stall. This is
       why the timestamp is not compared directly. */
    return (uint32_t)(now_us - fw->progressed_at_us) >= FW_UPGRADE_STALL_US;
}
