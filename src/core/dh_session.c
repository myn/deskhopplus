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
    if (in == NULL || in->token_len > DH_HELLO_TOKEN_MAX) return DH_FRAME_ERR_BUFFER;

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

static dh_frame_result answer_hello(dh_session *s, const dh_frame_view *f, uint32_t now_ms,
                                    uint8_t *out, size_t out_cap, size_t *out_len) {
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
     * Authentication is #46's; when it lands it sets DH_HELLO_AUTH_FAILED
     * here, and the effective fields stay zero on that path too.
     */
    if (hello.proto_version != DH_PROTO_VERSION) {
        ack.status = DH_HELLO_VERSION_INCOMPATIBLE;
        dh_session_drop(s);
    } else {
        ack.channel_count = negotiate_channels(hello.channel_count);
        ack.max_chunk = negotiate_chunk(hello.max_chunk);

        s->present = true;
        s->peer_os = hello.os;
        s->channel_count = ack.channel_count;
        s->max_chunk = ack.max_chunk;
        s->last_seen_ms = now_ms;
    }

    return dh_hello_ack_encode(&ack, out, out_cap, out_len);
}

dh_frame_result dh_session_on_frame(dh_session *s, const dh_frame_view *f, uint32_t now_ms,
                                    uint8_t *out, size_t out_cap, size_t *out_len) {
    *out_len = 0;

    switch (f->hdr.type) {
        case DH_MSG_HELLO:
            return answer_hello(s, f, now_ms, out, out_cap, out_len);

        case DH_MSG_HEARTBEAT:
            /* Only a session that said hello can be kept alive: the device
               would otherwise hold a helper present without knowing what it
               negotiated with. */
            if (s->present) s->last_seen_ms = now_ms;
            return DH_FRAME_OK;

        default:
            /* Pairing (#46), placement, and everything in the bulk band are
               other layers' frames. Silence, not a guess. */
            return DH_FRAME_OK;
    }
}

bool dh_session_tick(dh_session *s, uint32_t now_ms) {
    if (!s->present) return false;

    /* Unsigned difference, so a wrapping millisecond counter is just
       arithmetic rather than a session dropped once every 49 days. */
    if ((uint32_t)(now_ms - s->last_seen_ms) < DH_SESSION_ABSENT_MS) return false;

    dh_session_drop(s);
    return true;
}
