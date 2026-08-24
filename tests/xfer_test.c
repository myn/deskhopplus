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

#define RX_CAP (256u * 1024u)
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
    int block_credits;    /* while set, CLIP_CREDIT messages are discarded */
    struct wire_msg held_credits[16];
    size_t held_count;
    int cancel_rx_on_retransmit; /* receiver cancels after emitting a retransmit */
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

/* Encode one action from `from` into a wire message; false = nothing to send
   (local action) or deliberately faulted away. */
static int encode_action(struct side *from, const dh_xfer_action *a, struct wire_msg *m) {
    int n = -1;
    switch (a->type) {
    case DH_XFER_ACT_SEND_OFFER: {
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
        m->type = DH_MSG_CLIP_DONE;
        n = dh_clip_encode_id(a->id, m->payload, sizeof m->payload);
        break;
    case DH_XFER_ACT_SEND_REQUEST:
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
    default:
        CHECK(0, "wire", "unknown action type");
        return 0;
    }
    CHECK(n > 0, "wire", "payload encode failed");
    m->len = (uint16_t)(n > 0 ? n : 0);
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
        CHECK(++spins < 10000, "wire", "scenario did not quiesce");
        if (spins >= 10000)
            return;
    }
}

static void offer_and_run(struct side *from, const uint8_t *data, uint64_t total) {
    dh_xfer_action acts[ACTS_CAP];
    size_t n = dh_xfer_offer(&from->x, 0, NULL, 0, data, total, acts, ACTS_CAP);
    enqueue_actions(from, peer(from), acts, n);
    run_until_quiet();
}

static void fill_pattern(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(i * 7 + (i >> 8));
}

static uint8_t payload[64u * 1024u];

/* ---- scenarios ------------------------------------------------------- */

int main(void) {
    fill_pattern(payload, sizeof payload);

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

    /* A pump batch is bounded, and a partial batch never carries DONE
       (ported: bounded batches). */
    {
        reset_scenario();
        dh_xfer_action acts[ACTS_CAP];
        const size_t len = 40 * DH_XFER_CHUNK_SIZE;
        size_t n = dh_xfer_offer(&A.x, 0, NULL, 0, payload, len, acts, ACTS_CAP);
        (void)n;
        size_t rn = dh_xfer_handle_request(&A.x, A.x.tx.id, acts, ACTS_CAP);
        rn += dh_xfer_handle_credit(&A.x, A.x.tx.id, 64, acts + rn, ACTS_CAP - rn);
        CHECK(rn == 0, "batch", "handlers emitted chunks directly");
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

    if (failures == 0)
        printf("xfer_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
