#include "main.h"

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
                                  int16_t x, int16_t y) {
    if (!select_cursor_screen(state, output, screen))
        return false;
    state->pointer_x = x;
    state->pointer_y = y;
    return true;
}

void handle_cursor_position_msg(uart_packet_t *packet, device_t *state) {
    (void)apply_helper_cursor_position(state, packet->data[0], packet->data[1],
                                       (int16_t)packet->data16[1],
                                       (int16_t)packet->data16[2]);
}
