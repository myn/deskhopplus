/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */
#include "main.h"

/* Default configuration */
const config_t default_config = {
    .magic_header = CONFIG_MAGIC_HEADER,
    .version = CURRENT_CONFIG_VERSION,
    .output[OUTPUT_A] =
        {
            .number = OUTPUT_A,
            .speed_x = MOUSE_SPEED_A_FACTOR_X,
            .speed_y = MOUSE_SPEED_A_FACTOR_Y,
            .border = {
                .top = 0,
                .bottom = MAX_SCREEN_COORD,
            },
            .screen_count = 1,
            .screen_index = 1,
            .os = OUTPUT_A_OS,
            .pos = RIGHT,
            .screensaver = {
                .mode = SCREENSAVER_A_MODE,
                .only_if_inactive = SCREENSAVER_A_ONLY_IF_INACTIVE,
                .idle_time_us = (uint64_t)SCREENSAVER_A_IDLE_TIME_SEC * 1000000,
                .max_time_us = (uint64_t)SCREENSAVER_A_MAX_TIME_SEC * 1000000,
            }
        },
    .output[OUTPUT_B] =
        {
            .number = OUTPUT_B,
            .speed_x = MOUSE_SPEED_B_FACTOR_X,
            .speed_y = MOUSE_SPEED_B_FACTOR_Y,
            .border = {
                .top = 0,
                .bottom = MAX_SCREEN_COORD,
            },
            .screen_count = 1,
            .screen_index = 1,
            .os = OUTPUT_B_OS,
            .pos = LEFT,
            .screensaver = {
                .mode = SCREENSAVER_B_MODE,
                .only_if_inactive = SCREENSAVER_B_ONLY_IF_INACTIVE,
                .idle_time_us = (uint64_t)SCREENSAVER_B_IDLE_TIME_SEC * 1000000,
                .max_time_us = (uint64_t)SCREENSAVER_B_MAX_TIME_SEC * 1000000,
            }
        },
    .enforce_ports = ENFORCE_PORTS,
    .force_kbd_boot_protocol = ENFORCE_KEYBOARD_BOOT_PROTOCOL,
    .force_mouse_boot_mode = false,
    .enable_acceleration = ENABLE_ACCELERATION,
    .hotkeys = {
        [DH_HOTKEY_ACTION_OUTPUT_TOGGLE] = {.modifier=KEYBOARD_MODIFIER_LEFTCTRL,.keys={HID_KEY_CAPS_LOCK},.key_count=1,.action_id=DH_HOTKEY_ACTION_OUTPUT_TOGGLE},
        [DH_HOTKEY_ACTION_MOUSE_ZOOM] = {.modifier=KEYBOARD_MODIFIER_RIGHTALT|KEYBOARD_MODIFIER_RIGHTCTRL,.action_id=DH_HOTKEY_ACTION_MOUSE_ZOOM},
        [DH_HOTKEY_ACTION_SWITCHLOCK] = {.modifier=KEYBOARD_MODIFIER_RIGHTCTRL,.keys={HID_KEY_K},.key_count=1,.action_id=DH_HOTKEY_ACTION_SWITCHLOCK},
        [DH_HOTKEY_ACTION_SCREENLOCK] = {.modifier=KEYBOARD_MODIFIER_RIGHTCTRL,.keys={HID_KEY_L},.key_count=1,.action_id=DH_HOTKEY_ACTION_SCREENLOCK},
        [DH_HOTKEY_ACTION_GAMING_MODE] = {.modifier=KEYBOARD_MODIFIER_LEFTCTRL|KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_G},.key_count=1,.action_id=DH_HOTKEY_ACTION_GAMING_MODE},
        [DH_HOTKEY_ACTION_SCREENSAVER_PONG] = {.modifier=KEYBOARD_MODIFIER_LEFTCTRL|KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_S},.key_count=1,.action_id=DH_HOTKEY_ACTION_SCREENSAVER_PONG},
        [DH_HOTKEY_ACTION_SCREENSAVER_JITTER] = {.modifier=KEYBOARD_MODIFIER_LEFTCTRL|KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_J},.key_count=1,.action_id=DH_HOTKEY_ACTION_SCREENSAVER_JITTER},
        [DH_HOTKEY_ACTION_SCREENSAVER_DISABLE] = {.modifier=KEYBOARD_MODIFIER_LEFTCTRL|KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_X},.key_count=1,.action_id=DH_HOTKEY_ACTION_SCREENSAVER_DISABLE},
        [DH_HOTKEY_ACTION_WIPE_CONFIG] = {.modifier=KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_F12,HID_KEY_D},.key_count=2,.action_id=DH_HOTKEY_ACTION_WIPE_CONFIG},
        [DH_HOTKEY_ACTION_SCREEN_SEAM] = {.modifier=KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_F12,HID_KEY_Y},.key_count=2,.action_id=DH_HOTKEY_ACTION_SCREEN_SEAM},
        [DH_HOTKEY_ACTION_CONFIG_ENABLE] = {.modifier=KEYBOARD_MODIFIER_LEFTCTRL|KEYBOARD_MODIFIER_RIGHTSHIFT,.keys={HID_KEY_C,HID_KEY_O},.key_count=2,.action_id=DH_HOTKEY_ACTION_CONFIG_ENABLE},
        [DH_HOTKEY_ACTION_FW_UPGRADE_A] = {.modifier=KEYBOARD_MODIFIER_RIGHTSHIFT|KEYBOARD_MODIFIER_LEFTSHIFT,.keys={HID_KEY_A},.key_count=1,.action_id=DH_HOTKEY_ACTION_FW_UPGRADE_A},
        [DH_HOTKEY_ACTION_FW_UPGRADE_B] = {.modifier=KEYBOARD_MODIFIER_RIGHTSHIFT|KEYBOARD_MODIFIER_LEFTSHIFT,.keys={HID_KEY_B},.key_count=1,.action_id=DH_HOTKEY_ACTION_FW_UPGRADE_B},
    },
    .kbd_led_as_indicator = KBD_LED_AS_INDICATOR,
    .jump_threshold = JUMP_THRESHOLD,
};
