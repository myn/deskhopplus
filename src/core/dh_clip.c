/*
 * CLIP_* message payload codecs. Layouts: docs/protocol.md; the golden
 * vectors gate the bytes. Pure C11, no I/O, no platform dependencies.
 */

#include "dh_clip.h"

#include <string.h>

#include "dh_frame.h"

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v & 0xffff));
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v & 0xffffffffu));
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    return get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static uint64_t get64(const uint8_t *p) {
    return get32(p) | ((uint64_t)get32(p + 4) << 32);
}

#define OFFER_FIXED 15u /* id + kind + total + meta_len */
#define CHUNK_FIXED 12u /* id + seq + crc32 */

int dh_clip_encode_offer(const dh_clip_offer *offer, uint8_t *out, size_t cap) {
    const size_t total = OFFER_FIXED + offer->meta_len;
    if (total > cap || total > DH_FRAME_MAX_PAYLOAD)
        return -1;
    put32(out, offer->id);
    out[4] = offer->kind;
    put64(out + 5, offer->total);
    put16(out + 13, offer->meta_len);
    if (offer->meta_len > 0)
        memcpy(out + OFFER_FIXED, offer->meta, offer->meta_len);
    return (int)total;
}

int dh_clip_encode_chunk(const dh_clip_chunk *chunk, uint8_t *out, size_t cap) {
    const size_t total = CHUNK_FIXED + chunk->data_len;
    if (total > cap || total > DH_FRAME_MAX_PAYLOAD)
        return -1;
    put32(out, chunk->id);
    put32(out + 4, chunk->seq);
    put32(out + 8, chunk->crc32);
    memcpy(out + CHUNK_FIXED, chunk->data, chunk->data_len);
    return (int)total;
}

int dh_clip_encode_id(uint32_t id, uint8_t *out, size_t cap) {
    if (cap < 4)
        return -1;
    put32(out, id);
    return 4;
}

int dh_clip_encode_retransmit(uint32_t id, uint32_t seq, uint8_t *out, size_t cap) {
    if (cap < 8)
        return -1;
    put32(out, id);
    put32(out + 4, seq);
    return 8;
}

int dh_clip_encode_credit(uint32_t id, uint16_t credits, uint8_t *out, size_t cap) {
    if (cap < 6)
        return -1;
    put32(out, id);
    put16(out + 4, credits);
    return 6;
}

bool dh_clip_decode_offer(const uint8_t *payload, size_t len, dh_clip_offer *out) {
    if (len < OFFER_FIXED)
        return false;
    out->id = get32(payload);
    out->kind = payload[4];
    out->total = get64(payload + 5);
    out->meta_len = get16(payload + 13);
    if ((size_t)OFFER_FIXED + out->meta_len != len)
        return false;
    out->meta = payload + OFFER_FIXED;
    return true;
}

bool dh_clip_decode_chunk(const uint8_t *payload, size_t len, dh_clip_chunk *out) {
    if (len < CHUNK_FIXED)
        return false;
    out->id = get32(payload);
    out->seq = get32(payload + 4);
    out->crc32 = get32(payload + 8);
    out->data = payload + CHUNK_FIXED;
    out->data_len = (uint16_t)(len - CHUNK_FIXED);
    return true;
}

bool dh_clip_decode_id(const uint8_t *payload, size_t len, uint32_t *id) {
    if (len != 4)
        return false;
    *id = get32(payload);
    return true;
}

bool dh_clip_decode_retransmit(const uint8_t *payload, size_t len, uint32_t *id, uint32_t *seq) {
    if (len != 8)
        return false;
    *id = get32(payload);
    *seq = get32(payload + 4);
    return true;
}

bool dh_clip_decode_credit(const uint8_t *payload, size_t len, uint32_t *id, uint16_t *credits) {
    if (len != 6)
        return false;
    *id = get32(payload);
    *credits = get16(payload + 4);
    return true;
}
