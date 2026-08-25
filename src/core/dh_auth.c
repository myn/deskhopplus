/*
 * deskhopplus shared core — session keys, the frame tag, and the counter (#110).
 * See dh_auth.h. Layouts are docs/protocol.md v2; test-vectors/frames.txt is
 * the gate and auth_test verifies every tag in it.
 */

#include "dh_auth.h"

#include <string.h>

/* ASCII, no terminator — docs/protocol.md, "Session keys". */
static const char DH_INFO_HELLO[] = "deskhopplus/2 hello";
static const char DH_INFO_H2B[] = "deskhopplus/2 h2b";
static const char DH_INFO_B2H[] = "deskhopplus/2 b2h";

#define INFO_BYTES(s) ((const uint8_t *)(s)), (sizeof(s) - 1u)

void dh_auth_derive_hello_key(const uint8_t shared_secret[DH_P256_SHARED_SIZE],
                              const uint8_t helper_nonce[DH_NONCE_SIZE],
                              uint8_t k_hello[DH_SESSION_KEY_SIZE]) {
    dh_hkdf_sha256(shared_secret, DH_P256_SHARED_SIZE, helper_nonce, DH_NONCE_SIZE,
                   INFO_BYTES(DH_INFO_HELLO), k_hello, DH_SESSION_KEY_SIZE);
}

void dh_auth_derive_session_keys(const uint8_t shared_secret[DH_P256_SHARED_SIZE],
                                 const uint8_t helper_nonce[DH_NONCE_SIZE],
                                 const uint8_t board_nonce[DH_NONCE_SIZE],
                                 uint8_t k_h2b[DH_SESSION_KEY_SIZE],
                                 uint8_t k_b2h[DH_SESSION_KEY_SIZE]) {
    /* One salt, both nonces, in the order the spec writes them: helper first,
       because the helper's is the one that exists before the board answers. */
    uint8_t salt[DH_NONCE_SIZE * 2u];
    memcpy(salt, helper_nonce, DH_NONCE_SIZE);
    memcpy(salt + DH_NONCE_SIZE, board_nonce, DH_NONCE_SIZE);

    dh_hkdf_sha256(shared_secret, DH_P256_SHARED_SIZE, salt, sizeof salt, INFO_BYTES(DH_INFO_H2B),
                   k_h2b, DH_SESSION_KEY_SIZE);
    dh_hkdf_sha256(shared_secret, DH_P256_SHARED_SIZE, salt, sizeof salt, INFO_BYTES(DH_INFO_B2H),
                   k_b2h, DH_SESSION_KEY_SIZE);
}

void dh_auth_tag(const uint8_t key[DH_SESSION_KEY_SIZE], uint8_t type, uint8_t flags,
                 uint16_t len, uint64_t counter, const uint8_t *body, size_t body_len,
                 uint8_t out[DH_FRAME_TAG_SIZE]) {
    /* The four header bytes exactly as they appear on the wire, then the
       counter, then the body. The tag does not cover itself. */
    uint8_t covered[DH_FRAME_HEADER_SIZE + DH_FRAME_COUNTER_SIZE];
    covered[0] = type;
    covered[1] = flags;
    covered[2] = (uint8_t)(len & 0xFFu);
    covered[3] = (uint8_t)(len >> 8);
    for (unsigned i = 0; i < DH_FRAME_COUNTER_SIZE; i++)
        covered[DH_FRAME_HEADER_SIZE + i] = (uint8_t)(counter >> (i * 8u));

    dh_hmac_sha256_ctx c;
    dh_hmac_sha256_init(&c, key, DH_SESSION_KEY_SIZE);
    dh_hmac_sha256_update(&c, covered, sizeof covered);
    if (body_len > 0)
        dh_hmac_sha256_update(&c, body, body_len);

    uint8_t full[DH_SHA256_DIGEST_SIZE];
    dh_hmac_sha256_final(&c, full);
    memcpy(out, full, DH_FRAME_TAG_SIZE);
}

dh_auth_result dh_auth_wrap(const uint8_t key[DH_SESSION_KEY_SIZE], uint8_t type, uint8_t flags,
                            uint64_t counter, const uint8_t *body, size_t body_len, uint8_t *out,
                            size_t out_cap, size_t *out_len) {
    size_t total = DH_FRAME_AUTH_PREFIX_SIZE + body_len;
    if (total > DH_FRAME_MAX_PAYLOAD)
        return DH_AUTH_ERR_BUFFER;
    if (out_cap < total)
        return DH_AUTH_ERR_BUFFER;

    for (unsigned i = 0; i < DH_FRAME_COUNTER_SIZE; i++)
        out[i] = (uint8_t)(counter >> (i * 8u));
    if (body_len > 0)
        memcpy(out + DH_FRAME_AUTH_PREFIX_SIZE, body, body_len);

    dh_auth_tag(key, type, flags, (uint16_t)total, counter, body, body_len,
                out + DH_FRAME_COUNTER_SIZE);

    *out_len = total;
    return DH_AUTH_OK;
}

dh_frame_result dh_auth_frame(uint8_t type, uint8_t flags, const uint8_t key[DH_SESSION_KEY_SIZE],
                              uint64_t counter, const uint8_t *body, size_t body_len, uint8_t *out,
                              size_t cap, size_t *out_len) {
    const size_t payload_len = DH_FRAME_AUTH_PREFIX_SIZE + body_len;
    if (payload_len > DH_FRAME_MAX_PAYLOAD) return DH_FRAME_ERR_OVERSIZE;
    if (cap < DH_FRAME_HEADER_SIZE + payload_len) return DH_FRAME_ERR_BUFFER;

    out[0] = type;
    out[1] = flags;
    out[2] = (uint8_t)(payload_len & 0xFFu);
    out[3] = (uint8_t)(payload_len >> 8u);

    size_t written = 0;
    if (dh_auth_wrap(key, type, flags, counter, body, body_len, out + DH_FRAME_HEADER_SIZE,
                     cap - DH_FRAME_HEADER_SIZE, &written) != DH_AUTH_OK)
        return DH_FRAME_ERR_BUFFER;

    *out_len = DH_FRAME_HEADER_SIZE + written;
    return DH_FRAME_OK;
}

void dh_auth_counter_init(dh_auth_counter *c) {
    c->highest = 0;
    c->seen = 0;
    c->any = false;
}

bool dh_auth_counter_ok(const dh_auth_counter *c, uint64_t counter) {
    /* Counter 0 is a real counter — the hello is always 0 — so "nothing
       accepted yet" cannot be represented by a zero highest. */
    if (!c->any) return true;
    /* Ahead of everything seen: the ordinary case, and the only one before a
       reordering path existed. */
    if (counter > c->highest) return true;
    /* Older than the window. Refused outright rather than searched for: past
       this distance the record of what was seen no longer exists, and
       accepting on an absence of evidence is what a replay window is for
       preventing. */
    if (c->highest - counter >= DH_AUTH_WINDOW) return false;
    /* Inside the window, and only if it has not already been through. */
    return (c->seen & (UINT64_C(1) << (c->highest - counter))) == 0;
}

void dh_auth_counter_accept(dh_auth_counter *c, uint64_t counter) {
    if (!c->any) {
        c->any = true;
        c->highest = counter;
        c->seen = 1;
        return;
    }

    if (counter > c->highest) {
        const uint64_t shift = counter - c->highest;
        /* A jump wider than the window leaves nothing worth carrying: every
           bit would shift out. Said as a branch because shifting a uint64_t by
           64 is undefined, not zero. */
        c->seen = (shift >= DH_AUTH_WINDOW) ? UINT64_C(1)
                                            : ((c->seen << shift) | UINT64_C(1));
        c->highest = counter;
        return;
    }

    /* Inside the window. dh_auth_counter_ok has already refused anything
       older, so the shift is in range. */
    if (c->highest - counter < DH_AUTH_WINDOW)
        c->seen |= UINT64_C(1) << (c->highest - counter);
}

bool dh_auth_peek_counter(const uint8_t *payload, size_t payload_len, uint64_t *out) {
    /* The counter, not the whole prefix. docs/protocol.md puts the counter
       first precisely so a board can reject a replay after reading 12 bytes —
       4 of header and these 8 — without buffering a payload it is going to
       throw away. Requiring all 24 would make that read impossible and quietly
       cost the optimisation the layout exists for. */
    if (payload_len < DH_FRAME_COUNTER_SIZE)
        return false;

    uint64_t counter = 0;
    for (unsigned i = 0; i < DH_FRAME_COUNTER_SIZE; i++)
        counter |= (uint64_t)payload[i] << (i * 8u);
    *out = counter;
    return true;
}

dh_auth_result dh_auth_open(const uint8_t key[DH_SESSION_KEY_SIZE], const dh_frame_header *hdr,
                            const uint8_t *payload, dh_auth_counter *counter,
                            const uint8_t **body, size_t *body_len) {
    /* The tag is what needs the whole prefix; the counter alone needs 8 bytes.
       So the length is checked here rather than inferred from the peek. */
    if (hdr->len < DH_FRAME_AUTH_PREFIX_SIZE)
        return DH_AUTH_ERR_SHORT;

    uint64_t seen = 0;
    if (!dh_auth_peek_counter(payload, hdr->len, &seen))
        return DH_AUTH_ERR_SHORT;

    const uint8_t *frame_body = payload + DH_FRAME_AUTH_PREFIX_SIZE;
    size_t frame_body_len = (size_t)hdr->len - DH_FRAME_AUTH_PREFIX_SIZE;

    uint8_t expected[DH_FRAME_TAG_SIZE];
    dh_auth_tag(key, hdr->type, hdr->flags, hdr->len, seen, frame_body, frame_body_len, expected);
    if (!dh_auth_bytes_equal(expected, payload + DH_FRAME_COUNTER_SIZE, DH_FRAME_TAG_SIZE))
        return DH_AUTH_ERR_TAG;

    /*
     * The counter is checked second, and recorded only here. A frame that did
     * not authenticate must not be able to move this state: on a shared
     * endpoint (#95) anything that could push the window forward would lock
     * the real helper out of its own session with frames it never sent.
     */
    if (!dh_auth_counter_ok(counter, seen))
        return DH_AUTH_ERR_COUNTER;
    dh_auth_counter_accept(counter, seen);

    *body = frame_body;
    *body_len = frame_body_len;
    return DH_AUTH_OK;
}

bool dh_auth_bytes_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}
