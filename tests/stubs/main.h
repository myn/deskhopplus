#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dh_mouse_layout.h"
#include "screen.h"

#define NUM_SCREENS 2
#define MIN_SCREEN_COORD 0
#define MAX_SCREEN_COORD 32767
#define MOUSE_ZOOM_SCALING_FACTOR 2

enum screen_pos_e {
    NONE = DH_DIRECTION_NONE,
    LEFT = DH_DIRECTION_LEFT,
    RIGHT = DH_DIRECTION_RIGHT,
    TOP = DH_DIRECTION_TOP,
    BOTTOM = DH_DIRECTION_BOTTOM,
};

typedef struct {
    int32_t move_x;
    int32_t move_y;
    int32_t wheel;
    int32_t pan;
    int32_t buttons;
} mouse_values_t;

typedef struct {
    struct {
        uint16_t jump_threshold;
        uint8_t enable_acceleration;
        output_t output[NUM_SCREENS];
    } config;
    uint8_t active_output;
    int16_t pointer_x;
    int16_t pointer_y;
    int16_t mouse_buttons;
    bool mouse_zoom;
    bool switch_lock;
    bool gaming_mode;
} device_t;

extern device_t global_state;

void switch_to_another_pc(device_t *, output_t *, int, int);
void switch_virtual_desktop(device_t *, output_t *, int, int);
