/*
 * deskhopplus shared core — the v1 hello codecs, parked. See dh_session_v1.h,
 * which says why this file still exists and which ticket deletes it.
 */

#include "dh_session_v1.h"

#include <string.h>

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

bool dh_hello_v1_decode(const uint8_t *payload, size_t len, dh_hello_v1 *out) {
    if (payload == NULL || out == NULL) return false;
    if (len < DH_HELLO_FIXED_LEN_V1) return false;

    const size_t token_len = len - DH_HELLO_FIXED_LEN_V1;
    if (token_len > DH_HELLO_TOKEN_MAX_V1) return false;

    out->proto_version = rd_u16(payload);
    out->os = payload[2];
    out->build_type = payload[3];
    out->channel_count = payload[4];
    out->max_chunk = rd_u16(payload + 5);
    out->token = token_len ? payload + DH_HELLO_FIXED_LEN_V1 : NULL;
    out->token_len = (uint16_t)token_len;
    return true;
}

dh_frame_result dh_hello_v1_encode(const dh_hello_v1 *in, uint8_t *out, size_t cap,
                                   size_t *out_len) {
    /* A length without a pointer is a caller bug, not a short token: the
       bindings build this struct by hand, so it is checked rather than
       assumed. */
    if (in == NULL || in->token_len > DH_HELLO_TOKEN_MAX_V1) return DH_FRAME_ERR_BUFFER;
    if (in->token == NULL && in->token_len > 0) return DH_FRAME_ERR_BUFFER;

    uint8_t payload[DH_HELLO_FIXED_LEN_V1 + DH_HELLO_TOKEN_MAX_V1];
    wr_u16(payload, in->proto_version);
    payload[2] = in->os;
    payload[3] = in->build_type;
    payload[4] = in->channel_count;
    wr_u16(payload + 5, in->max_chunk);
    if (in->token_len) memcpy(payload + DH_HELLO_FIXED_LEN_V1, in->token, in->token_len);

    return dh_frame_encode(DH_MSG_HELLO, 0, payload, DH_HELLO_FIXED_LEN_V1 + in->token_len, out, cap,
                           out_len);
}

bool dh_hello_ack_v1_decode(const uint8_t *payload, size_t len, dh_hello_ack_v1 *out) {
    /* Fixed length: unlike the hello there is no trailing token, so anything
       longer is a frame this build does not understand rather than one it can
       read the front of. */
    if (payload == NULL || out == NULL || len != DH_HELLO_ACK_LEN_V1) return false;

    out->proto_version = rd_u16(payload);
    out->status = payload[2];
    out->build_type = payload[3];
    out->channel_count = payload[4];
    out->max_chunk = rd_u16(payload + 5);
    return true;
}

dh_frame_result dh_hello_ack_v1_encode(const dh_hello_ack_v1 *in, uint8_t *out, size_t cap,
                                       size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t payload[DH_HELLO_ACK_LEN_V1];
    wr_u16(payload, in->proto_version);
    payload[2] = in->status;
    payload[3] = in->build_type;
    payload[4] = in->channel_count;
    wr_u16(payload + 5, in->max_chunk);

    return dh_frame_encode(DH_MSG_HELLO_ACK, 0, payload, sizeof payload, out, cap, out_len);
}
