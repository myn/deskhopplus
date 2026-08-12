/*
 * deskhopplus shared core — the session layer (#45). See dh_session.h.
 */

#include "dh_session.h"

#include <string.h>

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

bool dh_hello_decode(const uint8_t *payload, size_t len, dh_hello *out) {
    if (payload == NULL || out == NULL) return false;
    if (len < DH_HELLO_FIXED_LEN) return false;

    const size_t token_len = len - DH_HELLO_FIXED_LEN;
    if (token_len > DH_HELLO_TOKEN_MAX) return false;

    out->proto_version = rd_u16(payload);
    out->os = payload[2];
    out->build_type = payload[3];
    out->channel_count = payload[4];
    out->max_chunk = rd_u16(payload + 5);
    out->token = token_len ? payload + DH_HELLO_FIXED_LEN : NULL;
    out->token_len = (uint16_t)token_len;
    return true;
}

dh_frame_result dh_hello_encode(const dh_hello *in, uint8_t *out, size_t cap, size_t *out_len) {
    /* A length without a pointer is a caller bug, not a short token: the
       bindings build this struct by hand, so it is checked rather than
       assumed. */
    if (in == NULL || in->token_len > DH_HELLO_TOKEN_MAX) return DH_FRAME_ERR_BUFFER;
    if (in->token == NULL && in->token_len > 0) return DH_FRAME_ERR_BUFFER;

    uint8_t payload[DH_HELLO_FIXED_LEN + DH_HELLO_TOKEN_MAX];
    wr_u16(payload, in->proto_version);
    payload[2] = in->os;
    payload[3] = in->build_type;
    payload[4] = in->channel_count;
    wr_u16(payload + 5, in->max_chunk);
    if (in->token_len) memcpy(payload + DH_HELLO_FIXED_LEN, in->token, in->token_len);

    return dh_frame_encode(DH_MSG_HELLO, 0, payload, DH_HELLO_FIXED_LEN + in->token_len, out, cap,
                           out_len);
}

bool dh_hello_ack_decode(const uint8_t *payload, size_t len, dh_hello_ack *out) {
    /* Fixed length: unlike the hello there is no trailing token, so anything
       longer is a frame this build does not understand rather than one it can
       read the front of. */
    if (payload == NULL || out == NULL || len != DH_HELLO_ACK_LEN) return false;

    out->proto_version = rd_u16(payload);
    out->status = payload[2];
    out->build_type = payload[3];
    out->channel_count = payload[4];
    out->max_chunk = rd_u16(payload + 5);
    return true;
}

dh_frame_result dh_hello_ack_encode(const dh_hello_ack *in, uint8_t *out, size_t cap,
                                    size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t payload[DH_HELLO_ACK_LEN];
    wr_u16(payload, in->proto_version);
    payload[2] = in->status;
    payload[3] = in->build_type;
    payload[4] = in->channel_count;
    wr_u16(payload + 5, in->max_chunk);

    return dh_frame_encode(DH_MSG_HELLO_ACK, 0, payload, sizeof payload, out, cap, out_len);
}

void dh_session_init(dh_session *s, uint8_t build_type) {
    memset(s, 0, sizeof *s);
    s->build_type = build_type;
}

void dh_session_drop(dh_session *s) {
    const uint8_t build_type = s->build_type;
    dh_session_init(s, build_type);
}

static uint16_t negotiate_chunk(uint16_t requested) {
    if (requested == 0 || requested < DH_SESSION_MIN_CHUNK) return DH_SESSION_MIN_CHUNK;
    return requested < DH_SESSION_MAX_CHUNK ? requested : DH_SESSION_MAX_CHUNK;
}

static uint8_t negotiate_channels(uint8_t requested) {
    if (requested == 0) return 1;
    return requested < DH_SESSION_CHANNEL_COUNT ? requested : DH_SESSION_CHANNEL_COUNT;
}

static dh_frame_result answer_hello(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                    uint32_t now_ms, uint8_t *out, size_t out_cap,
                                    size_t *out_len) {
    dh_hello hello;
    if (!dh_hello_decode(f->payload, f->hdr.len, &hello)) {
        /* Not a hello this build can read. Silence rather than a guessed
           status: a reply would have to claim a version negotiation that
           never happened. */
        return DH_FRAME_OK;
    }

    dh_hello_ack ack = {
        .proto_version = DH_PROTO_VERSION,
        .status = DH_HELLO_OK,
        .build_type = s->build_type,
        .channel_count = 0,
        .max_chunk = 0,
    };

    /*
     * A development build compiles the check out entirely (#44). A well-known
     * development secret was rejected as worse than none: it has the
     * appearance of security and would eventually ship. Such a build says so
     * in its build type, in its product string and in the configuration UI.
     */
    const bool authenticated = (s->build_type == DH_BUILD_DEVELOPMENT) ||
                               dh_pair_authenticate(pair, hello.token, hello.token_len);

    if (hello.proto_version != DH_PROTO_VERSION) {
        ack.status = DH_HELLO_VERSION_INCOMPATIBLE;
        /*
         * Refuse the hello, but do not tear down a session already running:
         * one process holds the channel, so a hello carrying a version this
         * device negotiated past is anomalous, and a working session is not
         * a stray frame's to end. A session that never started stays absent.
         */
        if (!s->present) dh_session_drop(s);
    } else if (!authenticated) {
        /*
         * Refused, and distinctly: the remedy is a chord press, not a helper
         * update. The session does not start, so nothing is relayed — but the
         * helper stays connected and may ask to be paired, which is what
         * makes provisioning during a window silent and automatic.
         */
        ack.status = DH_HELLO_AUTH_FAILED;
        if (!s->present) dh_session_drop(s);
    } else {
        ack.channel_count = negotiate_channels(hello.channel_count);
        ack.max_chunk = negotiate_chunk(hello.max_chunk);

        s->present = true;
        s->authenticated = true;
        s->peer_os = hello.os;
        s->channel_count = ack.channel_count;
        s->max_chunk = ack.max_chunk;
        s->last_seen_ms = now_ms;
        /* The ack about to be returned is this direction's first traffic, so
           the session starts with a full idle interval ahead of it rather
           than owing a beat immediately. */
        s->last_sent_ms = now_ms;
    }

    return dh_hello_ack_encode(&ack, out, out_cap, out_len);
}

dh_frame_result dh_session_on_frame(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                    uint32_t now_ms, uint8_t *out, size_t out_cap,
                                    size_t *out_len) {
    *out_len = 0;

    /* Arriving at all is the proof. The frame does not have to be one this
       layer acts on, or even one addressed to it — a helper that is writing
       is a helper that is alive. Bulk never reaches here, so the transport
       notes that itself. */
    dh_session_note_received(s, now_ms);

    switch (f->hdr.type) {
        case DH_MSG_HELLO:
            return answer_hello(s, pair, f, now_ms, out, out_cap, out_len);

        case DH_MSG_PAIR_REQUEST: {
            /*
             * Provisioned silently, with no user interaction — but only
             * inside a window, and a window can only be opened by a physical
             * chord on the device. Outside one, the request is refused by
             * saying nothing: there is no failure a caller could act on that
             * pressing the chord would not also fix.
             */
            uint8_t secret[DH_PAIR_SECRET_LEN];
            if (!dh_pair_grant(pair, now_ms, secret))
                return DH_FRAME_OK;

            return dh_frame_encode(DH_MSG_PAIR_GRANT, 0, secret, sizeof secret, out, out_cap,
                                   out_len);
        }

        case DH_MSG_HEARTBEAT:
            /* Nothing beyond having arrived, which is already accounted for
               above. The beat carries no more weight than any other frame —
               it exists to fill a direction with nothing else in it, so that
               silence means something. */
            return DH_FRAME_OK;

        default:
            /* Pairing (#46), placement, and everything in the bulk band are
               other layers' frames. Silence, not a guess. */
            return DH_FRAME_OK;
    }
}

void dh_session_note_sent(dh_session *s, uint32_t now_ms) {
    s->last_sent_ms = now_ms;
}

void dh_session_note_received(dh_session *s, uint32_t now_ms) {
    if (s->present) s->last_seen_ms = now_ms;
}

dh_frame_result dh_session_end(dh_session *s, uint8_t reason, uint8_t *out, size_t out_cap,
                               size_t *out_len) {
    *out_len = 0;
    if (!s->present) return DH_FRAME_OK;

    dh_session_drop(s);

    const uint8_t payload[DH_SESSION_END_LEN] = {reason};
    return dh_frame_encode(DH_MSG_SESSION_END, 0, payload, sizeof payload, out, out_cap, out_len);
}

dh_frame_result dh_session_tick(dh_session *s, uint32_t now_ms, uint8_t *out, size_t out_cap,
                                size_t *out_len) {
    *out_len = 0;
    if (!s->present) return DH_FRAME_OK;

    /* Unsigned difference throughout, so a wrapping millisecond counter is
       just arithmetic rather than a session dropped once every 49 days. */
    if ((uint32_t)(now_ms - s->last_seen_ms) >= (uint32_t)DH_SESSION_ABSENT_MS)
        return dh_session_end(s, DH_SESSION_END_LIVENESS_TIMEOUT, out, out_cap, out_len);

    /* Fill an idle direction, and only an idle one. Anything else this device
       sent has already told the helper the same thing. */
    if ((uint32_t)(now_ms - s->last_sent_ms) < DH_SESSION_HEARTBEAT_MS) return DH_FRAME_OK;

    const dh_frame_result rc =
        dh_frame_encode(DH_MSG_DEVICE_HEARTBEAT, 0, NULL, 0, out, out_cap, out_len);

    /*
     * The timer is charged for a beat this layer *produced*, which is not
     * quite the same as one the transport managed to send — deliberately.
     * The caller's slot refuses only while it is occupied by a frame already
     * draining to this same helper, and that frame refreshes the helper just
     * as well as a beat would. A slot stuck for longer than that means the
     * endpoint is wedged, and a helper that genuinely cannot hear this device
     * is one that should be reconnecting, not one to keep reassuring.
     *
     * Retrying instead would encode a beat on every tick for as long as the
     * slot stayed busy, and each refusal would be counted as loss against
     * #69's diagnostic — turning a healthy transfer into an alarm.
     */
    if (rc == DH_FRAME_OK) dh_session_note_sent(s, now_ms);
    return rc;
}
