#include "channel_lifecycle.h"

#include <string.h>

#include "dh_place.h"

dh_outq *channel_lifecycle_output(channel_lifecycle *c, uint8_t index) {
    return index == 0 ? &c->out : &c->extra_out[index - 1];
}

static void reset_readers(channel_lifecycle *c) {
    dh_frame_reader_init(&c->reader);
    for (uint8_t i = 0; i < DH_SESSION_CHANNEL_COUNT - 1; ++i)
        dh_frame_reader_init(&c->extra_reader[i]);
}

static void reset_output(channel_lifecycle *c) {
    for (uint8_t i = 0; i < DH_SESSION_CHANNEL_COUNT; ++i)
        dh_outq_reset(channel_lifecycle_output(c, i));
    c->next_bulk = 0;
}

bool channel_lifecycle_queue(channel_lifecycle *c, const uint8_t *frame, size_t len,
                             uint32_t now) {
    channel_lifecycle_lock();
    const bool bulk = len > 0 && dh_msg_is_bulk(frame[0]);
    const uint8_t count = c->session.channel_count ? c->session.channel_count : 1;
    const uint8_t index = bulk ? c->next_bulk % count : 0;
    const bool queued = dh_outq_offer(channel_lifecycle_output(c, index), frame, len) == DH_OUTQ_OK;
    if (queued && bulk)
        c->next_bulk = (uint8_t)((index + 1u) % count);
    if (queued)
        dh_session_note_sent(&c->session, now);
    channel_lifecycle_unlock();
    return dh_txq_track(&c->tx, queued);
}

void channel_lifecycle_on_frame(channel_lifecycle *c, const dh_frame_view *frame,
                                uint32_t now) {
    uint8_t reply[DH_SESSION_REPLY_MAX];
    size_t reply_len = 0;
    const uint32_t registrations = c->pair.registrations;
    const dh_frame_result rc = dh_session_on_frame(&c->session, &c->pair, frame, now,
                                                  reply, sizeof reply, &reply_len);
    if (c->pair.registrations != registrations)
        c->registration_unsaved = true;
    /* The successful ack is the transition: refusal or missing entropy cannot
       discard a live stream. Do not use the saturating diagnostic session count.
       A reopened helper starts with an empty reader, so retire the partial tail
       and queued old tags before offering its ack. Reset preserves refusals. */
    if (rc == DH_FRAME_OK && reply_len > 0 && reply[0] == DH_MSG_HELLO_ACK) {
        channel_lifecycle_lock();
        reset_output(c);
        channel_lifecycle_unlock();
        reset_readers(c);
        c->report_used = 0;
    }
    if (rc == DH_FRAME_OK && reply_len > 0)
        (void)channel_lifecycle_queue(c, reply, reply_len, now);
}

/*
 * Everything that belongs to one connection: the session, the frame reader,
 * the relay and whatever was owed to a helper that is no longer there.
 * Identity, registration and the physical pairing window survive.
 */
void channel_lifecycle_link_lost(channel_lifecycle *c) {
    dh_session_drop(&c->session);
    reset_readers(c);
    dh_relay_tx_reset(&c->relay_tx);
    dh_relay_rx_reset(&c->relay_rx);

    c->report_head = 0;
    c->report_used = 0;
    c->stream_broken = false;
    dh_inq_reset(&c->inbound);

    /* Reset, never init: what is queued belonged to the link that just went,
       and the drop totals did not (#142). */

    channel_lifecycle_lock();
    c->cursor_query_origin = CURSOR_QUERY_NONE;
    c->cursor_query_id = 0;
    reset_output(c);
    channel_lifecycle_unlock();
}

/*
 * End the session and tell the helper why — best effort. A refused queue
 * leaves it to notice for itself, which is precisely what its own timeout is
 * for: this is an optimisation over that timeout, never a substitute for it.
 */
static void end_session(channel_lifecycle *c, uint8_t reason, uint32_t now) {
    uint8_t frame[DH_SESSION_REPLY_MAX];
    size_t len = 0;
    /* Zero for every reason but a liveness timeout, which is the one that is
       asserting something about a clock and the one #107 needs to read. */
    if (dh_session_end(&c->session, reason, 0, frame, sizeof frame, &len) == DH_FRAME_OK &&
        len > 0)
        (void)channel_lifecycle_queue(c, frame, len, now);
}

void channel_lifecycle_receive_report(channel_lifecycle *c, const uint8_t *buffer,
                                       uint16_t bufsize) {
    channel_lifecycle_receive_channel_report(c, 0, buffer, bufsize);
}

void channel_lifecycle_receive_channel_report(channel_lifecycle *c, uint8_t index,
                                               const uint8_t *buffer, uint16_t bufsize) {
    if (bufsize == 0 || index >= DH_SESSION_CHANNEL_COUNT)
        return;

    /* Before the backlog check, so this is what arrived rather than what fitted. */
    if (c->reports_in != UINT32_MAX)
        c->reports_in++;

    if (c->report_used >= CHANNEL_REPORT_BACKLOG) {
        /*
         * Counted, never silent (#43) — and no longer only counted.
         *
         * A lost report does not lose one frame. The reader is a byte stream:
         * the hole is filled from the frames behind it, and the reader goes on
         * waiting for a body length it read before the loss. Nothing completes,
         * so nothing authenticates, so `last_seen_ms` stops moving — and three
         * seconds later this board evicts a helper that has been writing the
         * whole time, with no refusals to show for it. That is #161's exact
         * signature, and `test_a_gap_mid_frame_is_not_recoverable` is the proof
         * that no amount of waiting recovers.
         *
         * So the session ends instead. The helper reopens its handles and gets
         * a clean stream in about a second, which is the cheapest honest answer
         * to a stream that can no longer be trusted — and the same one this
         * board already gives a frame that will not decode.
         */
        c->reports_dropped++;
        c->stream_broken = true;
        return;
    }

    const uint8_t slot = (uint8_t)((c->report_head + c->report_used) %
                                   CHANNEL_REPORT_BACKLOG);
    const uint16_t take = bufsize < CHANNEL_REPORT_SIZE ? bufsize : CHANNEL_REPORT_SIZE;
    memcpy(c->reports[slot], buffer, take);
    c->report_len[slot] = take;
    c->report_channel[slot] = index;
    c->report_used++;
}

/*
 * A bulk frame the helper authenticated, on its way to the peer board.
 *
 * What crosses the inter-board link is the frame **without** its
 * authentication prefix: the tag is per hop, board A's means nothing to
 * board B, and board B writes its own before emitting it. Sending the dead
 * prefix would cost 24 bytes a frame on the link ADR-0002 measured as the wall
 * for no reader anywhere.
 *
 * The shortened frame is built in place, over the last four bytes of the tag
 * that has just been verified and will never be read again — so a 1 KB chunk
 * is relayed without a second buffer to hold it in. The reader's own header at
 * the front of its buffer is untouched, which is what dh_frame_reader_push
 * uses on the next call to release the frame it returned.
 */
static void relay_to_peer(channel_lifecycle *c, const dh_frame_view *frame, const uint8_t *body,
                                  size_t body_len) {
    uint8_t *header = (uint8_t *)body - DH_FRAME_HEADER_SIZE;
    header[0] = frame->hdr.type;
    header[1] = frame->hdr.flags;
    header[2] = (uint8_t)(body_len & 0xFFu);
    header[3] = (uint8_t)(body_len >> 8);

    /*
     * A refusal means the relay's queue is full, not merely that the previous
     * frame is still fragmenting — that burst is what the queue absorbs now
     * (#69, ADR-0005). Nothing here can hold the frame if it is refused, since
     * the reader releases it on the next push, so it is counted rather than
     * silently dropped (#43). Making the helper wait instead is the credit
     * window's job, and that window is end to end between the helpers: the
     * board may not enforce it without reading a payload (ADR-0003).
     */
    const dh_relay_result offered =
        dh_relay_tx_offer(&c->relay_tx, header, DH_FRAME_HEADER_SIZE + body_len);
    (void)dh_txq_track(&c->tx, offered == DH_RELAY_OK);
}

static void on_frame(channel_lifecycle *c, const dh_frame_view *frame, uint32_t now,
                     void *context) {
    if (dh_msg_is_bulk(frame->hdr.type) ||
        (frame->hdr.type >= DH_MSG_PLACE && frame->hdr.type <= DH_MSG_POS_RESPONSE)) {
        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (dh_session_authenticate(&c->session, frame, now, &body, &body_len) != DH_AUTH_OK)
            return;
        if (dh_msg_is_bulk(frame->hdr.type))
            relay_to_peer(c, frame, body, body_len);
        else if (frame->hdr.type == DH_MSG_POS_RESPONSE)
            channel_lifecycle_position(context, body, body_len);
        return;
    }
    channel_lifecycle_on_frame(c, frame, now);
}

/*
 * Drain the reports the USB callback left, decoding frames out of the stream.
 *
 * `now` is the caller's, never a fresh read. This read the clock for itself
 * once, and stamped the session's liveness deadline with a value *later* than
 * the one channel_task then judged that deadline against — so a millisecond
 * turning over between the two reads, while a frame happened to arrive, left
 * the stamp one ahead of the clock and the difference wrapped. The board then
 * evicted a helper it had heard from that instant (#107).
 */
static void drain_reports(channel_lifecycle *c, uint32_t now, void *context) {

    while (c->report_used > 0) {
        const uint8_t slot = c->report_head;
        const uint8_t *buffer = c->reports[slot];
        const uint16_t bufsize = c->report_len[slot];
        const uint8_t index = c->report_channel[slot];
        dh_frame_reader *reader = index == 0 ? &c->reader : &c->extra_reader[index - 1];
        c->report_head = (uint8_t)((c->report_head + 1u) % CHANNEL_REPORT_BACKLOG);
        c->report_used--;

        size_t offset = 0;
        while (offset < bufsize) {
            dh_frame_view frame;
            size_t consumed = 0;
            const dh_frame_result rc = dh_frame_reader_push(
                reader, buffer + offset, bufsize - offset, &consumed, &frame);

            if (rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN) {
                /* A protocol error drops the session: the stream is no longer
                   trustworthy and the helper reconnects (docs/protocol.md). It
                   is told so rather than left to time out, because until it
                   finds out it goes on writing into a reader it has
                   desynchronised — and this is the one path where the helper is
                   the thing in the wrong and could stop. */
                end_session(c, DH_SESSION_END_PROTOCOL_ERROR, now);
                reset_readers(c);
                return;
            }

            offset += consumed;

            if (rc == DH_FRAME_OK) {
                if (index == 0 || (index < c->session.channel_count &&
                                   dh_msg_is_bulk(frame.hdr.type)))
                    on_frame(c, &frame, now, context);
            } else if (consumed == 0)
                break; /* nothing more to take from this report */
        }
    }
}

/* Drain what the relay owes into the shared inter-board queue. The burst cap
   inside the relay is what keeps a chunk's packets from filling the queue
   ahead of keyboard and mouse traffic. */
static void pump_relay(channel_lifecycle *c) {
    dh_relay_tx_yield(&c->relay_tx);

    dh_relay_packet packet;
    while (dh_relay_tx_peek(&c->relay_tx, &packet)) {
        /* A refused enqueue leaves the packet owed rather than lost: a frame
           missing one data packet would corrupt everything after it. */
        if (!channel_lifecycle_send_relay(&packet))
            break;

        dh_relay_tx_commit(&c->relay_tx);
    }
}

void channel_lifecycle_config_wiped(channel_lifecycle *c, uint32_t now) {
    end_session(c, DH_SESSION_END_UNPAIRED, now);
    dh_pair_clear_registration(&c->pair);
    c->registration_unsaved = false;
}

bool channel_lifecycle_emit_placement(channel_lifecycle *c, uint8_t type, const uint8_t *body,
                                      size_t body_len, uint32_t now) {
    if (body_len > DH_PLACE_BODY_SIZE)
        return false;
    uint8_t frame_bytes[DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + DH_PLACE_BODY_SIZE];
    size_t frame_len = 0;
    const dh_frame_view frame = {
        .hdr = {.type = type, .flags = 0, .len = (uint16_t)body_len},
        .payload = body,
    };
    return dh_session_emit_relayed(&c->session, &frame, frame_bytes, sizeof frame_bytes,
                                   &frame_len) == DH_FRAME_OK &&
           channel_lifecycle_queue(c, frame_bytes, frame_len, now);
}

/*
 * Whatever the peer board handed over, tagged for this board's helper.
 *
 * Drained to exhaustion rather than one frame per pass. One per pass caps this
 * seam at 1000 frames a second, which sounds ample and is not: the burst that
 * overruns it is short frames — a relayed CLIP_CREDIT is ten bytes — and a
 * batch of those crosses the link inside a fraction of one pass. The ring
 * parks them; taking only one of them per pass would simply move where they
 * are lost (#139).
 *
 * The loop stops on a refused enqueue instead of running the ring dry into a
 * full queue. The frames behind it stay parked and go out on a later pass,
 * which is the back-pressure this seam otherwise has none of.
 *
 * What this costs per pass is a tag per frame, and that cost is not yet
 * measured (#115). It is bounded by DH_INQ_DEPTH and, in the traffic that
 * fills the ring, small: the ring fills with short frames, because a full
 * chunk takes about 4 ms to cross the link and is drained long before a second
 * one lands. A ring full of full-size chunks is not reachable at the rate the
 * link delivers them — if #115 finds otherwise, this is the loop to bound.
 */
static void pump_inbound(channel_lifecycle *c, uint32_t now) {
    const uint8_t *at = NULL;
    uint16_t len = 0;

    while (dh_inq_peek(&c->inbound, &at, &len)) {
        dh_frame_view frame;
        size_t consumed = 0;
        size_t tagged_len = 0;

        const bool tagged =
            dh_frame_decode(at, len, &frame, &consumed) == DH_FRAME_OK &&
            /*
             * The same gate as the outbound direction, and for the sharper
             * reason: without it, a local process that holds this board's
             * channel and never authenticates is still handed everything the
             * *other* computer's paired helper sends. That is precisely the
             * cross-machine path #34 exists to close, and it is not closed by
             * refusing to relay outward alone. dh_session_emit_relayed refuses
             * outright when there is no session, because without one there is
             * no key to tag under either.
             */
            dh_session_emit_relayed(&c->session, &frame, c->tagged,
                                    sizeof c->tagged, &tagged_len) == DH_FRAME_OK;

        /* Read before the slot goes back to core 1. */
        channel_lifecycle_barrier();
        dh_inq_release(&c->inbound);

        /* A frame the queue refused is lost and counted there, as it always
           was. What is new is that the rest of the ring is not lost with it. */
        if (tagged && !channel_lifecycle_queue(c, c->tagged, tagged_len, now))
            break;
    }
}

static void pump_query(channel_lifecycle *c, uint32_t now, void *context) {
    channel_lifecycle_lock();
    const channel_query_origin query_origin = c->cursor_query_origin;
    const uint8_t query_id = c->cursor_query_id;
    channel_lifecycle_unlock();
    bool query_finished = false;
    if (query_origin != CURSOR_QUERY_NONE) {
        if (!c->session.present) {
            if (query_origin == CURSOR_QUERY_PEER) {
                query_finished = channel_lifecycle_query_unavailable(context, query_id);
            } else {
                query_finished = true;
            }
        } else {
            const uint8_t body[] = {query_id};
            query_finished = channel_lifecycle_emit_placement(c, DH_MSG_POS_QUERY, body,
                                                               sizeof body, now);
        }
        if (query_finished) {
            channel_lifecycle_lock();
            if (c->cursor_query_origin == query_origin &&
                c->cursor_query_id == query_id)
                c->cursor_query_origin = CURSOR_QUERY_NONE;
            channel_lifecycle_unlock();
        }
    }
}

void channel_lifecycle_step(channel_lifecycle *c, uint32_t now, void *context) {
    /* A missing report invalidates every byte parked behind the gap. Drop
       backlog before decoding, but retain relay/outbound work as before. */
    if (c->stream_broken) {
        c->stream_broken = false;
        c->report_head = 0;
        c->report_used = 0;
        end_session(c, DH_SESSION_END_STREAM_GAP, now);
        reset_readers(c);
    }
    drain_reports(c, now, context);
    pump_query(c, now, context);
    if (c->registration_unsaved) {
        c->registration_unsaved = false;
        channel_lifecycle_save_registration(context);
    }
    pump_inbound(c, now);
    channel_lifecycle_update_config(context, now);
    uint8_t owed[DH_SESSION_REPLY_MAX];
    size_t len = 0;
    if (dh_session_tick(&c->session, now, owed, sizeof owed, &len) == DH_FRAME_OK &&
        len > 0 && channel_lifecycle_queue(c, owed, len, now))
        dh_session_note_owed_sent(&c->session, owed[0]);
    dh_pair_tick(&c->pair, now);
    pump_relay(c);
}
