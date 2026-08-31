#include "dh_cursor_trace.h"

#include <string.h>

_Static_assert(sizeof(dh_cursor_trace_record_t) == 12,
               "cursor trace record must split into two six-byte config responses");

static bool trace_is_valid(const dh_cursor_trace_t *trace) {
    return trace->magic == DH_CURSOR_TRACE_MAGIC &&
           trace->head < DH_CURSOR_TRACE_CAPACITY &&
           trace->count <= DH_CURSOR_TRACE_CAPACITY;
}

void dh_cursor_trace_init(dh_cursor_trace_t *trace, bool preserve) {
    if (preserve && trace_is_valid(trace))
        return;
    memset(trace, 0, sizeof *trace);
    trace->magic = DH_CURSOR_TRACE_MAGIC;
}

void dh_cursor_trace_append(dh_cursor_trace_t *trace, dh_cursor_trace_record_t record) {
    if (!trace_is_valid(trace))
        dh_cursor_trace_init(trace, false);
    trace->records[trace->head] = record;
    trace->head = (uint8_t)((trace->head + 1u) % DH_CURSOR_TRACE_CAPACITY);
    if (trace->count < DH_CURSOR_TRACE_CAPACITY)
        trace->count++;
}

size_t dh_cursor_trace_count(const dh_cursor_trace_t *trace) {
    return trace_is_valid(trace) ? trace->count : 0;
}

bool dh_cursor_trace_read(const dh_cursor_trace_t *trace, size_t index,
                          dh_cursor_trace_record_t *record) {
    if (!record || !trace_is_valid(trace) || index >= trace->count)
        return false;
    const size_t oldest =
        (trace->head + DH_CURSOR_TRACE_CAPACITY - trace->count) % DH_CURSOR_TRACE_CAPACITY;
    *record = trace->records[(oldest + index) % DH_CURSOR_TRACE_CAPACITY];
    return true;
}
