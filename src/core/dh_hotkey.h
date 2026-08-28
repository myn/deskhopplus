/*
 * deskhopplus — SDK-free hotkey matching.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_hotkey_actions.h"

#define DH_HOTKEY_KEY_CAPACITY 6u

enum dh_hotkey_action_id {
#define DH_HOTKEY_ACTION(symbol, name) DH_HOTKEY_ACTION_##symbol,
    DH_HOTKEY_ACTIONS(DH_HOTKEY_ACTION)
#undef DH_HOTKEY_ACTION
    DH_HOTKEY_ACTION_COUNT,
    DH_HOTKEY_ACTION_INVALID = 0xff
};

typedef struct {
    uint8_t modifier;
    uint8_t keys[DH_HOTKEY_KEY_CAPACITY];
    uint8_t key_count;
    uint8_t action_id;
} dh_hotkey_t;

const char *dh_hotkey_action_name(uint8_t action_id);
uint8_t dh_hotkey_action_id(const char *name);
bool dh_hotkey_binding_from_usages(dh_hotkey_t *binding, uint8_t action_id,
                                   const uint8_t *usages, size_t usage_count);
bool dh_hotkey_table_is_valid(const dh_hotkey_t *hotkeys, size_t count);
bool dh_hotkey_action_passes_to_os(uint8_t action_id);
bool dh_hotkey_action_acknowledges(uint8_t action_id);

typedef struct {
    bool matched;
    uint8_t action_id;
    bool pass_to_os;
    bool acknowledge;
} dh_keyboard_hotkey_result_t;

dh_keyboard_hotkey_result_t dh_keyboard_hotkey_resolve(
    const dh_hotkey_t *hotkeys, size_t count, uint8_t modifier,
    const uint8_t keys[DH_HOTKEY_KEY_CAPACITY]);

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
const dh_hotkey_t *dh_hotkey_match_with_recovery(
    const dh_hotkey_t *hotkeys, size_t count, uint8_t modifier,
    const uint8_t keys[DH_HOTKEY_KEY_CAPACITY]);
