#include "dh_mouse_layout.h"

static bool directions_are_perpendicular(dh_direction_t first, dh_direction_t second) {
    const bool first_is_horizontal =
        first == DH_DIRECTION_LEFT || first == DH_DIRECTION_RIGHT;
    const bool second_is_horizontal =
        second == DH_DIRECTION_LEFT || second == DH_DIRECTION_RIGHT;
    return first_is_horizontal != second_is_horizontal;
}

bool dh_direction_is_vertical(dh_direction_t direction) {
    return direction == DH_DIRECTION_TOP || direction == DH_DIRECTION_BOTTOM;
}

dh_direction_t dh_opposite_direction(dh_direction_t direction) {
    switch (direction) {
        case DH_DIRECTION_LEFT: return DH_DIRECTION_RIGHT;
        case DH_DIRECTION_RIGHT: return DH_DIRECTION_LEFT;
        case DH_DIRECTION_TOP: return DH_DIRECTION_BOTTOM;
        case DH_DIRECTION_BOTTOM: return DH_DIRECTION_TOP;
        case DH_DIRECTION_NONE: return DH_DIRECTION_NONE;
    }
    return DH_DIRECTION_NONE;
}

dh_mouse_transition_t dh_mouse_transition_for(
    const dh_mouse_layout_t *layout,
    uint32_t screen_index,
    uint32_t screen_count,
    dh_direction_t movement) {
    if (movement == layout->border_direction &&
        (screen_index == 1 ||
         directions_are_perpendicular(layout->chain_direction, layout->border_direction)))
        return DH_MOUSE_TRANSITION_OUTPUT;

    if (movement == layout->chain_direction && screen_index < screen_count)
        return DH_MOUSE_TRANSITION_CHAIN_FORWARD;

    if (movement == dh_opposite_direction(layout->chain_direction) && screen_index > 1)
        return DH_MOUSE_TRANSITION_CHAIN_BACK;

    return DH_MOUSE_TRANSITION_NONE;
}

uint16_t dh_mouse_jump_threshold_for(
    const dh_mouse_layout_t *layout,
    uint32_t screen_index,
    uint32_t screen_count,
    dh_direction_t movement,
    uint16_t configured_threshold) {
    return dh_mouse_transition_for(layout, screen_index, screen_count, movement) ==
                   DH_MOUSE_TRANSITION_OUTPUT
               ? configured_threshold
               : 0;
}

uint32_t dh_mouse_next_screen_index(dh_mouse_transition_t transition, uint32_t screen_index) {
    if (transition == DH_MOUSE_TRANSITION_CHAIN_FORWARD)
        return screen_index + 1;
    if (transition == DH_MOUSE_TRANSITION_CHAIN_BACK)
        return screen_index - 1;
    return screen_index;
}

int32_t dh_mouse_park_coordinate(
    uint8_t park_position, int32_t previous, int32_t minimum, int32_t maximum) {
    if (park_position == 0)
        return minimum;
    if (park_position == 1)
        return maximum;
    return previous;
}

dh_mouse_coordinates_t dh_mouse_hidden_coordinates(
    dh_direction_t direction,
    uint8_t park_position,
    dh_mouse_coordinates_t pointer,
    int32_t minimum,
    int32_t maximum) {
    if (dh_direction_is_vertical(direction))
        return (dh_mouse_coordinates_t){
            .x = dh_mouse_park_coordinate(park_position, pointer.x, minimum, maximum),
            .y = maximum,
        };
    return (dh_mouse_coordinates_t){
        .x = maximum,
        .y = dh_mouse_park_coordinate(park_position, pointer.y, minimum, maximum),
    };
}

dh_mouse_coordinates_t dh_mouse_entry_coordinates(
    dh_direction_t direction,
    dh_mouse_coordinates_t pointer,
    int32_t minimum,
    int32_t maximum) {
    dh_mouse_coordinates_t coordinates = pointer;
    if (direction == DH_DIRECTION_LEFT)
        coordinates.x = maximum;
    else if (direction == DH_DIRECTION_RIGHT)
        coordinates.x = minimum;
    else if (direction == DH_DIRECTION_TOP)
        coordinates.y = maximum;
    else if (direction == DH_DIRECTION_BOTTOM)
        coordinates.y = minimum;
    return coordinates;
}

dh_mouse_coordinates_t dh_mouse_edge_coordinates(
    dh_direction_t direction,
    dh_mouse_coordinates_t pointer,
    int32_t minimum,
    int32_t maximum) {
    return dh_mouse_entry_coordinates(
        dh_opposite_direction(direction), pointer, minimum, maximum);
}

dh_mouse_coordinates_t dh_mouse_nudge(dh_direction_t direction, int32_t distance) {
    dh_mouse_coordinates_t movement = {0};
    const int32_t signed_distance =
        (direction == DH_DIRECTION_LEFT || direction == DH_DIRECTION_TOP) ? -distance : distance;
    if (dh_direction_is_vertical(direction))
        movement.y = signed_distance;
    else
        movement.x = signed_distance;
    return movement;
}
