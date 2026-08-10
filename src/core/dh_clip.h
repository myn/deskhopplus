/*
 * deskhopplus shared core — CLIP_* message payload codecs.
 *
 * Layouts per docs/protocol.md; these encode into / decode from the payload
 * of a frame (dh_frame.h). All integers little-endian. Decoded structs view
 * the input buffer; nothing is copied. Pure C11, no I/O.
 */

#ifndef DH_CLIP_H_
#define DH_CLIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CLIP_OFFER: id:u32 kind:u8 total:u64 meta_len:u16 meta */
typedef struct {
    uint32_t id;
    uint8_t kind; /* 0=utf8-text 1=png 2=file-list */
    uint64_t total;
    const uint8_t *meta;
    uint16_t meta_len;
} dh_clip_offer;

/* CLIP_CHUNK: id:u32 seq:u32 crc32:u32 data */
typedef struct {
    uint32_t id;
    uint32_t seq;
    uint32_t crc32;
    const uint8_t *data;
    uint16_t data_len;
} dh_clip_chunk;

/* Each encoder returns the payload length written, or -1 if cap is too
   small (or meta/data exceed the frame payload maximum). */
int dh_clip_encode_offer(const dh_clip_offer *offer, uint8_t *out, size_t cap);
int dh_clip_encode_chunk(const dh_clip_chunk *chunk, uint8_t *out, size_t cap);
/* CLIP_REQUEST / CLIP_DONE / CLIP_CANCEL share the id-only payload. */
int dh_clip_encode_id(uint32_t id, uint8_t *out, size_t cap);
/* CLIP_RETRANSMIT: id:u32 seq:u32 */
int dh_clip_encode_retransmit(uint32_t id, uint32_t seq, uint8_t *out, size_t cap);
/* CLIP_CREDIT: id:u32 credits:u16 */
int dh_clip_encode_credit(uint32_t id, uint16_t credits, uint8_t *out, size_t cap);

/* Decoders return false on a malformed payload (short, or inconsistent
   lengths). Views point into the input buffer. */
bool dh_clip_decode_offer(const uint8_t *payload, size_t len, dh_clip_offer *out);
bool dh_clip_decode_chunk(const uint8_t *payload, size_t len, dh_clip_chunk *out);
bool dh_clip_decode_id(const uint8_t *payload, size_t len, uint32_t *id);
bool dh_clip_decode_retransmit(const uint8_t *payload, size_t len, uint32_t *id, uint32_t *seq);
bool dh_clip_decode_credit(const uint8_t *payload, size_t len, uint32_t *id, uint16_t *credits);

#endif /* DH_CLIP_H_ */
