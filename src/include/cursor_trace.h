#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_cursor_trace.h"

void cursor_trace_boot(bool preserve);
void cursor_trace_event(const device_t *state, dh_cursor_trace_event_t event,
                        uint8_t query_id, int16_t move_x, int16_t move_y,
                        uint8_t direction, uint8_t transition);
size_t cursor_trace_count(void);
bool cursor_trace_read(size_t index, dh_cursor_trace_record_t *record);
