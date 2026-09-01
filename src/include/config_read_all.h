/*
 * Where a config Read All has got to (#156).
 *
 * GET_ALL_VALS_MSG used to queue one HID response for every mapped field in a
 * single pass, before USB had any opportunity to drain them. That made the
 * whole map have to fit in the queue, and two requests arriving before the
 * first drained did not fit: queue_try_add's refusal was ignored, so the tail
 * of the second response was dropped silently and the page rendered those
 * fields empty.
 *
 * A walk hands out one field per drain pass instead, so the queue only ever
 * has to hold what a single pass produces. A repeated request restarts the
 * walk rather than stacking on top of it, and every mapped field is still
 * sent.
 *
 * The two cores share one walk: the request can arrive on either (USB on core
 * 0, a peer board's proxied read on core 1), while the steps always run on
 * core 0 beside the drain. No lock is taken. What makes that safe is that
 * `count` holds the same value on every start — the field map does not change
 * at runtime — and that a confirmation carries the field it belongs to, so a
 * start landing between a step's peek and its confirmation is not credited
 * with a field the new walk has not sent.
 *
 * Pure C11: no SDK, no queue, no device state. tests/config_read_all_test.c
 * is the gate.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t pending; /* fields still to hand out; zero when no read is running */
    uint16_t count;   /* fields in the map this walk was started for */
} config_read_all_t;

/* Begin, or restart, a walk over field_count fields. */
void config_read_all_start(config_read_all_t *walk, uint16_t field_count);

/*
 * The field due next, or false when no read is running. The walk does not
 * move until config_read_all_sent confirms the response was queued, so one
 * the queue refused is offered again on the next pass rather than skipped.
 */
bool config_read_all_peek(const config_read_all_t *walk, uint16_t *index);

/*
 * Confirm the field peeked at was queued, and move on. The index comes back
 * because the walk may have been restarted from the other core in between: a
 * confirmation for a field the walk is no longer on is stale, and ignoring it
 * is what stops a restart losing the first field of the map.
 */
void config_read_all_sent(config_read_all_t *walk, uint16_t index);

/*
 * Abandon a walk that cannot finish. Config mode ending is the case that
 * matters: outside it every response is refused, so a walk left running would
 * offer the same field on every pass for ever.
 */
void config_read_all_stop(config_read_all_t *walk);
