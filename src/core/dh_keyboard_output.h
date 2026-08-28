#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dh_keyboard_transport.h"

void dh_keyboard_output_prepare(
    const uint8_t canonical[DH_KEYBOARD_REPORT_LENGTH],
    dh_keyboard_provenance provenance, bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]);

void dh_keyboard_output_merge(
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH],
    const uint8_t synthesized[DH_KEYBOARD_REPORT_LENGTH], bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]);
