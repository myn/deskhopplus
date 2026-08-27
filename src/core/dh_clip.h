/*
 * deskhopplus shared core — CLIP_* message payload codecs.
 *
 * Layouts per docs/protocol.md; these encode into / decode from the payload
 * of a frame (dh_frame.h). All integers little-endian. Decoded structs view
 * the input buffer; nothing is copied. Pure C11, no I/O.
 *
 * The two messages that carry the user's bytes — CLIP_OFFER and CLIP_CHUNK —
 * are split in two here, and the split is the seal (ADR-0008, #113):
 *
 *   - the **head** is clear. It is what a receiver has to read before it holds
 *     anything it can trust: which transfer, which chunk, and which seal key
 *     opens the rest.
 *   - the **plain** part is the message proper, and on the wire it is only
 *     ever seen sealed.
 *
 * Nothing in this file holds a key or calls a cipher — dh_seal.h is what joins
 * the halves. Keeping them apart is what makes "a payload went out in clear" a
 * thing a caller cannot do by forgetting a step: there is no function here that
 * produces a complete CLIP_OFFER or CLIP_CHUNK payload.
 */

#ifndef DH_CLIP_H_
#define DH_CLIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_auth.h"
#include "dh_frame.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/* The clear heads, in bytes. */
#define DH_CLIP_OFFER_HEAD_LEN 16u /* id + seal_id + seal_counter */
#define DH_CLIP_CHUNK_HEAD_LEN 20u /* id + seq + seal_id + seal_counter */

/* What a sealed plaintext costs before its variable part. */
#define DH_CLIP_OFFER_PLAIN_FIXED 11u /* kind + total + meta_len */
/* Largest metadata an authenticated, sealed offer can carry. The seal adds
   one 16-byte tag beyond the clear and plaintext fixed fields. */
#define DH_CLIP_OFFER_META_WIRE_MAX                                                        \
    (DH_FRAME_MAX_PAYLOAD - DH_FRAME_AUTH_PREFIX_SIZE - DH_CLIP_OFFER_HEAD_LEN -          \
     DH_CLIP_OFFER_PLAIN_FIXED - 16u)
#define DH_CLIP_CHUNK_PLAIN_FIXED 4u  /* crc32 */

/* CLIP_OFFER head: id:u32 seal_id:u32 seal_counter:u64 */
typedef struct {
    uint32_t id;
    uint32_t seal_id;
    uint64_t seal_counter;
} dh_clip_offer_head;

/* CLIP_CHUNK head: id:u32 seq:u32 seal_id:u32 seal_counter:u64 */
typedef struct {
    uint32_t id;
    uint32_t seq;
    uint32_t seal_id;
    uint64_t seal_counter;
} dh_clip_chunk_head;

/*
 * The offer. `id` travels in the head, so the plain codecs neither write nor
 * read it: dh_seal fills it in from the head it already parsed.
 */
typedef struct {
    uint32_t id;
    uint8_t kind; /* 0=utf8-text 1=png 2=file-list */
    uint64_t total;
    const uint8_t *meta;
    uint16_t meta_len;
} dh_clip_offer;

/*
 * The chunk. `id` and `seq` travel in the head; `crc32` stays inside the seal.
 *
 * The CRC32 is not the integrity check against a hostile change — GCM already
 * authenticates — it is fidelity (CONTEXT.md, ADR-0003), covering the plaintext
 * end to end and catching a bug in the seal layer itself, which an
 * authenticator sitting outside the plaintext cannot.
 */
typedef struct {
    uint32_t id;
    uint32_t seq;
    uint32_t crc32;
    const uint8_t *data;
    uint16_t data_len;
} dh_clip_chunk;

/* Each encoder returns the length written, or -1 if cap is too small (or the
   result would exceed the frame payload maximum). */
int dh_clip_encode_offer_head(const dh_clip_offer_head *head, uint8_t *out, size_t cap);
int dh_clip_encode_chunk_head(const dh_clip_chunk_head *head, uint8_t *out, size_t cap);
int dh_clip_encode_offer_plain(const dh_clip_offer *offer, uint8_t *out, size_t cap);
int dh_clip_encode_chunk_plain(const dh_clip_chunk *chunk, uint8_t *out, size_t cap);
/* CLIP_REQUEST / CLIP_DONE / CLIP_CANCEL share the id-only payload, and none
   of the three is sealed: they carry a transfer id and nothing else. */
int dh_clip_encode_id(uint32_t id, uint8_t *out, size_t cap);
/* CLIP_RETRANSMIT: id:u32 seq:u32 */
int dh_clip_encode_retransmit(uint32_t id, uint32_t seq, uint8_t *out, size_t cap);
/* CLIP_CREDIT: id:u32 credits:u16 */
int dh_clip_encode_credit(uint32_t id, uint16_t credits, uint8_t *out, size_t cap);

/*
 * Decoders return false on a malformed input (short, or inconsistent lengths).
 * Views point into the input buffer.
 *
 * The head decoders take the whole payload and read the front of it, because
 * that is the position they are used from: a receiver reads the head to find
 * out which key opens what follows it.
 */
bool dh_clip_decode_offer_head(const uint8_t *payload, size_t len, dh_clip_offer_head *out);
bool dh_clip_decode_chunk_head(const uint8_t *payload, size_t len, dh_clip_chunk_head *out);
/* `out->id` is left untouched — it was in the head. */
bool dh_clip_decode_offer_plain(const uint8_t *plain, size_t len, dh_clip_offer *out);
/* `out->id` and `out->seq` are left untouched — they were in the head. */
bool dh_clip_decode_chunk_plain(const uint8_t *plain, size_t len, dh_clip_chunk *out);
bool dh_clip_decode_id(const uint8_t *payload, size_t len, uint32_t *id);
bool dh_clip_decode_retransmit(const uint8_t *payload, size_t len, uint32_t *id, uint32_t *seq);
bool dh_clip_decode_credit(const uint8_t *payload, size_t len, uint32_t *id, uint16_t *credits);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_CLIP_H_ */
