/*
 * deskhopplus shared core — the device's outbound frame queue. See dh_outq.h.
 */

#include "dh_outq.h"

#include <string.h>

typedef enum {
    BAND_NONE = 0,
    BAND_PRIORITY,
    BAND_BULK,
} band_id;

/*
 * A band is in flight once anything of its frame has reached the transport —
 * bytes, or the preamble that commits the far end to expecting them. That is
 * the point after which the frame must finish before another may begin.
 */
static bool band_in_flight(const dh_outq_band *b) {
    return b->len > 0 && (b->sent > 0 || b->preamble_done);
}

static void band_clear(dh_outq_band *b) {
    b->len = 0;
    b->sent = 0;
    b->preamble_done = false;
}

/*
 * Which band owes the next thing. Bulk already in flight finishes first;
 * otherwise priority goes ahead of bulk that is merely queued. At most one
 * band is ever in flight, which is what these three cases guarantee: a band
 * can only start while the other is not in flight.
 */
static band_id owed_band(const dh_outq *q) {
    if (band_in_flight(&q->bulk))
        return BAND_BULK;
    if (q->priority.len > 0)
        return BAND_PRIORITY;
    if (q->bulk.len > 0)
        return BAND_BULK;
    return BAND_NONE;
}

/* The next queued frame becomes the one in flight. Done as soon as its
   predecessor completes, so an empty in-flight buffer always means an empty
   band — which is what lets dh_outq_offer test that one field alone. */
static void promote(dh_outq *q) {
    band_clear(&q->bulk);
    if (q->stage_used == 0)
        return;

    const uint16_t len = q->stage_len[q->stage_first];
    /*
     * One chunk-sized copy per frame drained, ~1 KiB. Cheap against the ~16
     * ticks of USB drain it buys, and it is what keeps the in-flight buffer
     * frame-sized while the queued slots stay small.
     */
    memcpy(q->bulk_buf, q->stage[q->stage_first], len);
    q->bulk.len = len;
    q->stage_first = (uint8_t)((q->stage_first + 1u) % DH_OUTQ_DEPTH);
    q->stage_used--;
}

static void promote_priority(dh_outq *q) {
    band_clear(&q->priority);
    if (q->priority_stage_used == 0)
        return;

    const uint16_t len = q->priority_stage_len[q->priority_stage_first];
    memcpy(q->priority_buf, q->priority_stage[q->priority_stage_first], len);
    q->priority.len = len;
    q->priority_stage_first =
        (uint8_t)((q->priority_stage_first + 1u) % DH_OUTQ_PRIORITY_DEPTH);
    q->priority_stage_used--;
}

/* Every refusal is counted twice: once in the total the wire has always
   carried, and once against the cause, which is what a reader needs to know
   which seam to act on (#142). */
static void refuse(dh_outq *q, uint32_t *cause) {
    if (q->refused < UINT32_MAX)
        q->refused++;
    if (*cause < UINT32_MAX)
        (*cause)++;
}

void dh_outq_init(dh_outq *q) {
    memset(q, 0, sizeof *q);
}

void dh_outq_reset(dh_outq *q) {
    const uint32_t refused = q->refused;
    const uint32_t priority = q->refused_priority;
    const uint32_t bulk = q->refused_bulk;
    const uint32_t frame = q->refused_bad_header;

    dh_outq_init(q);

    q->refused = refused;
    q->refused_priority = priority;
    q->refused_bulk = bulk;
    q->refused_bad_header = frame;
}

dh_outq_result dh_outq_offer(dh_outq *q, const uint8_t *frame, size_t len) {
    /* Header only, and only for the two decisions the firmware may make: which
       band the frame is in, and whether it is well-formed enough to carry. */
    dh_frame_header hdr;
    if (dh_frame_header_parse(frame, len, &hdr) != DH_FRAME_OK ||
        len != (size_t)DH_FRAME_HEADER_SIZE + hdr.len) {
        /* Counted like any other refusal. A peer emitting a type this build
           does not know drops every frame here, and a counter that stayed at
           zero through that would hide exactly the version skew it should
           make obvious. */
        refuse(q, &q->refused_bad_header);
        return DH_OUTQ_ERR_FRAME;
    }

    if (!dh_msg_is_bulk(hdr.type)) {
        if (len > DH_OUTQ_PRIORITY_MAX) {
            refuse(q, &q->refused_priority);
            return DH_OUTQ_ERR_OVERSIZE;
        }
        if (q->priority.len > 0) {
            if (q->priority_stage_used >= DH_OUTQ_PRIORITY_DEPTH) {
                refuse(q, &q->refused_priority);
                return DH_OUTQ_ERR_BUSY;
            }
            const uint8_t at = (uint8_t)((q->priority_stage_first + q->priority_stage_used) %
                                         DH_OUTQ_PRIORITY_DEPTH);
            memcpy(q->priority_stage[at], frame, len);
            q->priority_stage_len[at] = (uint16_t)len;
            q->priority_stage_used++;
            return DH_OUTQ_OK;
        }
        memcpy(q->priority_buf, frame, len);
        band_clear(&q->priority);
        q->priority.len = (uint16_t)len;
        return DH_OUTQ_OK;
    }

    /*
     * The in-flight buffer takes any frame the wire format allows, so an
     * outsized one-off — a CLIP_OFFER carrying a long file list — stays
     * carryable. The header parse has already bounded len by DH_FRAME_MAX_SIZE.
     */
    if (q->bulk.len == 0) {
        memcpy(q->bulk_buf, frame, len);
        band_clear(&q->bulk);
        q->bulk.len = (uint16_t)len;
        return DH_OUTQ_OK;
    }

    /* Anything else has to wait in a queued slot, which holds a chunk. Too long
       for one is refused as busy, not oversize: it fits the moment the
       in-flight buffer is free, so the caller's remedy is to retry. */
    if (len > DH_OUTQ_STAGE_MAX || q->stage_used >= DH_OUTQ_DEPTH) {
        refuse(q, &q->refused_bulk);
        return DH_OUTQ_ERR_BUSY;
    }

    const uint8_t at = (uint8_t)((q->stage_first + q->stage_used) % DH_OUTQ_DEPTH);
    memcpy(q->stage[at], frame, len);
    q->stage_len[at] = (uint16_t)len;
    q->stage_used++;
    return DH_OUTQ_OK;
}

dh_outq_result dh_outq_offer_pair(dh_outq *q, const uint8_t *first, size_t first_len,
                                  const uint8_t *second, size_t second_len) {
    dh_frame_header first_hdr;
    dh_frame_header second_hdr;
    if (dh_frame_header_parse(first, first_len, &first_hdr) != DH_FRAME_OK ||
        first_len != (size_t)DH_FRAME_HEADER_SIZE + first_hdr.len ||
        dh_frame_header_parse(second, second_len, &second_hdr) != DH_FRAME_OK ||
        second_len != (size_t)DH_FRAME_HEADER_SIZE + second_hdr.len) {
        refuse(q, &q->refused_bad_header);
        return DH_OUTQ_ERR_FRAME;
    }
    if (dh_msg_is_bulk(first_hdr.type) || dh_msg_is_bulk(second_hdr.type)) {
        refuse(q, &q->refused_priority);
        return DH_OUTQ_ERR_FRAME;
    }
    if (first_len > DH_OUTQ_PRIORITY_MAX || second_len > DH_OUTQ_PRIORITY_MAX) {
        refuse(q, &q->refused_priority);
        return DH_OUTQ_ERR_OVERSIZE;
    }
    if (q->priority.len > 0 || q->priority_stage_used > 0 || band_in_flight(&q->bulk)) {
        refuse(q, &q->refused_priority);
        return DH_OUTQ_ERR_BUSY;
    }

    memcpy(q->priority_buf, first, first_len);
    band_clear(&q->priority);
    q->priority.len = (uint16_t)first_len;
    memcpy(q->priority_stage[0], second, second_len);
    q->priority_stage_len[0] = (uint16_t)second_len;
    q->priority_stage_first = 0;
    q->priority_stage_used = 1;
    return DH_OUTQ_OK;
}

bool dh_outq_peek(const dh_outq *q, dh_outq_view *out) {
    const band_id band = owed_band(q);
    if (band == BAND_NONE)
        return false;

    const bool bulk = (band == BAND_BULK);
    const dh_outq_band *b = bulk ? &q->bulk : &q->priority;
    const uint8_t *buf = bulk ? q->bulk_buf : q->priority_buf;

    out->at = buf + b->sent;
    out->remaining = (uint16_t)(b->len - b->sent);
    out->total = b->len;
    out->bulk = bulk;
    out->preamble_owed = !b->preamble_done;
    return true;
}

/*
 * Both of these credit the band the caller was shown, not whatever is owed
 * now. Only the draining side advances, so that band's frame cannot have
 * changed underneath it — but a frame offered from another core can change
 * which band is owed *next*, and crediting these bytes to that one would
 * corrupt two frames at once. An empty band clamps to zero and does nothing.
 *
 * Only `bulk` is read from the view, deliberately: a caller may hold it across
 * an unlock (channel.c does, around its USB write), by which point `at` and
 * `remaining` may describe a frame that has since been promoted away. The band
 * a frame belongs to is the one thing that cannot go stale.
 */
void dh_outq_advance(dh_outq *q, const dh_outq_view *view, uint16_t n) {
    dh_outq_band *b = view->bulk ? &q->bulk : &q->priority;
    if (b->len == 0)
        return;

    const uint16_t remaining = (uint16_t)(b->len - b->sent);
    b->sent = (uint16_t)(b->sent + (n < remaining ? n : remaining));

    if (b->sent < b->len)
        return;

    if (view->bulk)
        promote(q);
    else
        promote_priority(q);
}

void dh_outq_note_preamble(dh_outq *q, const dh_outq_view *view) {
    dh_outq_band *b = view->bulk ? &q->bulk : &q->priority;
    if (b->len == 0)
        return;
    b->preamble_done = true;
}

bool dh_outq_busy(const dh_outq *q) {
    return q->priority.len > 0 || q->bulk.len > 0;
}
