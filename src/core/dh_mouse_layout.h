/* Pure mouse-layout decisions shared by firmware and host tests (#24). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DH_DIRECTION_NONE = 0,
    DH_DIRECTION_LEFT = 1,
    DH_DIRECTION_RIGHT = 2,
    DH_DIRECTION_TOP = 4,
    DH_DIRECTION_BOTTOM = 5,
} dh_direction_t;

typedef struct {
    dh_direction_t chain_direction;
    dh_direction_t border_direction;
} dh_mouse_layout_t;

typedef struct {
    int32_t x;
    int32_t y;
} dh_mouse_coordinates_t;

typedef enum {
    DH_MOUSE_TRANSITION_NONE = 0,
    DH_MOUSE_TRANSITION_OUTPUT,
    DH_MOUSE_TRANSITION_CHAIN_BACK,
    DH_MOUSE_TRANSITION_CHAIN_FORWARD,
} dh_mouse_transition_t;

dh_direction_t dh_opposite_direction(dh_direction_t direction);
bool dh_direction_is_vertical(dh_direction_t direction);

dh_mouse_transition_t dh_mouse_transition_for(
    const dh_mouse_layout_t *layout,
    uint32_t screen_index,
    uint32_t screen_count,
    dh_direction_t movement);

uint16_t dh_mouse_jump_threshold_for(
    const dh_mouse_layout_t *layout,
    uint32_t screen_index,
    uint32_t screen_count,
    dh_direction_t movement,
    uint16_t configured_threshold);

uint32_t dh_mouse_next_screen_index(dh_mouse_transition_t transition, uint32_t screen_index);
int32_t dh_mouse_park_coordinate(
    uint8_t park_position, int32_t previous, int32_t minimum, int32_t maximum);
dh_mouse_coordinates_t dh_mouse_hidden_coordinates(
    dh_direction_t direction,
    uint8_t park_position,
    dh_mouse_coordinates_t pointer,
    int32_t minimum,
    int32_t maximum);
dh_mouse_coordinates_t dh_mouse_entry_coordinates(
    dh_direction_t direction,
    dh_mouse_coordinates_t pointer,
    int32_t minimum,
    int32_t maximum);
dh_mouse_coordinates_t dh_mouse_edge_coordinates(
    dh_direction_t direction,
    dh_mouse_coordinates_t pointer,
    int32_t minimum,
    int32_t maximum);
dh_mouse_coordinates_t dh_mouse_nudge(dh_direction_t direction, int32_t distance);
