/*
 * deskhopplus shared core — transmit-queue overflow accounting.
 *
 * The inter-board transmit queue historically dropped packets silently when
 * full: the enqueue result was discarded (#43). This is the one place that
 * result is turned into something observable — every enqueue passes through
 * dh_txq_track, so a full queue is always counted, whatever the caller then
 * does about it. Pure C11, no I/O, no platform dependencies; the fragmentation
 * work (#47) builds its backpressure on the same accounting.
 */

#ifndef DH_TXQ_H_
#define DH_TXQ_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t dropped; /* packets lost to a full queue since boot; saturates */
} dh_txq_stats;

/*
 * Record the result of one enqueue attempt and pass it through. A failed
 * attempt increments the drop counter, saturating at UINT32_MAX rather than
 * wrapping — a huge count must never look like a small one.
 */
static inline bool dh_txq_track(dh_txq_stats *stats, bool enqueued) {
    if (!enqueued && stats->dropped < UINT32_MAX)
        stats->dropped++;
    return enqueued;
}

#endif /* DH_TXQ_H_ */
