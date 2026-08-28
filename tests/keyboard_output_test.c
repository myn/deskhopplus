/* TDD specification for issue #22's public keyboard-output seam. */

#include <stdio.h>
#include <string.h>

#include "dh_keyboard_output.h"
#include "screen.h"

static int failures;
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);         \
        }                                                                       \
    } while (0)

static void test_physical_ctrl_and_gui_are_swapped_for_the_output(void) {
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH] = {
        0x53, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, NULL, true, emitted);

    CHECK(emitted[0] == 0xca);
    CHECK(memcmp(emitted + 1, physical + 1, DH_KEYBOARD_REPORT_LENGTH - 1) == 0);
    CHECK(physical[0] == 0x53);

    const uint8_t gui[DH_KEYBOARD_REPORT_LENGTH] = {0x88};
    dh_keyboard_output_prepare(gui, DH_KEYBOARD_PHYSICAL, NULL, true, emitted);
    CHECK(emitted[0] == 0x11);
}

static void test_disabled_and_synthesized_reports_are_unchanged(void) {
    const uint8_t report[DH_KEYBOARD_REPORT_LENGTH] = {
        0x11, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(report, DH_KEYBOARD_PHYSICAL, NULL, false, emitted);
    CHECK(memcmp(emitted, report, sizeof(report)) == 0);

    dh_keyboard_output_prepare(report, DH_KEYBOARD_SYNTHESIZED, NULL, true, emitted);
    CHECK(memcmp(emitted, report, sizeof(report)) == 0);
}

static void test_synthesized_lock_preserves_transformed_physical_state(void) {
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH] = {
        0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t synthesized_lock[DH_KEYBOARD_REPORT_LENGTH] = {
        0x08, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_merge(physical, synthesized_lock, NULL, true, emitted);

    CHECK(emitted[0] == 0x08);
    CHECK(emitted[2] == 0x04);
    CHECK(emitted[3] == 0x0f);
}

static void test_release_after_a_transformed_modifier_is_empty(void) {
    const uint8_t held[DH_KEYBOARD_REPORT_LENGTH] = {0x01};
    const uint8_t release[DH_KEYBOARD_REPORT_LENGTH] = {0};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0xff};

    dh_keyboard_output_prepare(held, DH_KEYBOARD_PHYSICAL, NULL, true, emitted);
    CHECK(emitted[0] == 0x08);

    dh_keyboard_output_prepare(release, DH_KEYBOARD_PHYSICAL, NULL, true, emitted);

    CHECK(memcmp(emitted, release, sizeof(release)) == 0);
}

static void test_only_macos_defaults_to_the_swap(void) {
    CHECK(!DEFAULT_CTRL_GUI_SWAP(LINUX));
    CHECK(DEFAULT_CTRL_GUI_SWAP(MACOS));
    CHECK(!DEFAULT_CTRL_GUI_SWAP(WINDOWS));
    CHECK(!DEFAULT_CTRL_GUI_SWAP(OTHER));
}

static void test_overrides_cross_keycode_and_modifier_classes(void) {
    const dh_keymap_profile_t profile = {
        .overrides = {{0x39, 0xe0}, {0xe1, 0x04}, {0xe0, 0xe3}, {0x05, 0x06}},
        .override_count = 4,
    };
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH] = {
        0x03, 0x00, 0x39, 0x05, 0x00, 0x00, 0x00, 0x00};
    const uint8_t expected[DH_KEYBOARD_REPORT_LENGTH] = {
        0x09, 0x00, 0x04, 0x06, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, &profile, false, emitted);

    CHECK(memcmp(emitted, expected, sizeof(expected)) == 0);
}

static void test_passthrough_precedes_override_and_swap(void) {
    const dh_keymap_profile_t profile = {
        .overrides = {{0x39, 0xe0}, {0xe0, 0x04}},
        .override_count = 2,
        .passthrough = {0x39, 0xe0},
        .passthrough_count = 2,
    };
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH] = {
        0x01, 0x00, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, &profile, true, emitted);

    CHECK(memcmp(emitted, physical, sizeof(physical)) == 0);
}

static void test_collisions_deduplicate_and_capacity_drops_the_tail(void) {
    const dh_keymap_profile_t profile = {
        .overrides = {{0x04, 0xe0}, {0x05, 0xe0}, {0x07, 0x06}},
        .override_count = 3,
    };
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH] = {
        0x00, 0x00, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    const uint8_t expected[DH_KEYBOARD_REPORT_LENGTH] = {
        0x01, 0x00, 0x06, 0x08, 0x09, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, &profile, false, emitted);

    CHECK(memcmp(emitted, expected, sizeof(expected)) == 0);

    const dh_keymap_profile_t full_profile = {
        .overrides = {{0x04, 0x0a}},
        .override_count = 1,
    };
    const uint8_t full[DH_KEYBOARD_REPORT_LENGTH] = {
        0x00, 0x00, 0x05, 0x06, 0x07, 0x08, 0x09, 0x04};
    const uint8_t full_expected[DH_KEYBOARD_REPORT_LENGTH] = {
        0x00, 0x00, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a};
    dh_keyboard_output_prepare(full, DH_KEYBOARD_PHYSICAL, &full_profile, false, emitted);
    CHECK(memcmp(emitted, full_expected, sizeof(full_expected)) == 0);
}

static void test_profile_changes_are_applied_to_the_next_snapshot(void) {
    const uint8_t held[DH_KEYBOARD_REPORT_LENGTH] = {0x00, 0x00, 0x39};
    const dh_keymap_profile_t first = {
        .overrides = {{0x39, 0xe0}}, .override_count = 1};
    const dh_keymap_profile_t second = {
        .overrides = {{0x39, 0x04}}, .override_count = 1};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(held, DH_KEYBOARD_PHYSICAL, &first, false, emitted);
    CHECK(emitted[0] == 0x01 && emitted[2] == 0x00);

    dh_keyboard_output_prepare(held, DH_KEYBOARD_PHYSICAL, &second, false, emitted);
    CHECK(emitted[0] == 0x00 && emitted[2] == 0x04);
}

/* The reference is the original mkroamer KeyMapper::map behavior over the
   shared HID inputs. Keeping it independent of the report implementation
   makes this a differential check rather than another spelling of its code. */
static uint8_t mkroamer_original_map(uint8_t usage, const dh_keymap_profile_t *profile,
                                    bool swap_ctrl_gui) {
    for (size_t i = 0; i < profile->passthrough_count; ++i)
        if (profile->passthrough[i] == usage)
            return usage;
    for (size_t i = 0; i < profile->override_count; ++i)
        if (profile->overrides[i].from == usage)
            return profile->overrides[i].to;
    if (swap_ctrl_gui) {
        const uint8_t from[] = {0xe0, 0xe3, 0xe4, 0xe7};
        const uint8_t to[] = {0xe3, 0xe0, 0xe7, 0xe4};
        for (size_t i = 0; i < sizeof(from); ++i)
            if (from[i] == usage)
                return to[i];
    }
    return usage;
}

static void test_shared_inputs_match_mkroamers_original_mapper(void) {
    const dh_keymap_profile_t profile = {
        .overrides = {{0xe0, 0xe0}, {0x39, 0xe0}, {0x04, 0x05}},
        .override_count = 3,
        .passthrough = {0xe4, 0x06},
        .passthrough_count = 2,
    };
    const uint8_t shared[] = {0x04, 0x06, 0x39, 0xe0, 0xe1, 0xe3, 0xe4, 0xe7};

    for (size_t i = 0; i < sizeof(shared); ++i) {
        const uint8_t usage = shared[i];
        uint8_t canonical[DH_KEYBOARD_REPORT_LENGTH] = {0};
        if (usage >= 0xe0)
            canonical[0] = (uint8_t)(1u << (usage - 0xe0));
        else
            canonical[2] = usage;
        uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};
        dh_keyboard_output_prepare(canonical, DH_KEYBOARD_PHYSICAL, &profile, true, emitted);
        const uint8_t expected = mkroamer_original_map(usage, &profile, true);
        if (expected >= 0xe0)
            CHECK(emitted[0] == (uint8_t)(1u << (expected - 0xe0)) && emitted[2] == 0);
        else
            CHECK(emitted[0] == 0 && emitted[2] == expected);
    }
}

int main(void) {
    test_physical_ctrl_and_gui_are_swapped_for_the_output();
    test_disabled_and_synthesized_reports_are_unchanged();
    test_synthesized_lock_preserves_transformed_physical_state();
    test_release_after_a_transformed_modifier_is_empty();
    test_only_macos_defaults_to_the_swap();
    test_overrides_cross_keycode_and_modifier_classes();
    test_passthrough_precedes_override_and_swap();
    test_collisions_deduplicate_and_capacity_drops_the_tail();
    test_profile_changes_are_applied_to_the_next_snapshot();
    test_shared_inputs_match_mkroamers_original_mapper();
    if (failures == 0)
        printf("keyboard_output_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
