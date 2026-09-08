#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "channel_lifecycle.h"
#include "dh_xfer.h"

/* Keep checks active in the MSVC Release host suite too. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static channel_lifecycle c;
static bool relay_accepts;
static dh_relay_packet packets[32];
static size_t packet_count;
static uint32_t observed_now;
static bool unavailable_accepts;
static unsigned unavailable_count;
static uint8_t unavailable_id;

void channel_lifecycle_position(void *context, const uint8_t *body, size_t len) {
    (void)context; (void)body; (void)len;
}
void channel_lifecycle_update_config(void *context, uint32_t now) {
    (void)context; observed_now = now;
}
bool channel_lifecycle_send_relay(const dh_relay_packet *packet) {
    if (!relay_accepts) return false;
    CHECK(packet_count < sizeof packets / sizeof packets[0]);
    packets[packet_count++] = *packet;
    return true;
}
void channel_lifecycle_barrier(void) {}
void channel_lifecycle_save_registration(void *context) { (void)context; }
bool channel_lifecycle_query_unavailable(void *context, uint8_t query_id) {
    (void)context;
    if (!unavailable_accepts) return false;
    unavailable_count++;
    unavailable_id = query_id;
    return true;
}

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

static size_t make_hello(bool valid, uint8_t *bytes) {
    uint8_t key[DH_SESSION_KEY_SIZE];
    size_t len = 0;
    dh_hello h = {.proto_version = DH_PROTO_VERSION, .os = DH_OS_MAC,
                  .channel_count = 1, .max_chunk = 1024, .correlation = 42};
    memcpy(h.helper_key_id, key_id, sizeof key_id);
    memcpy(h.helper_nonce, helper_nonce, sizeof helper_nonce);
    dh_auth_derive_hello_key(secret, helper_nonce, key);
    CHECK(dh_hello_encode(&h, key, 0, bytes, DH_SESSION_REPLY_MAX, &len) == DH_FRAME_OK);
    if (!valid) bytes[DH_FRAME_HEADER_SIZE + 8] ^= 1;
    return len;
}

static void hello(bool valid) {
    uint8_t bytes[DH_SESSION_REPLY_MAX];
    const size_t len = make_hello(valid, bytes);
    size_t consumed = 0;
    dh_session_stage_nonce(&c.session, board_nonce);
    dh_frame_reader reader;
    dh_frame_reader_init(&reader);
    dh_frame_view frame;
    CHECK(dh_frame_reader_push(&reader, bytes, len, &consumed, &frame) == DH_FRAME_OK);
    CHECK(consumed == len);
    channel_lifecycle_on_frame(&c, &frame, 100);
}

static void receive_bytes(const uint8_t *bytes, size_t len) {
    while (len > 0) {
        const uint16_t n = (uint16_t)(len < CHANNEL_REPORT_SIZE ? len : CHANNEL_REPORT_SIZE);
        channel_lifecycle_receive_report(&c, bytes, n);
        bytes += n;
        len -= n;
    }
}

static void receive_hello(void) {
    uint8_t bytes[DH_SESSION_REPLY_MAX];
    const size_t len = make_hello(true, bytes);
    dh_session_stage_nonce(&c.session, board_nonce);
    receive_bytes(bytes, len);
}

static void init(void) {
    memset(&c, 0, sizeof c);
    relay_accepts = false;
    packet_count = 0;
    unavailable_accepts = false;
    unavailable_count = 0;
    dh_session_init(&c.session, DH_BUILD_RELEASE);
    dh_pair_init(&c.pair);
    dh_pair_set_registration(&c.pair, key_id, secret);
    dh_outq_init(&c.out);
    dh_frame_reader_init(&c.reader);
    dh_inq_init(&c.inbound);
    dh_relay_tx_init(&c.relay_tx);
    dh_relay_rx_init(&c.relay_rx, c.relay_rx_buf, sizeof c.relay_rx_buf);
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

static void test_link_loss_discards_work_but_keeps_registration_and_window(void) {
    init();
    uint8_t bytes[256];
    queue_bulk(bytes);
    CHECK(drain(17) == 17);
    dh_pair_open_window(&c.pair, 100);
    channel_lifecycle_link_lost(&c);
    CHECK(drain(sizeof wire) == 0);
    CHECK(!c.session.present);
    CHECK(dh_pair_is_registered_key(&c.pair, key_id));
    CHECK(dh_pair_window_open(&c.pair, 101));
    hello(true);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_HELLO_ACK);
}

static void test_wipe_revokes_session_and_registration_but_preserves_identity(void) {
    init();
    uint8_t private_key[DH_P256_PRIVATE_SIZE] = {0};
    private_key[0] = 1;
    CHECK(dh_pair_set_identity(&c.pair, private_key));
    uint8_t public_key[DH_P256_PUBLIC_SIZE];
    memcpy(public_key, dh_pair_public_key(&c.pair), sizeof public_key);
    channel_lifecycle_config_wiped(&c, 101);
    CHECK(!dh_pair_is_registered_key(&c.pair, key_id));
    CHECK(!c.session.present);
    CHECK(memcmp(public_key, dh_pair_public_key(&c.pair), sizeof public_key) == 0);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_SESSION_END);
    hello(true);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_HELLO_REFUSED);
}

static void check_end_reason(uint8_t reason) {
    const size_t len = drain(sizeof wire);
    dh_frame_view frame;
    size_t consumed = 0;
    CHECK(dh_frame_decode(wire, len, &frame, &consumed) == DH_FRAME_OK);
    CHECK(frame.hdr.type == DH_MSG_SESSION_END);
    uint8_t h2b[DH_SESSION_KEY_SIZE], b2h[DH_SESSION_KEY_SIZE];
    dh_auth_derive_session_keys(secret, helper_nonce, board_nonce, h2b, b2h);
    dh_auth_counter rx = {0};
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(dh_auth_open(b2h, &frame.hdr, frame.payload, &rx, &body, &body_len) == DH_AUTH_OK);
    CHECK(body_len > 0 && body[0] == reason);
}

static void test_report_gap_discards_backlog_before_decoding(void) {
    init();
    const uint8_t partial[] = {DH_MSG_CLIP_CHUNK, 0, 100, 0, 0, 0};
    receive_bytes(partial, sizeof partial);
    channel_lifecycle_step(&c, 100, NULL);
    while (drain(sizeof wire) > 0) {}
    receive_hello(); /* Must not be decoded after the later report is lost. */
    const uint8_t pad = 0;
    for (unsigned i = 0; i < CHANNEL_REPORT_BACKLOG; ++i)
        channel_lifecycle_receive_report(&c, &pad, 1);
    channel_lifecycle_step(&c, 101, NULL);
    check_end_reason(DH_SESSION_END_STREAM_GAP);
    CHECK(drain(sizeof wire) == 0);
    CHECK(c.reports_dropped > 0 && !c.session.present);
    const uint32_t dropped = c.reports_dropped;
    receive_hello();
    channel_lifecycle_step(&c, 102, NULL);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_HELLO_ACK);
    CHECK(c.reports_dropped == dropped);
}

static void test_malformed_report_ends_session_and_allows_reconnect(void) {
    init();
    const uint8_t malformed[] = {0xff, 0, 0, 0};
    receive_bytes(malformed, sizeof malformed);
    channel_lifecycle_step(&c, 101, NULL);
    check_end_reason(DH_SESSION_END_PROTOCOL_ERROR);
    CHECK(!c.session.present && dh_pair_is_registered_key(&c.pair, key_id));
    receive_hello();
    channel_lifecycle_step(&c, 102, NULL);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_HELLO_ACK);
}

static void test_reception_and_liveness_share_the_pass_clock(void) {
    init();
    uint8_t bytes[DH_SESSION_REPLY_MAX], h2b[DH_SESSION_KEY_SIZE], b2h[DH_SESSION_KEY_SIZE];
    size_t len = 0;
    dh_auth_derive_session_keys(secret, helper_nonce, board_nonce, h2b, b2h);
    CHECK(dh_auth_frame(DH_MSG_HEARTBEAT, 0, h2b, 0, NULL, 0,
                         bytes, sizeof bytes, &len) == DH_FRAME_OK);
    receive_bytes(bytes, len);
    const uint32_t now = 100 + DH_SESSION_ABSENT_MS;
    channel_lifecycle_step(&c, now, NULL);
    CHECK(observed_now == now && c.session.present);
    while (drain(sizeof wire) > 0) CHECK(wire[0] != DH_MSG_SESSION_END);
    channel_lifecycle_step(&c, now + DH_SESSION_ABSENT_MS, NULL);
    check_end_reason(DH_SESSION_END_LIVENESS_TIMEOUT);
}

static void test_policy_refusal_leaves_work_owed(void) {
    init();
    const uint8_t query[] = {7};
    CHECK(channel_lifecycle_emit_placement(&c, DH_MSG_POS_QUERY, query, sizeof query, 100));
    CHECK(channel_lifecycle_emit_placement(&c, DH_MSG_POS_QUERY, query, sizeof query, 100));
    channel_lifecycle_step(&c, 101, NULL);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_POS_QUERY);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_POS_QUERY);
    CHECK(drain(sizeof wire) == 0);
    channel_lifecycle_step(&c, 102, NULL);
    CHECK(drain(sizeof wire) > 0 && wire[0] == DH_MSG_CLIP_POLICY);
}

static void test_relay_refusal_retries_and_preserves_opaque_bytes(void) {
    init();
    uint8_t bytes[128], h2b[DH_SESSION_KEY_SIZE], b2h[DH_SESSION_KEY_SIZE];
    const uint8_t payload[] = {0xff, 0, 0xa5, 0x12, 0x34, 0x56, 0x78, 0, 0x81};
    size_t len = 0;
    dh_auth_derive_session_keys(secret, helper_nonce, board_nonce, h2b, b2h);
    CHECK(dh_auth_frame(DH_MSG_CLIP_CHUNK, 0, h2b, 0, payload, sizeof payload,
                         bytes, sizeof bytes, &len) == DH_FRAME_OK);
    receive_bytes(bytes, len);
    channel_lifecycle_step(&c, 101, NULL);
    CHECK(packet_count == 0);
    relay_accepts = true;
    channel_lifecycle_step(&c, 102, NULL);
    dh_relay_rx rx;
    uint8_t buffer[128];
    dh_relay_rx_init(&rx, buffer, sizeof buffer);
    dh_frame_view f;
    dh_relay_result result = DH_RELAY_AGAIN;
    for (size_t i = 0; i < packet_count; ++i)
        result = dh_relay_rx_push(&rx, &packets[i], &f);
    CHECK(result == DH_RELAY_OK);
    CHECK(f.hdr.type == DH_MSG_CLIP_CHUNK && f.hdr.len == sizeof payload);
    CHECK(memcmp(f.payload, payload, sizeof payload) == 0);
}

static void test_peer_query_is_acknowledged_only_after_queue_acceptance(void) {
    init();
    channel_lifecycle_link_lost(&c);
    c.cursor_query_origin = CURSOR_QUERY_PEER;
    c.cursor_query_id = 19; /* Same cross-core handoff as the hardware adapter. */
    channel_lifecycle_step(&c, 101, NULL);
    CHECK(unavailable_count == 0);
    unavailable_accepts = true;
    channel_lifecycle_step(&c, 102, NULL);
    CHECK(unavailable_count == 1 && unavailable_id == 19);
    channel_lifecycle_step(&c, 103, NULL);
    CHECK(unavailable_count == 1);
}

static void test_link_loss_resets_relay_and_inbound_without_erasing_diagnostics(void) {
    init();
    const uint8_t frame[] = {DH_MSG_CLIP_CHUNK, 0, 1, 0, 0xa5};
    CHECK(dh_relay_tx_offer(&c.relay_tx, frame, sizeof frame) == DH_RELAY_OK);
    CHECK(dh_inq_stage(&c.inbound, frame, sizeof frame));
    dh_inq_publish(&c.inbound);
    /* An actual orphan supplies a diagnostic which reset must not erase. */
    const dh_relay_packet orphan = {.kind = DH_RELAY_PKT_DATA, .len = DH_RELAY_PAYLOAD};
    dh_frame_view f;
    CHECK(dh_relay_rx_push(&c.relay_rx, &orphan, &f) != DH_RELAY_OK);
    const uint32_t orphans = c.relay_rx.orphans;
    CHECK(orphans > 0);
    channel_lifecycle_link_lost(&c);
    relay_accepts = true;
    receive_hello();
    channel_lifecycle_step(&c, 101, NULL);
    CHECK(packet_count == 0 && c.relay_rx.orphans == orphans);
    while (drain(sizeof wire) > 0) CHECK(wire[0] != DH_MSG_CLIP_CHUNK);
}

static void test_inbound_pressure_retains_frames_behind_the_counted_loss(void) {
    init();
    uint8_t old[256];
    for (unsigned i = 0; i <= DH_OUTQ_DEPTH; ++i) queue_bulk(old);
    const uint8_t first[] = {DH_MSG_CLIP_CHUNK, 0, 1, 0, 0xa5};
    const uint8_t next[] = {DH_MSG_CLIP_CHUNK, 0, 1, 0, 0x5a};
    CHECK(dh_inq_stage(&c.inbound, first, sizeof first));
    dh_inq_publish(&c.inbound);
    CHECK(dh_inq_stage(&c.inbound, next, sizeof next));
    dh_inq_publish(&c.inbound);
    channel_lifecycle_step(&c, 101, NULL);
    CHECK(c.out.refused_bulk == 1);
    while (drain(sizeof wire) > 0) {}
    channel_lifecycle_step(&c, 102, NULL);
    /* Priority policy/diagnostic traffic may overtake the queued bulk. */
    bool found = false;
    size_t len;
    while ((len = drain(sizeof wire)) > 0) {
        if (wire[0] != DH_MSG_CLIP_CHUNK) continue;
        dh_frame_view f;
        size_t consumed = 0;
        CHECK(dh_frame_decode(wire, len, &f, &consumed) == DH_FRAME_OK);
        uint8_t h2b[DH_SESSION_KEY_SIZE], b2h[DH_SESSION_KEY_SIZE];
        dh_auth_derive_session_keys(secret, helper_nonce, board_nonce, h2b, b2h);
        dh_auth_counter rx = {0};
        const uint8_t *body;
        size_t body_len;
        CHECK(dh_auth_open(b2h, &f.hdr, f.payload, &rx, &body, &body_len) == DH_AUTH_OK);
        CHECK(body_len == 1 && body[0] == 0x5a);
        CHECK(!found);
        found = true;
    }
    CHECK(found);
}

static void test_two_channels_keep_frames_whole_and_priority_on_zero(void) {
    init();
    c.session.channel_count = 2;
    uint8_t first[256], second[256];
    const size_t n = queue_bulk(first);
    CHECK(queue_bulk(second) == n);
    dh_outq_view a, b;
    CHECK(dh_outq_peek(&c.out, &a));
    CHECK(dh_outq_peek(&c.extra_out[0], &b));
    CHECK(a.total == n && b.total == n);
    CHECK(memcmp(a.at, first, n) == 0 && memcmp(b.at, second, n) == 0);
    dh_outq_advance(&c.extra_out[0], &b, 64);
    dh_outq_advance(&c.out, &a, (uint16_t)n);
    const uint8_t beat[] = {DH_MSG_DEVICE_HEARTBEAT, 0, 0, 0};
    CHECK(channel_lifecycle_queue(&c, beat, sizeof beat, 101));
    CHECK(dh_outq_peek(&c.out, &a) && a.at[0] == DH_MSG_DEVICE_HEARTBEAT);
    CHECK(dh_outq_peek(&c.extra_out[0], &b));
    CHECK(b.remaining == n - 64 && memcmp(b.at, second + 64, n - 64) == 0);
    hello(true); /* A one-channel helper reconnects over a partial second stream. */
    CHECK(!dh_outq_busy(&c.extra_out[0]));
    CHECK(c.session.channel_count == 1);
}

static void test_interleaved_channel_reports_reassemble_independently(void) {
    init();
    c.session.channel_count = 2;
    uint8_t frames[2][256], body[100] = {0x5a};
    size_t len[2];
    for (unsigned i = 0; i < 2; ++i)
        CHECK(dh_auth_frame(DH_MSG_CLIP_CHUNK, 0, c.session.k_h2b, i, body,
                            sizeof body, frames[i], sizeof frames[i], &len[i]) == DH_FRAME_OK);
    channel_lifecycle_receive_channel_report(&c, 0, frames[0], 64);
    channel_lifecycle_receive_channel_report(&c, 1, frames[1], 64);
    channel_lifecycle_receive_channel_report(&c, 1, frames[1] + 64, (uint16_t)(len[1] - 64));
    channel_lifecycle_receive_channel_report(&c, 0, frames[0] + 64, (uint16_t)(len[0] - 64));
    channel_lifecycle_step(&c, 101, NULL);
    CHECK(c.session.rx.accepted == 2);
    CHECK(c.session.present);
}

static void test_two_channels_share_one_transfer_credit_window(void) {
    init();
    c.session.channel_count = 2;
    static dh_xfer transfer;
    static uint8_t payload[10 * DH_XFER_CHUNK_SIZE];
    dh_xfer_action actions[8];
    dh_xfer_init(&transfer, NULL, 0);
    CHECK(dh_xfer_offer(&transfer, 0, NULL, 0, payload, sizeof payload, actions, 8) == 1);
    (void)dh_xfer_handle_request(&transfer, transfer.tx.id, actions, 8);
    (void)dh_xfer_handle_credit(&transfer, transfer.tx.id, DH_XFER_CREDIT_WINDOW, actions, 8);
    const size_t n = dh_xfer_pump(&transfer, actions, 8);
    CHECK(n == 3);
    for (size_t i = 0; i < n; ++i) {
        CHECK(actions[i].type == DH_XFER_ACT_SEND_CHUNK);
        uint8_t frame[256];
        queue_bulk(frame); /* The opaque relay does not inspect chunk bodies. */
    }
    CHECK(dh_outq_busy(&c.out) && dh_outq_busy(&c.extra_out[0]));
    CHECK(dh_xfer_pump(&transfer, actions, 8) == 0);
    /* Draining either USB queue cannot mint end-to-end credits. */
    while (drain(sizeof wire) > 0) {}
    CHECK(dh_xfer_pump(&transfer, actions, 8) == 0);
    (void)dh_xfer_handle_credit(&transfer, transfer.tx.id, 1, actions, 8);
    CHECK(dh_xfer_pump(&transfer, actions, 8) == 1);
}

int main(void) {
    test_two_channels_share_one_transfer_credit_window();
    test_interleaved_channel_reports_reassemble_independently();
    test_two_channels_keep_frames_whole_and_priority_on_zero();
    test_fresh_hello_discards_partial_and_queued_frames();
    test_refused_hello_preserves_live_stream();
    test_queue_refusal_survives_reconnect();
    test_link_loss_discards_work_but_keeps_registration_and_window();
    test_wipe_revokes_session_and_registration_but_preserves_identity();
    test_report_gap_discards_backlog_before_decoding();
    test_malformed_report_ends_session_and_allows_reconnect();
    test_reception_and_liveness_share_the_pass_clock();
    test_policy_refusal_leaves_work_owed();
    test_relay_refusal_retries_and_preserves_opaque_bytes();
    test_peer_query_is_acknowledged_only_after_queue_acceptance();
    test_link_loss_resets_relay_and_inbound_without_erasing_diagnostics();
    test_inbound_pressure_retains_frames_behind_the_counted_loss();
    puts("channel lifecycle tests passed");
    return 0;
}
