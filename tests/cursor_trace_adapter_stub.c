/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "main.h"

int cursor_trace_input_count;

void cursor_trace_event(const device_t *state, dh_cursor_trace_event_t event,
                        uint8_t query_id, int16_t move_x, int16_t move_y,
                        uint8_t direction, uint8_t transition) {
    (void)state;
    if (event == DH_CURSOR_TRACE_INPUT)
        cursor_trace_input_count++;
    (void)query_id;
    (void)move_x;
    (void)move_y;
    (void)direction;
    (void)transition;
}
