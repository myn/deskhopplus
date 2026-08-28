/*
 * deskhopplus — SDK-free hotkey matching.
 */
#include "dh_hotkey.h"

#include <string.h>

static const char *const action_names[DH_HOTKEY_ACTION_COUNT] = {
#define DH_HOTKEY_ACTION(symbol, name) #name,
    DH_HOTKEY_ACTIONS(DH_HOTKEY_ACTION)
#undef DH_HOTKEY_ACTION
};

const char *dh_hotkey_action_name(uint8_t action_id) {
    return action_id < DH_HOTKEY_ACTION_COUNT ? action_names[action_id] : NULL;
}

bool dh_hotkey_table_is_valid(const dh_hotkey_t *hotkeys, size_t count) {
    if (!hotkeys || count != DH_HOTKEY_ACTION_COUNT)
        return false;
    uint16_t seen = 0;
    for (size_t i = 0; i < count; ++i) {
        const dh_hotkey_t *binding = &hotkeys[i];
        if (binding->action_id >= DH_HOTKEY_ACTION_COUNT ||
            binding->key_count > DH_HOTKEY_KEY_CAPACITY ||
            (binding->modifier == 0 && binding->key_count == 0) ||
            (seen & (1u << binding->action_id)))
            return false;
        for (size_t key = 0; key < binding->key_count; ++key)
            if (binding->keys[key] == 0)
                return false;
        seen |= (uint16_t)(1u << binding->action_id);
    }
    return seen == (uint16_t)((1u << DH_HOTKEY_ACTION_COUNT) - 1u);
}

bool dh_hotkey_action_passes_to_os(uint8_t action_id) {
    return action_id == DH_HOTKEY_ACTION_MOUSE_ZOOM;
}

bool dh_hotkey_action_acknowledges(uint8_t action_id) {
    return action_id < DH_HOTKEY_ACTION_COUNT && action_id != DH_HOTKEY_ACTION_OUTPUT_TOGGLE;
}

uint8_t dh_hotkey_action_id(const char *name) {
    if (!name)
        return DH_HOTKEY_ACTION_INVALID;
    for (uint8_t i = 0; i < DH_HOTKEY_ACTION_COUNT; ++i)
        if (strcmp(name, action_names[i]) == 0)
            return i;
    return DH_HOTKEY_ACTION_INVALID;
}

bool dh_hotkey_binding_from_usages(dh_hotkey_t *binding, uint8_t action_id,
                                   const uint8_t *usages, size_t usage_count) {
    if (!binding || !usages || action_id >= DH_HOTKEY_ACTION_COUNT || usage_count == 0 ||
        usage_count > DH_HOTKEY_KEY_CAPACITY)
        return false;
    dh_hotkey_t parsed = {.action_id = action_id};
    for (size_t i = 0; i < usage_count; ++i) {
        if (usages[i] >= 0xe0 && usages[i] <= 0xe7) {
            parsed.modifier |= (uint8_t)(1u << (usages[i] - 0xe0));
        } else {
            if (parsed.key_count >= DH_HOTKEY_KEY_CAPACITY || usages[i] == 0)
                return false;
            parsed.keys[parsed.key_count++] = usages[i];
        }
    }
    *binding = parsed;
    return true;
}

static unsigned modifier_count(uint8_t modifier) {
    unsigned count = 0;

    while (modifier != 0) {
        count += modifier & 1u;
        modifier >>= 1;
    }

    return count;
}

static unsigned specificity(const dh_hotkey_t *hotkey) {
    return modifier_count(hotkey->modifier) + hotkey->key_count;
}

bool dh_hotkey_configure_key(dh_hotkey_t *hotkeys,
                             size_t count,
                             uint8_t action_id,
                             uint8_t configured_key,
                             uint8_t fallback_key) {
    uint8_t key = configured_key != 0 ? configured_key : fallback_key;

    if (key == 0)
        return false;

    for (size_t i = 0; i < count; ++i) {
        if (hotkeys[i].action_id == action_id && hotkeys[i].key_count > 0 &&
            hotkeys[i].key_count <= DH_HOTKEY_KEY_CAPACITY) {
            hotkeys[i].keys[0] = key;
            return true;
        }
    }

    return false;
}

void dh_hotkey_prepare(dh_hotkey_t *hotkeys, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        dh_hotkey_t candidate = hotkeys[i];
        size_t position = i;

        while (position > 0 && specificity(&hotkeys[position - 1]) < specificity(&candidate)) {
            hotkeys[position] = hotkeys[position - 1];
            --position;
        }

        hotkeys[position] = candidate;
    }
}

static bool contains_key(const uint8_t keys[DH_HOTKEY_KEY_CAPACITY], uint8_t wanted) {
    if (wanted == 0)
        return false;

    for (size_t i = 0; i < DH_HOTKEY_KEY_CAPACITY; ++i) {
        if (keys[i] == wanted)
            return true;
    }

    return false;
}

const dh_hotkey_t *dh_hotkey_match(const dh_hotkey_t *hotkeys,
                                   size_t count,
                                   uint8_t modifier,
                                   const uint8_t keys[DH_HOTKEY_KEY_CAPACITY]) {
    for (size_t i = 0; i < count; ++i) {
        const dh_hotkey_t *hotkey = &hotkeys[i];

        if (hotkey->key_count > DH_HOTKEY_KEY_CAPACITY ||
            hotkey->modifier != (modifier & hotkey->modifier))
            continue;

        size_t key_index = 0;
        while (key_index < hotkey->key_count && contains_key(keys, hotkey->keys[key_index]))
            ++key_index;

        if (key_index == hotkey->key_count)
            return hotkey;
    }

    return NULL;
}

const dh_hotkey_t *dh_hotkey_match_with_recovery(
    const dh_hotkey_t *hotkeys, size_t count, uint8_t modifier,
    const uint8_t keys[DH_HOTKEY_KEY_CAPACITY]) {
    static const dh_hotkey_t recovery = {
        .modifier = 0x21, .keys = {0x06, 0x12}, .key_count = 2,
        .action_id = DH_HOTKEY_ACTION_CONFIG_ENABLE};
    if (dh_hotkey_match(&recovery, 1, modifier, keys))
        return &recovery;
    return dh_hotkey_match(hotkeys, count, modifier, keys);
}

dh_keyboard_hotkey_result_t dh_keyboard_hotkey_resolve(
    const dh_hotkey_t *hotkeys, size_t count, uint8_t modifier,
    const uint8_t keys[DH_HOTKEY_KEY_CAPACITY]) {
    const dh_hotkey_t *match = dh_hotkey_match_with_recovery(hotkeys, count, modifier, keys);
    if (!match || match->action_id >= DH_HOTKEY_ACTION_COUNT)
        return (dh_keyboard_hotkey_result_t){0};
    return (dh_keyboard_hotkey_result_t){
        .matched = true,
        .action_id = match->action_id,
        .pass_to_os = dh_hotkey_action_passes_to_os(match->action_id),
        .acknowledge = dh_hotkey_action_acknowledges(match->action_id),
    };
}
