#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "structs.h"

bool select_cursor_screen(device_t *, uint8_t output, uint8_t screen);
bool apply_helper_cursor_position(device_t *, uint8_t output, uint8_t screen,
                                  int16_t x, int16_t y);
void handle_cursor_position_msg(uart_packet_t *, device_t *);
