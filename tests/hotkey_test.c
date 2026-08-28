/*
 * Host tests for the public hotkey-table seam. No test framework: each
 * assertion prints its own failure and main returns non-zero, following the
 * convention established by session_test.c.
 */
#include "dh_hotkey.h"
#include "dh_hotkey_defaults.h"

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

static int all_actions_have_stable_names(void) {
    ASSERT_TRUE(DH_HOTKEY_ACTION_COUNT == 13);
    for (uint8_t action = 0; action < DH_HOTKEY_ACTION_COUNT; ++action) {
        const char *name = dh_hotkey_action_name(action);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(dh_hotkey_action_id(name) == action);
    }
    ASSERT_TRUE(dh_hotkey_action_id("not-an-action") == DH_HOTKEY_ACTION_INVALID);
    return 0;
}

static int chord_usages_become_a_runtime_binding(void) {
    const uint8_t chord[] = {0xe0, 0xe5, 0x06, 0x12}; /* lctrl+rshift+c+o */
    dh_hotkey_t binding = {0};

    ASSERT_TRUE(dh_hotkey_binding_from_usages(
        &binding, DH_HOTKEY_ACTION_CONFIG_ENABLE, chord, sizeof(chord)));
    ASSERT_TRUE(binding.modifier == (0x01 | 0x20));
    ASSERT_TRUE(binding.key_count == 2);
    ASSERT_TRUE(binding.keys[0] == 0x06 && binding.keys[1] == 0x12);
    ASSERT_TRUE(binding.action_id == DH_HOTKEY_ACTION_CONFIG_ENABLE);
    return 0;
}

static int recovery_chord_always_reaches_config_mode(void) {
    dh_hotkey_t configured[] = {{.modifier = 0x01,
                                 .keys = {0x04},
                                 .key_count = 1,
                                 .action_id = DH_HOTKEY_ACTION_OUTPUT_TOGGLE}};
    const uint8_t recovery[DH_HOTKEY_KEY_CAPACITY] = {0x06, 0x12};

    const dh_hotkey_t *match = dh_hotkey_match_with_recovery(
        configured, 1, 0x21, recovery);
    ASSERT_TRUE(match != NULL);
    ASSERT_TRUE(match->action_id == DH_HOTKEY_ACTION_CONFIG_ENABLE);
    return 0;
}

static int action_properties_and_complete_table_are_resolved_at_the_keyboard_seam(void) {
    dh_hotkey_t table[DH_HOTKEY_ACTION_COUNT];
    for (uint8_t action = 0; action < DH_HOTKEY_ACTION_COUNT; ++action) {
        table[action] = (dh_hotkey_t){.modifier = 0x01,
                                     .keys = {(uint8_t)(0x04 + action)},
                                     .key_count = 1,
                                     .action_id = action};
    }
    ASSERT_TRUE(dh_hotkey_table_is_valid(table, DH_HOTKEY_ACTION_COUNT));
    ASSERT_TRUE(dh_hotkey_action_passes_to_os(DH_HOTKEY_ACTION_MOUSE_ZOOM));
    ASSERT_TRUE(!dh_hotkey_action_passes_to_os(DH_HOTKEY_ACTION_SCREENLOCK));
    ASSERT_TRUE(!dh_hotkey_action_acknowledges(DH_HOTKEY_ACTION_OUTPUT_TOGGLE));
    ASSERT_TRUE(dh_hotkey_action_acknowledges(DH_HOTKEY_ACTION_SCREENLOCK));

    const uint8_t zoom_keys[DH_HOTKEY_KEY_CAPACITY] = {0x05};
    dh_keyboard_hotkey_result_t result = dh_keyboard_hotkey_resolve(
        table, DH_HOTKEY_ACTION_COUNT, 0x01, zoom_keys);
    ASSERT_TRUE(result.matched);
    ASSERT_TRUE(result.action_id == DH_HOTKEY_ACTION_MOUSE_ZOOM);
    ASSERT_TRUE(result.pass_to_os && result.acknowledge);

    table[12].action_id = 11;
    ASSERT_TRUE(!dh_hotkey_table_is_valid(table, DH_HOTKEY_ACTION_COUNT));
    table[12].action_id = 12;
    table[12].key_count = DH_HOTKEY_KEY_CAPACITY + 1;
    ASSERT_TRUE(!dh_hotkey_table_is_valid(table, DH_HOTKEY_ACTION_COUNT));
    return 0;
}

static bool binding_is(const dh_hotkey_t *binding, uint8_t action, uint8_t modifier,
                       uint8_t first_key, uint8_t second_key) {
    uint8_t key_count = second_key == 0 ? 1 : 2;
    return binding->action_id == action && binding->modifier == modifier &&
           binding->key_count == key_count && binding->keys[0] == first_key &&
           binding->keys[1] == second_key;
}

static int default_hotkeys_are_reachable_without_right_ctrl_and_preserve_existing_chords(void) {
    const dh_hotkey_t defaults[DH_HOTKEY_ACTION_COUNT] = DH_HOTKEY_DEFAULTS;

    ASSERT_TRUE(dh_hotkey_table_is_valid(defaults, DH_HOTKEY_ACTION_COUNT));

    /* Independent USB HID wire values make changes to the ten existing chords fail here. */
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_OUTPUT_TOGGLE],
                           DH_HOTKEY_ACTION_OUTPUT_TOGGLE, 0x01, 0x39, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_GAMING_MODE],
                           DH_HOTKEY_ACTION_GAMING_MODE, 0x21, 0x0a, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_SCREENSAVER_PONG],
                           DH_HOTKEY_ACTION_SCREENSAVER_PONG, 0x21, 0x16, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_SCREENSAVER_JITTER],
                           DH_HOTKEY_ACTION_SCREENSAVER_JITTER, 0x21, 0x0d, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_SCREENSAVER_DISABLE],
                           DH_HOTKEY_ACTION_SCREENSAVER_DISABLE, 0x21, 0x1b, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_WIPE_CONFIG],
                           DH_HOTKEY_ACTION_WIPE_CONFIG, 0x20, 0x45, 0x07));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_SCREEN_SEAM],
                           DH_HOTKEY_ACTION_SCREEN_SEAM, 0x20, 0x45, 0x1c));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_CONFIG_ENABLE],
                           DH_HOTKEY_ACTION_CONFIG_ENABLE, 0x21, 0x06, 0x12));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_FW_UPGRADE_A],
                           DH_HOTKEY_ACTION_FW_UPGRADE_A, 0x22, 0x04, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_FW_UPGRADE_B],
                           DH_HOTKEY_ACTION_FW_UPGRADE_B, 0x22, 0x05, 0));

    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_MOUSE_ZOOM],
                           DH_HOTKEY_ACTION_MOUSE_ZOOM, 0x40, 0x10, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_SWITCHLOCK],
                           DH_HOTKEY_ACTION_SWITCHLOCK, 0x40, 0x0e, 0));
    ASSERT_TRUE(binding_is(&defaults[DH_HOTKEY_ACTION_SCREENLOCK],
                           DH_HOTKEY_ACTION_SCREENLOCK, 0x40, 0x0f, 0));

    for (size_t action = 0; action < DH_HOTKEY_ACTION_COUNT; ++action)
        ASSERT_TRUE((defaults[action].modifier & DH_MOD_RIGHT_CTRL) == 0);

    dh_hotkey_t prepared[DH_HOTKEY_ACTION_COUNT] = DH_HOTKEY_DEFAULTS;
    dh_hotkey_prepare(prepared, DH_HOTKEY_ACTION_COUNT);
    for (size_t action = 0; action < DH_HOTKEY_ACTION_COUNT; ++action) {
        const dh_hotkey_t *match = dh_hotkey_match(
            prepared, DH_HOTKEY_ACTION_COUNT, defaults[action].modifier, defaults[action].keys);
        ASSERT_TRUE(match != NULL);
        ASSERT_TRUE(match->action_id == action);
    }

    ASSERT_TRUE(!dh_hotkey_action_passes_to_os(DH_HOTKEY_ACTION_SWITCHLOCK));
    ASSERT_TRUE(!dh_hotkey_action_passes_to_os(DH_HOTKEY_ACTION_SCREENLOCK));
    ASSERT_TRUE(dh_hotkey_action_passes_to_os(DH_HOTKEY_ACTION_MOUSE_ZOOM));
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
    if (all_actions_have_stable_names())
        return 1;
    if (chord_usages_become_a_runtime_binding())
        return 1;
    if (recovery_chord_always_reaches_config_mode())
        return 1;
    if (action_properties_and_complete_table_are_resolved_at_the_keyboard_seam())
        return 1;
    if (default_hotkeys_are_reachable_without_right_ctrl_and_preserve_existing_chords())
        return 1;

    printf("hotkey_test: PASS\n");
    return 0;
}
