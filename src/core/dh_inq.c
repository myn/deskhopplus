/* deskhopplus shared core — the inbound handoff from the peer board (#139).
   See dh_inq.h for what this is for and why it is not dh_outq. */

#include "dh_inq.h"

#include <string.h>

static uint8_t inq_slot_at(uint8_t counter) {
    return (uint8_t)(counter % DH_INQ_DEPTH);
}

void dh_inq_init(dh_inq *q) {
    memset(q, 0, sizeof *q);
}

void dh_inq_reset(dh_inq *q) {
    /*
     * The consumer moves its own index up to the producer's, and touches
     * nothing else. Zeroing both would be a write to the producer's index from
     * the wrong core, which can strand a frame the producer is midway through
     * staging; this cannot, and it discards everything parked either way.
     */
    q->head = q->tail;
}

bool dh_inq_stage(dh_inq *q, const uint8_t *frame, uint16_t len) {
    /*
     * Tested before anything is written, because when the ring is full the
     * slot this would write is the *oldest unread* one — tail and head land on
     * the same index — and the consumer is still holding it.
     */
    if (len == 0 || len > DH_INQ_SLOT_MAX || dh_inq_pending(q) >= DH_INQ_DEPTH) {
        q->staged = false;
        if (q->dropped != UINT32_MAX)
            q->dropped++;
        return false;
    }

    const uint8_t at = inq_slot_at(q->tail);
    memcpy(q->slot[at], frame, len);
    q->slot_len[at] = len;
    q->staged = true;
    return true;
}

void dh_inq_publish(dh_inq *q) {
    if (!q->staged)
        return;

    q->staged = false;
    q->tail = (uint8_t)(q->tail + 1u);
}

bool dh_inq_peek(const dh_inq *q, const uint8_t **at, uint16_t *len) {
    if (dh_inq_pending(q) == 0)
        return false;

    const uint8_t slot = inq_slot_at(q->head);
    *at = q->slot[slot];
    *len = q->slot_len[slot];
    return true;
}

void dh_inq_release(dh_inq *q) {
    if (dh_inq_pending(q) == 0)
        return;

    q->head = (uint8_t)(q->head + 1u);
}

uint8_t dh_inq_pending(const dh_inq *q) {
    return (uint8_t)(q->tail - q->head);
}
