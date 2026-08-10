/*
 * Frame codec for the helper↔firmware channel. Wire format: docs/protocol.md;
 * gate: test-vectors/frames.txt. Pure C11, no I/O, no platform dependencies.
 */

#include "dh_frame.h"

#include <string.h>

bool dh_msg_type_known(uint8_t type) {
    switch (type) {
#define DH_MSG_KNOWN_CASE(name, value) case name:
        DH_MSG_TYPE_LIST(DH_MSG_KNOWN_CASE)
#undef DH_MSG_KNOWN_CASE
        return true;
    default:
        return false;
    }
}

dh_frame_result dh_frame_header_parse(const uint8_t *buf, size_t len, dh_frame_header *out) {
    if (len < DH_FRAME_HEADER_SIZE)
        return DH_FRAME_AGAIN;
    const uint16_t payload_len = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    if (payload_len > DH_FRAME_MAX_PAYLOAD)
        return DH_FRAME_ERR_OVERSIZE;
    if (!dh_msg_type_known(buf[0]))
        return DH_FRAME_ERR_UNKNOWN_TYPE;
    out->type = buf[0];
    out->flags = buf[1];
    out->len = payload_len;
    return DH_FRAME_OK;
}

dh_frame_result dh_frame_decode(const uint8_t *buf, size_t len, dh_frame_view *out,
                                size_t *consumed) {
    dh_frame_header hdr;
    const dh_frame_result rc = dh_frame_header_parse(buf, len, &hdr);
    if (rc != DH_FRAME_OK)
        return rc;
    const size_t total = DH_FRAME_HEADER_SIZE + hdr.len;
    if (len < total)
        return DH_FRAME_AGAIN;
    out->hdr = hdr;
    out->payload = buf + DH_FRAME_HEADER_SIZE;
    *consumed = total;
    return DH_FRAME_OK;
}

dh_frame_result dh_frame_encode(uint8_t type, uint8_t flags, const uint8_t *payload,
                                size_t payload_len, uint8_t *out, size_t out_cap,
                                size_t *out_len) {
    if (payload_len > DH_FRAME_MAX_PAYLOAD)
        return DH_FRAME_ERR_OVERSIZE;
    if (!dh_msg_type_known(type))
        return DH_FRAME_ERR_UNKNOWN_TYPE;
    const size_t total = DH_FRAME_HEADER_SIZE + payload_len;
    if (out_cap < total)
        return DH_FRAME_ERR_BUFFER;
    out[0] = type;
    out[1] = flags;
    out[2] = (uint8_t)(payload_len & 0xff);
    out[3] = (uint8_t)(payload_len >> 8);
    if (payload_len > 0)
        memcpy(out + DH_FRAME_HEADER_SIZE, payload, payload_len);
    *out_len = total;
    return DH_FRAME_OK;
}

void dh_frame_reader_init(dh_frame_reader *r) {
    r->have = 0;
}

/* Buffer up to `want` more bytes from the unread part of data; returns bytes taken. */
static size_t reader_fill(dh_frame_reader *r, const uint8_t *data, size_t len, size_t used,
                          size_t want) {
    const size_t take = len - used < want ? len - used : want;
    memcpy(r->buf + r->have, data + used, take);
    r->have += (uint16_t)take;
    return take;
}

dh_frame_result dh_frame_reader_push(dh_frame_reader *r, const uint8_t *data, size_t len,
                                     size_t *consumed, dh_frame_view *out) {
    size_t used = 0;

    /* A complete frame only ever rests in the buffer after being returned,
     * so it is released on the next push. */
    if (r->have >= DH_FRAME_HEADER_SIZE) {
        dh_frame_header hdr;
        if (dh_frame_header_parse(r->buf, r->have, &hdr) == DH_FRAME_OK &&
            r->have == DH_FRAME_HEADER_SIZE + hdr.len)
            r->have = 0;
    }

    while (used < len) {
        /* Fill the header, then validate it once complete. */
        if (r->have < DH_FRAME_HEADER_SIZE) {
            used += reader_fill(r, data, len, used, DH_FRAME_HEADER_SIZE - r->have);
            if (r->have < DH_FRAME_HEADER_SIZE)
                break;
        }

        dh_frame_header hdr;
        const dh_frame_result rc = dh_frame_header_parse(r->buf, r->have, &hdr);
        if (rc != DH_FRAME_OK) {
            r->have = 0; /* protocol error: caller drops the connection */
            *consumed = used;
            return rc;
        }

        const size_t total = DH_FRAME_HEADER_SIZE + hdr.len;
        used += reader_fill(r, data, len, used, total - r->have);

        if (r->have == total) {
            out->hdr = hdr;
            out->payload = r->buf + DH_FRAME_HEADER_SIZE;
            *consumed = used;
            return DH_FRAME_OK;
        }
        break; /* frame incomplete; everything offered was buffered */
    }

    *consumed = used;
    return DH_FRAME_AGAIN;
}
