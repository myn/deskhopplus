#include "dh_keyboard_output.h"

#include <string.h>

#define LEFT_CTRL  0x01u
#define LEFT_GUI   0x08u
#define RIGHT_CTRL 0x10u
#define RIGHT_GUI  0x80u

void dh_keyboard_output_prepare(
    const uint8_t canonical[DH_KEYBOARD_REPORT_LENGTH],
    dh_keyboard_provenance provenance, bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]) {
    memcpy(emitted, canonical, DH_KEYBOARD_REPORT_LENGTH);
    if (!swap_ctrl_gui || provenance != DH_KEYBOARD_PHYSICAL)
        return;

    const uint8_t modifier = canonical[0];
    emitted[0] = (uint8_t)(modifier & ~(LEFT_CTRL | LEFT_GUI | RIGHT_CTRL | RIGHT_GUI));
    if (modifier & LEFT_CTRL)
        emitted[0] |= LEFT_GUI;
    if (modifier & LEFT_GUI)
        emitted[0] |= LEFT_CTRL;
    if (modifier & RIGHT_CTRL)
        emitted[0] |= RIGHT_GUI;
    if (modifier & RIGHT_GUI)
        emitted[0] |= RIGHT_CTRL;
}

void dh_keyboard_output_merge(
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH],
    const uint8_t synthesized[DH_KEYBOARD_REPORT_LENGTH], bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]) {
    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, swap_ctrl_gui, emitted);
    emitted[0] |= synthesized[0];

    for (size_t source = 2; source < DH_KEYBOARD_REPORT_LENGTH; ++source) {
        const uint8_t key = synthesized[source];
        if (key == 0 || memchr(emitted + 2, key, DH_KEYBOARD_REPORT_LENGTH - 2))
            continue;
        uint8_t *slot = memchr(emitted + 2, 0, DH_KEYBOARD_REPORT_LENGTH - 2);
        if (slot)
            *slot = key;
    }
}
