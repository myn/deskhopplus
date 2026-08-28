/*
 * deskhopplus — SDK-free hotkey matching.
 */
#include "dh_hotkey.h"

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
