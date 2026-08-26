/*
 * The inbound handoff from the peer board (#139).
 *
 * What is checked here is the property the single slot did not have: a burst
 * of frames arriving between two of core 0's passes survives. The measured
 * fault was not a stalled loop — it was short frames arriving several deep
 * inside one 1 ms tick, which the one-slot design had ruled out by assuming
 * every frame is a 4 ms chunk.
 *
 * The barriers are the caller's (dh_inq.h), so there is nothing threaded to
 * test here: what this pins is the ring's bookkeeping and its bounds.
 */

#include <stdio.h>
#include <string.h>

#include "dh_inq.h"
#include "dh_frame.h"
#include "dh_seal.h"
#include "dh_xfer.h"

static int failures = 0;

#define CHECK(cond, area, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (area), (msg));                     \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* A frame as it crosses the inter-board link: header and body, no
   authentication prefix — that is written on the way back out. */
static uint16_t make_frame(uint8_t type, uint8_t fill, uint16_t body, uint8_t *out, size_t cap) {
    uint8_t payload[DH_FRAME_MAX_PAYLOAD];
    memset(payload, fill, body);

    size_t len = 0;
    if (dh_frame_encode(type, 0, payload, body, out, cap, &len) != DH_FRAME_OK)
        return 0;
    return (uint16_t)len;
}

/* Push, then take, and the bytes are the ones that went in. */
static void test_a_frame_is_handed_over_byte_identically(void) {
    dh_inq q;
    dh_inq_init(&q);

    uint8_t frame[DH_INQ_SLOT_MAX];
    const uint16_t len = make_frame(DH_MSG_CLIP_CHUNK, 0xA5, 300, frame, sizeof frame);
    CHECK(len > 0, "identity", "encode failed");

    const uint8_t *at = NULL;
    uint16_t got = 0;
    CHECK(!dh_inq_peek(&q, &at, &got), "identity", "an empty ring offered a frame");

    CHECK(dh_inq_stage(&q, frame, len), "identity", "an empty ring refused a frame");
    CHECK(!dh_inq_peek(&q, &at, &got), "identity", "a staged frame was visible before publish");

    dh_inq_publish(&q);

    CHECK(dh_inq_peek(&q, &at, &got), "identity", "a published frame was not offered");
    CHECK(got == len && memcmp(at, frame, len) == 0, "identity",
          "the frame did not arrive byte-identically");

    dh_inq_release(&q);
    CHECK(!dh_inq_peek(&q, &at, &got), "identity", "a released frame was offered again");
}

/*
 * The fault #139 measured, in the shape it actually took: a burst of short
 * frames lands between two of core 0's passes. At one slot the first survives
 * and the rest are counted; the ring is here so they are not.
 */
static void test_a_burst_between_two_passes_survives(void) {
    dh_inq q;
    dh_inq_init(&q);

    /* A relayed CLIP_CREDIT: id and credits, and nothing else. The receiver
       grants one per chunk since #137, so a batch's worth arrive together.

       The count is the batch the transfer machine actually emits, deliberately
       not DH_INQ_DEPTH: a burst sized from the ring is a burst the ring passes
       by construction, and would have passed at one slot too. */
    const unsigned burst = DH_XFER_CREDIT_WINDOW + 1u;

    uint8_t credit[DH_INQ_SLOT_MAX];
    const uint16_t len = make_frame(DH_MSG_CLIP_CREDIT, 0x11, 6, credit, sizeof credit);
    CHECK(len > 0, "burst", "encode failed");

    for (unsigned i = 0; i < burst; i++) {
        credit[DH_FRAME_HEADER_SIZE] = (uint8_t)i; /* so order is checkable */
        CHECK(dh_inq_stage(&q, credit, len), "burst",
              "a frame of one pump batch was refused before core 0 could drain");
        dh_inq_publish(&q);
    }

    CHECK(q.dropped == 0, "burst", "a burst of one pump batch still lost a frame");
    CHECK(dh_inq_pending(&q) == burst, "burst", "the ring did not hold the whole burst");

    /* Core 0's next pass takes them all, oldest first. */
    for (unsigned i = 0; i < burst; i++) {
        const uint8_t *at = NULL;
        uint16_t got = 0;
        CHECK(dh_inq_peek(&q, &at, &got), "burst", "the ring ran out early");
        CHECK(got == len && at[DH_FRAME_HEADER_SIZE] == (uint8_t)i, "burst",
              "frames were handed over out of order");
        dh_inq_release(&q);
    }

    CHECK(dh_inq_pending(&q) == 0, "burst", "the ring did not empty");
}

/* Bounded, and a refusal past the bound is counted rather than invisible —
   the counter is what made this fault findable at all (#133). */
static void test_the_ring_refuses_past_its_depth_and_counts_it(void) {
    dh_inq q;
    dh_inq_init(&q);

    uint8_t frame[DH_INQ_SLOT_MAX];
    const uint16_t len = make_frame(DH_MSG_CLIP_CHUNK, 7, 64, frame, sizeof frame);

    for (unsigned i = 0; i < DH_INQ_DEPTH; i++) {
        CHECK(dh_inq_stage(&q, frame, len), "depth", "a frame within the depth was refused");
        dh_inq_publish(&q);
    }

    CHECK(!dh_inq_stage(&q, frame, len), "depth", "the ring accepted more than it can hold");
    CHECK(q.dropped == 1, "depth", "the refusal was not counted");
    CHECK(dh_inq_pending(&q) == DH_INQ_DEPTH, "depth", "a refusal disturbed the ring");
}

/*
 * A full ring puts the write index on the oldest unread slot. Refusing has to
 * leave that frame alone, and the publish that follows a refused stage has to
 * do nothing — otherwise the consumer is handed the same frame twice and the
 * newest one is lost in its place.
 */
static void test_a_refused_frame_does_not_disturb_the_one_being_read(void) {
    dh_inq q;
    dh_inq_init(&q);

    uint8_t frame[DH_INQ_SLOT_MAX];
    for (unsigned i = 0; i < DH_INQ_DEPTH; i++) {
        const uint16_t len = make_frame(DH_MSG_CLIP_CHUNK, (uint8_t)(i + 1), 64, frame,
                                        sizeof frame);
        CHECK(dh_inq_stage(&q, frame, len), "refuse", "a frame within the depth was refused");
        dh_inq_publish(&q);
    }

    uint8_t oversize_body[DH_INQ_SLOT_MAX];
    const uint16_t refused_len = make_frame(DH_MSG_CLIP_OFFER, 0xEE, 900, oversize_body,
                                            sizeof oversize_body);
    CHECK(!dh_inq_stage(&q, oversize_body, refused_len), "refuse", "the full ring took a frame");
    dh_inq_publish(&q); /* must be a no-op */

    CHECK(dh_inq_pending(&q) == DH_INQ_DEPTH, "refuse",
          "a publish after a refused stage moved the ring");

    const uint8_t *at = NULL;
    uint16_t got = 0;
    CHECK(dh_inq_peek(&q, &at, &got), "refuse", "the oldest frame went missing");
    CHECK(at[DH_FRAME_HEADER_SIZE] == 1, "refuse",
          "the refused frame overwrote the one still being read");
}

/* Longer than a slot is refused outright and counted, not truncated: a partial
   frame handed on would desynchronise the helper's reader. */
static void test_a_frame_longer_than_a_slot_is_refused(void) {
    dh_inq q;
    dh_inq_init(&q);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    const uint16_t len = make_frame(DH_MSG_CLIP_CHUNK, 3, DH_FRAME_MAX_PAYLOAD, frame,
                                    sizeof frame);
    CHECK(len > DH_INQ_SLOT_MAX, "oversize", "the test frame is not actually oversized");

    CHECK(!dh_inq_stage(&q, frame, len), "oversize", "a frame longer than a slot was accepted");
    CHECK(q.dropped == 1, "oversize", "the refusal was not counted");
    CHECK(dh_inq_pending(&q) == 0, "oversize", "a refused frame was made visible");
}

/* The counters wrap, and the ring has to keep working across the wrap — the
   depth divides their width, which the header asserts, and this exercises it. */
static void test_the_ring_survives_its_counters_wrapping(void) {
    dh_inq q;
    dh_inq_init(&q);

    uint8_t frame[DH_INQ_SLOT_MAX];

    for (unsigned round = 0; round < 600; round++) {
        const uint16_t len = make_frame(DH_MSG_CLIP_CREDIT, (uint8_t)round, 6, frame,
                                        sizeof frame);
        CHECK(dh_inq_stage(&q, frame, len), "wrap", "a frame was refused by an empty ring");
        dh_inq_publish(&q);

        const uint8_t *at = NULL;
        uint16_t got = 0;
        CHECK(dh_inq_peek(&q, &at, &got), "wrap", "the ring lost a frame across a wrap");
        CHECK(got == len && at[DH_FRAME_HEADER_SIZE] == (uint8_t)round, "wrap",
              "the wrong frame came back across a wrap");
        dh_inq_release(&q);
    }

    CHECK(q.dropped == 0, "wrap", "frames were lost across the counter wrap");
}

/* A session ending discards what is parked — it was reassembled under keys
   that no longer exist — but not the count of what this seam has dropped,
   which is a since-boot total the helper reads live (#133). */
static void test_a_reset_drops_the_parked_frames_and_keeps_the_count(void) {
    dh_inq q;
    dh_inq_init(&q);

    uint8_t frame[DH_INQ_SLOT_MAX];
    const uint16_t len = make_frame(DH_MSG_CLIP_CHUNK, 9, 64, frame, sizeof frame);

    for (unsigned i = 0; i < DH_INQ_DEPTH; i++) {
        CHECK(dh_inq_stage(&q, frame, len), "reset", "a frame within the depth was refused");
        dh_inq_publish(&q);
    }
    CHECK(!dh_inq_stage(&q, frame, len), "reset", "the full ring took a frame");

    dh_inq_reset(&q);

    CHECK(dh_inq_pending(&q) == 0, "reset", "a reset left frames parked");
    CHECK(q.dropped == 1, "reset", "a reset cleared the since-boot drop total");

    /* And it is usable again straight away. */
    CHECK(dh_inq_stage(&q, frame, len), "reset", "the ring refused a frame after a reset");
    dh_inq_publish(&q);
    CHECK(dh_inq_pending(&q) == 1, "reset", "the ring did not take a frame after a reset");
}

/*
 * The two bounds, pinned against the constants they were derived from rather
 * than argued in a comment — the same discipline outq_test applies to its own.
 *
 * A slot holds the largest bulk frame a transfer can complete with, as it
 * crosses the inter-board link: no authentication prefix, because the tag is
 * written per hop on the way back out.
 *
 * The depth holds one pump batch, which is what arrives back to back in both
 * directions — the window's chunks and the ungated CLIP_DONE behind them on
 * the receiving board, and one credit per chunk on the sending board.
 */
static void test_the_bounds_match_what_actually_arrives(void) {
    const size_t chunk_frame = DH_FRAME_HEADER_SIZE + DH_SEAL_CHUNK_OVERHEAD + DH_XFER_CHUNK_SIZE;
    const size_t offer_frame = DH_FRAME_HEADER_SIZE + DH_SEAL_OFFER_OVERHEAD + DH_XFER_META_MAX;

    CHECK(DH_INQ_SLOT_MAX >= chunk_frame, "sizing",
          "a slot cannot hold a relayed CLIP_CHUNK at the default chunk size");
    CHECK(DH_INQ_SLOT_MAX >= offer_frame, "sizing",
          "a slot cannot hold a relayed CLIP_OFFER at the largest metadata the "
          "receiver will accept");

    CHECK(DH_INQ_DEPTH >= DH_XFER_CREDIT_WINDOW + 1u, "sizing",
          "the ring cannot hold one pump batch — the window's chunks and the "
          "ungated CLIP_DONE behind them — so a burst is dropped rather than parked");
}

int main(void) {
    test_a_frame_is_handed_over_byte_identically();
    test_a_burst_between_two_passes_survives();
    test_the_ring_refuses_past_its_depth_and_counts_it();
    test_a_refused_frame_does_not_disturb_the_one_being_read();
    test_a_frame_longer_than_a_slot_is_refused();
    test_the_ring_survives_its_counters_wrapping();
    test_a_reset_drops_the_parked_frames_and_keeps_the_count();
    test_the_bounds_match_what_actually_arrives();

    if (failures) {
        printf("%d inq check(s) failed\n", failures);
        return 1;
    }
    printf("inq tests passed\n");
    return 0;
}
