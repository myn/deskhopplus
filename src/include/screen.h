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
#pragma once

#include <stdint.h>

#include "dh_keymap.h"


/*==============================================================================
 *  Constants
 *==============================================================================*/

#define MAX_SCREEN_COORD 32767
#define MIN_SCREEN_COORD 0

enum os_type_e {
    LINUX   = 1,
    MACOS   = 2,
    WINDOWS = 3,
    ANDROID = 4,
    OTHER   = 255,
};

#define DEFAULT_CTRL_GUI_SWAP(os) ((os) == MACOS)

/*==============================================================================
 *  Data Structures
 *==============================================================================*/

typedef struct {
    int top;    // When jumping from a smaller to a bigger screen, go to THIS top height
    int bottom; // When jumping from a smaller to a bigger screen, go to THIS bottom
                // height
} border_size_t;

typedef struct {
    uint8_t mode;
    uint8_t only_if_inactive;
    uint64_t idle_time_us;
    uint64_t max_time_us;
} screensaver_t;

typedef struct {
    uint32_t number;           // Number of this output (e.g. OUTPUT_A = 0 etc)
    uint32_t screen_count;     // How many monitors per output (e.g. Output A is Windows with 3 monitors)
    uint32_t screen_index;     // Current active screen
    int32_t speed_x;           // Mouse speed per output, in direction X
    int32_t speed_y;           // Mouse speed per output, in direction Y
    border_size_t border;      // Screen border size/offset to keep cursor at same height when switching
    uint8_t os;                // Operating system on this output
    uint8_t pos;               // Screen position on this output
    uint8_t mouse_park_pos;    // Where the mouse goes after switch
    uint8_t swap_ctrl_gui;     // Emit physical Ctrl as GUI and GUI as Ctrl
    dh_keymap_profile_t keymap; // Physical-key transform for this output
    screensaver_t screensaver; // Screensaver parameters for this output
} output_t;
