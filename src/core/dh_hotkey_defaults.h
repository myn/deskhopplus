/*
 * deskhopplus — default hotkey table, shared by firmware and host tests.
 */
#pragma once

#include "dh_hotkey.h"

enum dh_default_hotkey_usage {
    DH_KEY_A = 0x04,
    DH_KEY_B = 0x05,
    DH_KEY_C = 0x06,
    DH_KEY_D = 0x07,
    DH_KEY_G = 0x0a,
    DH_KEY_J = 0x0d,
    DH_KEY_K = 0x0e,
    DH_KEY_L = 0x0f,
    DH_KEY_M = 0x10,
    DH_KEY_O = 0x12,
    DH_KEY_S = 0x16,
    DH_KEY_X = 0x1b,
    DH_KEY_Y = 0x1c,
    DH_KEY_CAPS_LOCK = 0x39,
    DH_KEY_F12 = 0x45,
};

enum dh_default_hotkey_modifier {
    DH_MOD_LEFT_CTRL = 0x01,
    DH_MOD_LEFT_SHIFT = 0x02,
    DH_MOD_RIGHT_CTRL = 0x10,
    DH_MOD_RIGHT_SHIFT = 0x20,
    DH_MOD_RIGHT_ALT = 0x40,
};

#define DH_HOTKEY_DEFAULTS                                                                    \
    {                                                                                         \
        [DH_HOTKEY_ACTION_OUTPUT_TOGGLE] =                                                    \
            {.modifier = DH_MOD_LEFT_CTRL, .keys = {DH_KEY_CAPS_LOCK}, .key_count = 1,         \
             .action_id = DH_HOTKEY_ACTION_OUTPUT_TOGGLE},                                    \
        [DH_HOTKEY_ACTION_MOUSE_ZOOM] =                                                       \
            {.modifier = DH_MOD_RIGHT_ALT, .keys = {DH_KEY_M}, .key_count = 1,                \
             .action_id = DH_HOTKEY_ACTION_MOUSE_ZOOM},                                       \
        [DH_HOTKEY_ACTION_SWITCHLOCK] =                                                       \
            {.modifier = DH_MOD_RIGHT_ALT, .keys = {DH_KEY_K}, .key_count = 1,                \
             .action_id = DH_HOTKEY_ACTION_SWITCHLOCK},                                       \
        [DH_HOTKEY_ACTION_SCREENLOCK] =                                                       \
            {.modifier = DH_MOD_RIGHT_ALT, .keys = {DH_KEY_L}, .key_count = 1,                \
             .action_id = DH_HOTKEY_ACTION_SCREENLOCK},                                       \
        [DH_HOTKEY_ACTION_GAMING_MODE] =                                                      \
            {.modifier = DH_MOD_LEFT_CTRL | DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_G},            \
             .key_count = 1,                                                                   \
             .action_id = DH_HOTKEY_ACTION_GAMING_MODE},                                      \
        [DH_HOTKEY_ACTION_SCREENSAVER_PONG] =                                                 \
            {.modifier = DH_MOD_LEFT_CTRL | DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_S},            \
             .key_count = 1,                                                                   \
             .action_id = DH_HOTKEY_ACTION_SCREENSAVER_PONG},                                 \
        [DH_HOTKEY_ACTION_SCREENSAVER_JITTER] =                                               \
            {.modifier = DH_MOD_LEFT_CTRL | DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_J},            \
             .key_count = 1,                                                                   \
             .action_id = DH_HOTKEY_ACTION_SCREENSAVER_JITTER},                               \
        [DH_HOTKEY_ACTION_SCREENSAVER_DISABLE] =                                              \
            {.modifier = DH_MOD_LEFT_CTRL | DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_X},            \
             .key_count = 1,                                                                   \
             .action_id = DH_HOTKEY_ACTION_SCREENSAVER_DISABLE},                              \
        [DH_HOTKEY_ACTION_WIPE_CONFIG] =                                                      \
            {.modifier = DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_F12, DH_KEY_D}, .key_count = 2,   \
             .action_id = DH_HOTKEY_ACTION_WIPE_CONFIG},                                      \
        [DH_HOTKEY_ACTION_SCREEN_SEAM] =                                                      \
            {.modifier = DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_F12, DH_KEY_Y}, .key_count = 2,   \
             .action_id = DH_HOTKEY_ACTION_SCREEN_SEAM},                                      \
        [DH_HOTKEY_ACTION_CONFIG_ENABLE] =                                                    \
            {.modifier = DH_MOD_LEFT_CTRL | DH_MOD_RIGHT_SHIFT,                               \
             .keys = {DH_KEY_C, DH_KEY_O}, .key_count = 2,                                    \
             .action_id = DH_HOTKEY_ACTION_CONFIG_ENABLE},                                    \
        [DH_HOTKEY_ACTION_FW_UPGRADE_A] =                                                     \
            {.modifier = DH_MOD_LEFT_SHIFT | DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_A},           \
             .key_count = 1,                                                                   \
             .action_id = DH_HOTKEY_ACTION_FW_UPGRADE_A},                                     \
        [DH_HOTKEY_ACTION_FW_UPGRADE_B] =                                                     \
            {.modifier = DH_MOD_LEFT_SHIFT | DH_MOD_RIGHT_SHIFT, .keys = {DH_KEY_B},           \
             .key_count = 1,                                                                   \
             .action_id = DH_HOTKEY_ACTION_FW_UPGRADE_B},                                     \
    }
