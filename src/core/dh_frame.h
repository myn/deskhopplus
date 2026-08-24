/*
 * deskhopplus shared core — frame codec.
 *
 * The wire format is docs/protocol.md; test-vectors/frames.txt is the gate.
 * Pure C11, no I/O, no platform or SDK dependencies: compiled into the
 * firmware and linked into both helpers through thin bindings.
 */

#ifndef DH_FRAME_H_
#define DH_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

#define DH_FRAME_HEADER_SIZE 4u
#define DH_FRAME_MAX_PAYLOAD 4096u
#define DH_FRAME_MAX_SIZE (DH_FRAME_HEADER_SIZE + DH_FRAME_MAX_PAYLOAD)

/*
 * The message registry, banded per docs/protocol.md — the single list the
 * enum and the known-type check are both generated from.
 *   session   0x01-0x1F  (0x08-0x0F is the unauthenticated sub-band)
 *   placement 0x20-0x2F
 *   bulk      0x30-0x3F
 */
#define DH_MSG_TYPE_LIST(X)          \
    X(DH_MSG_HELLO, 0x01)            \
    X(DH_MSG_HELLO_ACK, 0x02)        \
    X(DH_MSG_LISTENER_ALERT, 0x03)   \
    X(DH_MSG_HEARTBEAT, 0x05)        \
    X(DH_MSG_DEVICE_HEARTBEAT, 0x06) \
    X(DH_MSG_SESSION_END, 0x07)      \
    X(DH_MSG_PAIR_REQUEST, 0x08)     \
    X(DH_MSG_PAIR_GRANT, 0x09)       \
    X(DH_MSG_PAIR_REFUSED, 0x0A)     \
    X(DH_MSG_HELLO_REFUSED, 0x0B)    \
    X(DH_MSG_PLACE, 0x20)            \
    X(DH_MSG_POS_QUERY, 0x21)        \
    X(DH_MSG_POS_RESPONSE, 0x22)     \
    X(DH_MSG_CLIP_OFFER, 0x30)       \
    X(DH_MSG_CLIP_REQUEST, 0x31)     \
    X(DH_MSG_CLIP_CHUNK, 0x32)       \
    X(DH_MSG_CLIP_DONE, 0x33)        \
    X(DH_MSG_CLIP_CANCEL, 0x34)      \
    X(DH_MSG_CLIP_RETRANSMIT, 0x35)  \
    X(DH_MSG_CLIP_CREDIT, 0x36)      \
    X(DH_MSG_SEAL_OFFER, 0x37)       \
    X(DH_MSG_SEAL_ACCEPT, 0x38)      \
    X(DH_MSG_SEAL_STALE, 0x39)

enum dh_msg_type {
#define DH_MSG_ENUM_ENTRY(name, value) name = value,
    DH_MSG_TYPE_LIST(DH_MSG_ENUM_ENTRY)
#undef DH_MSG_ENUM_ENTRY
};

#define DH_MSG_BULK_BASE 0x30u

/*
 * The authentication prefix (ADR-0008, docs/protocol.md v2): a monotonic
 * counter and a 16-byte tag, sitting between the 4-byte header and the body.
 * It is inside `len`, so this file's codec is unchanged by it — a frame is
 * still a header and a payload, and the relay still parses headers only.
 * Verifying the tag is dh_auth's job (#110), not this codec's.
 */
#define DH_FRAME_COUNTER_SIZE 8u
#define DH_FRAME_TAG_SIZE 16u
#define DH_FRAME_AUTH_PREFIX_SIZE (DH_FRAME_COUNTER_SIZE + DH_FRAME_TAG_SIZE)

/*
 * The unauthenticated sub-band, 0x08-0x0F: pairing, and the two refusals a
 * board sends when it has no key to tag with. Whether a frame carries the
 * prefix must be decidable from the type byte alone, before any payload is
 * read — which is why a refused hello is its own type rather than a status
 * inside DH_MSG_HELLO_ACK.
 */
#define DH_MSG_UNAUTH_FIRST 0x08u
#define DH_MSG_UNAUTH_LAST 0x0Fu

/*
 * Whether a frame of this type carries the authentication prefix.
 *
 * PRECONDITION: type is in the registry. This is a range test and nothing
 * more, so it answers true for DH_FRAME_PAD, for the gaps in the registry,
 * and for any byte at all outside 0x08-0x0F — "not a pairing message" is not
 * "is a message". Every decode path reaches this only after
 * dh_frame_header_parse has rejected unknown types, and a caller that reaches
 * it any other way must call dh_msg_type_known first. Folding that check in
 * here would put a switch on the relay's hot path for a case the decoder has
 * already excluded.
 */
static inline bool dh_msg_is_authenticated(uint8_t type) {
    return type < DH_MSG_UNAUTH_FIRST || type > DH_MSG_UNAUTH_LAST;
}

/*
 * Inter-frame padding. The registry starts at 0x01, so 0x00 is not a message
 * type and cannot begin a frame. A fixed-size report carrier — the channel's
 * 64-byte HID reports, which have no length field of their own — fills the
 * tail of its last report with this byte, and dh_frame_reader_push skips it
 * between frames. Inside a frame it is ordinary payload, distinguished by the
 * length the header already gave.
 */
#define DH_FRAME_PAD 0x00u

typedef enum {
    DH_FRAME_OK = 0,
    DH_FRAME_AGAIN = 1, /* incomplete: need more bytes */
    DH_FRAME_ERR_OVERSIZE = -1,
    DH_FRAME_ERR_UNKNOWN_TYPE = -2,
    DH_FRAME_ERR_BUFFER = -3, /* output buffer too small */
} dh_frame_result;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t len;
} dh_frame_header;

/* A decoded frame viewing the caller's (or reader's) storage; payload is not copied. */
typedef struct {
    dh_frame_header hdr;
    const uint8_t *payload;
} dh_frame_view;

bool dh_msg_type_known(uint8_t type);

/* Both firmware decisions — priority and routing — are this one comparison. */
static inline bool dh_msg_is_bulk(uint8_t type) {
    return type >= DH_MSG_BULK_BASE;
}

/*
 * Parse exactly the 4-byte header: type, flags, length. Never touches payload
 * bytes — this is the firmware's whole view of a frame. Rejects an unknown
 * type and a length over the maximum; DH_FRAME_AGAIN if len < 4.
 */
dh_frame_result dh_frame_header_parse(const uint8_t *buf, size_t len, dh_frame_header *out);

/*
 * One-shot decode of a complete frame at the start of buf. On DH_FRAME_OK,
 * *out views buf and *consumed is the frame's total size. DH_FRAME_AGAIN
 * means the buffer holds a truncated frame.
 */
dh_frame_result dh_frame_decode(const uint8_t *buf, size_t len, dh_frame_view *out,
                                size_t *consumed);

/*
 * Encode one frame into out. payload may be NULL when payload_len is 0.
 * On DH_FRAME_OK, *out_len is the encoded size.
 */
dh_frame_result dh_frame_encode(uint8_t type, uint8_t flags, const uint8_t *payload,
                                size_t payload_len, uint8_t *out, size_t out_cap,
                                size_t *out_len);

/*
 * Incremental reader over an ordered byte stream — the channel delivers frames
 * packed into 64-byte HID report carriers, so frame boundaries never align
 * with delivery boundaries. DH_FRAME_PAD bytes between frames are skipped.
 * Fixed storage, no allocation. Feed arbitrary slices; call in a loop while
 * *consumed < len:
 *
 *   DH_FRAME_OK    — a complete frame is in *out (viewing the reader's buffer,
 *                    valid until the next push); *consumed bytes were eaten.
 *   DH_FRAME_AGAIN — all len bytes eaten, no complete frame yet.
 *   error          — protocol error (oversize length or unknown type): drop
 *                    the connection per docs/protocol.md. The reader resets.
 */
typedef struct {
    uint8_t buf[DH_FRAME_MAX_SIZE];
    uint16_t have;
} dh_frame_reader;

void dh_frame_reader_init(dh_frame_reader *r);
dh_frame_result dh_frame_reader_push(dh_frame_reader *r, const uint8_t *data, size_t len,
                                     size_t *consumed, dh_frame_view *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_FRAME_H_ */
