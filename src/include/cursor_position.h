#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "structs.h"

/* Serializes the crossing phase shared by the helper channel on core 0 and
   mouse processing on core 1. Initialized before core 1 is launched. */
void cursor_crossing_init(void);
void cursor_crossing_enter(void);
void cursor_crossing_exit(void);

bool select_cursor_screen(device_t *, uint8_t output, uint8_t screen);
bool apply_helper_cursor_position(device_t *, uint8_t output, uint8_t screen,
                                  int16_t x, int16_t y, uint8_t query_id);
void handle_cursor_position_msg(uart_packet_t *, device_t *);
void handle_cursor_query_unavailable_msg(uart_packet_t *, device_t *);
