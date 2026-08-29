#include "dh_mouse_layout.h"

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
    if (movement == layout->border_direction && screen_index == 1)
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
