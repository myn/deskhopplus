/*
 * deskhopplus shared core — the inter-board relay (#47). See dh_relay.h.
 */

#include "dh_relay.h"

#include <string.h>

void dh_relay_tx_init(dh_relay_tx *t) {
    memset(t, 0, sizeof *t);
    dh_outq_init(&t->q);
}

void dh_relay_tx_reset(dh_relay_tx *t) {
    /* Field by field, not memset-and-restore: dh_outq is 7.5 KB and holding a
       copy of one would put it on core 0's 2 KB stack — the same overrun
       channel.c keeps its relayed-frame buffer static to avoid. */
    t->burst = 0;
    dh_outq_reset(&t->q);
}

void dh_relay_tx_yield(dh_relay_tx *t) {
    t->burst = 0;
}

bool dh_relay_tx_busy(const dh_relay_tx *t) {
    return dh_outq_busy(&t->q);
}

dh_relay_result dh_relay_tx_offer(dh_relay_tx *t, const uint8_t *frame, size_t len) {
    switch (dh_outq_offer(&t->q, frame, len)) {
    case DH_OUTQ_OK:
        return DH_RELAY_OK;
    case DH_OUTQ_ERR_BUSY:
        return DH_RELAY_ERR_BUSY;
    case DH_OUTQ_ERR_OVERSIZE:
        return DH_RELAY_ERR_OVERSIZE;
    default:
        return DH_RELAY_ERR_FRAME;
    }
}

/*
 * What the wire owes next, once the burst cap has had its say. The cap bounds
 * bulk only: priority traffic is small and rare, and holding it back would
 * defeat the point of having a priority band.
 */
static bool relay_owed(dh_relay_tx *t, dh_outq_view *view) {
    if (!dh_outq_peek(&t->q, view))
        return false;
    return !(view->bulk && t->burst >= DH_RELAY_BURST_MAX);
}

/* Bytes of the frame this packet carries — the last one is part padding. */
static uint16_t packet_take(const dh_outq_view *view) {
    return view->remaining < DH_RELAY_PAYLOAD ? view->remaining : DH_RELAY_PAYLOAD;
}

bool dh_relay_tx_peek(dh_relay_tx *t, dh_relay_packet *out) {
    dh_outq_view view;
    if (!relay_owed(t, &view))
        return false;

    memset(out->data, 0, sizeof out->data);
    out->len = DH_RELAY_PAYLOAD;

    /* Each frame is prefixed with its total length: the wire has no length
       field of its own, and this is what the reassembler sizes against. */
    if (view.preamble_owed) {
        out->kind = DH_RELAY_PKT_START;
        out->data[0] = (uint8_t)(view.total & 0xFFu);
        out->data[1] = (uint8_t)(view.total >> 8);
        return true;
    }

    out->kind = DH_RELAY_PKT_DATA;
    memcpy(out->data, view.at, packet_take(&view));
    /* The tail of the last packet is zero padding: a full payload every time
       is what buys the wire its missing length field. */
    return true;
}

void dh_relay_tx_commit(dh_relay_tx *t) {
    dh_outq_view view;
    if (!relay_owed(t, &view))
        return;

    if (view.bulk)
        t->burst++;

    if (view.preamble_owed)
        dh_outq_note_preamble(&t->q, &view);
    else
        dh_outq_advance(&t->q, &view, packet_take(&view));
}

void dh_relay_rx_init(dh_relay_rx *r, uint8_t *buf, uint16_t cap) {
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->cap = cap;
}

void dh_relay_rx_reset(dh_relay_rx *r) {
    /* Abandon the frame in progress, keep the buffer it was using and the two
       totals. Not dh_relay_rx_init with the same arguments, because a caller
       that had to hand the buffer back would be a caller that could hand back
       the wrong one. */
    r->expected = 0;
    r->have = 0;
}

dh_relay_result dh_relay_rx_push(dh_relay_rx *r, const dh_relay_packet *packet,
                                 dh_frame_view *out) {
    if (packet->kind == DH_RELAY_PKT_START) {
        const uint16_t total = (uint16_t)(packet->data[0] | ((uint16_t)packet->data[1] << 8));
        /* A start arriving mid-frame is how a lost data packet shows up: the
           frame before it ended short. It is abandoned, never delivered. */
        const bool interrupted = r->expected != 0;
        if (interrupted)
            r->truncated++;

        r->expected = 0;
        r->have = 0;

        if (total < DH_FRAME_HEADER_SIZE || total > r->cap || total > DH_FRAME_MAX_SIZE)
            return DH_RELAY_ERR_OVERSIZE;

        r->expected = total;
        return interrupted ? DH_RELAY_ERR_TRUNCATED : DH_RELAY_AGAIN;
    }

    if (r->expected == 0) {
        /* Orphaned data — the start was lost. Discarded rather than
           misinterpreted: the packet kinds differ, so this is detectable
           without spending a byte on a sequence number. */
        r->orphans++;
        return DH_RELAY_ERR_ORPHAN;
    }

    const uint16_t remaining = (uint16_t)(r->expected - r->have);
    const uint16_t take = remaining < packet->len ? remaining : packet->len;
    memcpy(r->buf + r->have, packet->data, take);
    r->have = (uint16_t)(r->have + take);

    if (r->have < r->expected)
        return DH_RELAY_AGAIN;

    const uint16_t total = r->expected;
    r->expected = 0;
    r->have = 0;

    /* Header-only, again: enough to hand back a view, never a payload read. */
    size_t consumed = 0;
    if (dh_frame_decode(r->buf, total, out, &consumed) != DH_FRAME_OK || consumed != total)
        return DH_RELAY_ERR_FRAME;

    return DH_RELAY_OK;
}
