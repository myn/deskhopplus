/*
 * Host tests for the public hotkey-table seam. No test framework: each
 * assertion prints its own failure and main returns non-zero, following the
 * convention established by session_test.c.
 */
#include "dh_hotkey.h"

#include <stdio.h>

#define ASSERT_TRUE(expr)                                                                         \
    do {                                                                                          \
        if (!(expr)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                       \
            return 1;                                                                             \
        }                                                                                         \
    } while (0)

static int more_specific_overlapping_chord_wins(void) {
    dh_hotkey_t hotkeys[] = {
        {.modifier = 0x01, .keys = {0x0e}, .key_count = 1},
        {.modifier = 0x01, .keys = {0x0e, 0x0f}, .key_count = 2},
    };
    const uint8_t pressed[DH_HOTKEY_KEY_CAPACITY] = {0x0e, 0x0f};

    dh_hotkey_prepare(hotkeys, 2);
    const dh_hotkey_t *match = dh_hotkey_match(hotkeys, 2, 0x01, pressed);

    ASSERT_TRUE(match != NULL);
    ASSERT_TRUE(match->key_count == 2);
    return 0;
}

static int modifier_count_contributes_to_specificity(void) {
    dh_hotkey_t hotkeys[] = {
        {.modifier = 0x01, .keys = {0x0e}, .key_count = 1},
        {.modifier = 0x03, .keys = {0x0e}, .key_count = 1},
    };
    const uint8_t pressed[DH_HOTKEY_KEY_CAPACITY] = {0x0e};

    dh_hotkey_prepare(hotkeys, 2);
    const dh_hotkey_t *match = dh_hotkey_match(hotkeys, 2, 0x03, pressed);

    ASSERT_TRUE(match != NULL);
    ASSERT_TRUE(match->modifier == 0x03);
    return 0;
}

static int extra_modifiers_preserve_subset_matching(void) {
    const dh_hotkey_t hotkeys[] = {
        {.modifier = 0x01, .keys = {0x0e}, .key_count = 1},
    };
    const uint8_t pressed[DH_HOTKEY_KEY_CAPACITY] = {0x0e};

    const dh_hotkey_t *match = dh_hotkey_match(hotkeys, 1, 0x05, pressed);

    ASSERT_TRUE(match == &hotkeys[0]);
    return 0;
}

static int zero_is_not_a_required_key(void) {
    const dh_hotkey_t hotkeys[] = {
        {.modifier = 0x01, .keys = {0x00}, .key_count = 1},
    };
    const uint8_t pressed[DH_HOTKEY_KEY_CAPACITY] = {0};

    ASSERT_TRUE(dh_hotkey_match(hotkeys, 1, 0x01, pressed) == NULL);
    return 0;
}

static int zero_configured_key_uses_fallback(void) {
    dh_hotkey_t hotkeys[] = {
        {.modifier = 0x01, .keys = {0x39}, .key_count = 1, .action_id = 7},
    };
    const uint8_t fallback_pressed[DH_HOTKEY_KEY_CAPACITY] = {0x3a};

    ASSERT_TRUE(dh_hotkey_configure_key(hotkeys, 1, 7, 0, 0x3a));
    ASSERT_TRUE(dh_hotkey_match(hotkeys, 1, 0x01, fallback_pressed) == &hotkeys[0]);
    return 0;
}

int main(void) {
    if (more_specific_overlapping_chord_wins())
        return 1;
    if (modifier_count_contributes_to_specificity())
        return 1;
    if (extra_modifiers_preserve_subset_matching())
        return 1;
    if (zero_is_not_a_required_key())
        return 1;
    if (zero_configured_key_uses_fallback())
        return 1;

    printf("hotkey_test: PASS\n");
    return 0;
}
