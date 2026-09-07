/* Cursor crossing owner: movement, helper readback and switching completion.
 * Hardware adapters provide the lock, clock, channel and output effects.
 * Core 1 drives movement/completion; core 0 publishes readback under the lock.
 */
#include "main.h"
#include <math.h>

#define MACOS_SWITCH_MOVE_X 10
#define MACOS_SWITCH_MOVE_COUNT 5

static bool position_is_at_pending_edge(const device_t *state, int16_t x, int16_t y) {
    const int threshold = state->config.jump_threshold;
    switch (state->cursor_crossing.direction) {
        case LEFT: return x <= MIN_SCREEN_COORD + threshold;
        case RIGHT: return x >= MAX_SCREEN_COORD - threshold;
        case TOP: return y <= MIN_SCREEN_COORD + threshold;
        case BOTTOM: return y >= MAX_SCREEN_COORD - threshold;
        default: return false;
    }
}

static bool position_confirms_pending_placement(const device_t *state, int16_t x, int16_t y) {
    const cursor_crossing_t *crossing = &state->cursor_crossing;
    const int expected_along = (int)(
        ((uint32_t)crossing->target_position * MAX_SCREEN_COORD +
         DH_SEAM_POSITION_MAX / 2) / DH_SEAM_POSITION_MAX);
    const int actual_along = dh_direction_is_vertical((dh_direction_t)crossing->direction)
                                 ? x
                                 : y;
    if (actual_along < expected_along - 2 || actual_along > expected_along + 2)
        return false;
    const int threshold = state->config.jump_threshold;
    switch (crossing->direction) {
        case LEFT: return x >= MAX_SCREEN_COORD - threshold;
        case RIGHT: return x <= MIN_SCREEN_COORD + threshold;
        case TOP: return y >= MAX_SCREEN_COORD - threshold;
        case BOTTOM: return y <= MIN_SCREEN_COORD + threshold;
        default: return false;
    }
}

static bool select_cursor_screen(device_t *state, uint8_t output, uint8_t screen) {
    if (output > OUTPUT_B || output != state->active_output || screen == 0 ||
        screen > state->config.output[output].screen_count)
        return false;
    state->config.output[output].screen_index = screen;
    const uint8_t os = state->config.output[output].os;
    state->relative_mouse = os == WINDOWS && screen > 1;
    return true;
}

bool apply_helper_cursor_position(device_t *state, uint8_t output, uint8_t screen,
                                  int16_t x, int16_t y, uint8_t query_id) {
    if (output > OUTPUT_B)
        return false;
    cursor_crossing_enter();
    cursor_crossing_t *crossing = &state->cursor_crossing;
    const cursor_crossing_phase_t phase = crossing->phase;
    if ((phase == CURSOR_CROSSING_WAITING &&
         (query_id == 0 || crossing->output != output || crossing->query_id != query_id)) ||
        (query_id != 0 && phase != CURSOR_CROSSING_WAITING)) {
        cursor_crossing_exit();
        return false;
    }
    if (query_id != 0 && crossing->kind == CURSOR_CROSSING_MACOS_PLACEMENT &&
        (screen != crossing->target_screen ||
         !position_confirms_pending_placement(state, x, y))) {
        cursor_crossing_exit();
        return false;
    }
    /* q=0 is the immediate readback of a placement whose target screen was
       already selected by firmware. At an internal seam, continued fast
       motion (or an asynchronous OS observation) can report the neighbouring
       screen before this uncorrelated readback arrives. Accepting that screen
       rewinds screen_index and makes the next chain crossing start from the
       wrong monitor (#28). Correlated re-anchor queries remain authoritative
       because their purpose is to repair a relative-source estimate. */
    if (query_id == 0 && screen != state->config.output[output].screen_index) {
        cursor_crossing_exit();
        return false;
    }
    if (!select_cursor_screen(state, output, screen)) {
        cursor_crossing_exit();
        return false;
    }
    state->pointer_x = x;
    state->pointer_y = y;
    const uint8_t direction = crossing->direction;
    if (crossing->phase == CURSOR_CROSSING_WAITING && crossing->output == output) {
        if (crossing->kind == CURSOR_CROSSING_MACOS_PLACEMENT ||
            (crossing->kind == CURSOR_CROSSING_SOURCE_REANCHOR &&
             position_is_at_pending_edge(state, x, y))) {
            crossing->phase = CURSOR_CROSSING_REANCHORED;
        } else {
            /* Core 1 owns the metadata and performs the full clear. Core 0
               publishes only the terminal result of this query. */
            crossing->phase = CURSOR_CROSSING_CANCELLED;
        }
    }
    cursor_crossing_exit();
    cursor_trace_event(state, DH_CURSOR_TRACE_RESPONSE, query_id, x, y, direction,
                       DH_MOUSE_TRANSITION_OUTPUT);
    return true;
}

/* Map the coordinate along the seam between the two legacy output ranges. */
static int16_t map_output_seam_coordinate(int pointer, int screen_from, int screen_to, device_t *state) {
    output_t *from = &state->config.output[screen_from];
    output_t *to   = &state->config.output[screen_to];
    return (int16_t)dh_seam_map_coordinate(pointer, from->border.start, from->border.end,
                                           to->border.start, to->border.end);
}

static void switch_to_another_pc(
    device_t *state, output_t *output, int output_to, int direction) {
    uint8_t *mouse_park_pos = &state->config.output[state->active_output].mouse_park_pos;
    const bool vertical = dh_direction_is_vertical((dh_direction_t)direction);
    const dh_mouse_coordinates_t pointer = {.x = state->pointer_x, .y = state->pointer_y};
    const dh_mouse_coordinates_t hidden = dh_mouse_hidden_coordinates(
        (dh_direction_t)direction,
        *mouse_park_pos,
        pointer,
        MIN_SCREEN_COORD,
        MAX_SCREEN_COORD);
    mouse_report_t hidden_pointer = {
        .x = (int16_t)hidden.x,
        .y = (int16_t)hidden.y,
    };

    output_mouse_report(&hidden_pointer, state);
    set_active_output(state, output_to);
    const dh_mouse_coordinates_t entry = dh_mouse_entry_coordinates(
        (dh_direction_t)direction,
        pointer,
        MIN_SCREEN_COORD,
        MAX_SCREEN_COORD);
    state->pointer_x = (int16_t)entry.x;
    state->pointer_y = (int16_t)entry.y;
    if (vertical)
        state->pointer_x = map_output_seam_coordinate(
            state->pointer_x, output->number, 1 - output->number, state);
    else
        state->pointer_y = map_output_seam_coordinate(
            state->pointer_y, output->number, 1 - output->number, state);
}

static void switch_virtual_desktop_macos(device_t *state, int direction) {
    /*
     * Fix for MACOS: Before sending new absolute report setting X to 0:
     * 1. Move the cursor to the edge of the screen directly in the middle to handle screens
     *    of different heights
     * 2. Send relative mouse movement one or two pixels in the direction of movement to get
     *    the cursor onto the next screen
     */
    const dh_mouse_coordinates_t edge = dh_mouse_edge_coordinates(
        (dh_direction_t)direction,
        (dh_mouse_coordinates_t){.x = state->pointer_x, .y = state->pointer_y},
        MIN_SCREEN_COORD,
        MAX_SCREEN_COORD);
    mouse_report_t edge_position = {
        .x = (int16_t)edge.x,
        .y = (int16_t)edge.y,
        .mode = ABSOLUTE,
        .buttons = state->mouse_buttons,
    };

    const dh_mouse_coordinates_t nudge =
        dh_mouse_nudge((dh_direction_t)direction, MACOS_SWITCH_MOVE_X);
    mouse_report_t move_relative_one = {
        .x = (int16_t)nudge.x,
        .y = (int16_t)nudge.y,
        .mode = RELATIVE,
        /* Force buttons to 0 for relative movement to avoid duplicating the button
           press state, which would leave the relative HID mouse permanently stuck
           down if the user is dragging an item while switching desktops. */
        .buttons = 0,
    };

    output_mouse_report(&edge_position, state);

    /* Once doesn't seem reliable enough, do it a few times */
    for (int i = 0; i < MACOS_SWITCH_MOVE_COUNT; i++)
        output_mouse_report(&move_relative_one, state);
}

static void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    switch (output->os) {
        case MACOS:
            switch_virtual_desktop_macos(state, direction);
            break;

        case WINDOWS:
            break;

        case LINUX:
        case ANDROID:
        case OTHER:
            /* Linux should treat all desktops as a single virtual screen, so you should leave
            screen_count at 1 and it should just work */
            break;
    }

    const dh_mouse_coordinates_t entry = dh_mouse_entry_coordinates(
        (dh_direction_t)direction,
        (dh_mouse_coordinates_t){.x = state->pointer_x, .y = state->pointer_y},
        MIN_SCREEN_COORD,
        MAX_SCREEN_COORD);
    state->pointer_x = (int16_t)entry.x;
    state->pointer_y = (int16_t)entry.y;
    (void)select_cursor_screen(state, (uint8_t)output->number, (uint8_t)new_index);
}

#define ACCEL_POINTS 7
#define CURSOR_REANCHOR_TIMEOUT_US 30000u

static uint32_t unsigned_magnitude(int32_t value) {
    return value < 0 ? 0u - (uint32_t)value : (uint32_t)value;
}

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

static dh_mouse_transition_t actionable_transition_for(
    const device_t *state, const output_t *output, enum screen_pos_e direction, int buttons) {
    if (direction == NONE || state->switch_lock || state->gaming_mode)
        return DH_MOUSE_TRANSITION_NONE;
    const dh_mouse_layout_t layout = mouse_layout_for(output);
    const dh_mouse_transition_t transition = dh_mouse_transition_for(
        &layout, output->screen_index, output->screen_count,
        (dh_direction_t)direction);
    return transition == DH_MOUSE_TRANSITION_OUTPUT && buttons
               ? DH_MOUSE_TRANSITION_NONE
               : transition;
}

static void cursor_crossing_clear(device_t *state) {
    state->cursor_crossing = (cursor_crossing_t){.phase = CURSOR_CROSSING_IDLE};
}

static uint8_t next_cursor_query_id(device_t *state) {
    if (++state->next_cursor_query_id == 0)
        ++state->next_cursor_query_id;
    return state->next_cursor_query_id;
}

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
    /* A relative-source output crossing is a short transaction: its helper
       must report the OS cursor before seam mapping can finish. Fast diagonal
       packets can otherwise take a monitor-chain seam while that query is in
       flight, changing screen_index underneath the pending output crossing.
       Hold positional motion until the transaction resolves, while still
       forwarding buttons, wheel and pan through create_mouse_report(). */
    cursor_crossing_enter();
    const bool crossing_pending =
        state->cursor_crossing.phase != CURSOR_CROSSING_IDLE;
    const uint8_t pending_query_id = state->cursor_crossing.query_id;
    const uint8_t pending_direction = state->cursor_crossing.direction;
    cursor_crossing_exit();
    if (crossing_pending) {
        cursor_trace_event(state, DH_CURSOR_TRACE_INPUT, pending_query_id,
                           values->move_x, values->move_y, pending_direction,
                           DH_MOUSE_TRANSITION_OUTPUT);
        values->move_x = 0;
        values->move_y = 0;
        state->mouse_buttons = values->buttons;
        return NONE;
    }

    output_t *current = &state->config.output[state->active_output];
    uint8_t reduce_speed = state->mouse_zoom ? MOUSE_ZOOM_SCALING_FACTOR : 0;
    float acceleration = calculate_mouse_acceleration_factor(values->move_x, values->move_y);
    int offset_x = round(values->move_x * acceleration * (current->speed_x >> reduce_speed));
    int offset_y = round(values->move_y * acceleration * (current->speed_y >> reduce_speed));
    const screen_boundary_crossing_t horizontal =
        screen_boundary_crossing(current, state->pointer_x, offset_x, LEFT, RIGHT);
    const screen_boundary_crossing_t vertical =
        screen_boundary_crossing(current, state->pointer_y, offset_y, TOP, BOTTOM);
    bool horizontal_actionable =
        actionable_transition_for(state, current, horizontal.direction, values->buttons) !=
        DH_MOUSE_TRANSITION_NONE;
    bool vertical_actionable =
        actionable_transition_for(state, current, vertical.direction, values->buttons) !=
        DH_MOUSE_TRANSITION_NONE;
    const dh_direction_t arrival_guard = (dh_direction_t)state->output_arrival_guard;
    if (arrival_guard != DH_DIRECTION_NONE) {
        const int32_t raw_guard_axis = dh_direction_is_vertical(arrival_guard)
                                           ? values->move_y
                                           : values->move_x;
        const int32_t raw_cross_axis = dh_direction_is_vertical(arrival_guard)
                                           ? values->move_x
                                           : values->move_y;
        const uint32_t guard_magnitude = unsigned_magnitude(raw_guard_axis);
        const uint32_t cross_magnitude = unsigned_magnitude(raw_cross_axis);
        const bool moved_inward =
            guard_magnitude >= cross_magnitude &&
            ((arrival_guard == DH_DIRECTION_LEFT && offset_x > 0) ||
             (arrival_guard == DH_DIRECTION_RIGHT && offset_x < 0) ||
             (arrival_guard == DH_DIRECTION_TOP && offset_y > 0) ||
             (arrival_guard == DH_DIRECTION_BOTTOM && offset_y < 0));
        const bool moved_reverse =
            (arrival_guard == DH_DIRECTION_LEFT && raw_guard_axis < 0) ||
            (arrival_guard == DH_DIRECTION_RIGHT && raw_guard_axis > 0) ||
            (arrival_guard == DH_DIRECTION_TOP && raw_guard_axis < 0) ||
            (arrival_guard == DH_DIRECTION_BOTTOM && raw_guard_axis > 0);
        if (moved_reverse) {
            const uint32_t accumulated =
                state->output_arrival_reverse + guard_magnitude;
            state->output_arrival_reverse = (uint16_t)(
                accumulated > UINT16_MAX ? UINT16_MAX : accumulated);
        }
        const bool deliberate_reverse =
            state->output_arrival_reverse > state->config.jump_threshold;
        if (moved_inward || deliberate_reverse) {
            state->output_arrival_guard = DH_DIRECTION_NONE;
            state->output_arrival_reverse = 0;
        } else {
            if (horizontal.direction == (enum screen_pos_e)arrival_guard)
                horizontal_actionable = false;
            if (vertical.direction == (enum screen_pos_e)arrival_guard)
                vertical_actionable = false;
        }
    }
    const enum screen_pos_e direction =
        vertical_actionable &&
                (!horizontal_actionable || vertical.overshoot > horizontal.overshoot)
            ? vertical.direction
            : horizontal_actionable ? horizontal.direction : NONE;
    if (horizontal.direction != NONE || vertical.direction != NONE) {
        const dh_mouse_transition_t chosen_transition = actionable_transition_for(
            state, current, direction, values->buttons);
        cursor_trace_event(state, DH_CURSOR_TRACE_DECISION, 0, values->move_x,
                           values->move_y, (uint8_t)direction,
                           (uint8_t)chosen_transition);
    }

    /* Relative reports (Windows secondary monitors) otherwise let the OS take
       the losing seam before firmware performs the winning transition. Keep
       only the winning-axis motion in a simultaneous actionable crossing. */
    if (state->relative_mouse && horizontal_actionable && vertical_actionable) {
        if (dh_direction_is_vertical((dh_direction_t)direction))
            values->move_x = 0;
        else
            values->move_y = 0;
    }

    /* A crossing report chooses one seam below, but it may have overshot both
       axes near a corner. Keep every crossing axis at its last valid position
       so the unchosen seam cannot become a synthetic exact corner on the next
       report. This is the vertical-layout behavior proven in the #28 prior
       art, combined with our farther-overshoot arbitration. */
    if (!horizontal_actionable)
        state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    if (!vertical_actionable)
        state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);
    state->mouse_buttons = values->buttons;
    return direction;
}

static void cross_screen(device_t *state, int direction, bool source_resolved) {
    output_t *output = &state->config.output[state->active_output];
    const dh_mouse_transition_t transition = actionable_transition_for(
        state, output, (enum screen_pos_e)direction, state->mouse_buttons);
    switch (transition) {
        case DH_MOUSE_TRANSITION_OUTPUT:
                /* Windows secondary monitors are relative, so their stored
                   coordinate is only an estimate. Resolve the seam only after
                   the source helper has reported the OS cursor position. */
                if (state->relative_mouse && !source_resolved) {
                    const uint8_t query_id = next_cursor_query_id(state);
                    cursor_crossing_enter();
                    state->cursor_crossing.direction = (uint8_t)direction;
                    state->cursor_crossing.output = state->active_output;
                    state->cursor_crossing.query_id = query_id;
                    state->cursor_crossing.kind = CURSOR_CROSSING_SOURCE_REANCHOR;
                    state->cursor_crossing.started_us = time_us_32();
                    state->cursor_crossing.phase = CURSOR_CROSSING_WAITING;
                    cursor_crossing_exit();
                    cursor_trace_event(state, DH_CURSOR_TRACE_QUERY, query_id, 0, 0,
                                       (uint8_t)direction, (uint8_t)transition);
                    const cursor_query_result_t query_result =
                        channel_query_cursor(state->active_output, query_id);
                    if (query_result != CURSOR_QUERY_UNAVAILABLE) {
                        if (query_result == CURSOR_QUERY_SENT) {
                            cursor_crossing_enter();
                            if (state->cursor_crossing.phase == CURSOR_CROSSING_WAITING &&
                                state->cursor_crossing.query_id == query_id)
                                state->cursor_crossing.query_sent = true;
                            cursor_crossing_exit();
                        }
                        break;
                    }
                    cursor_crossing_enter();
                    if (state->cursor_crossing.phase == CURSOR_CROSSING_WAITING &&
                        state->cursor_crossing.query_id == query_id)
                        cursor_crossing_clear(state);
                    cursor_crossing_exit();
                    cursor_trace_event(state, DH_CURSOR_TRACE_CANCEL, query_id, 0, 0,
                                       (uint8_t)direction, (uint8_t)transition);
                }
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
                if (crossing == DH_SEAM_CROSSING_BLOCKED) {
                    const dh_mouse_coordinates_t edge = dh_mouse_edge_coordinates(
                        (dh_direction_t)direction,
                        (dh_mouse_coordinates_t){.x = state->pointer_x,
                                                 .y = state->pointer_y},
                        MIN_SCREEN_COORD, MAX_SCREEN_COORD);
                    state->pointer_x = (int16_t)edge.x;
                    state->pointer_y = (int16_t)edge.y;
                    break;
                }
                switch_to_another_pc(state, output, 1 - state->active_output, direction);
                state->output_arrival_guard = (uint8_t)dh_opposite_direction(
                    (dh_direction_t)direction);
                state->output_arrival_reverse = 0;
                cursor_trace_event(state, DH_CURSOR_TRACE_SWITCH, 0, 0, 0,
                                   (uint8_t)direction, (uint8_t)transition);
                if (crossing == DH_SEAM_CROSSING_MAPPED) {
                    const int entry = (int)(((uint32_t)mapped_entry.position * MAX_SCREEN_COORD +
                                             DH_SEAM_POSITION_MAX / 2) /
                                            DH_SEAM_POSITION_MAX);
                    if (dh_direction_is_vertical((dh_direction_t)direction))
                        state->pointer_x = (int16_t)entry;
                    else
                        state->pointer_y = (int16_t)entry;
                    if (!select_cursor_screen(state, (uint8_t)target->number,
                                              mapped_entry.screen_index))
                        break;
                    channel_place_cursor((uint8_t)target->number,
                                         mapped_entry.screen_index,
                                         target->chain_direction,
                                         target->border_direction,
                                         mapped_entry.position);
                    cursor_trace_event(state, DH_CURSOR_TRACE_PLACE, 0, 0, 0,
                                       (uint8_t)direction, (uint8_t)transition);
                }
            break;
        case DH_MOUSE_TRANSITION_CHAIN_BACK:
        case DH_MOUSE_TRANSITION_CHAIN_FORWARD:
            if (output->os == MACOS) {
                const uint8_t target_screen = dh_mouse_next_screen_index(
                    transition, output->screen_index);
                const int along = dh_mouse_along_seam(
                    (dh_direction_t)direction,
                    (dh_mouse_coordinates_t){.x = state->pointer_x,
                                             .y = state->pointer_y});
                const uint16_t normalized = (uint16_t)(
                    ((uint32_t)along * DH_SEAM_POSITION_MAX + MAX_SCREEN_COORD / 2) /
                    MAX_SCREEN_COORD);
                const uint8_t query_id = next_cursor_query_id(state);
                cursor_crossing_enter();
                state->cursor_crossing = (cursor_crossing_t){
                    .phase = CURSOR_CROSSING_WAITING,
                    .kind = CURSOR_CROSSING_MACOS_PLACEMENT,
                    .direction = (uint8_t)direction,
                    .output = state->active_output,
                    .query_id = query_id,
                    .target_screen = target_screen,
                    .target_position = normalized,
                    .started_us = time_us_32(),
                };
                cursor_crossing_exit();
                if (channel_place_cursor_correlated(
                        state->active_output, target_screen, output->chain_direction,
                        (uint8_t)dh_opposite_direction((dh_direction_t)direction),
                        normalized, query_id))
                    break;
                cursor_crossing_enter();
                if (state->cursor_crossing.phase == CURSOR_CROSSING_WAITING &&
                    state->cursor_crossing.query_id == query_id)
                    cursor_crossing_clear(state);
                cursor_crossing_exit();
            }
            switch_virtual_desktop(state, output,
                                  dh_mouse_next_screen_index(transition, output->screen_index),
                                  direction);
            cursor_trace_event(state, DH_CURSOR_TRACE_SWITCH, 0, 0, 0,
                               (uint8_t)direction, (uint8_t)transition);
            break;
        case DH_MOUSE_TRANSITION_NONE:
            break;
    }
}

void do_screen_switch(device_t *state, int direction) {
    cursor_crossing_enter();
    const bool pending = state->cursor_crossing.phase != CURSOR_CROSSING_IDLE;
    cursor_crossing_exit();
    if (!pending)
        cross_screen(state, direction, false);
}

void mouse_crossing_task(device_t *state, uint32_t now_us) {
    cursor_crossing_enter();
    cursor_crossing_t *crossing = &state->cursor_crossing;
    if (crossing->phase == CURSOR_CROSSING_IDLE) {
        cursor_crossing_exit();
        return;
    }
    if (state->active_output != crossing->output ||
        (crossing->kind == CURSOR_CROSSING_SOURCE_REANCHOR && state->mouse_buttons) ||
        state->switch_lock || state->gaming_mode) {
        const uint8_t query_id = crossing->query_id;
        const uint8_t direction = crossing->direction;
        cursor_crossing_clear(state);
        cursor_crossing_exit();
        cursor_trace_event(state, DH_CURSOR_TRACE_CANCEL, query_id, 0, 0, direction, 0);
        return;
    }
    if (crossing->phase == CURSOR_CROSSING_CANCELLED) {
        cursor_crossing_clear(state);
        cursor_crossing_exit();
        return;
    }
    if (crossing->phase == CURSOR_CROSSING_WAITING &&
        crossing->kind == CURSOR_CROSSING_SOURCE_REANCHOR &&
        !crossing->query_sent) {
        const uint8_t retry_output = crossing->output;
        const uint8_t retry_query_id = crossing->query_id;
        cursor_crossing_exit();
        const cursor_query_result_t retry_result =
            channel_query_cursor(retry_output, retry_query_id);
        cursor_crossing_enter();
        crossing = &state->cursor_crossing;
        if (crossing->phase != CURSOR_CROSSING_WAITING ||
            crossing->kind != CURSOR_CROSSING_SOURCE_REANCHOR ||
            crossing->output != retry_output || crossing->query_id != retry_query_id) {
            cursor_crossing_exit();
            return;
        }
        if (retry_result == CURSOR_QUERY_SENT) {
            crossing->query_sent = true;
            crossing->started_us = now_us;
        } else if (retry_result == CURSOR_QUERY_UNAVAILABLE) {
            crossing->phase = CURSOR_CROSSING_FALLBACK;
        }
    }
    bool timed_out = false;
    uint8_t timeout_query_id = 0;
    uint8_t timeout_direction = NONE;
    if (crossing->phase == CURSOR_CROSSING_WAITING &&
        (uint32_t)(now_us - crossing->started_us) >= CURSOR_REANCHOR_TIMEOUT_US) {
        crossing->phase = CURSOR_CROSSING_FALLBACK;
        timed_out = true;
        timeout_query_id = crossing->query_id;
        timeout_direction = crossing->direction;
    }
    if (crossing->phase == CURSOR_CROSSING_WAITING) {
        cursor_crossing_exit();
        return;
    }

    const uint8_t direction = crossing->direction;
    const cursor_crossing_kind_t kind = crossing->kind;
    const uint8_t target_screen = crossing->target_screen;
    if (kind == CURSOR_CROSSING_MACOS_PLACEMENT &&
        crossing->phase == CURSOR_CROSSING_REANCHORED) {
        cursor_crossing_clear(state);
        cursor_crossing_exit();
        return;
    }
    /* Completion runs synchronously on core 1. Retire the query before emitting
       effects, so every return (including a changed layout) releases motion. */
    cursor_crossing_clear(state);
    cursor_crossing_exit();
    if (timed_out)
        cursor_trace_event(state, DH_CURSOR_TRACE_TIMEOUT, timeout_query_id, 0, 0,
                           timeout_direction, DH_MOUSE_TRANSITION_OUTPUT);
    if (kind == CURSOR_CROSSING_MACOS_PLACEMENT) {
        output_t *output = &state->config.output[state->active_output];
        switch_virtual_desktop(state, output, target_screen, direction);
    } else {
        cross_screen(state, direction, true);
    }
}

void mouse_crossing_query_unavailable(device_t *state, uint8_t output, uint8_t query_id) {
    cursor_crossing_enter();
    cursor_crossing_t *crossing = &state->cursor_crossing;
    if (crossing->phase == CURSOR_CROSSING_WAITING && crossing->output == output &&
        crossing->query_id == query_id)
        crossing->phase = CURSOR_CROSSING_FALLBACK;
    cursor_crossing_exit();
}
