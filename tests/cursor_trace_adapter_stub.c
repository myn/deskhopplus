#include "main.h"

void cursor_trace_event(const device_t *state, dh_cursor_trace_event_t event,
                        uint8_t query_id, int16_t move_x, int16_t move_y,
                        uint8_t direction, uint8_t transition) {
    (void)state;
    (void)event;
    (void)query_id;
    (void)move_x;
    (void)move_y;
    (void)direction;
    (void)transition;
}
