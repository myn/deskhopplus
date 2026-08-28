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

    dh_keyboard_output_prepare(physical, DH_KEYBOARD_PHYSICAL, true, emitted);

    CHECK(emitted[0] == 0xca);
    CHECK(memcmp(emitted + 1, physical + 1, DH_KEYBOARD_REPORT_LENGTH - 1) == 0);
    CHECK(physical[0] == 0x53);

    const uint8_t gui[DH_KEYBOARD_REPORT_LENGTH] = {0x88};
    dh_keyboard_output_prepare(gui, DH_KEYBOARD_PHYSICAL, true, emitted);
    CHECK(emitted[0] == 0x11);
}

static void test_disabled_and_synthesized_reports_are_unchanged(void) {
    const uint8_t report[DH_KEYBOARD_REPORT_LENGTH] = {
        0x11, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_prepare(report, DH_KEYBOARD_PHYSICAL, false, emitted);
    CHECK(memcmp(emitted, report, sizeof(report)) == 0);

    dh_keyboard_output_prepare(report, DH_KEYBOARD_SYNTHESIZED, true, emitted);
    CHECK(memcmp(emitted, report, sizeof(report)) == 0);
}

static void test_synthesized_lock_preserves_transformed_physical_state(void) {
    const uint8_t physical[DH_KEYBOARD_REPORT_LENGTH] = {
        0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t synthesized_lock[DH_KEYBOARD_REPORT_LENGTH] = {
        0x08, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0};

    dh_keyboard_output_merge(physical, synthesized_lock, true, emitted);

    CHECK(emitted[0] == 0x08);
    CHECK(emitted[2] == 0x04);
    CHECK(emitted[3] == 0x0f);
}

static void test_release_after_a_transformed_modifier_is_empty(void) {
    const uint8_t held[DH_KEYBOARD_REPORT_LENGTH] = {0x01};
    const uint8_t release[DH_KEYBOARD_REPORT_LENGTH] = {0};
    uint8_t emitted[DH_KEYBOARD_REPORT_LENGTH] = {0xff};

    dh_keyboard_output_prepare(held, DH_KEYBOARD_PHYSICAL, true, emitted);
    CHECK(emitted[0] == 0x08);

    dh_keyboard_output_prepare(release, DH_KEYBOARD_PHYSICAL, true, emitted);

    CHECK(memcmp(emitted, release, sizeof(release)) == 0);
}

static void test_only_macos_defaults_to_the_swap(void) {
    CHECK(!DEFAULT_CTRL_GUI_SWAP(LINUX));
    CHECK(DEFAULT_CTRL_GUI_SWAP(MACOS));
    CHECK(!DEFAULT_CTRL_GUI_SWAP(WINDOWS));
    CHECK(!DEFAULT_CTRL_GUI_SWAP(OTHER));
}

int main(void) {
    test_physical_ctrl_and_gui_are_swapped_for_the_output();
    test_disabled_and_synthesized_reports_are_unchanged();
    test_synthesized_lock_preserves_transformed_physical_state();
    test_release_after_a_transformed_modifier_is_empty();
    test_only_macos_defaults_to_the_swap();
    if (failures == 0)
        printf("keyboard_output_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
