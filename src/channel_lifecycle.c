#include "channel_lifecycle.h"

bool channel_lifecycle_queue(channel_lifecycle *c, const uint8_t *frame, size_t len,
                             uint32_t now) {
    channel_lifecycle_lock();
    const bool queued = dh_outq_offer(&c->out, frame, len) == DH_OUTQ_OK;
    if (queued)
        dh_session_note_sent(&c->session, now);
    channel_lifecycle_unlock();
    return dh_txq_track(&c->tx, queued);
}

void channel_lifecycle_on_frame(channel_lifecycle *c, const dh_frame_view *frame,
                                uint32_t now) {
    uint8_t reply[DH_SESSION_REPLY_MAX];
    size_t reply_len = 0;
    const dh_frame_result rc = dh_session_on_frame(&c->session, &c->pair, frame, now,
                                                  reply, sizeof reply, &reply_len);
    /* The successful ack is the transition: refusal or missing entropy cannot
       discard a live stream. Do not use the saturating diagnostic session count.
       A reopened helper starts with an empty reader, so retire the partial tail
       and queued old tags before offering its ack. Reset preserves refusals. */
    if (rc == DH_FRAME_OK && reply_len > 0 && reply[0] == DH_MSG_HELLO_ACK) {
        channel_lifecycle_lock();
        dh_outq_reset(&c->out);
        channel_lifecycle_unlock();
    }
    if (rc == DH_FRAME_OK && reply_len > 0)
        (void)channel_lifecycle_queue(c, reply, reply_len, now);
}
