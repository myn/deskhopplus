#include "main.h"

#include "dh_cursor_trace.h"

static dh_cursor_trace_t __uninitialized_ram(cursor_trace_storage);
static critical_section_t cursor_trace_lock;

static uint16_t pack_state(const device_t *state, uint8_t direction, uint8_t transition) {
    const output_t *output = &state->config.output[state->active_output];
    uint16_t packed = (uint16_t)(state->active_output & 1u) << DH_CURSOR_TRACE_OUTPUT_SHIFT;
    packed |= (uint16_t)(output->screen_index & 7u) << DH_CURSOR_TRACE_SCREEN_SHIFT;
    packed |= (uint16_t)(direction & 7u) << DH_CURSOR_TRACE_DIRECTION_SHIFT;
    packed |= (uint16_t)(state->cursor_crossing.phase & 7u) << DH_CURSOR_TRACE_PHASE_SHIFT;
    packed |= (uint16_t)(state->relative_mouse ? 1u : 0u) << DH_CURSOR_TRACE_RELATIVE_SHIFT;
    packed |= (uint16_t)(transition & 3u) << DH_CURSOR_TRACE_TRANSITION_SHIFT;
    return packed;
}

void cursor_trace_boot(bool preserve) {
    critical_section_init(&cursor_trace_lock);
    critical_section_enter_blocking(&cursor_trace_lock);
    dh_cursor_trace_init(&cursor_trace_storage, preserve);
    critical_section_exit(&cursor_trace_lock);
}

void cursor_trace_event(const device_t *state, dh_cursor_trace_event_t event,
                        uint8_t query_id, int16_t move_x, int16_t move_y,
                        uint8_t direction, uint8_t transition) {
    dh_cursor_trace_record_t record = {
        .event = (uint8_t)event,
        .query_id = query_id,
        .move_x = move_x,
        .move_y = move_y,
        .pointer_x = state->pointer_x,
        .pointer_y = state->pointer_y,
        .state = pack_state(state, direction, transition),
    };
    critical_section_enter_blocking(&cursor_trace_lock);
    dh_cursor_trace_append(&cursor_trace_storage, record);
    critical_section_exit(&cursor_trace_lock);
}

size_t cursor_trace_count(void) {
    critical_section_enter_blocking(&cursor_trace_lock);
    const size_t count = dh_cursor_trace_count(&cursor_trace_storage);
    critical_section_exit(&cursor_trace_lock);
    return count;
}

bool cursor_trace_read(size_t index, dh_cursor_trace_record_t *record) {
    critical_section_enter_blocking(&cursor_trace_lock);
    const bool found = dh_cursor_trace_read(&cursor_trace_storage, index, record);
    critical_section_exit(&cursor_trace_lock);
    return found;
}
