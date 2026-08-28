/*
 * deskhopplus — SDK-free hotkey matching.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DH_HOTKEY_KEY_CAPACITY 6u

typedef struct {
    uint8_t modifier;
    uint8_t keys[DH_HOTKEY_KEY_CAPACITY];
    uint8_t key_count;
    uint8_t action_id;
} dh_hotkey_t;

bool dh_hotkey_configure_key(dh_hotkey_t *hotkeys,
                             size_t count,
                             uint8_t action_id,
                             uint8_t configured_key,
                             uint8_t fallback_key);

void dh_hotkey_prepare(dh_hotkey_t *hotkeys, size_t count);

const dh_hotkey_t *dh_hotkey_match(const dh_hotkey_t *hotkeys,
                                   size_t count,
                                   uint8_t modifier,
                                   const uint8_t keys[DH_HOTKEY_KEY_CAPACITY]);
