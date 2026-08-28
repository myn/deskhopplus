#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dh_keyboard_transport.h"
#include "dh_keymap.h"

void dh_keyboard_output_prepare(
    const uint8_t canonical[DH_KEYBOARD_REPORT_LENGTH],
    dh_keyboard_provenance provenance, const dh_keymap_profile_t *profile,
    bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]);

void dh_keyboard_output_merge(
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH],
    const uint8_t synthesized[DH_KEYBOARD_REPORT_LENGTH],
    const dh_keymap_profile_t *profile, bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]);
