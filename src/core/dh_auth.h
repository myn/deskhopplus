/*
 * deskhopplus shared core — session keys, the frame tag, and the counter (#110).
 *
 * This is the wire half of ADR-0008 in one file: what a frame's 24-byte
 * authentication prefix is made of, and what a receiver has to decide before
 * it may act on the frame behind it. The layouts are docs/protocol.md v2 and
 * test-vectors/frames.txt is the gate; the primitives underneath are
 * dh_sha256.h and dh_p256.h.
 *
 * The rule this file exists to enforce is per frame, not per session. v1
 * gated relay on one flag for the whole board, so any process writing into a
 * shared endpoint could push bulk into a session it never authenticated
 * (#34, #95). A board holding a live session still drops a frame whose tag
 * does not verify.
 *
 * What it deliberately does not do: decide what to *say* about a failure.
 * Silence on a failed tag, the refusal messages, the listener alert and its
 * threshold are the board's (#111) and the helper's (#112), not the codec's.
 *
 * Pure C11: no allocation, no I/O, no clock, no entropy source.
 */

#ifndef DH_AUTH_H_
#define DH_AUTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_frame.h"
#include "dh_p256.h"
#include "dh_sha256.h"

#define DH_SESSION_KEY_SIZE 32u
#define DH_NONCE_SIZE 16u

typedef enum {
    DH_AUTH_OK = 0,
    DH_AUTH_ERR_SHORT = -1,   /* payload smaller than the prefix it must carry */
    DH_AUTH_ERR_TAG = -2,     /* the tag did not verify: not acted on, not relayed */
    DH_AUTH_ERR_COUNTER = -3, /* not strictly greater than the highest accepted */
    DH_AUTH_ERR_BUFFER = -4,  /* output buffer too small */
    /* There was no key to check against. Never returned by this file, which
       always has one — it is here because this enum is the vocabulary for what
       happened to a frame's authentication, and "nothing could be checked" is
       one of the answers a caller has to be able to give without calling it a
       failed tag. */
    DH_AUTH_ERR_NO_KEY = -5,
} dh_auth_result;

/*
 * The three session keys of docs/protocol.md, all 32 bytes, all HKDF-SHA256
 * over the pairing-time shared secret. No asymmetric work per session — the
 * constraint that shaped the design more than any other.
 *
 * k_hello exists because the hello has to be authenticated before the board's
 * nonce is known: the helper cannot key on a value it has not received yet.
 * It authenticates exactly one frame and is then discarded. Everything after
 * uses the key for its own direction, so a frame can never be replayed back
 * at its sender.
 */
void dh_auth_derive_hello_key(const uint8_t shared_secret[DH_P256_SHARED_SIZE],
                              const uint8_t helper_nonce[DH_NONCE_SIZE],
                              uint8_t k_hello[DH_SESSION_KEY_SIZE]);

void dh_auth_derive_session_keys(const uint8_t shared_secret[DH_P256_SHARED_SIZE],
                                 const uint8_t helper_nonce[DH_NONCE_SIZE],
                                 const uint8_t board_nonce[DH_NONCE_SIZE],
                                 uint8_t k_h2b[DH_SESSION_KEY_SIZE],
                                 uint8_t k_b2h[DH_SESSION_KEY_SIZE]);

/*
 * tag = HMAC-SHA256(key, type || flags || len || counter || body)[0..16]
 *
 * `len` is the wire length — the whole payload, prefix included — not the
 * body's. Binding type and len is what stops a frame being re-typed or
 * truncated into a different message carrying the same tag. The tag does not
 * cover itself.
 */
void dh_auth_tag(const uint8_t key[DH_SESSION_KEY_SIZE], uint8_t type, uint8_t flags,
                 uint16_t len, uint64_t counter, const uint8_t *body, size_t body_len,
                 uint8_t out[DH_FRAME_TAG_SIZE]);

/*
 * Build the payload of an authenticated frame: counter || tag || body, ready
 * for dh_frame_encode. The caller owns the counter, because the counter space
 * belongs to the key and one key may be used by more than one queue.
 */
dh_auth_result dh_auth_wrap(const uint8_t key[DH_SESSION_KEY_SIZE], uint8_t type, uint8_t flags,
                            uint64_t counter, const uint8_t *body, size_t body_len, uint8_t *out,
                            size_t out_cap, size_t *out_len);

/*
 * A complete authenticated frame — the 4-byte header, then dh_auth_wrap
 * writing the prefix and the body behind it — built straight into the caller's
 * buffer.
 *
 * Not dh_frame_encode over a staged payload, deliberately. That would copy a
 * relayed 1 KB body twice on a chip where the relay is already the hot path,
 * and the second copy would need a payload-sized buffer somewhere to live.
 *
 * Here rather than in either end's session file because both ends build these,
 * and "an authenticated frame is a header plus a wrap" is a rule that must not
 * have two implementations to drift between (#80).
 */
dh_frame_result dh_auth_frame(uint8_t type, uint8_t flags, const uint8_t key[DH_SESSION_KEY_SIZE],
                              uint64_t counter, const uint8_t *body, size_t body_len, uint8_t *out,
                              size_t cap, size_t *out_len);

/*
 * The highest counter accepted under one key, in one direction.
 *
 * A receiver refuses anything not strictly greater. Refusing anything not
 * *exactly* one greater would be wrong: the device's outbound path is a short
 * bounded queue (ADR-0005) and a frame it cannot take is a silent loss, so
 * gaps happen in normal operation and are not evidence of an attack.
 *
 * Wrapping is not a case to handle — 64 bits wide, and the keys do not outlive
 * a session, which #107 measured at about 98 seconds.
 */
typedef struct {
    uint64_t highest;
    bool any; /* whether anything has been accepted yet, so counter 0 is usable */
} dh_auth_counter;

void dh_auth_counter_init(dh_auth_counter *c);

/* Would this counter be accepted? Asks without recording — the board can call
   it after reading 12 bytes, before it has buffered a payload it is going to
   throw away. It is only a hint there: nothing is recorded until the tag has
   verified, which is what dh_auth_open does. */
bool dh_auth_counter_ok(const dh_auth_counter *c, uint64_t counter);

/*
 * Verify one received frame and hand back its body.
 *
 * The tag is checked first and the counter second, so a frame that does not
 * authenticate cannot move the counter state — otherwise anything writing
 * into a shared endpoint could push the window forward and lock the real
 * helper out. On DH_AUTH_OK, and only then, the counter is recorded.
 *
 * PRECONDITION: dh_msg_is_authenticated(hdr->type). Types 0x08-0x0F carry no
 * prefix; their payload is the body.
 */
dh_auth_result dh_auth_open(const uint8_t key[DH_SESSION_KEY_SIZE], const dh_frame_header *hdr,
                            const uint8_t *payload, dh_auth_counter *counter,
                            const uint8_t **body, size_t *body_len);

/* The counter a received frame carries, before anything about it is trusted.
   Needs the 8 counter bytes and no more, which is what lets a board call it
   12 bytes into a frame; false below that. */
bool dh_auth_peek_counter(const uint8_t *payload, size_t payload_len, uint64_t *out);

/*
 * Constant time in len. An early return would leak how much of a guessed tag
 * was right, one byte at a time, to a process that can retry as fast as it
 * likes — the same reasoning dh_pair_is_registered_key carries, and the reason
 * this is not memcmp.
 */
bool dh_auth_bytes_equal(const uint8_t *a, const uint8_t *b, size_t len);

#endif /* DH_AUTH_H_ */
