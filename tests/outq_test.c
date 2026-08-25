/*
 * Outbound frame queue tests (#69, ADR-0005): the burst a slow drain creates,
 * the bound on it, the band discipline, and the sustained overrun that used to
 * be silent truncation.
 *
 * Frames are drained the way a byte-stream transport drains them — a fixed
 * number of bytes per tick — and reassembled with dh_frame_reader, the reader
 * the helper itself runs. That is deliberate: two frames' bytes interleaved in
 * the stream desynchronise that reader, so the invariant "one frame at a time,
 * finished before the next begins" is checked by the thing it exists to
 * protect rather than by reading the queue's own fields.
 *
 * Style follows relay_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdio.h>
#include <string.h>

#include "dh_outq.h"
#include "dh_seal.h"
#include "dh_xfer.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* A frame with a recognisable payload, so a frame delivered in the wrong order
   or spliced into another is visible in the bytes and not only in the length. */
static size_t make_frame(uint8_t type, uint8_t tag, size_t payload_len, uint8_t *out,
                         size_t cap) {
    static uint8_t body[DH_FRAME_MAX_PAYLOAD];
    for (size_t i = 0; i < payload_len; i++)
        body[i] = (uint8_t)(tag + i);

    size_t len = 0;
    if (dh_frame_encode(type, 0, body, payload_len, out, cap, &len) != DH_FRAME_OK)
        return 0;
    return len;
}

/* Everything the far end received, in the order it recovered it. */
#define SINK_FRAMES 64u

typedef struct {
    dh_frame_reader reader;
    uint8_t bytes[80u * 1024u]; /* delivered frames, concatenated */
    size_t len;
    uint16_t frame_len[SINK_FRAMES];
    size_t frames;
    int errors; /* a framing error: the stream was desynchronised */
} sink;

static void sink_init(sink *s) {
    dh_frame_reader_init(&s->reader);
    s->len = 0;
    s->frames = 0;
    s->errors = 0;
}

static void sink_feed(sink *s, const uint8_t *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t consumed = 0;
        dh_frame_view v;
        const dh_frame_result rc =
            dh_frame_reader_push(&s->reader, data + off, n - off, &consumed, &v);

        if (rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN) {
            s->errors++;
            return;
        }
        off += consumed;

        if (rc == DH_FRAME_OK) {
            const size_t total = DH_FRAME_HEADER_SIZE + v.hdr.len;
            if (s->len + total <= sizeof s->bytes && s->frames < SINK_FRAMES) {
                memcpy(s->bytes + s->len, v.payload - DH_FRAME_HEADER_SIZE, total);
                s->len += total;
                s->frame_len[s->frames++] = (uint16_t)total;
            }
        } else if (consumed == 0) {
            break;
        }
    }
}

/* One tick of a byte-stream transport: up to `unit` bytes of whatever is owed.
   preamble_owed is ignored here — it is the relay's per-frame start packet, and
   a byte stream has no such thing. Returns the bytes moved. */
static uint16_t drain_once(dh_outq *q, sink *s, uint16_t unit) {
    dh_outq_view v;
    if (!dh_outq_peek(q, &v))
        return 0;

    const uint16_t take = v.remaining < unit ? v.remaining : unit;
    sink_feed(s, v.at, take);
    dh_outq_advance(q, &v, take);
    return take;
}

static void drain_all(dh_outq *q, sink *s, uint16_t unit) {
    while (drain_once(q, s, unit) > 0)
        ;
}

/* The everyday case, and the baseline the rest depends on. */
static void test_a_frame_is_carried_byte_identically(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    const size_t len = make_frame(DH_MSG_CLIP_CHUNK, 1, 1024, frame, sizeof frame);
    CHECK(len > 0, "carry", "encode failed");
    CHECK(dh_outq_offer(&q, frame, len) == DH_OUTQ_OK, "carry", "frame refused");

    sink s;
    sink_init(&s);
    drain_all(&q, &s, 64);

    CHECK(s.errors == 0, "carry", "the stream desynchronised the reader");
    CHECK(s.frames == 1, "carry", "expected exactly one frame");
    CHECK(s.len == len && memcmp(s.bytes, frame, len) == 0, "carry",
          "the frame did not arrive byte-identically");
    CHECK(!dh_outq_busy(&q), "carry", "the queue still owes something");
}

/*
 * The defect. A 4 KiB frame owns the outbound slot for ~64 ms of USB drain,
 * and the peer board can complete another inside that window; before this
 * queue the second was refused with no retransmit beneath it.
 */
static void test_a_second_frame_is_accepted_while_the_first_drains(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t first[DH_FRAME_MAX_SIZE], second[DH_FRAME_MAX_SIZE];
    const size_t first_len = make_frame(DH_MSG_CLIP_CHUNK, 1, 4096, first, sizeof first);
    const size_t second_len = make_frame(DH_MSG_CLIP_CHUNK, 9, 1024, second, sizeof second);

    CHECK(dh_outq_offer(&q, first, first_len) == DH_OUTQ_OK, "burst", "first frame refused");

    sink s;
    sink_init(&s);
    CHECK(drain_once(&q, &s, 64) == 64, "burst", "nothing drained");

    CHECK(dh_outq_offer(&q, second, second_len) == DH_OUTQ_OK, "burst",
          "a frame arriving mid-drain was refused");

    drain_all(&q, &s, 64);

    CHECK(s.errors == 0, "burst", "the stream desynchronised the reader");
    CHECK(s.frames == 2, "burst", "both frames should have been delivered");
    CHECK(s.len == first_len + second_len, "burst", "bytes went missing");
    CHECK(memcmp(s.bytes, first, first_len) == 0 &&
              memcmp(s.bytes + first_len, second, second_len) == 0,
          "burst", "the two frames were not delivered whole and in order");
}

/* Bounded, and a refusal past the bound is counted rather than invisible. */
static void test_the_queue_refuses_past_its_depth_and_counts_it(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    const size_t len = make_frame(DH_MSG_CLIP_CHUNK, 1, 512, frame, sizeof frame);

    /* One in flight plus DH_OUTQ_DEPTH waiting to become it. */
    for (unsigned i = 0; i < 1u + DH_OUTQ_DEPTH; i++)
        CHECK(dh_outq_offer(&q, frame, len) == DH_OUTQ_OK, "depth",
              "a frame within the queue's depth was refused");

    CHECK(dh_outq_offer(&q, frame, len) == DH_OUTQ_ERR_BUSY, "depth",
          "the queue accepted more than it can hold");
    CHECK(q.refused == 1, "depth", "the refusal was not counted");
}

static void test_frames_keep_their_order_across_promotion(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t frames[3][DH_FRAME_MAX_SIZE];
    size_t lens[3];
    for (unsigned i = 0; i < 3; i++) {
        lens[i] = make_frame(DH_MSG_CLIP_CHUNK, (uint8_t)(i * 37 + 1), 300 + i * 100, frames[i],
                             sizeof frames[i]);
        CHECK(dh_outq_offer(&q, frames[i], lens[i]) == DH_OUTQ_OK, "order", "frame refused");
    }

    sink s;
    sink_init(&s);
    drain_all(&q, &s, 64);

    CHECK(s.errors == 0, "order", "the stream desynchronised the reader");
    CHECK(s.frames == 3, "order", "expected three frames");

    size_t at = 0;
    for (unsigned i = 0; i < 3; i++) {
        CHECK(s.frame_len[i] == lens[i] && memcmp(s.bytes + at, frames[i], lens[i]) == 0, "order",
              "frames were delivered out of order or spliced");
        at += lens[i];
    }
}

/*
 * What the queue is actually for. A refused frame is *gone*: the peer board's
 * reader releases it on the next push, and there is no retransmit at this
 * layer — so the only thing that saves it is room to be accepted the moment it
 * arrives. Frames arrive in bursts (the 3.6 Mbaud link completes several
 * inside one 64 ms drain) while the average rate is drainable, which is the
 * shape the depth is chosen for: a burst within it costs nothing at all.
 */
static void test_a_burst_is_absorbed_when_the_average_rate_is_drainable(void) {
    dh_outq q;
    dh_outq_init(&q);

    /* What the transfer machine actually emits: a chunk at the default size. */
    uint8_t frame[DH_FRAME_MAX_SIZE];
    const size_t len = make_frame(DH_MSG_CLIP_CHUNK, 3, DH_XFER_CHUNK_SIZE, frame, sizeof frame);
    CHECK(len > 0, "burst-rate", "encode failed");

    sink s;
    sink_init(&s);

    const unsigned rounds = 10, per_burst = 1u + DH_OUTQ_DEPTH;
    /* Ticks enough to drain the whole burst, so the queue never falls behind
       on average — only in the instant the burst lands. */
    const unsigned ticks = per_burst * ((unsigned)len / 64u + 2u);
    size_t lost = 0;

    for (unsigned round = 0; round < rounds; round++) {
        for (unsigned i = 0; i < per_burst; i++)
            if (dh_outq_offer(&q, frame, len) != DH_OUTQ_OK)
                lost++; /* nothing to retry with: the frame is gone */

        for (unsigned tick = 0; tick < ticks; tick++)
            drain_once(&q, &s, 64);
    }
    drain_all(&q, &s, 64);

    CHECK(lost == 0, "burst-rate", "a burst inside the queue's depth still lost frames");
    CHECK(s.errors == 0, "burst-rate", "the stream desynchronised the reader");
    CHECK(s.frames == rounds * per_burst, "burst-rate", "not every frame was delivered");
    for (size_t i = 0; i < s.frames; i++)
        CHECK(s.frame_len[i] == len && memcmp(s.bytes + i * len, frame, len) == 0, "burst-rate",
              "a frame was corrupted or delivered out of order");
}

/*
 * And the case the acceptance criteria single out: an offerer that stays ahead
 * of the drain indefinitely. No queue of any depth can fix that — sustained
 * overrun is the credit window's problem, and the window is end-to-end between
 * helpers (ADR-0005). What must hold is that the failure stays honest: every
 * frame the queue accepted arrives whole and in order, every frame it could not
 * take is refused and counted, and nothing is ever delivered short.
 */
static void test_a_sustained_overrun_truncates_nothing(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    const size_t len = make_frame(DH_MSG_CLIP_CHUNK, 3, DH_XFER_CHUNK_SIZE, frame, sizeof frame);

    sink s;
    sink_init(&s);

    size_t accepted = 0, refused = 0;

    /* A frame offered every tick against 64 bytes of drain: an overrun by
       construction, and one the queue can only ever delay. */
    for (unsigned tick = 0; tick < 400; tick++) {
        if (accepted < SINK_FRAMES) {
            if (dh_outq_offer(&q, frame, len) == DH_OUTQ_OK)
                accepted++;
            else
                refused++;
        }
        drain_once(&q, &s, 64);
    }
    drain_all(&q, &s, 64);

    CHECK(refused > 0, "overrun", "the overrun never actually saturated the queue");
    CHECK(s.errors == 0, "overrun", "a sustained overrun desynchronised the reader");
    CHECK(s.frames == accepted, "overrun",
          "the number of frames delivered does not match the number accepted");
    CHECK(s.len == accepted * len, "overrun", "a frame was delivered short");

    for (size_t i = 0; i < s.frames; i++)
        CHECK(s.frame_len[i] == len && memcmp(s.bytes + i * len, frame, len) == 0, "overrun",
              "a frame was corrupted under sustained overrun");
    CHECK(q.refused == refused, "overrun", "refusals were not all counted");
}

static void test_priority_overtakes_queued_bulk(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t bulk[DH_FRAME_MAX_SIZE], beat[DH_FRAME_MAX_SIZE];
    const size_t bulk_len = make_frame(DH_MSG_CLIP_CHUNK, 1, 512, bulk, sizeof bulk);
    const size_t beat_len = make_frame(DH_MSG_DEVICE_HEARTBEAT, 0, 0, beat, sizeof beat);

    /* Bulk offered first but not yet drained: the beat must go ahead of it. */
    CHECK(dh_outq_offer(&q, bulk, bulk_len) == DH_OUTQ_OK, "priority", "bulk refused");
    CHECK(dh_outq_offer(&q, beat, beat_len) == DH_OUTQ_OK, "priority", "beat refused");

    sink s;
    sink_init(&s);
    drain_all(&q, &s, 64);

    CHECK(s.frames == 2, "priority", "expected both frames");
    CHECK(s.frame_len[0] == beat_len && memcmp(s.bytes, beat, beat_len) == 0, "priority",
          "bulk went out ahead of a session frame that was waiting");
}

/*
 * The invariant, in the direction that matters: once a frame's first byte is
 * on the stream it finishes. A session frame offered mid-bulk waits, because
 * the far end recovers frame boundaries from the byte stream alone.
 */
static void test_a_frame_in_flight_is_never_interrupted(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t bulk[DH_FRAME_MAX_SIZE], beat[DH_FRAME_MAX_SIZE];
    const size_t bulk_len = make_frame(DH_MSG_CLIP_CHUNK, 1, 1024, bulk, sizeof bulk);
    const size_t beat_len = make_frame(DH_MSG_DEVICE_HEARTBEAT, 0, 0, beat, sizeof beat);

    CHECK(dh_outq_offer(&q, bulk, bulk_len) == DH_OUTQ_OK, "atomic", "bulk refused");

    sink s;
    sink_init(&s);
    CHECK(drain_once(&q, &s, 64) == 64, "atomic", "bulk did not start");

    CHECK(dh_outq_offer(&q, beat, beat_len) == DH_OUTQ_OK, "atomic", "beat refused mid-bulk");

    drain_all(&q, &s, 64);

    CHECK(s.errors == 0, "atomic", "a frame was spliced into another");
    CHECK(s.frames == 2, "atomic", "expected both frames");
    CHECK(s.frame_len[0] == bulk_len && memcmp(s.bytes, bulk, bulk_len) == 0, "atomic",
          "the in-flight bulk frame was interrupted");
    CHECK(memcmp(s.bytes + bulk_len, beat, beat_len) == 0, "atomic", "the beat never went out");
}

static void test_the_priority_band_stays_single_buffered(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t beat[DH_FRAME_MAX_SIZE];
    const size_t beat_len = make_frame(DH_MSG_DEVICE_HEARTBEAT, 0, 0, beat, sizeof beat);

    CHECK(dh_outq_offer(&q, beat, beat_len) == DH_OUTQ_OK, "single", "first beat refused");
    CHECK(dh_outq_offer(&q, beat, beat_len) == DH_OUTQ_ERR_BUSY, "single",
          "a second session frame was accepted while one was in flight");
}

/*
 * A CLIP_OFFER's file-list metadata can legally exceed a chunk, and the
 * in-flight buffer is frame-sized precisely so it stays carryable. The queued
 * slots behind it are not, so a second outsized frame waits (ADR-0005).
 */
static void test_an_outsized_frame_rides_the_in_flight_buffer(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t big[DH_FRAME_MAX_SIZE];
    const size_t big_len = make_frame(DH_MSG_CLIP_OFFER, 7, DH_OUTQ_STAGE_MAX, big, sizeof big);
    CHECK(big_len > DH_OUTQ_STAGE_MAX, "outsized", "the test frame is not actually outsized");

    CHECK(dh_outq_offer(&q, big, big_len) == DH_OUTQ_OK, "outsized",
          "an outsized frame was refused by an idle queue");
    CHECK(dh_outq_offer(&q, big, big_len) == DH_OUTQ_ERR_BUSY, "outsized",
          "an outsized frame was queued behind another");

    /* Retryable, not fatal: it fits again once the queue has drained. */
    sink s;
    sink_init(&s);
    drain_all(&q, &s, 64);
    CHECK(s.frames == 1 && s.len == big_len && memcmp(s.bytes, big, big_len) == 0, "outsized",
          "the outsized frame did not arrive intact");
    CHECK(dh_outq_offer(&q, big, big_len) == DH_OUTQ_OK, "outsized",
          "an outsized frame was still refused by a drained queue");
}

static void test_malformed_frames_are_refused(void) {
    dh_outq q;
    dh_outq_init(&q);

    const uint8_t unknown[] = {0xEE, 0x00, 0x00, 0x00};
    CHECK(dh_outq_offer(&q, unknown, sizeof unknown) == DH_OUTQ_ERR_FRAME, "refuse",
          "an unknown type was queued");

    /* A header promising more than it brought is not a short frame. */
    const uint8_t short_frame[] = {DH_MSG_CLIP_DONE, 0x00, 0x04, 0x00, 0x02};
    CHECK(dh_outq_offer(&q, short_frame, sizeof short_frame) == DH_OUTQ_ERR_FRAME, "refuse",
          "a truncated frame was queued");

    CHECK(dh_outq_offer(&q, unknown, 0) == DH_OUTQ_ERR_FRAME, "refuse",
          "an empty frame was queued");

    /* Too long for the priority band: refused, never truncated to fit. Only
       that band has a cap below the frame maximum. */
    uint8_t big[DH_FRAME_MAX_SIZE];
    const size_t big_len =
        make_frame(DH_MSG_PAIR_GRANT, 1, DH_OUTQ_PRIORITY_MAX, big, sizeof big);
    CHECK(dh_outq_offer(&q, big, big_len) == DH_OUTQ_ERR_OVERSIZE, "refuse",
          "a session frame too long for its band was accepted");

    CHECK(!dh_outq_busy(&q), "refuse", "a refused frame occupied the queue");
    CHECK(q.refused > 0, "refuse", "refusals were not counted");
}

/*
 * The relay prefixes each frame with a start packet, so it needs to know once
 * per frame that a new one has begun. Byte-stream callers ignore this.
 */
static void test_the_preamble_is_owed_once_per_frame(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    const size_t len = make_frame(DH_MSG_CLIP_CHUNK, 1, 16, frame, sizeof frame);
    CHECK(dh_outq_offer(&q, frame, len) == DH_OUTQ_OK, "preamble", "frame refused");
    CHECK(dh_outq_offer(&q, frame, len) == DH_OUTQ_OK, "preamble", "second frame refused");

    dh_outq_view v;
    CHECK(dh_outq_peek(&q, &v) && v.preamble_owed, "preamble",
          "a fresh frame did not owe its preamble");
    CHECK(v.total == len, "preamble", "the view reports the wrong frame length");

    dh_outq_note_preamble(&q, &v);
    CHECK(dh_outq_peek(&q, &v) && !v.preamble_owed, "preamble",
          "the preamble was still owed after being noted");

    /* Drain the first frame; the second must owe its own preamble. */
    while (dh_outq_peek(&q, &v) && !v.preamble_owed)
        dh_outq_advance(&q, &v, v.remaining);

    CHECK(dh_outq_peek(&q, &v) && v.preamble_owed, "preamble",
          "the promoted frame did not owe a preamble of its own");
}

/*
 * The queued slots are sized to the largest bulk frame a transfer can complete
 * with. Pinned here rather than in dh_outq.h so the firmware need not include
 * the helper-side transfer machine to state its own buffer size.
 *
 * The offer is the binding one, and the reason is worth keeping: a chunk
 * refused here is re-requested end to end, but a refused CLIP_OFFER has no
 * retransmit behind it and costs the whole transfer.
 */
static void test_a_queued_slot_holds_the_largest_viable_bulk_frame(void) {
    /* The authentication prefix is inside the frame's length (#111), so it is
       part of what a slot has to hold — the omission would not fail loudly,
       it would quietly stop anything queueing behind the frame in flight.
       So is the seal (#113). What crosses this seam is the *sealed* body, and
       pinning the clear sizes here is exactly what let #135 through: a
       full-size chunk was 24 bytes over the bound and could never queue behind
       anything, so a clipboard payload past one chunk lost most of itself. */
    const size_t chunk_frame = DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE
                               + DH_SEAL_CHUNK_OVERHEAD + DH_XFER_CHUNK_SIZE;
    const size_t offer_frame = DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE
                               + DH_SEAL_OFFER_OVERHEAD + DH_XFER_META_MAX;

    CHECK(DH_OUTQ_STAGE_MAX >= chunk_frame, "sizing",
          "a queued slot cannot hold a CLIP_CHUNK at the default chunk size");
    CHECK(DH_OUTQ_STAGE_MAX >= offer_frame, "sizing",
          "a queued slot cannot hold a CLIP_OFFER at the largest metadata the "
          "receiver will accept");
}

/*
 * The credit window is the only back-pressure this queue has, so it may not
 * grant more than the queue can hold.
 *
 * The board may not read a payload (ADR-0003), so it cannot enforce a credit
 * of its own — dh_outq.h says outright that a *sustained* overrun "is the
 * credit window's problem". That only holds while the window is sized against
 * this queue. It was not: the receiver granted 16 chunks up front
 * (`dh_xfer.c`), the sender emitted all 16 in one pump batch, and the queue on
 * the far board holds one in flight and DH_OUTQ_DEPTH behind it. The rest were
 * refused.
 *
 * Refused is not free. A chunk is re-requested end to end, but a CLIP_OFFER or
 * a CLIP_DONE has no retransmit and costs the whole transfer out to a
 * thirty-second timeout (#78) — which at the desk is a copy that silently does
 * not arrive until you copy it again (#137). Measured on hardware 2026-08-25:
 * 16 refusals on the receiving board across a handful of copies (#133).
 *
 * Pinned here rather than argued, because the two constants live in different
 * headers and neither mentions the other.
 */
static void test_the_credit_window_fits_the_queue_it_bursts_into(void) {
    const unsigned in_flight = 1u;
    const unsigned queue_holds = in_flight + DH_OUTQ_DEPTH;

    CHECK(DH_XFER_CREDIT_WINDOW <= queue_holds, "credit",
          "the credit window grants more chunks than the outbound queue can hold, "
          "so a burst is refused rather than paced");

    /*
     * **This is one short of the whole truth, and the shortfall is #141.**
     * dh_xfer_pump emits the last chunks and the CLIP_DONE behind them in the
     * same batch, and the DONE is not credit-gated — so the worst-case
     * loss-free burst is DH_XFER_CREDIT_WINDOW + 1, and the frame that does
     * not fit is the DONE, the one nothing retransmits.
     *
     * Asserting the honest bound here would fail, and the two ways to satisfy
     * it both make things worse: a window of 2 stalls double-loss recovery
     * outright (xfer_test's "did not converge"), and so does capping the
     * accumulated credit, because the covering grants that pay for
     * retransmissions are exactly what gets discarded. The remedy is a deeper
     * queue, which costs board RAM and a flash.
     *
     * So this pins what is true and safe today and no more. Do not tighten it
     * without reading #141.
     */

    /*
     * **This bounds the loss-free burst and nothing more.** Credit accumulates
     * without a ceiling (dh_xfer_handle_credit): a covering grant rides every
     * retransmit request, deliberately, because draining the window is fatal
     * where inflating it is not. So after a lossy round the sender can hold
     * more credit than the window names and pump all of it — up to
     * DH_XFER_BATCH_MAX — into a queue sized for the window.
     *
     * That gap is open. Capping the running total at the window was tried and
     * it breaks recovery: a double loss never converges, because the grants
     * that would pay for the retransmissions are the ones discarded.
     *
     * DH_XFER_BATCH_MAX is an action-array bound, not a queue-safety one. It
     * is checked here only so that a pump can emit the window it was granted;
     * do not read it as a bound on the burst.
     */
    CHECK(DH_XFER_BATCH_MAX >= DH_XFER_CREDIT_WINDOW, "credit",
          "a pump cannot emit the window it was granted");
}

/* An offer at that bound queues like anything else, rather than needing an
   idle queue the way a genuinely outsized frame does. */
static void test_a_full_size_offer_can_queue_behind_a_chunk(void) {
    dh_outq q;
    dh_outq_init(&q);

    uint8_t chunk[DH_FRAME_MAX_SIZE], offer[DH_FRAME_MAX_SIZE];
    const size_t chunk_len =
        make_frame(DH_MSG_CLIP_CHUNK, 1,
                   DH_FRAME_AUTH_PREFIX_SIZE + DH_SEAL_CHUNK_OVERHEAD + DH_XFER_CHUNK_SIZE,
                   chunk, sizeof chunk);
    const size_t offer_len =
        make_frame(DH_MSG_CLIP_OFFER, 5,
                   DH_FRAME_AUTH_PREFIX_SIZE + DH_SEAL_OFFER_OVERHEAD + DH_XFER_META_MAX,
                   offer, sizeof offer);

    CHECK(dh_outq_offer(&q, chunk, chunk_len) == DH_OUTQ_OK, "offer-fit", "chunk refused");
    CHECK(dh_outq_offer(&q, offer, offer_len) == DH_OUTQ_OK, "offer-fit",
          "a CLIP_OFFER at the largest accepted metadata could not queue");

    sink s;
    sink_init(&s);
    drain_all(&q, &s, 64);

    CHECK(s.errors == 0, "offer-fit", "the stream desynchronised the reader");
    CHECK(s.frames == 2 && s.frame_len[1] == offer_len, "offer-fit",
          "the offer did not arrive whole and in order");
}

int main(void) {
    test_a_frame_is_carried_byte_identically();
    test_a_second_frame_is_accepted_while_the_first_drains();
    test_the_queue_refuses_past_its_depth_and_counts_it();
    test_frames_keep_their_order_across_promotion();
    test_a_burst_is_absorbed_when_the_average_rate_is_drainable();
    test_a_sustained_overrun_truncates_nothing();
    test_priority_overtakes_queued_bulk();
    test_a_frame_in_flight_is_never_interrupted();
    test_the_priority_band_stays_single_buffered();
    test_an_outsized_frame_rides_the_in_flight_buffer();
    test_malformed_frames_are_refused();
    test_the_preamble_is_owed_once_per_frame();
    test_a_queued_slot_holds_the_largest_viable_bulk_frame();
    test_the_credit_window_fits_the_queue_it_bursts_into();
    test_a_full_size_offer_can_queue_behind_a_chunk();

    if (failures) {
        printf("%d outq check(s) failed\n", failures);
        return 1;
    }
    printf("outq tests passed\n");
    return 0;
}
