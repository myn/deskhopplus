#include "main.h"

static critical_section_t crossing_lock;

void cursor_crossing_init(void) { critical_section_init(&crossing_lock); }
void cursor_crossing_enter(void) { critical_section_enter_blocking(&crossing_lock); }
void cursor_crossing_exit(void) { critical_section_exit(&crossing_lock); }

void handle_cursor_position_msg(uart_packet_t *packet, device_t *state) {
    (void)apply_helper_cursor_position(state, packet->data[0], packet->data[1],
                                       (int16_t)packet->data16[1],
                                       (int16_t)packet->data16[2], packet->data[6]);
}

void handle_cursor_query_unavailable_msg(uart_packet_t *packet, device_t *state) {
    mouse_crossing_query_unavailable(state, packet->data[0], packet->data[1]);
}
