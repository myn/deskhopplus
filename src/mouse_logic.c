/* Host-compilable mouse movement and screen-transition seam (#24). */
#include "main.h"
#include <math.h>

#define ACCEL_POINTS 7

static dh_mouse_layout_t mouse_layout_for(const output_t *output) {
    return (dh_mouse_layout_t){
        .chain_direction = (dh_direction_t)output->chain_direction,
        .border_direction = (dh_direction_t)output->border_direction,
    };
}

uint16_t get_jump_threshold(output_t *output, enum screen_pos_e direction) {
    const dh_mouse_layout_t layout = mouse_layout_for(output);
    return dh_mouse_jump_threshold_for(&layout, output->screen_index, output->screen_count,
                                       (dh_direction_t)direction,
                                       global_state.config.jump_threshold);
}

typedef struct {
    enum screen_pos_e direction;
    int overshoot;
} screen_boundary_crossing_t;

static screen_boundary_crossing_t screen_boundary_crossing(
    output_t *output,
    int position,
    int offset,
    enum screen_pos_e negative_direction,
    enum screen_pos_e positive_direction) {
    const enum screen_pos_e direction =
        (offset < 0) ? negative_direction : positive_direction;
    if (offset == 0)
        return (screen_boundary_crossing_t){.direction = NONE};

    const int threshold = get_jump_threshold(output, direction);
    const int next_position = position + offset;

    if (next_position < MIN_SCREEN_COORD - threshold)
        return (screen_boundary_crossing_t){
            .direction = negative_direction,
            .overshoot = MIN_SCREEN_COORD - next_position,
        };
    if (next_position > MAX_SCREEN_COORD + threshold)
        return (screen_boundary_crossing_t){
            .direction = positive_direction,
            .overshoot = next_position - MAX_SCREEN_COORD,
        };
    return (screen_boundary_crossing_t){.direction = NONE};
}

int32_t move_and_keep_on_screen(int position, int offset) {
    if (position + offset < MIN_SCREEN_COORD)
        return MIN_SCREEN_COORD;
    if (position + offset > MAX_SCREEN_COORD)
        return MAX_SCREEN_COORD;
    return position + offset;
}

float calculate_mouse_acceleration_factor(int32_t offset_x, int32_t offset_y) {
    const struct curve {
        int value;
        float factor;
    } acceleration[ACCEL_POINTS] = {
        {2, 1}, {5, 1.1}, {15, 1.4}, {30, 1.9}, {45, 2.6}, {60, 3.4}, {70, 4.0},
    };

    if (offset_x == 0 && offset_y == 0)
        return 1.0;
    if (!global_state.config.enable_acceleration)
        return 1.0;

    const float magnitude = sqrtf((float)(offset_x * offset_x) + (float)(offset_y * offset_y));
    if (magnitude <= acceleration[0].value)
        return acceleration[0].factor;
    if (magnitude >= acceleration[ACCEL_POINTS - 1].value)
        return acceleration[ACCEL_POINTS - 1].factor;

    for (int i = 0; i < ACCEL_POINTS - 1; i++) {
        if (magnitude < acceleration[i + 1].value) {
            const struct curve *lower = &acceleration[i];
            const struct curve *upper = &acceleration[i + 1];
            const float interpolation =
                (magnitude - lower->value) / (upper->value - lower->value);
            return lower->factor + interpolation * (upper->factor - lower->factor);
        }
    }
    return 1.0;
}

enum screen_pos_e update_mouse_position(device_t *state, mouse_values_t *values) {
    output_t *current = &state->config.output[state->active_output];
    uint8_t reduce_speed = state->mouse_zoom ? MOUSE_ZOOM_SCALING_FACTOR : 0;
    float acceleration = calculate_mouse_acceleration_factor(values->move_x, values->move_y);
    int offset_x = round(values->move_x * acceleration * (current->speed_x >> reduce_speed));
    int offset_y = round(values->move_y * acceleration * (current->speed_y >> reduce_speed));
    const screen_boundary_crossing_t horizontal =
        screen_boundary_crossing(current, state->pointer_x, offset_x, LEFT, RIGHT);
    const screen_boundary_crossing_t vertical =
        screen_boundary_crossing(current, state->pointer_y, offset_y, TOP, BOTTOM);
    const enum screen_pos_e direction =
        vertical.overshoot > horizontal.overshoot ? vertical.direction : horizontal.direction;

    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);
    state->mouse_buttons = values->buttons;
    return direction;
}

void do_screen_switch(device_t *state, int direction) {
    output_t *output = &state->config.output[state->active_output];
    if (state->switch_lock || state->gaming_mode)
        return;

    const dh_mouse_layout_t layout = mouse_layout_for(output);
    dh_mouse_transition_t transition =
        dh_mouse_transition_for(&layout, output->screen_index, output->screen_count,
                                (dh_direction_t)direction);
    switch (transition) {
        case DH_MOUSE_TRANSITION_OUTPUT:
            if (!state->mouse_buttons) {
                output_t *target = &state->config.output[1 - state->active_output];
                const int along = dh_mouse_along_seam(
                    (dh_direction_t)direction,
                    (dh_mouse_coordinates_t){.x = state->pointer_x, .y = state->pointer_y});
                const uint16_t normalized = (uint16_t)(
                    ((uint32_t)along * DH_SEAM_POSITION_MAX + MAX_SCREEN_COORD / 2) /
                    MAX_SCREEN_COORD);
                dh_seam_entry_t mapped_entry;
                dh_seam_crossing_kind_t crossing = dh_seam_resolve_crossing(
                    output->seam_ranges, target->seam_ranges, output->screen_index,
                    output->screen_count, target->screen_count, normalized,
                    &mapped_entry);
                if (crossing == DH_SEAM_CROSSING_BLOCKED)
                    break;
                switch_to_another_pc(state, output, 1 - state->active_output, direction);
                if (crossing == DH_SEAM_CROSSING_MAPPED) {
                    const int entry = (int)(((uint32_t)mapped_entry.position * MAX_SCREEN_COORD +
                                             DH_SEAM_POSITION_MAX / 2) /
                                            DH_SEAM_POSITION_MAX);
                    if (dh_direction_is_vertical((dh_direction_t)direction))
                        state->pointer_x = (int16_t)entry;
                    else
                        state->pointer_y = (int16_t)entry;
                    target->screen_index = mapped_entry.screen_index;
                    channel_place_cursor((uint8_t)target->number,
                                         mapped_entry.screen_index,
                                         target->chain_direction,
                                         target->border_direction,
                                         mapped_entry.position);
                }
            }
            break;
        case DH_MOUSE_TRANSITION_CHAIN_BACK:
        case DH_MOUSE_TRANSITION_CHAIN_FORWARD:
            switch_virtual_desktop(state, output,
                                  dh_mouse_next_screen_index(transition, output->screen_index),
                                  direction);
            break;
        case DH_MOUSE_TRANSITION_NONE:
            break;
    }
}
