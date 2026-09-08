#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "channel_lifecycle.h"

/* Keep checks active in the MSVC Release host suite too. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static channel_lifecycle c;
static bool locked;
static const uint8_t secret[DH_P256_SHARED_SIZE] = {1};
static const uint8_t key_id[DH_KEY_ID_SIZE] = {2};
static uint8_t board_nonce[DH_NONCE_SIZE] = {3};
static uint8_t helper_nonce[DH_NONCE_SIZE] = {4};
static uint8_t wire[DH_FRAME_MAX_SIZE];

void channel_lifecycle_lock(void) { CHECK(!locked); locked = true; }
void channel_lifecycle_unlock(void) { CHECK(locked); locked = false; }

static size_t drain(size_t cap) {
    dh_outq_view v;
    if (!dh_outq_peek(&c.out, &v)) return 0;
    const uint16_t n = (uint16_t)(v.remaining < cap ? v.remaining : cap);
    memcpy(wire, v.at, n);
    dh_outq_advance(&c.out, &v, n);
    return n;
}

static void hello(bool valid) {
    uint8_t bytes[DH_SESSION_REPLY_MAX], key[DH_SESSION_KEY_SIZE];
    size_t len = 0, consumed = 0;
    dh_hello h = {.proto_version = DH_PROTO_VERSION, .os = DH_OS_MAC,
                  .channel_count = 1, .max_chunk = 1024, .correlation = 42};
    memcpy(h.helper_key_id, key_id, sizeof key_id);
    memcpy(h.helper_nonce, helper_nonce, sizeof helper_nonce);
    dh_auth_derive_hello_key(secret, helper_nonce, key);
    CHECK(dh_hello_encode(&h, key, 0, bytes, sizeof bytes, &len) == DH_FRAME_OK);
    if (!valid) bytes[DH_FRAME_HEADER_SIZE + 8] ^= 1;
    dh_session_stage_nonce(&c.session, board_nonce);
    dh_frame_reader reader;
    dh_frame_reader_init(&reader);
    dh_frame_view frame;
    CHECK(dh_frame_reader_push(&reader, bytes, len, &consumed, &frame) == DH_FRAME_OK);
    CHECK(consumed == len);
    channel_lifecycle_on_frame(&c, &frame, 100);
}

static void init(void) {
    memset(&c, 0, sizeof c);
    dh_session_init(&c.session, DH_BUILD_RELEASE);
    dh_pair_init(&c.pair);
    dh_pair_set_registration(&c.pair, key_id, secret);
    dh_outq_init(&c.out);
    hello(true);
    CHECK(drain(sizeof wire) > 0);
}

static size_t queue_bulk(uint8_t *encoded) {
    const uint8_t body[100] = {0x5a};
    dh_frame_view f = {.hdr = {.type = DH_MSG_CLIP_CHUNK, .len = sizeof body},
                       .payload = body};
    size_t len = 0;
    CHECK(dh_session_emit_relayed(&c.session, &f, encoded, 256, &len) == DH_FRAME_OK);
    CHECK(channel_lifecycle_queue(&c, encoded, len, 100));
    return len;
}

static void test_fresh_hello_discards_partial_and_queued_frames(void) {
    init();
    uint8_t old[256];
    queue_bulk(old);
    CHECK(drain(17) == 17);
    queue_bulk(old);
    helper_nonce[0]++;
    board_nonce[0]++;
    hello(true);
    const size_t n = drain(sizeof wire);
    dh_frame_view f;
    size_t consumed = 0;
    CHECK(dh_frame_decode(wire, n, &f, &consumed) == DH_FRAME_OK);
    CHECK(f.hdr.type == DH_MSG_HELLO_ACK);
    uint8_t h2b[DH_SESSION_KEY_SIZE], b2h[DH_SESSION_KEY_SIZE];
    dh_auth_derive_session_keys(secret, helper_nonce, board_nonce, h2b, b2h);
    dh_auth_counter rx = {0};
    const uint8_t *body;
    size_t body_len;
    CHECK(dh_auth_open(b2h, &f.hdr, f.payload, &rx, &body, &body_len) == DH_AUTH_OK);
    dh_hello_ack ack;
    CHECK(dh_hello_ack_decode(body, body_len, &ack));
    CHECK(ack.correlation == 42);
    CHECK(drain(sizeof wire) == 0);
}

static void test_refused_hello_preserves_live_stream(void) {
    init();
    uint8_t old[256], queued[256];
    const size_t len = queue_bulk(old);
    CHECK(drain(17) == 17);
    const size_t next_len = queue_bulk(queued);
    hello(false);
    CHECK(drain(sizeof wire) == len - 17);
    CHECK(memcmp(wire, old + 17, len - 17) == 0);
    CHECK(drain(sizeof wire) == next_len);
    CHECK(memcmp(wire, queued, next_len) == 0);
    CHECK(drain(sizeof wire) == 0);
    /* Still usable under the original session, not only leftover bytes. */
    queue_bulk(old);
    CHECK(drain(sizeof wire) == len);
}

static void test_queue_refusal_survives_reconnect(void) {
    init();
    uint8_t bytes[256];
    const uint8_t body[] = {7};
    const dh_frame_view f = {.hdr = {.type = DH_MSG_POS_QUERY, .len = sizeof body},
                             .payload = body};
    size_t len = 0;
    CHECK(dh_session_emit_relayed(&c.session, &f, bytes, sizeof bytes, &len) == DH_FRAME_OK);
    CHECK(channel_lifecycle_queue(&c, bytes, len, 100));
    CHECK(channel_lifecycle_queue(&c, bytes, len, 100));
    CHECK(!channel_lifecycle_queue(&c, bytes, len, 100));
    CHECK(c.out.refused == 1 && c.out.refused_priority == 1 && c.tx.dropped == 1);
    hello(true);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_HELLO_ACK);
    CHECK(drain(sizeof wire) == 0);
    CHECK(c.out.refused == 1 && c.out.refused_priority == 1 && c.tx.dropped == 1);
}

int main(void) {
    test_fresh_hello_discards_partial_and_queued_frames();
    test_refused_hello_preserves_live_stream();
    test_queue_refusal_survives_reconnect();
    puts("channel lifecycle tests passed");
    return 0;
}
