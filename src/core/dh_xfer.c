/*
 * The chunked transfer state machine. Semantics: docs/protocol.md,
 * "Transfer semantics". Ported from mkroamer's clip-transfer core; the
 * reliability additions (CRC32, selective retransmit, credit window,
 * abandon-on-drop) are this project's (#32, #48).
 *
 * Chunk emission happens only in dh_xfer_pump — handlers record state and
 * emit control actions. Callers pump after handling each message and
 * whenever the bulk band has room.
 *
 * There is still no clock in here. Recovery from a lost *control* message (a
 * retransmit request, a credit grant, the CLIP_DONE that drives a sweep) is
 * therefore the caller's to time: it calls dh_xfer_sweep_rx on a receive that
 * has stopped moving, and this machine works out what to ask for (#145).
 * Before that, every one of those losses cost the whole transfer.
 */

#include "dh_xfer.h"

#include <string.h>

#include "dh_crc32.h"

static bool bit_get(const uint8_t *bits, uint32_t i) {
    return (bits[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint8_t *bits, uint32_t i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static void bit_clear(uint8_t *bits, uint32_t i) {
    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static uint32_t chunk_count(uint64_t total) {
    return (uint32_t)((total + DH_XFER_CHUNK_SIZE - 1) / DH_XFER_CHUNK_SIZE);
}

static uint16_t chunk_len(uint64_t total, uint32_t nchunks, uint32_t seq) {
    if (seq + 1 == nchunks)
        return (uint16_t)(total - (uint64_t)seq * DH_XFER_CHUNK_SIZE);
    return DH_XFER_CHUNK_SIZE;
}

static dh_xfer_action *emit(dh_xfer_action *acts, size_t *n, size_t cap) {
    if (*n >= cap)
        return NULL;
    dh_xfer_action *a = &acts[(*n)++];
    memset(a, 0, sizeof *a);
    return a;
}

static void tx_reset(dh_xfer *x) {
    memset(&x->tx, 0, sizeof x->tx);
}

/* End the arriving transfer but keep the identity behind it: the offer that
   opened it stays known, so a duplicate of it is still recognised as one. */
static void rx_reset(dh_xfer *x) {
    x->rx.active = false;
}

/* The stronger one: forget the incoming direction outright, the far end's
   offer ordering with it. Even a completed identity must not make the first
   offer of the *next* namespace look stale. */
static void rx_forget(dh_xfer *x) {
    memset(&x->rx, 0, sizeof x->rx);
}

/* Give up on an arriving transfer and forget the direction with it — what both
   boundaries of the far end's offer ordering do (dh_xfer_link_down and
   dh_xfer_rx_seal_replaced). Silent when nothing is arriving; the ordering goes
   either way. Partial data is never kept. */
static void rx_abandon(dh_xfer *x, uint8_t reason, dh_xfer_action *acts, size_t *n,
                       size_t acts_cap) {
    if (x->rx.active) {
        dh_xfer_action *a = emit(acts, n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_FAILED;
            a->id = x->rx.id;
            a->reason = reason;
        }
    }
    rx_forget(x);
}

static bool serial_newer(uint32_t candidate, uint32_t current) {
    return candidate != current && (uint32_t)(candidate - current) < UINT32_C(0x80000000);
}

void dh_xfer_init(dh_xfer *x, uint8_t *rx_buf, size_t rx_cap) {
    memset(x, 0, sizeof *x);
    x->rx_buf = rx_buf;
    x->rx_cap = rx_cap;
    x->next_id = 1;
}

bool dh_xfer_set_rx_buffer(dh_xfer *x, uint8_t *rx_buf, size_t rx_cap) {
    if (x == NULL || rx_buf == NULL || rx_cap == 0) return false;
    /*
     * Any incoming transfer blocks the swap, including a lazy one that is
     * holding an accepted offer and no bytes yet. That offer's total was
     * measured against the *old* capacity, and nothing re-measures it when the
     * request finally goes out — so a buffer that shrank underneath it would
     * be written past its end by a payload the machine already agreed to.
     */
    if (x->rx.active) return false;
    x->rx_buf = rx_buf;
    x->rx_cap = rx_cap;
    return true;
}

/* ---- sender ----------------------------------------------------------- */

size_t dh_xfer_offer(dh_xfer *x, uint8_t kind, const uint8_t *meta, uint16_t meta_len,
                     const uint8_t *data, uint64_t total, dh_xfer_action *acts,
                     size_t acts_cap) {
    size_t n = 0;
    tx_reset(x); /* a newer offer supersedes whatever was in flight */
    x->tx.active = true;
    x->tx.id = x->next_id++;
    if (x->next_id == 0)
        x->next_id = 1;
    x->tx.kind = kind;
    x->tx.meta = meta;
    x->tx.meta_len = meta_len;
    x->tx.data = data;
    x->tx.total = total;
    x->tx.nchunks = chunk_count(total);
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_OFFER;
        a->id = x->tx.id;
    }
    return n;
}

size_t dh_xfer_retry_offer(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    if (!x->tx.active || x->tx.streaming || x->tx.lazy_pending)
        return 0;
    size_t n = 0;
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_OFFER_RETRY;
        a->id = x->tx.id;
        x->tx.offer_retries++;
    }
    return n;
}

/*
 * The transfer starts flowing.
 *
 * A receiver that has heard nothing sends its CLIP_REQUEST again every couple
 * of seconds, and a window's worth of covering credit with it
 * (dh_xfer_sweep_rx). A transfer that took several rounds to start would
 * therefore begin holding the sum of every grant sent while there was nothing
 * to spend one on — a first pump several windows deep, which is exactly the
 * burst no outbound queue on the path is sized for (ADR-0005). Streaming
 * starts from one window instead, however many rounds it took.
 *
 * Once streaming, credit accumulates without a ceiling as before: the covering
 * grants that pay for retransmissions are what a ceiling would discard, and
 * that is measured (docs/protocol.md, "Flow control").
 */
static void tx_start_streaming(dh_xfer *x) {
    x->tx.streaming = true;
    x->tx.need_done = true;
    if (x->tx.credits > DH_XFER_CREDIT_WINDOW)
        x->tx.credits = DH_XFER_CREDIT_WINDOW;
    x->tx.opening_credit_accepted = x->tx.credits > 0;
}

size_t dh_xfer_provide(dh_xfer *x, const uint8_t *data, dh_xfer_action *acts, size_t acts_cap) {
    (void)acts;
    (void)acts_cap;
    if (!x->tx.active || !x->tx.lazy_pending)
        return 0;
    x->tx.data = data;
    x->tx.lazy_pending = false;
    tx_start_streaming(x);
    return 0; /* chunks flow from the next pump */
}

size_t dh_xfer_provide_fail(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    if (!x->tx.active || !x->tx.lazy_pending)
        return 0;
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_CANCEL;
        a->id = x->tx.id;
    }
    tx_reset(x);
    return n;
}

static void tx_queue_retransmit(dh_xfer *x, uint32_t seq) {
    for (uint32_t i = 0; i < x->tx.retx_count; i++) {
        if (x->tx.retx[(x->tx.retx_head + i) % DH_XFER_RETX_MAX] == seq)
            return; /* already queued */
    }
    if (x->tx.retx_count >= DH_XFER_RETX_MAX)
        return; /* overflow: the receiver's next DONE sweep re-requests */
    x->tx.retx[(x->tx.retx_head + x->tx.retx_count) % DH_XFER_RETX_MAX] = seq;
    x->tx.retx_count++;
}

size_t dh_xfer_pump(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    if (!x->tx.active || !x->tx.streaming || x->tx.lazy_pending)
        return 0;
    size_t chunks = 0;
    while (x->tx.credits > 0 && chunks < DH_XFER_BATCH_MAX && n + 1 < acts_cap &&
           (x->tx.retx_count > 0 || x->tx.next_seq < x->tx.nchunks)) {
        uint32_t seq;
        if (x->tx.retx_count > 0) {
            seq = x->tx.retx[x->tx.retx_head];
            x->tx.retx_head = (x->tx.retx_head + 1) % DH_XFER_RETX_MAX;
            x->tx.retx_count--;
            x->tx.retx_sent++;
        } else {
            seq = x->tx.next_seq++;
        }
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        a->type = DH_XFER_ACT_SEND_CHUNK;
        a->id = x->tx.id;
        a->seq = seq;
        x->tx.credits--;
        chunks++;
    }
    /* DONE is not credit-gated; owed once everything has been emitted. */
    if (x->tx.need_done && x->tx.retx_count == 0 && x->tx.next_seq >= x->tx.nchunks) {
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_SEND_DONE;
            a->id = x->tx.id;
            x->tx.need_done = false;
        }
    }
    return n;
}

size_t dh_xfer_cancel_tx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    if (!x->tx.active)
        return 0;
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_CANCEL;
        a->id = x->tx.id;
    }
    tx_reset(x);
    return n;
}

size_t dh_xfer_handle_request(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    if (!x->tx.active || x->tx.id != id || x->tx.streaming || x->tx.lazy_pending)
        return 0;
    if (x->tx.data == NULL && x->tx.total > 0) {
        x->tx.lazy_pending = true;
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_NEED_DATA;
            a->id = x->tx.id;
        }
        return n;
    }
    tx_start_streaming(x);
    return n; /* chunks flow from the next pump */
}

size_t dh_xfer_handle_retransmit(dh_xfer *x, uint32_t id, uint32_t seq, dh_xfer_action *acts,
                                 size_t acts_cap) {
    (void)acts;
    (void)acts_cap;
    if (!x->tx.active || x->tx.id != id || seq >= x->tx.nchunks)
        return 0;
    /* Its covering grant is recovery credit even when this sequence is ahead
       of the sender's frontier and therefore is not queued as a retransmit. */
    x->tx.recovery_credit_due = true;
    /*
     * A chunk this end has not sent yet is not a retransmission: a stall sweep
     * names the chunks the receiver is *waiting on*, which past the frontier
     * are ones the sender simply has not reached (dh_xfer_sweep_rx). Queueing
     * one would send it twice, once from the ring and once from next_seq. What
     * restarts the sender there is the covering credit that came with the
     * request, not the request itself.
     */
    if (seq < x->tx.next_seq) {
        tx_queue_retransmit(x, seq);
        x->tx.retx_asked++;
    }
    x->tx.need_done = true; /* repeat DONE after servicing */
    return 0;
}

size_t dh_xfer_handle_credit(dh_xfer *x, uint32_t id, uint16_t credits, dh_xfer_action *acts,
                             size_t acts_cap) {
    (void)acts;
    (void)acts_cap;
    if (!x->tx.active || x->tx.id != id)
        return 0; /* stale grant for a superseded or finished transfer */
    /*
     * A full-window grant with no retransmission request behind it can belong
     * to another delayed copy of the same offer's opening response. Requests
     * and credits are separate frames, so tx_start_streaming cannot know that
     * more opening grants are still in flight, even after its first pump.
     * Accept only one such grant for the transfer. Credit covering an observed
     * retransmission request still accumulates without a ceiling (ADR-0005,
     * #145), as do the ordinary per-chunk replenishment grants.
     */
    const bool opening_grant = x->tx.streaming && credits == DH_XFER_CREDIT_WINDOW &&
                               !x->tx.recovery_credit_due;
    if (opening_grant && !x->tx.opening_credit_accepted) {
        const uint32_t room = x->tx.credits < DH_XFER_CREDIT_WINDOW
                                  ? DH_XFER_CREDIT_WINDOW - x->tx.credits
                                  : 0;
        x->tx.credits += credits < room ? credits : room;
        x->tx.opening_credit_accepted = true;
    } else if (!opening_grant) {
        x->tx.credits += credits;
    }
    x->tx.recovery_credit_due = false;
    return 0;
}

bool dh_xfer_offer_info(const dh_xfer *x, dh_clip_offer *out) {
    if (!x->tx.active)
        return false;
    out->id = x->tx.id;
    out->kind = x->tx.kind;
    out->total = x->tx.total;
    out->meta = x->tx.meta;
    out->meta_len = x->tx.meta_len;
    return true;
}

bool dh_xfer_chunk_at(const dh_xfer *x, uint32_t seq, dh_clip_chunk *out) {
    if (!x->tx.active || x->tx.data == NULL || seq >= x->tx.nchunks)
        return false;
    out->id = x->tx.id;
    out->seq = seq;
    out->data = x->tx.data + (uint64_t)seq * DH_XFER_CHUNK_SIZE;
    out->data_len = chunk_len(x->tx.total, x->tx.nchunks, seq);
    out->crc32 = dh_crc32(out->data, out->data_len);
    return true;
}

/* ---- receiver --------------------------------------------------------- */

static size_t handle_offer(dh_xfer *x, const dh_clip_offer *offer, bool lazy,
                           dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    const uint32_t nchunks = chunk_count(offer->total);
    if (offer->id == 0) {
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) a->type = DH_XFER_ACT_PROTOCOL_ERROR;
        return n;
    }
    if (x->rx.seen_offer && offer->id == x->rx.id) {
        const bool same = offer->meta_len <= DH_XFER_IDENTITY_META_MAX &&
                          offer->kind == x->rx.kind && offer->total == x->rx.total &&
                          offer->meta_len == x->rx.meta_len &&
                          (offer->meta_len == 0 ||
                           memcmp(offer->meta, x->rx.meta, offer->meta_len) == 0);
        if (!same) {
            dh_xfer_action *a = emit(acts, &n, acts_cap);
            if (a) {
                a->type = DH_XFER_ACT_PROTOCOL_ERROR;
                a->id = offer->id;
            }
            return n;
        }
        x->rx.duplicate_offers++;
        if (!x->rx.active || x->rx.any_seen || x->rx.lazy)
            return 0;
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_SEND_REQUEST;
            a->id = offer->id;
        }
        a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_SEND_CREDIT;
            a->id = offer->id;
            a->credits = DH_XFER_CREDIT_WINDOW;
        }
        return n;
    }
    if (x->rx.seen_offer && !serial_newer(offer->id, x->rx.id))
        return 0;
    /* A genuinely newer offer supersedes an incomplete incoming transfer. */
    rx_forget(x);
    x->rx.seen_offer = true;
    x->rx.id = offer->id;
    x->rx.kind = offer->kind;
    x->rx.total = offer->total;
    x->rx.nchunks = nchunks;
    x->rx.meta_len = offer->meta_len;
    if (offer->meta_len <= DH_XFER_IDENTITY_META_MAX && offer->meta_len > 0)
        memcpy(x->rx.meta, offer->meta, offer->meta_len);
    if (offer->total > x->rx_cap || nchunks > DH_XFER_MAX_CHUNKS ||
        offer->meta_len > DH_XFER_META_MAX) {
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_SEND_CANCEL;
            a->id = offer->id;
        }
        return n;
    }
    x->rx.active = true;
    x->rx.lazy = lazy;
    if (lazy)
        return 0;
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_REQUEST;
        a->id = offer->id;
    }
    a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_CREDIT;
        a->id = offer->id;
        a->credits = DH_XFER_CREDIT_WINDOW;
    }
    return n;
}

size_t dh_xfer_handle_offer(dh_xfer *x, const dh_clip_offer *offer, dh_xfer_action *acts,
                            size_t acts_cap) {
    return handle_offer(x, offer, false, acts, acts_cap);
}

size_t dh_xfer_handle_offer_lazy(dh_xfer *x, const dh_clip_offer *offer,
                                 dh_xfer_action *acts, size_t acts_cap) {
    return handle_offer(x, offer, true, acts, acts_cap);
}

size_t dh_xfer_request_lazy(dh_xfer *x, uint32_t id, dh_xfer_action *acts,
                            size_t acts_cap) {
    if (!x->rx.active || !x->rx.lazy || x->rx.id != id)
        return 0;
    x->rx.lazy = false;
    size_t n = 0;
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) { a->type = DH_XFER_ACT_SEND_REQUEST; a->id = id; }
    a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_CREDIT;
        a->id = id;
        a->credits = DH_XFER_CREDIT_WINDOW;
    }
    return n;
}

/* Request retransmission of `seq`, with its covering credit counted into
   *credits. `stale` marks a sweep-issued request as already a round old, so
   the next sweep may repeat it if it goes unanswered. */
static void rx_request_chunk(dh_xfer *x, uint32_t seq, bool stale, dh_xfer_action *acts,
                             size_t *n, size_t acts_cap, uint16_t *credits) {
    if (bit_get(x->rx.requested, seq))
        return;
    if (*n + 1 >= acts_cap)
        return; /* the rest is caught at the next DONE sweep */
    dh_xfer_action *a = emit(acts, n, acts_cap);
    a->type = DH_XFER_ACT_SEND_RETRANSMIT;
    a->id = x->rx.id;
    a->seq = seq;
    bit_set(x->rx.requested, seq);
    x->rx.retx_asked++;
    if (stale)
        bit_set(x->rx.requested_stale, seq);
    else
        bit_clear(x->rx.requested_stale, seq);
    (*credits)++;
}

/* Hand the finished payload up. total/kind/meta stay readable until the next
   offer, which is what dh_xfer_delivered_len and friends read. False when the
   action would not fit, which leaves the transfer active so that a CLIP_DONE
   behind it still completes the transfer rather than the payload being lost. */
static bool rx_complete(dh_xfer *x, dh_xfer_action *acts, size_t *n, size_t acts_cap) {
    dh_xfer_action *a = emit(acts, n, acts_cap);
    if (a == NULL)
        return false;
    a->type = DH_XFER_ACT_DELIVERED;
    a->id = x->rx.id;
    x->rx.active = false;
    return true;
}

static void rx_grant(uint32_t id, dh_xfer_action *acts, size_t *n, size_t acts_cap,
                     uint16_t credits) {
    if (credits == 0)
        return;
    dh_xfer_action *a = emit(acts, n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_CREDIT;
        a->id = id;
        a->credits = credits;
    }
}

size_t dh_xfer_handle_chunk(dh_xfer *x, const dh_clip_chunk *chunk, dh_xfer_action *acts,
                            size_t acts_cap) {
    size_t n = 0;
    uint16_t credits = 0;
    if (!x->rx.active || x->rx.lazy || x->rx.id != chunk->id ||
        chunk->seq >= x->rx.nchunks)
        return 0;

    if (bit_get(x->rx.received, chunk->seq)) {
        /* Duplicate: its credit was spent but adds no progress — return it. */
        rx_grant(x->rx.id, acts, &n, acts_cap, 1);
        return n;
    }

    const uint16_t expected = chunk_len(x->rx.total, x->rx.nchunks, chunk->seq);
    if (chunk->data_len != expected || dh_crc32(chunk->data, chunk->data_len) != chunk->crc32) {
        bit_clear(x->rx.requested, chunk->seq); /* a bad retransmit may be re-requested */
        rx_request_chunk(x, chunk->seq, false, acts, &n, acts_cap, &credits);
        rx_grant(x->rx.id, acts, &n, acts_cap, credits);
        return n;
    }

    /* A skipped seq is a loss candidate: report the gap as soon as it shows. */
    const uint32_t next_expected = x->rx.any_seen ? x->rx.max_seq_seen + 1 : 0;
    for (uint32_t m = next_expected; m < chunk->seq; m++) {
        if (!bit_get(x->rx.received, m))
            rx_request_chunk(x, m, false, acts, &n, acts_cap, &credits);
    }
    if (!x->rx.any_seen || chunk->seq > x->rx.max_seq_seen) {
        x->rx.max_seq_seen = chunk->seq;
        x->rx.any_seen = true;
    }

    memcpy(x->rx_buf + (uint64_t)chunk->seq * DH_XFER_CHUNK_SIZE, chunk->data,
           chunk->data_len);
    bit_set(x->rx.received, chunk->seq);
    if (bit_get(x->rx.requested, chunk->seq))
        x->rx.retx_answered++;
    bit_clear(x->rx.requested, chunk->seq);
    x->rx.nreceived++;

    /*
     * The last chunk completes the transfer here rather than at CLIP_DONE
     * (#132). Every chunk is length-checked and CRC32-verified on arrival, so
     * a receiver holding all of them already knows it is finished; DONE
     * carries nothing but the id.
     *
     * Waiting to be told made one dropped frame cost the whole payload. The
     * device's outbound queue refuses frames under a burst deeper than itself
     * and there is no retransmit beneath that seam (ADR-0005, channel.c) — and
     * DONE is always part of such a burst, because dh_xfer_pump emits it in
     * the same batch as the last chunk. A chunk lost that way is re-requested;
     * a DONE lost that way used to strand a complete payload until the
     * helper's stall timeout.
     *
     * No credit is granted alongside it: nothing further is owed, and a frame
     * not sent is one the queue cannot refuse.
     */
    if (x->rx.nreceived >= x->rx.nchunks && rx_complete(x, acts, &n, acts_cap))
        return n;

    x->rx.ungranted++;
    if (x->rx.ungranted >= DH_XFER_CREDIT_WINDOW / 2) {
        credits = (uint16_t)(credits + x->rx.ungranted);
        x->rx.ungranted = 0;
    }
    rx_grant(x->rx.id, acts, &n, acts_cap, credits);
    return n;
}

/*
 * Ask again for the chunks missing below `limit`, each with its covering
 * credit. Shared by the two sweeps, which differ in exactly two ways.
 *
 * `aged` is the DONE sweep's discipline: a chunk asked for since the last
 * round is left alone once, because its retransmission is behind that DONE in
 * the FIFO, and asked for again only if it survives a whole round. What it
 * asks for it also marks as already a round old, so the next DONE may repeat
 * it. The stall sweep does not age, because it runs only after a whole quiet
 * interval in which nothing arrived at all — there is nothing still in flight
 * to leave alone.
 *
 * `limit` is how far the sweep may look. At a DONE the sender has emitted
 * everything, so every hole is a loss and the limit is the whole payload. On a
 * stall it is the *frontier* — the highest seq that has arrived — because past
 * that the sender may simply not have got there, and asking would be one
 * request per remaining chunk. A stall with no frontier yet does not come here
 * at all.
 */
static void rx_sweep_range(dh_xfer *x, uint32_t limit, bool aged, dh_xfer_action *acts, size_t *n,
                           size_t acts_cap, uint16_t *credits) {
    for (uint32_t m = 0; m < limit; m++) {
        if (bit_get(x->rx.received, m))
            continue;
        if (aged && bit_get(x->rx.requested, m) && !bit_get(x->rx.requested_stale, m)) {
            bit_set(x->rx.requested_stale, m); /* skip once, age */
            continue;
        }
        bit_clear(x->rx.requested, m);
        rx_request_chunk(x, m, aged, acts, n, acts_cap, credits);
    }
}

size_t dh_xfer_sweep_rx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    uint16_t credits = 0;
    if (!x->rx.active || x->rx.lazy)
        return 0;

    /*
     * Nothing has arrived at all. This end cannot tell whether the sender ever
     * started — its CLIP_REQUEST may have been lost, or the window covering
     * it, or the whole opening burst of chunks — so the request goes again and
     * the chunks below are named as well. A sender already streaming ignores
     * the repeated request; one that never started acts on it and begins from
     * a single window however many times this repeats (tx_start_streaming).
     *
     * Otherwise there is a frontier, and below it the sender has been and
     * gone: every hole down there is a loss.
     */
    if (!x->rx.any_seen) {
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_SEND_REQUEST;
            a->id = x->rx.id;
        }
    } else {
        rx_sweep_range(x, x->rx.max_seq_seen, false, acts, &n, acts_cap, &credits);
    }

    /*
     * At and above the frontier — or from the very start, when nothing has
     * arrived — a window's worth and no more. A window is what the sender
     * could have had in flight when everything stopped, and the covering
     * credits are what pay a sender stopped by a lost grant to start again.
     * The sender ignores a request for a chunk it has not reached and sends it
     * in its own order (dh_xfer_handle_retransmit); what crosses there is the
     * credit.
     */
    uint32_t named = 0;
    const uint32_t from = x->rx.any_seen ? x->rx.max_seq_seen : 0;
    for (uint32_t m = from; m < x->rx.nchunks && named < DH_XFER_CREDIT_WINDOW; m++) {
        if (bit_get(x->rx.received, m))
            continue;
        bit_clear(x->rx.requested, m);
        rx_request_chunk(x, m, false, acts, &n, acts_cap, &credits);
        named++;
    }
    rx_grant(x->rx.id, acts, &n, acts_cap, credits);
    return n;
}

size_t dh_xfer_handle_done(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    uint16_t credits = 0;
    if (!x->rx.active || x->rx.lazy || x->rx.id != id)
        return 0;
    if (x->rx.nreceived < x->rx.nchunks) {
        /* DONE ends a round: the copy side has sent everything asked of it so
           far, so every hole in the payload is a loss and is asked for again,
           subject to the one-round ageing above. A round that ages every hole
           and so asks for nothing ends the exchange — there is no request for
           a DONE to come back behind — and that, like every other lost control
           message, is what dh_xfer_sweep_rx picks up. */
        rx_sweep_range(x, x->rx.nchunks, true, acts, &n, acts_cap, &credits);
        rx_grant(x->rx.id, acts, &n, acts_cap, credits);
        return n;
    }
    /* Since #132 the ordinary completion is in dh_xfer_handle_chunk, so this
       is reached only by a transfer whose set was already full on arrival: a
       zero-length offer, which has no chunk to complete it. (It also backstops
       a DELIVERED the chunk handler had no room to emit, though on that path
       nothing precedes it in the batch, so it never fails to fit.) */
    rx_complete(x, acts, &n, acts_cap);
    return n;
}

/* ---- both directions -------------------------------------------------- */

size_t dh_xfer_cancel_rx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    if (!x->rx.active)
        return 0;
    dh_xfer_action *a = emit(acts, &n, acts_cap);
    if (a) {
        a->type = DH_XFER_ACT_SEND_CANCEL;
        a->id = x->rx.id;
    }
    rx_reset(x);
    return n;
}

size_t dh_xfer_handle_cancel(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    if (x->rx.active && x->rx.id == id) {
        rx_reset(x);
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_FAILED;
            a->id = id;
            a->reason = DH_XFER_FAIL_CANCELLED;
        }
    }
    if (x->tx.active && x->tx.id == id) {
        tx_reset(x);
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_FAILED;
            a->id = id;
            a->reason = DH_XFER_FAIL_CANCELLED;
        }
    }
    return n;
}

size_t dh_xfer_link_down(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    /* This end's session is one of the two boundaries the far end's offer
       ordering has; dh_xfer_rx_seal_replaced is the other. */
    rx_abandon(x, DH_XFER_FAIL_LINK_DROP, acts, &n, acts_cap);
    if (x->tx.active) {
        uint32_t id = x->tx.id;
        tx_reset(x);
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_FAILED;
            a->id = id;
            a->reason = DH_XFER_FAIL_LINK_DROP;
        }
    }
    return n;
}

size_t dh_xfer_rx_seal_replaced(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    /* The copy side's offer ordering restarted with its process, so the id
       frontier held here is the dead namespace's and has to go. */
    rx_abandon(x, DH_XFER_FAIL_SEAL_REPLACED, acts, &n, acts_cap);
    return n;
}
