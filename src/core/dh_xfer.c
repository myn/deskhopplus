/*
 * The chunked transfer state machine. Semantics: docs/protocol.md,
 * "Transfer semantics". Ported from mkroamer's clip-transfer core; the
 * reliability additions (CRC32, selective retransmit, credit window,
 * abandon-on-drop) are this project's (#32, #48).
 *
 * Chunk emission happens only in dh_xfer_pump — handlers record state and
 * emit control actions. Callers pump after handling each message and
 * whenever the bulk band has room. Recovery from a lost *control* message
 * (a retransmit request, a credit grant) is the helper's timeout, not this
 * machine's: it has no clock, and an interrupted transfer abandons.
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

static void rx_reset(dh_xfer *x) {
    x->rx.active = false;
}

void dh_xfer_init(dh_xfer *x, uint8_t *rx_buf, size_t rx_cap) {
    memset(x, 0, sizeof *x);
    x->rx_buf = rx_buf;
    x->rx_cap = rx_cap;
    x->next_id = 1;
}

/* ---- sender ----------------------------------------------------------- */

size_t dh_xfer_offer(dh_xfer *x, uint8_t kind, const uint8_t *meta, uint16_t meta_len,
                     const uint8_t *data, uint64_t total, dh_xfer_action *acts,
                     size_t acts_cap) {
    size_t n = 0;
    tx_reset(x); /* a newer offer supersedes whatever was in flight */
    x->tx.active = true;
    x->tx.id = x->next_id++;
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

size_t dh_xfer_provide(dh_xfer *x, const uint8_t *data, dh_xfer_action *acts, size_t acts_cap) {
    (void)acts;
    (void)acts_cap;
    if (!x->tx.active || !x->tx.lazy_pending)
        return 0;
    x->tx.data = data;
    x->tx.lazy_pending = false;
    x->tx.streaming = true;
    x->tx.need_done = true;
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
    x->tx.streaming = true;
    x->tx.need_done = true;
    return n; /* chunks flow from the next pump */
}

size_t dh_xfer_handle_retransmit(dh_xfer *x, uint32_t id, uint32_t seq, dh_xfer_action *acts,
                                 size_t acts_cap) {
    (void)acts;
    (void)acts_cap;
    if (!x->tx.active || x->tx.id != id || seq >= x->tx.nchunks)
        return 0;
    tx_queue_retransmit(x, seq);
    x->tx.need_done = true; /* repeat DONE after servicing */
    return 0;
}

size_t dh_xfer_handle_credit(dh_xfer *x, uint32_t id, uint16_t credits, dh_xfer_action *acts,
                             size_t acts_cap) {
    (void)acts;
    (void)acts_cap;
    if (!x->tx.active || x->tx.id != id)
        return 0; /* stale grant for a superseded or finished transfer */
    x->tx.credits += credits;
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

size_t dh_xfer_handle_offer(dh_xfer *x, const dh_clip_offer *offer, dh_xfer_action *acts,
                            size_t acts_cap) {
    size_t n = 0;
    const uint32_t nchunks = chunk_count(offer->total);
    /* A newer offer supersedes an incomplete incoming transfer. */
    memset(&x->rx, 0, sizeof x->rx);
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
    x->rx.id = offer->id;
    x->rx.kind = offer->kind;
    x->rx.total = offer->total;
    x->rx.nchunks = nchunks;
    x->rx.meta_len = offer->meta_len;
    if (offer->meta_len > 0)
        memcpy(x->rx.meta, offer->meta, offer->meta_len);
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
    if (!x->rx.active || x->rx.id != chunk->id || chunk->seq >= x->rx.nchunks)
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

size_t dh_xfer_handle_done(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap) {
    size_t n = 0;
    uint16_t credits = 0;
    if (!x->rx.active || x->rx.id != id)
        return 0;
    if (x->rx.nreceived < x->rx.nchunks) {
        /* DONE ends a round: the copy side has sent everything asked of it
           so far. A missing chunk with a fresh outstanding request is left
           alone once — its retransmission is behind this DONE in the FIFO —
           but aged, so if it is still missing at the next DONE its
           retransmission was itself lost and it is asked for again. Every
           retransmit round ends in another DONE, so the exchange converges;
           only a request with no DONE behind it is left to the helper's
           transfer timeout. */
        for (uint32_t m = 0; m < x->rx.nchunks; m++) {
            if (bit_get(x->rx.received, m))
                continue;
            if (bit_get(x->rx.requested, m) && !bit_get(x->rx.requested_stale, m)) {
                bit_set(x->rx.requested_stale, m); /* skip once, age */
                continue;
            }
            bit_clear(x->rx.requested, m);
            rx_request_chunk(x, m, true, acts, &n, acts_cap, &credits);
        }
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
    if (x->rx.active) {
        rx_reset(x); /* the partial payload is never delivered */
        dh_xfer_action *a = emit(acts, &n, acts_cap);
        if (a) {
            a->type = DH_XFER_ACT_FAILED;
            a->id = x->rx.id;
            a->reason = DH_XFER_FAIL_LINK_DROP;
        }
    }
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
