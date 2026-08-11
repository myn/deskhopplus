/*
 * deskhopplus shared core — the inter-board relay (#47). See dh_relay.h.
 */

#include "dh_relay.h"

#include <string.h>

static void slot_init(dh_relay_slot *s, uint8_t *buf, uint16_t cap) {
    s->buf = buf;
    s->cap = cap;
    s->len = 0;
    s->sent = 0;
    s->started = false;
}

static bool slot_idle(const dh_relay_slot *s) {
    return s->len == 0;
}

/* In flight: the start packet is out, so its fragments must stay contiguous. */
static bool slot_in_progress(const dh_relay_slot *s) {
    return s->len > 0 && s->started;
}

static bool slot_owes_packet(const dh_relay_slot *s) {
    return s->len > 0 && (!s->started || s->sent < s->len);
}

void dh_relay_tx_init(dh_relay_tx *t, uint8_t *priority_buf, uint16_t priority_cap,
                      uint8_t *bulk_buf, uint16_t bulk_cap) {
    memset(t, 0, sizeof *t);
    slot_init(&t->priority, priority_buf, priority_cap);
    slot_init(&t->bulk, bulk_buf, bulk_cap);
}

void dh_relay_tx_yield(dh_relay_tx *t) {
    t->burst = 0;
}

bool dh_relay_tx_busy(const dh_relay_tx *t) {
    return !slot_idle(&t->priority) || !slot_idle(&t->bulk);
}

dh_relay_result dh_relay_tx_offer(dh_relay_tx *t, const uint8_t *frame, size_t len) {
    /* Header only, and only for the two decisions the firmware may make.
       A malformed header is refused here rather than relayed onward. */
    dh_frame_header hdr;
    if (dh_frame_header_parse(frame, len, &hdr) != DH_FRAME_OK)
        return DH_RELAY_ERR_FRAME;
    if (len != (size_t)DH_FRAME_HEADER_SIZE + hdr.len)
        return DH_RELAY_ERR_FRAME;

    /* Both firmware decisions - priority and routing - are this comparison. */
    dh_relay_slot *slot = dh_msg_is_bulk(hdr.type) ? &t->bulk : &t->priority;

    if (!slot_idle(slot)) {
        t->refused++;
        return DH_RELAY_ERR_BUSY;
    }
    if (len > slot->cap) {
        t->refused++;
        return DH_RELAY_ERR_OVERSIZE;
    }

    memcpy(slot->buf, frame, len);
    slot->len = (uint16_t)len;
    slot->sent = 0;
    slot->started = false;
    return DH_RELAY_OK;
}

/*
 * Which slot owes the next packet. Bulk already in flight finishes first —
 * one reassembly context per direction means a priority frame spliced into it
 * would corrupt both. Otherwise priority goes ahead of bulk that is queued.
 */
static dh_relay_slot *next_slot(dh_relay_tx *t, bool *is_bulk) {
    if (slot_in_progress(&t->bulk) && slot_owes_packet(&t->bulk)) {
        *is_bulk = true;
        return &t->bulk;
    }
    if (slot_owes_packet(&t->priority)) {
        *is_bulk = false;
        return &t->priority;
    }
    if (slot_owes_packet(&t->bulk)) {
        *is_bulk = true;
        return &t->bulk;
    }
    return NULL;
}

bool dh_relay_tx_peek(dh_relay_tx *t, dh_relay_packet *out) {
    bool is_bulk = false;
    const dh_relay_slot *slot = next_slot(t, &is_bulk);
    if (slot == NULL)
        return false;

    /* The cap bounds bulk only: priority traffic is small and rare, and
       holding it back would defeat the point of having a priority band. */
    if (is_bulk && t->burst >= DH_RELAY_BURST_MAX)
        return false;

    memset(out->data, 0, sizeof out->data);
    out->len = DH_RELAY_PAYLOAD;

    if (!slot->started) {
        out->kind = DH_RELAY_PKT_START;
        out->data[0] = (uint8_t)(slot->len & 0xFFu);
        out->data[1] = (uint8_t)(slot->len >> 8);
        return true;
    }

    out->kind = DH_RELAY_PKT_DATA;
    const uint16_t remaining = (uint16_t)(slot->len - slot->sent);
    const uint16_t take = remaining < DH_RELAY_PAYLOAD ? remaining : DH_RELAY_PAYLOAD;
    memcpy(out->data, slot->buf + slot->sent, take);
    /* The tail of the last packet is zero padding: a full payload every time
       is what buys the wire its missing length field. */
    return true;
}

void dh_relay_tx_commit(dh_relay_tx *t) {
    bool is_bulk = false;
    dh_relay_slot *slot = next_slot(t, &is_bulk);
    if (slot == NULL)
        return;
    if (is_bulk && t->burst >= DH_RELAY_BURST_MAX)
        return;

    if (is_bulk)
        t->burst++;

    if (!slot->started) {
        slot->started = true;
        return;
    }

    const uint16_t remaining = (uint16_t)(slot->len - slot->sent);
    slot->sent += remaining < DH_RELAY_PAYLOAD ? remaining : DH_RELAY_PAYLOAD;

    if (slot->sent >= slot->len) {
        slot->len = 0;
        slot->sent = 0;
        slot->started = false;
    }
}

void dh_relay_rx_init(dh_relay_rx *r, uint8_t *buf, uint16_t cap) {
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->cap = cap;
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
