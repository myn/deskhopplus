#include "main.h"

static critical_section_t crossing_lock;

void cursor_crossing_init(void) { critical_section_init(&crossing_lock); }
void cursor_crossing_enter(void) { critical_section_enter_blocking(&crossing_lock); }
void cursor_crossing_exit(void) { critical_section_exit(&crossing_lock); }

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

bool select_cursor_screen(device_t *state, uint8_t output, uint8_t screen) {
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
    cursor_crossing_enter();
    cursor_crossing_t *crossing = &state->cursor_crossing;
    const cursor_crossing_phase_t phase = crossing->phase;
    if ((phase == CURSOR_CROSSING_WAITING &&
         (query_id == 0 || crossing->output != output || crossing->query_id != query_id)) ||
        (query_id != 0 && phase != CURSOR_CROSSING_WAITING)) {
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
        if (position_is_at_pending_edge(state, x, y)) {
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

void handle_cursor_position_msg(uart_packet_t *packet, device_t *state) {
    (void)apply_helper_cursor_position(state, packet->data[0], packet->data[1],
                                       (int16_t)packet->data16[1],
                                       (int16_t)packet->data16[2], packet->data[6]);
}

void handle_cursor_query_unavailable_msg(uart_packet_t *packet, device_t *state) {
    mouse_crossing_query_unavailable(state, packet->data[0], packet->data[1]);
}
