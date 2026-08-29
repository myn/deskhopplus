#include "dh_keyboard_output.h"

#include <string.h>

static bool contains(const uint8_t *values, size_t count, uint8_t usage) {
    return memchr(values, usage, count) != NULL;
}

static uint8_t map_usage(uint8_t usage, const dh_keymap_profile_t *profile,
                         bool swap_ctrl_gui) {
    if (profile && contains(profile->passthrough,
                            profile->passthrough_count <= DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY
                                ? profile->passthrough_count
                                : DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY,
                            usage))
        return usage;

    if (profile) {
        const size_t count = profile->override_count <= DH_CONFIG_TEXT_OVERRIDE_CAPACITY
                                 ? profile->override_count
                                 : DH_CONFIG_TEXT_OVERRIDE_CAPACITY;
        for (size_t i = 0; i < count; ++i) {
            if (profile->overrides[i].from == usage)
                return profile->overrides[i].to;
        }
    }

    if (!swap_ctrl_gui)
        return usage;
    switch (usage) {
        case 0xe0: return 0xe3;
        case 0xe3: return 0xe0;
        case 0xe4: return 0xe7;
        case 0xe7: return 0xe4;
        default: return usage;
    }
}

static void add_usage(uint8_t usage, uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]) {
    if (usage >= 0xe0 && usage <= 0xe7) {
        emitted[0] |= (uint8_t)(1u << (usage - 0xe0));
        return;
    }
    if (usage == 0)
        return;
    /* HID keyboard error usages fill every key slot deliberately. Collapsing
       ErrorRollOver (0x01), POSTFail (0x02), or ErrorUndefined (0x03) would
       turn the host-visible error report into an ordinary one-key report. */
    if (usage > 0x03 && memchr(emitted + 2, usage, DH_KEYBOARD_REPORT_LENGTH - 2))
        return;
    uint8_t *slot = memchr(emitted + 2, 0, DH_KEYBOARD_REPORT_LENGTH - 2);
    if (slot)
        *slot = usage;
}

void dh_keyboard_output_prepare(
    const uint8_t canonical[DH_KEYBOARD_REPORT_LENGTH],
    dh_keyboard_provenance provenance, const dh_keymap_profile_t *profile,
    bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]) {
    if (provenance != DH_KEYBOARD_PHYSICAL) {
        memcpy(emitted, canonical, DH_KEYBOARD_REPORT_LENGTH);
        return;
    }

    memset(emitted, 0, DH_KEYBOARD_REPORT_LENGTH);
    emitted[1] = canonical[1];
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (canonical[0] & (1u << bit))
            add_usage(map_usage((uint8_t)(0xe0u + bit), profile, swap_ctrl_gui), emitted);
    }
    for (size_t i = 2; i < DH_KEYBOARD_REPORT_LENGTH; ++i)
        add_usage(map_usage(canonical[i], profile, swap_ctrl_gui), emitted);
}

void dh_keyboard_output_merge(
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH],
    const uint8_t synthesized[DH_KEYBOARD_REPORT_LENGTH],
    const dh_keymap_profile_t *profile, bool swap_ctrl_gui,
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH]) {
    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, profile, swap_ctrl_gui, emitted);
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
