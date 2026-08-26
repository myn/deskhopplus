/*
 * deskhopplus shared core — the inbound handoff from the peer board (#139).
 *
 * A frame reassembled from the inter-board link is not finished when it
 * arrives. It has to be re-tagged before it reaches this board's helper: the
 * tag is per hop, so it must be written under *this* board's key with *this*
 * board's counter, and both belong to the session, which lives on core 0. The
 * reassembly happens on core 1. So the frame is parked here and core 0 takes
 * it on its next pass.
 *
 * This was one slot, and the justification for one slot was that "a frame
 * takes about 4 ms to arrive over a 3.6 Mbaud link and core 0 drains at
 * 1000 Hz, so a second slot would hold something that is never there." That is
 * true of a full-size CLIP_CHUNK and of nothing else. A relayed CLIP_CREDIT is
 * ten bytes — three inter-board packets, about 100 us — and since the credit
 * window was sized against the outbound queue (#137) the receiver grants one
 * credit per chunk rather than one per eight. So the reverse path now carries
 * short frames in bursts that land several deep inside a single 1 ms tick, and
 * the second slot holds something that is there constantly. Measured on both
 * boards under nothing more than ordinary copying (#139).
 *
 * A dropped frame here is a whole frame. A chunk is re-requested end to end,
 * but a CLIP_OFFER, a CLIP_DONE or a CLIP_CREDIT has no retransmit behind it
 * and costs the entire transfer out to the helper's thirty-second timeout
 * (#78) — the same asymmetry that sizes dh_outq's slots.
 *
 * **Not a transport queue.** dh_outq hands bytes to a drain that takes them in
 * small fixed units, so it tracks progress through the frame in flight. Here a
 * frame is taken whole or not at all, and the only thing that has to be right
 * is that one core may write while the other reads. Hence a plain ring rather
 * than a second copy of dh_outq.
 *
 * Pure C11, no I/O, no allocation, no locking, and **no memory barriers** —
 * see the ordering contract on the four calls below. The caller owns the
 * barrier because the barrier is a platform instruction and this file has no
 * platform.
 */

#ifndef DH_INQ_H_
#define DH_INQ_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_outq.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * The largest frame a slot holds — the same bound dh_outq's queued slots use,
 * and for the same reason: it is the largest bulk frame a transfer can
 * actually *complete* with. A frame arriving from the peer board carries no
 * authentication prefix (it is re-tagged on the way out), so it is 24 bytes
 * inside this bound rather than at it, and the negotiated chunk size is
 * clamped to DH_XFER_CHUNK_SIZE on the way down (DH_SESSION_MAX_CHUNK) so it
 * cannot grow past it either.
 *
 * A longer frame is refused and counted rather than carried. That is a real
 * narrowing of what the single full-size slot accepted, and it costs nothing
 * worth keeping: a frame past this bound is one the far board's outbound queue
 * could never queue behind anything either, so it was only ever carried when
 * that queue happened to be idle.
 */
#define DH_INQ_SLOT_MAX DH_OUTQ_STAGE_MAX

/*
 * Frames parked at once.
 *
 * Sized to one whole pump batch, which is what arrives back to back in both
 * directions: on the receiving board the batch itself (DH_XFER_CREDIT_WINDOW
 * chunks and the ungated CLIP_DONE behind them), and on the sending board the
 * credits that batch earns, one per chunk. inq_test pins it against the
 * transfer machine's own constants so the two cannot drift apart.
 */
#define DH_INQ_DEPTH 4u

/* head and tail are free-running and wrap at their own width, so the depth
   has to divide that width evenly for `tail - head` to stay meaningful. */
_Static_assert(256u % DH_INQ_DEPTH == 0u, "DH_INQ_DEPTH must divide 256");

typedef struct {
    uint8_t slot[DH_INQ_DEPTH][DH_INQ_SLOT_MAX];
    uint16_t slot_len[DH_INQ_DEPTH];

    /* Free-running slot counters, one owned by each core: the producer only
       ever advances tail, the consumer only ever advances head. That is what
       makes the ring safe with no lock between two cores. */
    volatile uint8_t head;
    volatile uint8_t tail;

    /* Producer-only, and never read by the consumer: whether the slot at tail
       holds a frame waiting to be published. It is what makes a publish after
       a refused stage do nothing, which the slot length cannot say — a full
       ring puts tail on the oldest unread slot, whose length is live. */
    bool staged;

    uint32_t dropped; /* frames the ring could not take; saturates */
} dh_inq;

void dh_inq_init(dh_inq *q);

/*
 * Drop everything parked, and keep the drop counter. **The consumer's call**,
 * and it writes only the consumer's own index.
 *
 * For the caller whose session has just ended: what is parked was reassembled
 * under a session that no longer exists and would be tagged under keys that no
 * longer exist. The counter survives because it is a since-boot diagnostic
 * (#133) — the number is read to find out how often this seam has overrun, and
 * a total that restarts every time a helper reconnects cannot answer that.
 *
 * A frame the producer publishes during the call may survive it. That is the
 * price of not reaching across to the producer's index, and it costs nothing:
 * a stale frame has no live session to be tagged under and is refused.
 */
void dh_inq_reset(dh_inq *q);

/*
 * Producer, first half. Copy a frame into the slot behind the newest one,
 * without making it visible to the consumer. False when the ring is full or
 * the frame is longer than a slot, counted either way.
 *
 * Split from the publish below so the caller can put its memory barrier
 * between the two: the consumer reads `tail` to decide the bytes exist, so the
 * bytes must be written before `tail` moves. In-order retirement is not the
 * same promise as the compiler keeping the order it was given.
 */
bool dh_inq_stage(dh_inq *q, const uint8_t *frame, uint16_t len);

/* Producer, second half. Hand the staged frame to the consumer. Call a
   barrier first. Does nothing if the last stage was refused. */
void dh_inq_publish(dh_inq *q);

/*
 * Consumer, first half. The oldest frame, without taking it. False when the
 * ring is empty. The view stays valid until this consumer releases it — the
 * producer can only ever write to slots behind it.
 */
bool dh_inq_peek(const dh_inq *q, const uint8_t **at, uint16_t *len);

/* Consumer, second half. Done with the peeked frame; its slot is now the
   producer's again. Call a barrier first, so the reads land before the slot
   is released. */
void dh_inq_release(dh_inq *q);

/* Frames waiting to be taken. */
uint8_t dh_inq_pending(const dh_inq *q);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_INQ_H_ */
