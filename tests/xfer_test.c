/*
 * Tests for the chunked transfer state machine (#48), at the seam the spec
 * names: given an offer, chunk headers, data, CRCs, retransmit requests, and
 * credit updates, assert the next action and the assembled payload.
 *
 * Every message between the two sides crosses the real codecs — dh_clip
 * payloads inside dh_frame frames, and since #113 through the real seal as
 * well — with fault injection for the cases hardware will not produce on
 * demand: a dropped chunk, a corrupted chunk, credit exhaustion mid-transfer,
 * cancellation during retransmission, and a link dropping mid-payload. The
 * base scenarios (round trip, chunk count, cap refusal, cancel-then-recover,
 * supersede, lazy reads, no streaming before a request) port from mkroamer's
 * clip_transfer_test.cpp.
 *
 * The seal changes what a corrupted chunk means, and the change is the wire's
 * rather than this file's: a flipped byte is caught by the GCM tag and the
 * chunk never reaches the transfer machine at all. So the CRC32 is checked
 * directly instead — it is fidelity, covering the plaintext end to end, and
 * what it now catches is a bug in the seal layer rather than a bus glitch.
 */

#include <stdio.h>
#include <string.h>

#include "aes_gcm_ref.h"
#include "dh_crc32.h"
#include "dh_frame.h"
#include "dh_seal.h"
#include "dh_xfer.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                   \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++failures;                                                           \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what));   \
        }                                                                         \
    } while (0)

/* ---- harness: two sides joined by the real codecs -------------------- */

/* Big enough for the paste #145's acceptance names — 3,782 chunks — because
   the sustained-loss scenarios are the ones that only bite at that length. */
#define RX_CAP (4u * 1024u * 1024u)
#define ACTS_CAP 64u
#define QUEUE_CAP 512u

struct wire_msg {
    uint8_t type;
    uint8_t payload[DH_FRAME_MAX_PAYLOAD];
    uint16_t len;
};

struct side {
    dh_xfer x;
    uint8_t rx_buf[RX_CAP];
    /* This side's outgoing seal and the one it accepted from the other. */
    dh_seal_tx seal_tx;
    dh_seal_rx seal_rx;
    /* what landed */
    int delivered;
    int failed;
    uint8_t fail_reason;
    uint64_t delivered_len;
    /* counters */
    int chunks_sent;
    int retransmits_sent;
    int seal_refused; /* arrived sealed and would not open */
};

struct fault_plan {
    uint32_t drop_seq;    /* drop the first chunk with this seq once */
    int drop_armed;
    uint32_t corrupt_seq; /* corrupt the first chunk with this seq once */
    int corrupt_armed;
    int drop_chunks;      /* drop this many CLIP_CHUNK messages */
    int drop_done;        /* drop this many CLIP_DONE messages */
    int credit_pass;      /* let this many CLIP_CREDIT messages through first */
    int drop_credits;     /* then drop this many, then pass the rest */
    int drop_requests;    /* drop this many CLIP_REQUEST messages, then pass the rest */
    int block_credits;    /* while set, CLIP_CREDIT messages are discarded */
    struct wire_msg held_credits[16];
    size_t held_count;
    int cancel_rx_on_retransmit; /* receiver cancels after emitting a retransmit */
    /* A link that loses roughly one message in `loss_one_in`, deterministically.
       The offer is exempt: a lost CLIP_OFFER is the sender's to time out
       (docs/protocol.md), and no sweep on the receiving side can recover a
       transfer it was never told about. */
    unsigned loss_one_in;
    unsigned loss_state;
};

static struct side A, B;
static struct fault_plan plan;

/*
 * One seal, established the way the two helpers establish one: the sender
 * offers, the receiver accepts, and each direction gets its own. The ephemeral
 * keys and nonces are fixed rather than drawn, because this file has no
 * entropy source and does not need one — what is being tested is the transfer,
 * not the key agreement (seal_test.c has that).
 */
static void establish_seal(struct side *from, struct side *to, uint8_t material) {
    uint8_t offer_private[DH_P256_PRIVATE_SIZE], accept_private[DH_P256_PRIVATE_SIZE];
    uint8_t offer_nonce[DH_NONCE_SIZE], accept_nonce[DH_NONCE_SIZE];
    for (size_t i = 0; i < DH_P256_PRIVATE_SIZE; i++) {
        offer_private[i] = (uint8_t)(i + 1u + material);
        accept_private[i] = (uint8_t)(i + 101u + material);
    }
    for (size_t i = 0; i < DH_NONCE_SIZE; i++) {
        offer_nonce[i] = (uint8_t)(i + 0x20u + material);
        accept_nonce[i] = (uint8_t)(i + 0x40u + material);
    }

    uint8_t offer[DH_SEAL_EXCHANGE_LEN], accept[DH_SEAL_EXCHANGE_LEN];
    size_t offer_len = 0, accept_len = 0;
    CHECK(dh_seal_tx_offer(&from->seal_tx, 0x51EA1000u + material, offer_private, offer_nonce,
                           offer, sizeof offer, &offer_len) == DH_SEAL_OK,
          "seal", "the offer was not built");
    CHECK(dh_seal_rx_offered(&to->seal_rx, offer, offer_len, accept_private, accept_nonce, accept,
                             sizeof accept, &accept_len) == DH_SEAL_OK,
          "seal", "the offer was not accepted");
    CHECK(dh_seal_tx_accepted(&from->seal_tx, accept, accept_len) == DH_SEAL_OK, "seal",
          "the accept was refused");
}

static void reset_scenario(void) {
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    memset(&plan, 0, sizeof plan);
    dh_xfer_init(&A.x, A.rx_buf, RX_CAP);
    dh_xfer_init(&B.x, B.rx_buf, RX_CAP);
    establish_seal(&A, &B, 0);
    establish_seal(&B, &A, 1);
}

/* The lossy link, as an LCG so a failure is reproducible from the seed alone. */
static int lossy_drop(uint8_t type) {
    if (plan.loss_one_in == 0)
        return 0;
    if (type == DH_MSG_CLIP_OFFER || type == DH_MSG_CLIP_CANCEL)
        return 0;
    plan.loss_state = plan.loss_state * 1103515245u + 12345u;
    return ((plan.loss_state >> 16) % plan.loss_one_in) == 0;
}

/* Encode one action from `from` into a wire message; false = nothing to send
   (local action) or deliberately faulted away. */
static int encode_action(struct side *from, const dh_xfer_action *a, struct wire_msg *m) {
    int n = -1;
    switch (a->type) {
    case DH_XFER_ACT_SEND_OFFER:
    case DH_XFER_ACT_SEND_OFFER_RETRY: {
        dh_clip_offer offer;
        CHECK(dh_xfer_offer_info(&from->x, &offer), "wire", "offer_info failed");
        m->type = DH_MSG_CLIP_OFFER;
        size_t sealed = 0;
        if (dh_seal_encode_offer(&from->seal_tx, aes_gcm_ref_aead(), &offer, m->payload,
                                 sizeof m->payload, &sealed) == DH_SEAL_OK)
            n = (int)sealed;
        break;
    }
    case DH_XFER_ACT_SEND_CHUNK: {
        dh_clip_chunk chunk;
        CHECK(dh_xfer_chunk_at(&from->x, a->seq, &chunk), "wire", "chunk_at failed");
        from->chunks_sent++;
        if (plan.drop_chunks > 0) {
            plan.drop_chunks--;
            return 0; /* lost in transit */
        }
        if (plan.drop_armed > 0 && a->seq == plan.drop_seq) {
            plan.drop_armed--;
            return 0; /* lost in transit */
        }
        m->type = DH_MSG_CLIP_CHUNK;
        size_t sealed = 0;
        if (dh_seal_encode_chunk(&from->seal_tx, aes_gcm_ref_aead(), &chunk, m->payload,
                                 sizeof m->payload, &sealed) == DH_SEAL_OK)
            n = (int)sealed;
        if (n > 0 && plan.corrupt_armed && a->seq == plan.corrupt_seq) {
            plan.corrupt_armed = 0;
            /* A byte of ciphertext, which the GCM tag covers: the far end
               refuses the chunk instead of assembling it. */
            m->payload[n - 1] ^= 0xff;
        }
        break;
    }
    case DH_XFER_ACT_SEND_DONE:
        if (plan.drop_done > 0) {
            plan.drop_done--;
            return 0; /* lost in transit, with no retransmit beneath it */
        }
        m->type = DH_MSG_CLIP_DONE;
        n = dh_clip_encode_id(a->id, m->payload, sizeof m->payload);
        break;
    case DH_XFER_ACT_SEND_REQUEST:
        if (plan.drop_requests > 0) {
            plan.drop_requests--;
            return 0; /* lost in transit: the sender never starts */
        }
        m->type = DH_MSG_CLIP_REQUEST;
        n = dh_clip_encode_id(a->id, m->payload, sizeof m->payload);
        break;
    case DH_XFER_ACT_SEND_CANCEL:
        m->type = DH_MSG_CLIP_CANCEL;
        n = dh_clip_encode_id(a->id, m->payload, sizeof m->payload);
        break;
    case DH_XFER_ACT_SEND_RETRANSMIT:
        from->retransmits_sent++;
        m->type = DH_MSG_CLIP_RETRANSMIT;
        n = dh_clip_encode_retransmit(a->id, a->seq, m->payload, sizeof m->payload);
        break;
    case DH_XFER_ACT_SEND_CREDIT:
        if (plan.credit_pass > 0) {
            plan.credit_pass--; /* the window that starts the transfer lands */
        } else if (plan.drop_credits > 0) {
            plan.drop_credits--;
            return 0; /* lost in transit: the sender stops at zero credit */
        }
        m->type = DH_MSG_CLIP_CREDIT;
        n = dh_clip_encode_credit(a->id, a->credits, m->payload, sizeof m->payload);
        if (n > 0 && plan.block_credits) {
            if (plan.held_count < 16) {
                m->len = (uint16_t)n;
                plan.held_credits[plan.held_count++] = *m;
            }
            return 0; /* withheld */
        }
        break;
    case DH_XFER_ACT_DELIVERED:
        from->delivered++;
        from->delivered_len = dh_xfer_delivered_len(&from->x);
        return 0;
    case DH_XFER_ACT_FAILED:
        from->failed++;
        from->fail_reason = a->reason;
        return 0;
    case DH_XFER_ACT_NEED_DATA:
        return 0; /* scenario answers explicitly */
    case DH_XFER_ACT_PROTOCOL_ERROR:
        return 0; /* local session-ending result, asserted directly */
    default:
        CHECK(0, "wire", "unknown action type");
        return 0;
    }
    CHECK(n > 0, "wire", "payload encode failed");
    m->len = (uint16_t)(n > 0 ? n : 0);
    if (n > 0 && lossy_drop(m->type))
        return 0; /* lost in transit */
    return n > 0;
}

/* Round-trip a wire message through the frame codec, then dispatch. */
static size_t dispatch(struct side *to, const struct wire_msg *m, dh_xfer_action *acts) {
    uint8_t framed[DH_FRAME_MAX_SIZE];
    size_t framed_len = 0;
    CHECK(dh_frame_encode(m->type, 0, m->payload, m->len, framed, sizeof framed,
                          &framed_len) == DH_FRAME_OK,
          "wire", "frame encode failed");
    dh_frame_view fv;
    size_t consumed = 0;
    CHECK(dh_frame_decode(framed, framed_len, &fv, &consumed) == DH_FRAME_OK,
          "wire", "frame decode failed");

    /* The plaintext of whichever sealed message is being opened. One buffer,
       because a message is opened and handled before the next arrives. */
    static uint8_t plain[DH_FRAME_MAX_PAYLOAD];

    switch (fv.hdr.type) {
    case DH_MSG_CLIP_OFFER: {
        dh_clip_offer offer;
        if (dh_seal_open_offer(&to->seal_rx, aes_gcm_ref_aead(), fv.payload, fv.hdr.len, plain,
                               sizeof plain, &offer) != DH_SEAL_OK) {
            to->seal_refused++;
            return 0;
        }
        return dh_xfer_handle_offer(&to->x, &offer, acts, ACTS_CAP);
    }
    case DH_MSG_CLIP_CHUNK: {
        dh_clip_chunk chunk;
        if (dh_seal_open_chunk(&to->seal_rx, aes_gcm_ref_aead(), fv.payload, fv.hdr.len, plain,
                               sizeof plain, &chunk) != DH_SEAL_OK) {
            to->seal_refused++;
            return 0;
        }
        return dh_xfer_handle_chunk(&to->x, &chunk, acts, ACTS_CAP);
    }
    case DH_MSG_CLIP_REQUEST: {
        uint32_t id;
        CHECK(dh_clip_decode_id(fv.payload, fv.hdr.len, &id), "wire", "request decode");
        return dh_xfer_handle_request(&to->x, id, acts, ACTS_CAP);
    }
    case DH_MSG_CLIP_DONE: {
        uint32_t id;
        CHECK(dh_clip_decode_id(fv.payload, fv.hdr.len, &id), "wire", "done decode");
        return dh_xfer_handle_done(&to->x, id, acts, ACTS_CAP);
    }
    case DH_MSG_CLIP_CANCEL: {
        uint32_t id;
        CHECK(dh_clip_decode_id(fv.payload, fv.hdr.len, &id), "wire", "cancel decode");
        return dh_xfer_handle_cancel(&to->x, id, acts, ACTS_CAP);
    }
    case DH_MSG_CLIP_RETRANSMIT: {
        uint32_t id, seq;
        CHECK(dh_clip_decode_retransmit(fv.payload, fv.hdr.len, &id, &seq), "wire",
              "retransmit decode");
        return dh_xfer_handle_retransmit(&to->x, id, seq, acts, ACTS_CAP);
    }
    case DH_MSG_CLIP_CREDIT: {
        uint32_t id;
        uint16_t credits;
        CHECK(dh_clip_decode_credit(fv.payload, fv.hdr.len, &id, &credits), "wire",
              "credit decode");
        return dh_xfer_handle_credit(&to->x, id, credits, acts, ACTS_CAP);
    }
    default:
        CHECK(0, "wire", "unexpected message type");
        return 0;
    }
}

/* Route a batch of actions from one side into the other, collecting the
   replies onto a queue, until both sides go quiet. */
struct queued {
    struct wire_msg m;
    struct side *to;
};

static struct queued queue[QUEUE_CAP];
static size_t q_head, q_tail;

static void enqueue_actions(struct side *from, struct side *to, const dh_xfer_action *acts,
                            size_t n) {
    for (size_t i = 0; i < n; i++) {
        struct wire_msg m;
        if (encode_action(from, &acts[i], &m)) {
            CHECK(q_tail < QUEUE_CAP, "wire", "queue overflow");
            if (q_tail < QUEUE_CAP)
                queue[q_tail++] = (struct queued){m, to};
        }
        if (plan.cancel_rx_on_retransmit && acts[i].type == DH_XFER_ACT_SEND_RETRANSMIT) {
            plan.cancel_rx_on_retransmit = 0;
            dh_xfer_action cancel_acts[ACTS_CAP];
            size_t cn = dh_xfer_cancel_rx(&from->x, cancel_acts, ACTS_CAP);
            enqueue_actions(from, to, cancel_acts, cn);
        }
    }
}

static struct side *peer(struct side *s) { return s == &A ? &B : &A; }

static void run_until_quiet(void) {
    int spins = 0;
    for (;;) {
        while (q_head < q_tail) {
            struct queued q = queue[q_head++];
            dh_xfer_action acts[ACTS_CAP];
            size_t n = dispatch(q.to, &q.m, acts);
            enqueue_actions(q.to, peer(q.to), acts, n);
        }
        q_head = q_tail = 0;
        /* both senders pump whatever credit and batch caps allow */
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_pump(&A.x, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        n = dh_xfer_pump(&B.x, acts, ACTS_CAP);
        enqueue_actions(&B, &A, acts, n);
        if (q_head == q_tail)
            return;
        /* One spin carries about one chunk, so the bound is the longest
           scenario's chunk count with room for its retransmit rounds. */
        CHECK(++spins < 200000, "wire", "scenario did not quiesce");
        if (spins >= 200000)
            return;
    }
}

static void offer_and_run(struct side *from, const uint8_t *data, uint64_t total) {
    dh_xfer_action acts[ACTS_CAP];
    size_t n = dh_xfer_offer(&from->x, 0, NULL, 0, data, total, acts, ACTS_CAP);
    enqueue_actions(from, peer(from), acts, n);
    run_until_quiet();
}

/*
 * Stand in for the helper's tick: a receive that has stopped moving sweeps for
 * what it is waiting on, and whatever it asks for is carried. Returns the
 * number of sweeps it took, or -1 if the receive never finished — bounded,
 * because a scenario needing more rounds than this has stopped converging.
 */
static int sweep_until_delivered(struct side *rx, int max_rounds) {
    for (int round = 0; round < max_rounds; round++) {
        if (!dh_xfer_is_receiving(&rx->x))
            return round;
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_sweep_rx(&rx->x, acts, ACTS_CAP);
        enqueue_actions(rx, peer(rx), acts, n);
        run_until_quiet();
    }
    return dh_xfer_is_receiving(&rx->x) ? -1 : max_rounds;
}

static void fill_pattern(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(i * 7 + (i >> 8));
}

static uint8_t payload[64u * 1024u];
/* The paste #145's acceptance names, byte for byte. */
#define BIG_CHUNKS 3782u
static uint8_t big_payload[BIG_CHUNKS * DH_XFER_CHUNK_SIZE];

/* ---- scenarios ------------------------------------------------------- */

int main(void) {
    fill_pattern(payload, sizeof payload);
    fill_pattern(big_payload, sizeof big_payload);

    /* CRC32 known vectors (the check everything else leans on). */
    {
        CHECK(dh_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc", "check value");
        CHECK(dh_crc32((const uint8_t *)"hi", 2) == 0xd8932aacu, "crc", "golden-vector value");
        CHECK(dh_crc32(NULL, 0) == 0, "crc", "empty");
    }

    /* Payload codecs round-trip through the frame codec and, for the two that
       carry the user's bytes, through the seal (AC: offer, request, chunk,
       done, cancel round-trip through the shared core's codec). */
    {
        reset_scenario();
        const uint8_t meta[] = {0x61, 0x62};
        dh_clip_offer offer = {7, 2, 123456, meta, 2};
        uint8_t p[DH_FRAME_MAX_PAYLOAD], f[DH_FRAME_MAX_SIZE], plain[DH_FRAME_MAX_PAYLOAD];
        size_t n = 0;
        CHECK(dh_seal_encode_offer(&A.seal_tx, aes_gcm_ref_aead(), &offer, p, sizeof p, &n) ==
                      DH_SEAL_OK &&
                  n == DH_SEAL_OFFER_OVERHEAD + 2u,
              "codec", "sealed offer length");
        size_t fl = 0, consumed = 0;
        dh_frame_view fv;
        CHECK(dh_frame_encode(DH_MSG_CLIP_OFFER, 0, p, n, f, sizeof f, &fl) == DH_FRAME_OK &&
                  dh_frame_decode(f, fl, &fv, &consumed) == DH_FRAME_OK,
              "codec", "offer through frame codec");
        dh_clip_offer back;
        CHECK(dh_seal_open_offer(&B.seal_rx, aes_gcm_ref_aead(), fv.payload, fv.hdr.len, plain,
                                 sizeof plain, &back) == DH_SEAL_OK &&
                  back.id == 7 && back.kind == 2 && back.total == 123456 && back.meta_len == 2 &&
                  memcmp(back.meta, meta, 2) == 0,
              "codec", "offer fields survive");

        dh_clip_chunk chunk = {7, 3, dh_crc32(payload, 100), payload, 100};
        CHECK(dh_seal_encode_chunk(&A.seal_tx, aes_gcm_ref_aead(), &chunk, p, sizeof p, &n) ==
                      DH_SEAL_OK &&
                  n == DH_SEAL_CHUNK_OVERHEAD + 100u,
              "codec", "sealed chunk length");
        dh_clip_chunk cback;
        CHECK(dh_seal_open_chunk(&B.seal_rx, aes_gcm_ref_aead(), p, n, plain,
                                 sizeof plain, &cback) == DH_SEAL_OK &&
                  cback.id == 7 && cback.seq == 3 && cback.crc32 == chunk.crc32 &&
                  cback.data_len == 100 && memcmp(cback.data, payload, 100) == 0,
              "codec", "chunk fields survive");

        uint32_t id, seq;
        uint16_t credits;
        CHECK(dh_clip_encode_id(9, p, sizeof p) == 4 && dh_clip_decode_id(p, 4, &id) &&
                  id == 9,
              "codec", "id round-trip");
        CHECK(dh_clip_encode_retransmit(9, 5, p, sizeof p) == 8 &&
                  dh_clip_decode_retransmit(p, 8, &id, &seq) && id == 9 && seq == 5,
              "codec", "retransmit round-trip");
        CHECK(dh_clip_encode_credit(9, 16, p, sizeof p) == 6 &&
                  dh_clip_decode_credit(p, 6, &id, &credits) && id == 9 && credits == 16,
              "codec", "credit round-trip");
        dh_clip_offer_head head;
        CHECK(!dh_clip_decode_offer_head(p, 3, &head), "codec", "short offer head rejected");
        CHECK(!dh_clip_decode_id(p, 5, &id), "codec", "wrong-length id rejected");
    }

    /* Clean multi-chunk round trip (ported: eager text round trip; chunking). */
    {
        reset_scenario();
        const size_t len = 3 * DH_XFER_CHUNK_SIZE + 100; /* 4 chunks */
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 1, "roundtrip", "not delivered");
        CHECK(B.delivered_len == len, "roundtrip", "length wrong");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "roundtrip", "bytes differ");
        CHECK(A.chunks_sent == 4, "roundtrip", "chunk count not ceil(len/chunk)");
        CHECK(B.retransmits_sent == 0, "roundtrip", "spurious retransmits");
        /* The sender retains the drained transfer for straggling retransmits. */
        CHECK(A.x.tx.active && A.x.tx.next_seq == A.x.tx.nchunks && !A.x.tx.need_done,
              "roundtrip", "sender not fully drained");
    }

    /* Single-chunk and exact-multiple payloads. */
    {
        reset_scenario();
        offer_and_run(&A, payload, 10);
        CHECK(B.delivered == 1 && B.delivered_len == 10, "small", "single chunk failed");
        reset_scenario();
        offer_and_run(&A, payload, 2 * DH_XFER_CHUNK_SIZE);
        CHECK(B.delivered == 1 && A.chunks_sent == 2, "exact", "exact multiple failed");
    }

    /* Over the receiver's cap: refused with a cancel, nothing delivered,
       and the next transfer still works (ported). */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, NULL, RX_CAP + 1, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        CHECK(B.delivered == 0, "cap", "oversize delivered");
        CHECK(!A.x.tx.active, "cap", "sender kept a refused transfer");
        offer_and_run(&A, payload, 100);
        CHECK(B.delivered == 1, "cap", "next transfer failed");
    }

    /* No streaming before a request (ported: the lazy-files bug). */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, 5000, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_SEND_OFFER, "prestream",
              "offer emitted more than the offer");
        CHECK(dh_xfer_pump(&A.x, acts, ACTS_CAP) == 0, "prestream",
              "pump streamed before a request");
    }

    /* Lazy payload: provider consulted only on request; failure cancels (ported). */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, NULL, 3000, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet(); /* B requested; A now owes NEED_DATA */
        CHECK(A.x.tx.lazy_pending, "lazy", "provider not consulted after request");
        CHECK(B.delivered == 0, "lazy", "delivered before data provided");
        n = dh_xfer_provide(&A.x, payload, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        CHECK(B.delivered == 1 && B.delivered_len == 3000, "lazy", "provided data not delivered");

        reset_scenario();
        n = dh_xfer_offer(&A.x, 0, NULL, 0, NULL, 3000, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        n = dh_xfer_provide_fail(&A.x, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        CHECK(B.delivered == 0, "lazy", "failed provider still delivered");
        CHECK(!A.x.tx.active, "lazy", "failed provider left transfer active");
    }

    /* AC: a dropped chunk is detected and re-requested — transfer completes. */
    {
        reset_scenario();
        plan.drop_seq = 2;
        plan.drop_armed = 1;
        const size_t len = 5 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 1 && B.delivered_len == len, "drop", "not delivered after loss");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "drop", "bytes differ after retransmit");
        CHECK(B.retransmits_sent == 1, "drop", "expected exactly one retransmit request");
        CHECK(A.chunks_sent == 6, "drop", "expected 5 chunks + 1 retransmit");
    }

    /* A chunk lost twice — the retransmitted copy lost as well — is
       re-requested at the next DONE round and the transfer still converges
       (the review-found deadlock). */
    {
        reset_scenario();
        plan.drop_seq = 2;
        plan.drop_armed = 2; /* lose the original AND its first retransmission */
        const size_t len = 5 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 1 && B.delivered_len == len, "double-loss",
              "did not converge after a lost retransmission");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "double-loss", "bytes differ");
        CHECK(B.retransmits_sent == 2, "double-loss", "expected two retransmit requests");
        CHECK(A.chunks_sent == 7, "double-loss", "expected 5 chunks + 2 retransmissions");
    }

    /* A dropped CLIP_DONE costs nothing (#132). The board's outbound queue
       refuses frames under burst with no retransmit beneath it, and DONE is
       emitted in the same pump batch as the last chunk — so it is the frame
       most likely to be the casualty. The receiver has every chunk, each one
       length-checked and CRC32-verified, so it completes on the last chunk
       rather than waiting to be told what it can already see. */
    {
        reset_scenario();
        plan.drop_done = 1;
        offer_and_run(&A, payload, 40);
        CHECK(B.delivered == 1, "done-drop", "not delivered without CLIP_DONE");
        CHECK(B.delivered_len == 40, "done-drop", "length wrong");
        CHECK(memcmp(B.rx_buf, payload, 40) == 0, "done-drop", "bytes differ");
        CHECK(B.retransmits_sent == 0, "done-drop", "asked for chunks it already had");
    }

    /* The same across a multi-chunk payload: completion is the last chunk
       landing, not the chunk count. */
    {
        reset_scenario();
        plan.drop_done = 1;
        const size_t len = 3 * DH_XFER_CHUNK_SIZE + 7;
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 1, "done-drop", "multi-chunk not delivered without CLIP_DONE");
        CHECK(B.delivered_len == len, "done-drop", "multi-chunk length wrong");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "done-drop", "multi-chunk bytes differ");
        CHECK(B.retransmits_sent == 0, "done-drop", "multi-chunk asked for chunks it already had");
    }

    /* DONE keeps its other job. With a chunk lost as well, the sweep it
       drives is still what re-requests the gap. */
    {
        reset_scenario();
        plan.drop_seq = 1;
        plan.drop_armed = 1;
        const size_t len = 3 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 1, "done-sweep", "not delivered after a lost chunk");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "done-sweep", "bytes differ");
        CHECK(B.retransmits_sent == 1, "done-sweep", "gap not re-requested exactly once");
    }

    /* A pump batch is bounded, and a partial batch never carries DONE
       (ported: bounded batches). */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        const size_t len = 40 * DH_XFER_CHUNK_SIZE;
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, len, acts, ACTS_CAP);
        (void)n;
        size_t rn = dh_xfer_handle_request(&A.x, A.x.tx.id, acts, ACTS_CAP);
        rn += dh_xfer_handle_credit(&A.x, A.x.tx.id, DH_XFER_CREDIT_WINDOW, acts + rn,
                                    ACTS_CAP - rn);
        CHECK(rn == 0, "batch", "handlers emitted chunks directly");
        n = dh_xfer_pump(&A.x, acts, ACTS_CAP);
        CHECK(n == DH_XFER_CREDIT_WINDOW, "batch", "opening batch did not spend its window");
        rn = dh_xfer_handle_credit(&A.x, A.x.tx.id, 64, acts, ACTS_CAP);
        CHECK(rn == 0, "batch", "streaming credit emitted chunks directly");
        n = dh_xfer_pump(&A.x, acts, ACTS_CAP);
        CHECK(n == DH_XFER_BATCH_MAX, "batch", "batch not capped at DH_XFER_BATCH_MAX");
        for (size_t i = 0; i < n; i++)
            CHECK(acts[i].type == DH_XFER_ACT_SEND_CHUNK, "batch",
                  "partial batch carried a non-chunk action");
    }

    /* AC: a corrupt chunk produces a retransmit for that chunk alone. The seal
       is what refuses it now, one layer before the transfer machine sees it. */
    {
        reset_scenario();
        plan.corrupt_seq = 1;
        plan.corrupt_armed = 1;
        const size_t len = 4 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(B.seal_refused == 1, "corrupt", "the corrupted chunk was not refused by the seal");
        CHECK(B.delivered == 1, "corrupt", "not delivered after corruption");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "corrupt", "bytes differ");
        CHECK(B.retransmits_sent == 1, "corrupt", "retransmit not selective");
    }

    /* The CRC32 inside the seal is fidelity, not authentication: what it
       catches is a chunk whose bytes and checksum disagree, which after #113
       means a bug in the seal layer rather than anything the wire did. Driven
       directly, because no wire fault can reach it — GCM refuses first. */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        const dh_clip_offer offer = {
            .id = 77, .kind = 0, .total = sizeof data, .meta = NULL, .meta_len = 0};
        (void)dh_xfer_handle_offer(&B.x, &offer, acts, ACTS_CAP);

        dh_clip_chunk chunk = {.id = 77,
                               .seq = 0,
                               .crc32 = dh_crc32(data, sizeof data) ^ 0xffffffffu,
                               .data = data,
                               .data_len = (uint16_t)sizeof data};
        (void)dh_xfer_handle_chunk(&B.x, &chunk, acts, ACTS_CAP);
        CHECK(B.x.rx.nreceived == 0, "crc", "a chunk whose CRC32 lies was accepted");

        chunk.crc32 = dh_crc32(data, sizeof data);
        (void)dh_xfer_handle_chunk(&B.x, &chunk, acts, ACTS_CAP);
        CHECK(B.x.rx.nreceived == 1, "crc", "the honest chunk was not accepted");
    }

    /* AC: credit exhaustion stops the sender; replenishment resumes it. */
    {
        reset_scenario();
        plan.block_credits = 1; /* B's grants are withheld, including the initial one */
        dh_xfer_action acts[ACTS_CAP];
        const size_t len = 40 * DH_XFER_CHUNK_SIZE;
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, len, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        CHECK(A.chunks_sent == 0, "credit", "sent without any credit");
        CHECK(B.delivered == 0, "credit", "delivered without chunks");

        /* Release the withheld grants: the transfer completes. */
        plan.block_credits = 0;
        for (size_t i = 0; i < plan.held_count; i++) {
            n = dispatch(&A, &plan.held_credits[i], acts);
            enqueue_actions(&A, &B, acts, n);
        }
        plan.held_count = 0;
        run_until_quiet();
        CHECK(B.delivered == 1 && B.delivered_len == len, "credit",
              "replenishment did not resume");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "credit", "bytes differ");
    }

    /* Mid-stream credit pacing: sender never exceeds granted credit. */
    {
        reset_scenario();
        const size_t len = 40 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 1 && B.delivered_len == len, "pacing", "long transfer failed");
        CHECK(A.chunks_sent == 40, "pacing", "wrong chunk count");
    }

    /* AC: cancellation arriving during a retransmission leaves both sides
       consistent, and a fresh transfer works. */
    {
        reset_scenario();
        plan.drop_seq = 1;
        plan.drop_armed = 1;
        plan.cancel_rx_on_retransmit = 1;
        offer_and_run(&A, payload, 6 * DH_XFER_CHUNK_SIZE);
        CHECK(B.delivered == 0, "cancel-retx", "cancelled transfer delivered");
        CHECK(!A.x.tx.active, "cancel-retx", "sender kept cancelled transfer");
        CHECK(!B.x.rx.active, "cancel-retx", "receiver kept cancelled transfer");
        offer_and_run(&A, payload, 100);
        CHECK(B.delivered == 1, "cancel-retx", "next transfer failed");
    }

    /* AC: a link drop mid-payload abandons — no partial delivery. */
    {
        reset_scenario();
        plan.block_credits = 1; /* stall after the initial window */
        dh_xfer_action acts[ACTS_CAP];
        const size_t len = 40 * DH_XFER_CHUNK_SIZE;
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, len, acts, ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet(); /* stalled mid-payload */
        n = dh_xfer_link_down(&B.x, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_FAILED &&
                  acts[0].reason == DH_XFER_FAIL_LINK_DROP,
              "linkdrop", "receiver did not report the abandon");
        CHECK(!B.x.rx.active, "linkdrop", "receiver kept partial transfer");
        CHECK(B.delivered == 0, "linkdrop", "partial payload delivered");
        n = dh_xfer_link_down(&A.x, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_FAILED, "linkdrop",
              "sender did not report the abandon");
        CHECK(!A.x.tx.active, "linkdrop", "sender kept dead transfer");
    }

    /* A newer offer supersedes an incomplete one (ported). */
    {
        reset_scenario();
        plan.block_credits = 1; /* freeze transfer 1 before any chunk flows */
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, 8 * DH_XFER_CHUNK_SIZE, acts,
                                 ACTS_CAP);
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        plan.block_credits = 0;
        plan.held_count = 0; /* stale grants for transfer 1 die with it */
        offer_and_run(&A, payload, 200); /* supersedes */
        CHECK(B.delivered == 1 && B.delivered_len == 200, "supersede",
              "superseding transfer did not deliver");
        CHECK(B.delivered == 1, "supersede", "stale transfer delivered too");
    }

    /* The sender retains its payload after DONE: a straggling retransmit
       request is still honoured, then DONE repeats. */
    {
        reset_scenario();
        offer_and_run(&A, payload, 2 * DH_XFER_CHUNK_SIZE);
        CHECK(B.delivered == 1, "retain", "setup transfer failed");
        CHECK(A.x.tx.active, "retain", "sender released payload at DONE");
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_handle_retransmit(&A.x, A.x.tx.id, 1, acts, ACTS_CAP);
        n += dh_xfer_handle_credit(&A.x, A.x.tx.id, 1, acts + n, ACTS_CAP - n);
        size_t got_chunk = 0, got_done = 0;
        n += dh_xfer_pump(&A.x, acts + n, ACTS_CAP - n);
        for (size_t i = 0; i < n; i++) {
            if (acts[i].type == DH_XFER_ACT_SEND_CHUNK && acts[i].seq == 1)
                got_chunk = 1;
            if (acts[i].type == DH_XFER_ACT_SEND_DONE)
                got_done = 1;
        }
        CHECK(got_chunk, "retain", "post-DONE retransmit not honoured");
        CHECK(got_done, "retain", "DONE not repeated after retransmit");
    }

    /* Transfer ids increment (ported), so both sides can offer independently. */
    {
        reset_scenario();
        offer_and_run(&A, payload, 100);
        uint32_t first = A.x.tx.id;
        offer_and_run(&A, payload, 100);
        CHECK(A.x.tx.id == first + 1, "ids", "ids do not increment");
        offer_and_run(&B, payload, 300);
        CHECK(A.delivered == 1 && A.delivered_len == 300, "ids",
              "reverse-direction transfer failed");
    }

    /* ---- #145: a receive that stalls asks again for what it wants -------- */

    /* AC: a lost credit grant strands the sender; a stall sweep restarts it.
       Nothing else can: no chunk arrives, so nothing prompts the receiver, and
       no DONE arrives to drive the sweep in handle_done. */
    {
        reset_scenario();
        plan.credit_pass = 1;  /* the opening window lands */
        plan.drop_credits = 3; /* every grant the first three chunks earn is lost */
        const size_t len = 40 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(B.delivered == 0, "sweep-credit", "the scenario did not stall");
        CHECK(A.x.tx.credits == 0, "sweep-credit", "the sender was not starved");
        const int rounds = sweep_until_delivered(&B, 20);
        CHECK(rounds > 0, "sweep-credit", "the sweep did not finish the transfer");
        CHECK(B.delivered == 1 && B.delivered_len == len, "sweep-credit", "not delivered");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "sweep-credit", "bytes differ");
    }

    /* AC: a lost CLIP_REQUEST means the sender never starts. The sweep sends
       it again, with the window that covers it, because this end cannot tell a
       lost request from a lost grant. */
    {
        reset_scenario();
        plan.drop_requests = 1;
        const size_t len = 6 * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(A.chunks_sent == 0, "sweep-request", "the sender started without a request");

        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_sweep_rx(&B.x, acts, ACTS_CAP);
        size_t asked = 0, granted = 0;
        for (size_t i = 0; i < n; i++) {
            if (acts[i].type == DH_XFER_ACT_SEND_REQUEST)
                asked++;
            if (acts[i].type == DH_XFER_ACT_SEND_CREDIT)
                granted += acts[i].credits;
        }
        CHECK(asked == 1, "sweep-request", "the request was not sent again");
        CHECK(granted == DH_XFER_CREDIT_WINDOW, "sweep-request", "no window came with it");
        enqueue_actions(&B, &A, acts, n);
        run_until_quiet();
        CHECK(B.delivered == 1 && B.delivered_len == len, "sweep-request", "not delivered");
    }

    /* AC (#137): the same burst takes the last chunk and the DONE behind it.
       Nothing completes the set and no DONE arrives to drive a sweep. */
    {
        reset_scenario();
        const size_t chunks = 12;
        plan.drop_seq = (uint32_t)chunks - 1;
        plan.drop_armed = 1;
        plan.drop_done = 1;
        offer_and_run(&A, payload, chunks * DH_XFER_CHUNK_SIZE);
        CHECK(B.delivered == 0, "sweep-tail", "the scenario did not stall");
        CHECK(dh_xfer_rx_received(&B.x) == chunks - 1, "sweep-tail", "wrong stall point");
        CHECK(sweep_until_delivered(&B, 20) > 0, "sweep-tail", "the tail was never recovered");
        CHECK(B.delivered == 1, "sweep-tail", "not delivered");
        CHECK(memcmp(B.rx_buf, payload, chunks * DH_XFER_CHUNK_SIZE) == 0, "sweep-tail",
              "bytes differ");
    }

    /* AC: the whole opening burst is lost — every chunk of it and the DONE
       behind them. The receiver has seen nothing at all, so it cannot tell a
       sender that never started from one that started and was not heard, and
       has to cover both. */
    {
        reset_scenario();
        const size_t chunks = DH_XFER_CREDIT_WINDOW;
        plan.drop_chunks = (int)chunks;
        plan.drop_done = 1;
        const size_t len = chunks * DH_XFER_CHUNK_SIZE;
        offer_and_run(&A, payload, len);
        CHECK(dh_xfer_rx_received(&B.x) == 0, "sweep-opening", "the scenario did not stall");
        CHECK(A.x.tx.next_seq == chunks, "sweep-opening", "the sender never emitted the burst");
        CHECK(sweep_until_delivered(&B, 20) > 0, "sweep-opening", "the burst was never recovered");
        CHECK(B.delivered == 1, "sweep-opening", "not delivered");
        CHECK(memcmp(B.rx_buf, payload, len) == 0, "sweep-opening", "bytes differ");
    }

    /* AC: the sweep names what it is waiting for — the chunks at the frontier,
       a window's worth and no more, each with its covering credit. Asking for
       the whole tail would be one request per remaining chunk. */
    {
        reset_scenario();
        plan.credit_pass = 1;
        plan.drop_credits = 3;
        offer_and_run(&A, payload, 40 * DH_XFER_CHUNK_SIZE);
        const uint32_t stopped_at = dh_xfer_rx_received(&B.x);
        CHECK(stopped_at > 0 && stopped_at < 40, "sweep-names", "the scenario did not stall");

        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_sweep_rx(&B.x, acts, ACTS_CAP);
        size_t asked = 0, granted = 0;
        for (size_t i = 0; i < n; i++) {
            if (acts[i].type == DH_XFER_ACT_SEND_RETRANSMIT) {
                CHECK(acts[i].seq >= stopped_at, "sweep-names", "asked for a chunk it holds");
                asked++;
            }
            if (acts[i].type == DH_XFER_ACT_SEND_CREDIT)
                granted += acts[i].credits;
        }
        CHECK(asked == DH_XFER_CREDIT_WINDOW, "sweep-names", "wrong number of chunks named");
        CHECK(granted == asked, "sweep-names", "a request went out without covering credit");
        CHECK(dh_xfer_rx_retx_asked(&B.x) == asked, "sweep-names", "the reading does not say so");
    }

    /* A stall says whether a retransmit was asked for, and whether one came
       back — on both ends, which is what no log line said before (#145). */
    {
        reset_scenario();
        plan.drop_seq = 2;
        plan.drop_armed = 1;
        offer_and_run(&A, payload, 8 * DH_XFER_CHUNK_SIZE);
        CHECK(B.delivered == 1, "sweep-reading", "the transfer did not recover");
        CHECK(dh_xfer_rx_retx_asked(&B.x) >= 1, "sweep-reading", "the receiver asked for nothing");
        CHECK(dh_xfer_rx_retx_answered(&B.x) >= 1, "sweep-reading", "nothing came back");
        CHECK(dh_xfer_tx_retx_asked(&A.x) >= 1, "sweep-reading", "the sender was asked nothing");
        CHECK(dh_xfer_tx_retx_sent(&A.x) >= 1, "sweep-reading", "the sender sent nothing again");
    }

    /* A sender that has not started must not bank every window a repeating
       receiver sends it, or the first pump is a burst no outbound queue on the
       path is sized for (ADR-0005). Driven directly: what is being pinned is
       what the sender does with credit it cannot yet spend. */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, payload, 40 * DH_XFER_CHUNK_SIZE, acts, ACTS_CAP);
        for (int i = 0; i < 4; i++) /* four sweeps whose requests were all lost */
            (void)dh_xfer_handle_credit(&A.x, A.x.tx.id, DH_XFER_CREDIT_WINDOW, acts, ACTS_CAP);
        CHECK(A.x.tx.credits == 4 * DH_XFER_CREDIT_WINDOW, "sweep-burst",
              "the scenario did not bank anything");

        (void)dh_xfer_handle_request(&A.x, A.x.tx.id, acts, ACTS_CAP);
        size_t n = dh_xfer_pump(&A.x, acts, ACTS_CAP);
        size_t chunks = 0;
        for (size_t i = 0; i < n; i++)
            if (acts[i].type == DH_XFER_ACT_SEND_CHUNK)
                chunks++;
        CHECK(chunks <= DH_XFER_CREDIT_WINDOW, "sweep-burst",
              "the first pump spent every banked window at once");
    }

    /* AC: the paste #145 names — 3,782 chunks — completes in both directions
       over a link losing roughly one message in thirty. */
    {
        const size_t len = sizeof big_payload;
        reset_scenario();
        plan.loss_one_in = 30;
        plan.loss_state = 0x5eed145u;
        offer_and_run(&A, big_payload, len);
        CHECK(sweep_until_delivered(&B, 4000) >= 0, "sweep-big", "A to B never finished");
        CHECK(B.delivered == 1 && B.delivered_len == len, "sweep-big", "A to B not delivered");
        CHECK(memcmp(B.rx_buf, big_payload, len) == 0, "sweep-big", "A to B bytes differ");

        reset_scenario();
        plan.loss_one_in = 30;
        plan.loss_state = 0x5eed145u;
        offer_and_run(&B, big_payload, len);
        CHECK(sweep_until_delivered(&A, 4000) >= 0, "sweep-big", "B to A never finished");
        CHECK(A.delivered == 1 && A.delivered_len == len, "sweep-big", "B to A not delivered");
        CHECK(memcmp(A.rx_buf, big_payload, len) == 0, "sweep-big", "B to A bytes differ");
    }

    /* A lost original offer is recoverable from the sender, without making
       retry activity look like streaming progress. */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, 10, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_SEND_OFFER, "offer-retry",
              "the original offer was not produced");
        n = dh_xfer_retry_offer(&A.x, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_SEND_OFFER_RETRY, "offer-retry",
              "an unanswered offer was not retried");
        CHECK(dh_xfer_tx_offer_retries(&A.x) == 1, "offer-retry", "retry was not counted");
        enqueue_actions(&A, &B, acts, n);
        run_until_quiet();
        CHECK(B.delivered == 1, "offer-retry", "the retry did not recover the transfer");
        CHECK(dh_xfer_retry_offer(&A.x, acts, ACTS_CAP) == 0, "offer-retry",
              "a requested offer remained retryable");

        reset_scenario();
        (void)dh_xfer_offer(&A.x, 2, NULL, 0, NULL, 10, acts, ACTS_CAP);
        (void)dh_xfer_handle_request(&A.x, A.x.tx.id, acts, ACTS_CAP);
        CHECK(A.x.tx.lazy_pending && dh_xfer_retry_offer(&A.x, acts, ACTS_CAP) == 0,
              "offer-retry", "lazy data-pending state remained retryable");
    }

    /* Identical retries preserve receive state: before data they repeat one
       opening request/window, after data and completion they are silent. */
    {
        reset_scenario();
        dh_clip_offer offer = {7, 0, 2 * DH_XFER_CHUNK_SIZE, NULL, 0};
        dh_xfer_action acts[ACTS_CAP];
        size_t n = dh_xfer_handle_offer(&B.x, &offer, acts, ACTS_CAP);
        CHECK(n == 2, "offer-idempotent", "the original did not open the receive");
        n = dh_xfer_handle_offer(&B.x, &offer, acts, ACTS_CAP);
        CHECK(n == 2 && acts[0].type == DH_XFER_ACT_SEND_REQUEST &&
                  acts[1].type == DH_XFER_ACT_SEND_CREDIT,
              "offer-idempotent", "an empty receive did not repeat its opening window");
        dh_clip_chunk first = {7, 0, dh_crc32(payload, DH_XFER_CHUNK_SIZE), payload,
                               DH_XFER_CHUNK_SIZE};
        (void)dh_xfer_handle_chunk(&B.x, &first, acts, ACTS_CAP);
        n = dh_xfer_handle_offer(&B.x, &offer, acts, ACTS_CAP);
        CHECK(n == 0 && dh_xfer_rx_received(&B.x) == 1, "offer-idempotent",
              "a retry erased partial receive state");
        CHECK(dh_xfer_rx_duplicate_offers(&B.x) == 2, "offer-idempotent",
              "duplicates observed were not counted");

        reset_scenario();
        offer_and_run(&A, payload, 10);
        dh_clip_offer completed;
        CHECK(dh_xfer_offer_info(&A.x, &completed), "offer-idempotent",
              "completed sender identity was unavailable");
        n = dh_xfer_handle_offer(&B.x, &completed, acts, ACTS_CAP);
        CHECK(n == 0 && B.delivered == 1, "offer-idempotent",
              "a completed duplicate recreated or delivered a receive");
    }

    /* Multiple delayed copies of one offer can make all of their response
       pairs arrive before the sender gets a chance to pump. Requests and
       credits are separate frames, so deliver the requests first and the
       covering grants afterwards: the first emitted batch is still exactly
       one opening window, not one window per duplicate (#150). */
    {
        reset_scenario();
        dh_xfer_action offer_acts[ACTS_CAP];
        dh_xfer_action response_acts[3][ACTS_CAP];
        size_t response_counts[3];
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, payload, 10 * DH_XFER_CHUNK_SIZE,
                            offer_acts, ACTS_CAP);
        dh_clip_offer offer;
        CHECK(dh_xfer_offer_info(&A.x, &offer), "offer-opening-credit",
              "sender offer unavailable");
        for (size_t i = 0; i < 3; i++)
            response_counts[i] = dh_xfer_handle_offer(&B.x, &offer, response_acts[i], ACTS_CAP);

        for (size_t i = 0; i < 3; i++) {
            CHECK(response_counts[i] == 2 &&
                      response_acts[i][0].type == DH_XFER_ACT_SEND_REQUEST &&
                      response_acts[i][1].type == DH_XFER_ACT_SEND_CREDIT,
                  "offer-opening-credit", "duplicate response pair was incomplete");
            (void)dh_xfer_handle_request(&A.x, response_acts[i][0].id, offer_acts, ACTS_CAP);
        }
        (void)dh_xfer_handle_credit(&A.x, response_acts[0][1].id,
                                    response_acts[0][1].credits, offer_acts, ACTS_CAP);

        size_t n = dh_xfer_pump(&A.x, offer_acts, ACTS_CAP);
        size_t chunks = 0;
        for (size_t i = 0; i < n; i++)
            if (offer_acts[i].type == DH_XFER_ACT_SEND_CHUNK)
                chunks++;
        CHECK(chunks == DH_XFER_CREDIT_WINDOW, "offer-opening-credit",
              "duplicate offers inflated the first emitted batch");

        /* The remaining opening grants can also trail the first pump. They
           cannot refill the one opening window the transfer already spent. */
        for (size_t i = 1; i < 3; i++)
            (void)dh_xfer_handle_credit(&A.x, response_acts[i][1].id,
                                        response_acts[i][1].credits, offer_acts, ACTS_CAP);
        n = dh_xfer_pump(&A.x, offer_acts, ACTS_CAP);
        chunks = 0;
        for (size_t i = 0; i < n; i++)
            if (offer_acts[i].type == DH_XFER_ACT_SEND_CHUNK)
                chunks++;
        CHECK(chunks == 0, "offer-opening-credit",
              "late duplicate-offer credits refilled the spent opening window");

        reset_scenario();
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, NULL, 0, offer_acts, ACTS_CAP);
        CHECK(dh_xfer_offer_info(&A.x, &offer), "offer-opening-credit",
              "zero-length offer unavailable");
        for (size_t i = 0; i < 3; i++) {
            size_t rn = dh_xfer_handle_offer(&B.x, &offer, response_acts[i], ACTS_CAP);
            CHECK(rn == 2, "offer-opening-credit", "zero-length response pair incomplete");
            (void)dh_xfer_handle_request(&A.x, response_acts[i][0].id, offer_acts, ACTS_CAP);
            (void)dh_xfer_handle_credit(&A.x, response_acts[i][1].id,
                                        response_acts[i][1].credits, offer_acts, ACTS_CAP);
        }
        n = dh_xfer_pump(&A.x, offer_acts, ACTS_CAP);
        CHECK(n == 1 && offer_acts[0].type == DH_XFER_ACT_SEND_DONE,
              "offer-opening-credit", "zero-length transfer did not start normally");

        reset_scenario();
        (void)dh_xfer_offer(&A.x, 2, NULL, 0, NULL, 10 * DH_XFER_CHUNK_SIZE,
                            offer_acts, ACTS_CAP);
        CHECK(dh_xfer_offer_info(&A.x, &offer), "offer-opening-credit",
              "lazy offer unavailable");
        for (size_t i = 0; i < 3; i++) {
            size_t rn = dh_xfer_handle_offer(&B.x, &offer, response_acts[i], ACTS_CAP);
            CHECK(rn == 2, "offer-opening-credit", "lazy response pair incomplete");
            (void)dh_xfer_handle_request(&A.x, response_acts[i][0].id, offer_acts, ACTS_CAP);
            (void)dh_xfer_handle_credit(&A.x, response_acts[i][1].id,
                                        response_acts[i][1].credits, offer_acts, ACTS_CAP);
        }
        (void)dh_xfer_provide(&A.x, payload, offer_acts, ACTS_CAP);
        n = dh_xfer_pump(&A.x, offer_acts, ACTS_CAP);
        chunks = 0;
        for (size_t i = 0; i < n; i++)
            if (offer_acts[i].type == DH_XFER_ACT_SEND_CHUNK)
                chunks++;
        CHECK(chunks == DH_XFER_CREDIT_WINDOW, "offer-opening-credit",
              "duplicate offers inflated a lazy transfer's opening batch");
    }

    /* Ordered identity makes stale offers silent, newer unacceptable offers
       supersede, conflicts end the local session, and zero is never issued. */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        dh_clip_offer current = {UINT32_MAX, 0, 10, NULL, 0};
        CHECK(dh_xfer_handle_offer(&B.x, &current, acts, ACTS_CAP) == 2, "offer-order",
              "the first identity was not accepted");
        dh_clip_offer wrapped = {1, 0, RX_CAP + 1, NULL, 0};
        size_t n = dh_xfer_handle_offer(&B.x, &wrapped, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_SEND_CANCEL && !B.x.rx.active,
              "offer-order", "a newer unacceptable offer did not supersede and cancel");
        uint8_t large_meta[DH_XFER_META_MAX + 1] = {0};
        dh_clip_offer large = {2, 2, 10, large_meta, sizeof large_meta};
        n = dh_xfer_handle_offer(&B.x, &large, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_SEND_CANCEL, "offer-order",
              "oversized metadata was not refused");
        CHECK(dh_xfer_handle_offer(&B.x, &large, acts, ACTS_CAP) == 0, "offer-order",
              "an identical retry of a refused offer became a conflict");
        CHECK(dh_xfer_handle_offer(&B.x, &current, acts, ACTS_CAP) == 0, "offer-order",
              "an older offer was not ignored");
        dh_clip_offer conflict = large;
        conflict.kind = 1;
        n = dh_xfer_handle_offer(&B.x, &conflict, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_PROTOCOL_ERROR, "offer-order",
              "conflicting immutable content was not a protocol error");
        dh_clip_offer zero = {0, 0, 10, NULL, 0};
        n = dh_xfer_handle_offer(&A.x, &zero, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_PROTOCOL_ERROR, "offer-order",
              "the non-transfer sentinel was accepted as an offer id");

        A.x.next_id = UINT32_MAX;
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, payload, 10, acts, ACTS_CAP);
        CHECK(A.x.tx.id == UINT32_MAX, "offer-order", "the last id changed");
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, payload, 10, acts, ACTS_CAP);
        CHECK(A.x.tx.id == 1, "offer-order", "sender issued zero at wrap");

        dh_xfer_link_down(&B.x, acts, ACTS_CAP);
        dh_clip_offer after_link = {1, 0, 10, NULL, 0};
        CHECK(dh_xfer_handle_offer(&B.x, &after_link, acts, ACTS_CAP) == 2, "offer-order",
              "offer ordering did not reset with the link");
    }

    /*
     * The paste side's own session is not the only boundary of that ordering:
     * the copy side's helper can restart while this side stays up, and its ids
     * start again at one (#151). A fresh incoming seal is the evidence, and it
     * has to end the previous process's namespace along with its transfers.
     */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        dh_clip_offer first = {1, 0, 10, NULL, 0};
        dh_clip_offer second = {2, 0, 2 * DH_XFER_CHUNK_SIZE, NULL, 0};
        CHECK(dh_xfer_handle_offer(&B.x, &first, acts, ACTS_CAP) == 2, "offer-restart",
              "the first identity was not accepted");
        CHECK(dh_xfer_handle_offer(&B.x, &second, acts, ACTS_CAP) == 2, "offer-restart",
              "the frontier did not move on");

        /* An incomplete receive belongs to the seal it arrived under, so a
           restarted copy side can never finish it. Abandoned, never delivered
           in part. */
        dh_clip_chunk part = {2, 0, dh_crc32(payload, DH_XFER_CHUNK_SIZE), payload,
                              DH_XFER_CHUNK_SIZE};
        (void)dh_xfer_handle_chunk(&B.x, &part, acts, ACTS_CAP);
        CHECK(dh_xfer_rx_received(&B.x) == 1, "offer-restart", "the partial chunk was refused");
        size_t n = dh_xfer_rx_seal_replaced(&B.x, acts, ACTS_CAP);
        CHECK(n == 1 && acts[0].type == DH_XFER_ACT_FAILED && acts[0].id == 2 &&
                  acts[0].reason == DH_XFER_FAIL_SEAL_REPLACED,
              "offer-restart", "the receive under the replaced seal was not abandoned");
        CHECK(!dh_xfer_is_receiving(&B.x) && dh_xfer_rx_received(&B.x) == 0, "offer-restart",
              "a partial receive survived the seal it arrived under");

        /* And the restarted copy side's id 1 is a fresh transfer, not a stale
           one and not a conflict with what the dead process sent under it. */
        dh_clip_offer restarted = {1, 0, 20, NULL, 0};
        CHECK(dh_xfer_handle_offer(&B.x, &restarted, acts, ACTS_CAP) == 2, "offer-restart",
              "the restarted copy side's first offer was refused");

        /* Nothing this end is sending is touched: that recovers by the
           ordinary stale-seal exchange re-offering it under a key the far end
           can open. */
        reset_scenario();
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, payload, 10, acts, ACTS_CAP);
        const uint32_t sending = A.x.tx.id;
        CHECK(dh_xfer_rx_seal_replaced(&A.x, acts, ACTS_CAP) == 0, "offer-restart",
              "a fresh seal reported something with nothing arriving");
        CHECK(dh_xfer_is_sending(&A.x) && A.x.tx.id == sending, "offer-restart",
              "a fresh incoming seal disturbed the outgoing transfer");
    }

    /* Offer recovery state is per direction: simultaneous copies can both
       lose their opening announcement and recover independently. */
    {
        reset_scenario();
        dh_xfer_action aacts[ACTS_CAP], bacts[ACTS_CAP];
        (void)dh_xfer_offer(&A.x, 0, NULL, 0, payload, 20, aacts, ACTS_CAP);
        (void)dh_xfer_offer(&B.x, 0, NULL, 0, payload + 20, 30, bacts, ACTS_CAP);
        size_t an = dh_xfer_retry_offer(&A.x, aacts, ACTS_CAP);
        size_t bn = dh_xfer_retry_offer(&B.x, bacts, ACTS_CAP);
        enqueue_actions(&A, &B, aacts, an);
        enqueue_actions(&B, &A, bacts, bn);
        run_until_quiet();
        CHECK(A.delivered == 1 && A.delivered_len == 30 && B.delivered == 1 &&
                  B.delivered_len == 20,
              "offer-bidirectional", "one direction disturbed the other's recovery");
    }

    if (failures == 0)
        printf("xfer_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
