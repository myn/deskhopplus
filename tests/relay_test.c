/*
 * Inter-board relay tests (#47): fragmentation, reassembly, the priority
 * band, the burst cap, and every way a packet can go missing.
 *
 * The golden vectors are the corpus — a frame that survives the USB hop must
 * survive this one identically, and the relay must not care what is in it.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "dh_relay.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

#define MAX_VECTORS 64

struct vector {
    char name[64];
    uint8_t bytes[DH_FRAME_MAX_SIZE];
    size_t len;
};

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t load_vectors(const char *path, struct vector *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[16384];
    size_t n = 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *bar = strchr(p, '|');
        if (!bar) continue;

        struct vector *v = &out[n];
        size_t name_len = 0;
        for (char *q = p; q < bar && name_len + 1 < sizeof v->name; q++)
            if (!isspace((unsigned char)*q)) v->name[name_len++] = *q;
        v->name[name_len] = '\0';
        v->len = 0;
        int hi = -1;
        for (char *q = bar + 1; *q; q++) {
            if (isspace((unsigned char)*q)) continue;
            int nib = hex_nibble((unsigned char)*q);
            if (nib < 0) break;
            if (hi < 0) {
                hi = nib;
            } else {
                if (v->len >= sizeof v->bytes) break;
                v->bytes[v->len++] = (uint8_t)((hi << 4) | nib);
                hi = -1;
            }
        }
        if (v->len) n++;
    }
    fclose(f);
    return n;
}

/* Storage the firmware would own. The transmitter carries its own (dh_outq);
   only the reassembler still takes a caller buffer. */
static uint8_t rx_buf[DH_FRAME_MAX_SIZE];

/* Drain everything the transmitter owes, yielding between pumps as the
 * firmware's scheduler would, and feed each packet to a reassembler. */
static size_t relay_across(dh_relay_tx *tx, dh_relay_rx *rx, uint8_t *out, size_t out_cap,
                           size_t *pumps) {
    size_t delivered = 0;
    *pumps = 0;

    while (dh_relay_tx_busy(tx)) {
        dh_relay_tx_yield(tx);
        (*pumps)++;

        dh_relay_packet packet;
        size_t this_pump = 0;
        while (dh_relay_tx_peek(tx, &packet)) {
            dh_relay_tx_commit(tx);
            this_pump++;

            dh_frame_view view;
            if (dh_relay_rx_push(rx, &packet, &view) == DH_RELAY_OK) {
                const size_t total = DH_FRAME_HEADER_SIZE + view.hdr.len;
                if (delivered + total <= out_cap) {
                    memcpy(out + delivered, view.payload - DH_FRAME_HEADER_SIZE, total);
                    delivered += total;
                }
            }
        }
        if (this_pump == 0) break; /* nothing movable: avoid spinning */
    }
    return delivered;
}

static void test_every_frame_crosses_intact(const char *path) {
    static struct vector vectors[MAX_VECTORS];
    const size_t nvec = load_vectors(path, vectors, MAX_VECTORS);
    CHECK(nvec >= 15, "load", "vector file missing or too small");

    for (size_t i = 0; i < nvec; i++) {
        dh_relay_tx tx;
        dh_relay_rx rx;
        dh_relay_tx_init(&tx);
        dh_relay_rx_init(&rx, rx_buf, sizeof rx_buf);

        CHECK(dh_relay_tx_offer(&tx, vectors[i].bytes, vectors[i].len) == DH_RELAY_OK,
              vectors[i].name, "frame refused by the relay");

        uint8_t out[DH_FRAME_MAX_SIZE];
        size_t pumps = 0;
        const size_t delivered = relay_across(&tx, &rx, out, sizeof out, &pumps);

        CHECK(delivered == vectors[i].len && memcmp(out, vectors[i].bytes, delivered) == 0,
              vectors[i].name, "frame did not cross the link identically");
        CHECK(rx.orphans == 0 && rx.truncated == 0, vectors[i].name, "loss counted on a clean run");
    }
}

/* A payload full of bytes that look like frame headers must be carried, not
 * read: the firmware parses headers and never payloads. */
static void test_a_payload_that_looks_like_frames_is_still_opaque(void) {
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)(i % 2 ? 0x00 : DH_MSG_CLIP_CHUNK);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    CHECK(dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, payload, sizeof payload, frame, sizeof frame,
                          &frame_len) == DH_FRAME_OK,
          "opaque", "encode failed");

    dh_relay_tx tx;
    dh_relay_rx rx;
    dh_relay_tx_init(&tx);
    dh_relay_rx_init(&rx, rx_buf, sizeof rx_buf);
    CHECK(dh_relay_tx_offer(&tx, frame, frame_len) == DH_RELAY_OK, "opaque", "frame refused");

    uint8_t out[DH_FRAME_MAX_SIZE];
    size_t pumps = 0;
    const size_t delivered = relay_across(&tx, &rx, out, sizeof out, &pumps);
    CHECK(delivered == frame_len && memcmp(out, frame, frame_len) == 0, "opaque",
          "a payload resembling frames was not carried verbatim");
}

static void test_data_packets_carry_a_full_payload(void) {
    uint8_t payload[64] = {0};
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, payload, sizeof payload, frame, sizeof frame,
                          &frame_len);

    dh_relay_tx tx;
    dh_relay_tx_init(&tx);
    CHECK(dh_relay_tx_offer(&tx, frame, frame_len) == DH_RELAY_OK, "payload", "frame refused");

    size_t starts = 0, data = 0;
    dh_relay_packet packet;
    while (dh_relay_tx_busy(&tx)) {
        dh_relay_tx_yield(&tx);
        while (dh_relay_tx_peek(&tx, &packet)) {
            dh_relay_tx_commit(&tx);
            if (packet.kind == DH_RELAY_PKT_START) {
                starts++;
                const uint16_t total = (uint16_t)(packet.data[0] | (packet.data[1] << 8));
                CHECK(total == frame_len, "payload", "start packet carries the wrong length");
            } else {
                data++;
                CHECK(packet.len == DH_RELAY_PAYLOAD, "payload",
                      "a data packet carried less than a full payload");
            }
        }
    }

    CHECK(starts == 1, "payload", "expected exactly one start packet");
    /* 68 bytes over 8-byte payloads: 9 packets, the last one part padding. */
    CHECK(data == (frame_len + DH_RELAY_PAYLOAD - 1) / DH_RELAY_PAYLOAD, "payload",
          "wrong number of data packets");
}

static void test_a_lost_start_orphans_its_data(void) {
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    const uint8_t body[12] = {1, 0, 0, 0};
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, frame, sizeof frame,
                          &frame_len);

    dh_relay_tx tx;
    dh_relay_rx rx;
    dh_relay_tx_init(&tx);
    dh_relay_rx_init(&rx, rx_buf, sizeof rx_buf);
    (void)dh_relay_tx_offer(&tx, frame, frame_len);

    dh_relay_packet packet;
    dh_frame_view view;
    bool dropped_start = false;
    size_t delivered = 0;

    while (dh_relay_tx_busy(&tx)) {
        dh_relay_tx_yield(&tx);
        while (dh_relay_tx_peek(&tx, &packet)) {
            dh_relay_tx_commit(&tx);
            if (packet.kind == DH_RELAY_PKT_START && !dropped_start) {
                dropped_start = true; /* the wire ate it */
                continue;
            }
            if (dh_relay_rx_push(&rx, &packet, &view) == DH_RELAY_OK) delivered++;
        }
    }

    CHECK(delivered == 0, "orphan", "a frame was delivered without its start packet");
    CHECK(rx.orphans > 0, "orphan", "orphaned data was not counted");
}

static void test_a_lost_data_packet_abandons_the_frame(void) {
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    uint8_t body[40] = {0};
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, frame, sizeof frame,
                          &frame_len);

    dh_relay_rx rx;
    dh_relay_rx_init(&rx, rx_buf, sizeof rx_buf);

    /* Two frames sent; a data packet of the first goes missing, so the
       second's start is where the loss surfaces. */
    dh_relay_packet packets[64];
    size_t n = 0;
    for (int round = 0; round < 2; round++) {
        dh_relay_tx tx;
        dh_relay_tx_init(&tx);
        (void)dh_relay_tx_offer(&tx, frame, frame_len);
        while (dh_relay_tx_busy(&tx)) {
            dh_relay_tx_yield(&tx);
            while (dh_relay_tx_peek(&tx, &packets[n])) {
                dh_relay_tx_commit(&tx);
                if (n + 1 < sizeof packets / sizeof packets[0]) n++;
            }
        }
    }

    size_t delivered = 0, truncations = 0;
    bool dropped_one = false;
    for (size_t i = 0; i < n; i++) {
        if (packets[i].kind == DH_RELAY_PKT_DATA && !dropped_one) {
            dropped_one = true;
            continue;
        }
        dh_frame_view view;
        const dh_relay_result rc = dh_relay_rx_push(&rx, &packets[i], &view);
        if (rc == DH_RELAY_OK) delivered++;
        if (rc == DH_RELAY_ERR_TRUNCATED) truncations++;
    }

    CHECK(truncations == 1, "loss", "a short frame was not detected at the next start");
    CHECK(rx.truncated == 1, "loss", "the truncation was not counted");
    CHECK(delivered == 1, "loss",
          "the intact frame did not survive its predecessor's loss");
}

static void test_malformed_and_oversized_frames_are_refused(void) {
    dh_relay_tx tx;
    dh_relay_tx_init(&tx);

    const uint8_t unknown[] = {0xEE, 0x00, 0x00, 0x00};
    CHECK(dh_relay_tx_offer(&tx, unknown, sizeof unknown) == DH_RELAY_ERR_FRAME, "refuse",
          "an unknown type was relayed");

    const uint8_t oversize[] = {DH_MSG_CLIP_CHUNK, 0x00, 0x01, 0x10}; /* len 4097 */
    CHECK(dh_relay_tx_offer(&tx, oversize, sizeof oversize) == DH_RELAY_ERR_FRAME, "refuse",
          "a 4097-byte length was relayed");

    /* A header promising more than it brought is not a short frame. */
    const uint8_t short_frame[] = {DH_MSG_CLIP_DONE, 0x00, 0x04, 0x00, 0x02};
    CHECK(dh_relay_tx_offer(&tx, short_frame, sizeof short_frame) == DH_RELAY_ERR_FRAME,
          "refuse", "a truncated frame was relayed");

    /* Too long for the band's slot: refused, never truncated to fit. */
    uint8_t big[DH_FRAME_MAX_SIZE];
    size_t big_len = 0;
    uint8_t body[DH_OUTQ_PRIORITY_MAX] = {0};
    (void)dh_frame_encode(DH_MSG_PAIR_GRANT, 0, body, sizeof body, big, sizeof big, &big_len);
    CHECK(dh_relay_tx_offer(&tx, big, big_len) == DH_RELAY_ERR_OVERSIZE, "refuse",
          "a frame too long for its slot was accepted");
    CHECK(tx.q.refused > 0, "refuse", "refusals were not counted");
    CHECK(!dh_relay_tx_busy(&tx), "refuse", "a refused frame occupied a slot");
}

static void test_a_reassembler_rejects_an_impossible_length(void) {
    dh_relay_rx rx;
    dh_relay_rx_init(&rx, rx_buf, sizeof rx_buf);

    dh_relay_packet start = {.kind = DH_RELAY_PKT_START, .len = DH_RELAY_PAYLOAD};
    start.data[0] = 0xFF;
    start.data[1] = 0xFF; /* 65535, far past the frame maximum */

    dh_frame_view view;
    CHECK(dh_relay_rx_push(&rx, &start, &view) == DH_RELAY_ERR_OVERSIZE, "rx",
          "an impossible length was accepted");

    dh_relay_packet data = {.kind = DH_RELAY_PKT_DATA, .len = DH_RELAY_PAYLOAD};
    CHECK(dh_relay_rx_push(&rx, &data, &view) == DH_RELAY_ERR_ORPHAN, "rx",
          "data was accepted after a refused start");
}

static void test_priority_goes_ahead_of_queued_bulk(void) {
    dh_relay_tx tx;
    dh_relay_tx_init(&tx);

    uint8_t bulk[DH_FRAME_MAX_SIZE];
    size_t bulk_len = 0;
    uint8_t body[512] = {0};
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, bulk, sizeof bulk, &bulk_len);

    uint8_t place[DH_FRAME_MAX_SIZE];
    size_t place_len = 0;
    const uint8_t placement[4] = {1, 0, 0, 0x80};
    (void)dh_frame_encode(DH_MSG_PLACE, 0, placement, sizeof placement, place, sizeof place,
                          &place_len);

    /* Bulk offered first, and not yet pumped: placement must overtake it. */
    CHECK(dh_relay_tx_offer(&tx, bulk, bulk_len) == DH_RELAY_OK, "priority", "bulk refused");
    CHECK(dh_relay_tx_offer(&tx, place, place_len) == DH_RELAY_OK, "priority",
          "placement refused while bulk was queued");

    dh_relay_tx_yield(&tx);
    dh_relay_packet packet;
    CHECK(dh_relay_tx_peek(&tx, &packet), "priority", "nothing owed");
    CHECK(packet.kind == DH_RELAY_PKT_START, "priority", "expected a start packet");
    const uint16_t first_len = (uint16_t)(packet.data[0] | (packet.data[1] << 8));
    CHECK(first_len == place_len, "priority", "bulk went out ahead of placement");

    /* And the two bands do not contend: one more priority frame can wait. */
    CHECK(dh_relay_tx_offer(&tx, place, place_len) == DH_RELAY_OK, "priority",
          "a second placement was refused while one was in flight");
    CHECK(dh_relay_tx_offer(&tx, place, place_len) == DH_RELAY_ERR_BUSY, "priority",
          "the priority band accepted more than its bounded pair");
}

static void test_bulk_in_flight_is_not_interrupted(void) {
    dh_relay_tx tx;
    dh_relay_tx_init(&tx);

    uint8_t bulk[DH_FRAME_MAX_SIZE];
    size_t bulk_len = 0;
    uint8_t body[64] = {0};
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, bulk, sizeof bulk, &bulk_len);
    (void)dh_relay_tx_offer(&tx, bulk, bulk_len);

    /* Start the bulk frame, then offer placement mid-flight. */
    dh_relay_tx_yield(&tx);
    dh_relay_packet packet;
    CHECK(dh_relay_tx_peek(&tx, &packet) && packet.kind == DH_RELAY_PKT_START, "atomic",
          "bulk did not start");
    dh_relay_tx_commit(&tx);

    uint8_t place[DH_FRAME_MAX_SIZE];
    size_t place_len = 0;
    const uint8_t placement[4] = {1, 0, 0, 0x80};
    (void)dh_frame_encode(DH_MSG_PLACE, 0, placement, sizeof placement, place, sizeof place,
                          &place_len);
    CHECK(dh_relay_tx_offer(&tx, place, place_len) == DH_RELAY_OK, "atomic",
          "placement refused mid-bulk");

    /* Every remaining packet of the bulk frame goes first: one reassembly
       context means interleaved fragments would splice into each other. */
    size_t bulk_data = 0;
    bool saw_second_start = false;
    while (dh_relay_tx_busy(&tx)) {
        dh_relay_tx_yield(&tx);
        while (dh_relay_tx_peek(&tx, &packet)) {
            dh_relay_tx_commit(&tx);
            if (packet.kind == DH_RELAY_PKT_START) {
                saw_second_start = true;
                const uint16_t len = (uint16_t)(packet.data[0] | (packet.data[1] << 8));
                CHECK(len == place_len, "atomic", "the second frame was not the placement");
                CHECK(bulk_data == (bulk_len + DH_RELAY_PAYLOAD - 1) / DH_RELAY_PAYLOAD,
                      "atomic", "placement was spliced into the bulk frame");
            } else if (!saw_second_start) {
                bulk_data++;
            }
        }
    }
    CHECK(saw_second_start, "atomic", "the placement never went out");
}

static void test_bulk_never_starves_the_shared_queue(void) {
    dh_relay_tx tx;
    dh_relay_tx_init(&tx);

    uint8_t bulk[DH_FRAME_MAX_SIZE];
    size_t bulk_len = 0;
    uint8_t body[1024] = {0};
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, bulk, sizeof bulk, &bulk_len);
    (void)dh_relay_tx_offer(&tx, bulk, bulk_len);

    /* A 1 KiB chunk is 129 packets. Emitting them in one go would put every
       keyboard packet behind them in the shared inter-board queue. */
    size_t pumps = 0, total = 0;
    while (dh_relay_tx_busy(&tx)) {
        dh_relay_tx_yield(&tx);
        pumps++;
        size_t this_pump = 0;
        dh_relay_packet packet;
        while (dh_relay_tx_peek(&tx, &packet)) {
            dh_relay_tx_commit(&tx);
            this_pump++;
        }
        CHECK(this_pump <= DH_RELAY_BURST_MAX, "burst", "a pump exceeded the burst cap");
        total += this_pump;
    }

    CHECK(total == 1 + (bulk_len + DH_RELAY_PAYLOAD - 1) / DH_RELAY_PAYLOAD, "burst",
          "packets went missing across pumps");
    CHECK(pumps > 1, "burst", "the whole chunk went out in one pump");
}

/* A full transmit queue must cost a retry, never a hole: a frame missing one
 * data packet corrupts everything the reassembler does after it. */
static void test_a_full_queue_loses_nothing(void) {
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t frame_len = 0;
    uint8_t body[64] = {0};
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i + 1);
    (void)dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, frame, sizeof frame,
                          &frame_len);

    dh_relay_tx tx;
    dh_relay_rx rx;
    dh_relay_tx_init(&tx);
    dh_relay_rx_init(&rx, rx_buf, sizeof rx_buf);
    (void)dh_relay_tx_offer(&tx, frame, frame_len);

    uint8_t out[DH_FRAME_MAX_SIZE];
    size_t delivered = 0;
    int enqueue = 0;

    while (dh_relay_tx_busy(&tx)) {
        dh_relay_tx_yield(&tx);
        dh_relay_packet packet;
        while (dh_relay_tx_peek(&tx, &packet)) {
            /* Every other enqueue fails, as a full queue would. */
            if (++enqueue % 2 == 0)
                break; /* not committed: still owed */

            dh_relay_tx_commit(&tx);
            dh_frame_view view;
            if (dh_relay_rx_push(&rx, &packet, &view) == DH_RELAY_OK) {
                const size_t total = DH_FRAME_HEADER_SIZE + view.hdr.len;
                memcpy(out, view.payload - DH_FRAME_HEADER_SIZE, total);
                delivered = total;
            }
        }
    }

    CHECK(delivered == frame_len && memcmp(out, frame, frame_len) == 0, "backpressure",
          "a frame did not survive a queue that kept refusing packets");
    CHECK(rx.orphans == 0 && rx.truncated == 0, "backpressure", "backpressure looked like loss");
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : DH_TEST_VECTORS;

    test_every_frame_crosses_intact(path);
    test_a_payload_that_looks_like_frames_is_still_opaque();
    test_data_packets_carry_a_full_payload();
    test_a_lost_start_orphans_its_data();
    test_a_lost_data_packet_abandons_the_frame();
    test_malformed_and_oversized_frames_are_refused();
    test_a_reassembler_rejects_an_impossible_length();
    test_priority_goes_ahead_of_queued_bulk();
    test_bulk_in_flight_is_not_interrupted();
    test_bulk_never_starves_the_shared_queue();
    test_a_full_queue_loses_nothing();

    if (failures) {
        printf("%d relay check(s) failed\n", failures);
        return 1;
    }
    printf("relay tests passed\n");
    return 0;
}
