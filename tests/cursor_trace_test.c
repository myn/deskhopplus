#include <stdio.h>

#include "dh_cursor_trace.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL cursor_trace: %s\n", message); failures++; } \
} while (0)

static dh_cursor_trace_record_t record(uint8_t query_id) {
    return (dh_cursor_trace_record_t){
        .event = DH_CURSOR_TRACE_DECISION,
        .query_id = query_id,
        .move_x = (int16_t)(query_id * 10),
        .move_y = (int16_t)(-query_id * 10),
        .pointer_x = (int16_t)(100 + query_id),
        .pointer_y = (int16_t)(200 + query_id),
        .state = (uint16_t)(0x1200u | query_id),
    };
}

static void test_trace_is_bounded_and_reads_oldest_to_newest(void) {
    dh_cursor_trace_t trace = {0};
    dh_cursor_trace_init(&trace, false);
    for (uint8_t i = 1; i <= DH_CURSOR_TRACE_CAPACITY + 2; i++)
        dh_cursor_trace_append(&trace, record(i));

    CHECK(dh_cursor_trace_count(&trace) == DH_CURSOR_TRACE_CAPACITY,
          "bounded trace retained more than its capacity");
    dh_cursor_trace_record_t first = {0};
    dh_cursor_trace_record_t last = {0};
    CHECK(dh_cursor_trace_read(&trace, 0, &first), "oldest retained record was unreadable");
    CHECK(dh_cursor_trace_read(&trace, DH_CURSOR_TRACE_CAPACITY - 1, &last),
          "newest retained record was unreadable");
    CHECK(first.query_id == 3 && last.query_id == DH_CURSOR_TRACE_CAPACITY + 2,
          "wrapped trace was not returned oldest to newest");
    CHECK(!dh_cursor_trace_read(&trace, DH_CURSOR_TRACE_CAPACITY, &last),
          "out-of-range trace record was readable");
}

static void test_soft_config_reboot_preserves_only_a_valid_trace(void) {
    dh_cursor_trace_t trace = {0};
    dh_cursor_trace_init(&trace, false);
    dh_cursor_trace_append(&trace, record(7));
    dh_cursor_trace_init(&trace, true);
    CHECK(dh_cursor_trace_count(&trace) == 1,
          "soft config reboot discarded a valid cursor trace");

    trace.magic ^= 1u;
    dh_cursor_trace_init(&trace, true);
    CHECK(dh_cursor_trace_count(&trace) == 0,
          "invalid no-init RAM was accepted as a cursor trace");

    dh_cursor_trace_append(&trace, record(9));
    dh_cursor_trace_init(&trace, false);
    CHECK(dh_cursor_trace_count(&trace) == 0,
          "ordinary boot preserved a stale cursor trace");
}

int main(void) {
    test_trace_is_bounded_and_reads_oldest_to_newest();
    test_soft_config_reboot_preserves_only_a_valid_trace();
    if (failures) return 1;
    printf("cursor_trace_test: all checks passed\n");
    return 0;
}
