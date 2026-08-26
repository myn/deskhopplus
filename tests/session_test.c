/*
 * The board's side of the v2 session (#111, ADR-0008): the hello exchange, the
 * pairing exchange, per-frame authentication, liveness, and the listener alert.
 *
 * Gated by test-vectors/frames.txt for the bytes and test-vectors/primitives.txt
 * for the key material those bytes were tagged under. Both were produced by an
 * independent implementation (tools/gen-frame-vectors.py), which is what makes
 * "the board answers the golden hello with the golden ack" a real check rather
 * than this file agreeing with itself.
 *
 * The v1 frames that used to be frozen here are gone with the protocol that
 * needed them.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "dh_session.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

#define MAX_BYTES DH_FRAME_MAX_SIZE

/* ------------------------------------------------------------------ loading */

/* Both files are loaded into one table: primitives.txt for the key material
   and frames.txt for the bytes. Sized generously, and a table that fills up
   fails loudly rather than quietly gating fewer frames than it claims. */
#define MAX_VECTORS 128
#define MAX_FIELDS 8
#define MAX_FIELD_BYTES 256

struct vector {
    char name[48];
    size_t fields;
    uint8_t f[MAX_FIELDS][MAX_FIELD_BYTES];
    size_t len[MAX_FIELDS];
};

static struct vector vectors[MAX_VECTORS];
static size_t vector_count;

static uint8_t helper_private[DH_P256_PRIVATE_SIZE];
static uint8_t board_private[DH_P256_PRIVATE_SIZE];
static uint8_t helper_nonce[DH_NONCE_SIZE];
static uint8_t board_nonce[DH_NONCE_SIZE];
static uint8_t shared_secret[DH_P256_SHARED_SIZE];
static uint8_t k_hello[DH_SESSION_KEY_SIZE];
static uint8_t k_h2b[DH_SESSION_KEY_SIZE];
static uint8_t k_b2h[DH_SESSION_KEY_SIZE];
static uint8_t helper_key_id[DH_KEY_ID_SIZE];

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `<name> | <hex> | <hex> ...`, comments on '#'. frames.txt is the one-field
   case of the same format primitives.txt uses. */
static bool load_vectors(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        ++failures;
        printf("FAIL cannot open %s\n", path);
        return false;
    }

    char line[16384];
    while (fgets(line, sizeof line, file)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        if (vector_count >= MAX_VECTORS) {
            ++failures;
            printf("FAIL vector capacity (%d) exceeded in %s\n", MAX_VECTORS, path);
            break;
        }

        struct vector *v = &vectors[vector_count];
        size_t name_len = 0;
        for (char *q = p; q < bar && name_len + 1 < sizeof v->name; q++)
            if (!isspace((unsigned char)*q)) v->name[name_len++] = *q;
        v->name[name_len] = '\0';

        v->fields = 0;
        bool bad = false;
        for (char *q = bar; *q && v->fields < MAX_FIELDS;) {
            const size_t idx = v->fields++;
            v->len[idx] = 0;
            int hi = -1;
            for (q++; *q && *q != '|'; q++) {
                if (isspace((unsigned char)*q)) continue;
                const int nib = hex_nibble((unsigned char)*q);
                if (nib < 0 || v->len[idx] >= MAX_FIELD_BYTES) { bad = true; break; }
                if (hi < 0) {
                    hi = nib;
                } else {
                    v->f[idx][v->len[idx]++] = (uint8_t)((hi << 4) | nib);
                    hi = -1;
                }
            }
            if (bad) break;
            while (*q && *q != '|') q++;
        }
        if (!bad) vector_count++;
    }

    fclose(file);
    return true;
}

static const struct vector *find(const char *name) {
    for (size_t i = 0; i < vector_count; i++)
        if (strcmp(vectors[i].name, name) == 0) return &vectors[i];
    ++failures;
    printf("FAIL vector %s missing\n", name);
    return NULL;
}

/* One session_material line out of primitives.txt: the two private keys, the
   two nonces, the shared secret and the three session keys. */
static bool load_session_material(void) {
    const struct vector *m = find("session_material");
    if (m == NULL || m->fields < 8) return false;

    memcpy(helper_private, m->f[0], DH_P256_PRIVATE_SIZE);
    memcpy(board_private, m->f[1], DH_P256_PRIVATE_SIZE);
    memcpy(helper_nonce, m->f[2], DH_NONCE_SIZE);
    memcpy(board_nonce, m->f[3], DH_NONCE_SIZE);
    memcpy(shared_secret, m->f[4], DH_P256_SHARED_SIZE);
    memcpy(k_hello, m->f[5], DH_SESSION_KEY_SIZE);
    memcpy(k_h2b, m->f[6], DH_SESSION_KEY_SIZE);
    memcpy(k_b2h, m->f[7], DH_SESSION_KEY_SIZE);

    uint8_t helper_public[DH_P256_PUBLIC_SIZE];
    if (!dh_p256_public_from_private(helper_private, helper_public)) return false;
    dh_p256_key_id(helper_public, helper_key_id);
    return true;
}

/* ----------------------------------------------------------------- fixtures */

static dh_pair pairing;

/* A board that has paired with the published helper key, holding the published
   identity. The nonce it will put in its next ack is the published one, so its
   answers can be compared with the golden frames byte for byte. */
static void a_paired_board(dh_session *s, uint8_t build_type) {
    dh_session_init(s, build_type);
    dh_session_stage_nonce(s, board_nonce);

    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);
    dh_pair_set_registration(&pairing, helper_key_id, shared_secret);
}

static void an_unpaired_board(dh_session *s) {
    dh_session_init(s, DH_BUILD_RELEASE);
    dh_session_stage_nonce(s, board_nonce);

    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);
}

/* Feed a complete frame and hand back the reply's length. */
static size_t feed(dh_session *s, const uint8_t *frame, size_t frame_len, uint32_t now_ms,
                   uint8_t *reply, size_t reply_cap) {
    dh_frame_view v;
    size_t consumed = 0;
    if (dh_frame_decode(frame, frame_len, &v, &consumed) != DH_FRAME_OK) return 0;
    size_t out_len = 0;
    if (dh_session_on_frame(s, &pairing, &v, now_ms, reply, reply_cap, &out_len) != DH_FRAME_OK)
        return 0;
    return out_len;
}

static size_t tick(dh_session *s, uint32_t now_ms, uint8_t *out, size_t out_cap) {
    size_t out_len = 0;
    if (dh_session_tick(s, now_ms, out, out_cap, &out_len) != DH_FRAME_OK) return 0;
    return out_len;
}

/*
 * Take the two frames a fresh session owes its helper off the queue, exactly
 * as a board does when it accepts them: the clipboard policy (#52) and the
 * baseline drop totals (#133). They are what any tick after a handshake
 * produces first, and the tests below are about the beat, the alert and the
 * eviction — so they settle these here once rather than each accounting for
 * frames that are not their subject.
 *
 * Both are settled at the same `now_ms`, so the idle timer they charge leaves
 * the direction idle a full interval later, as it would after any traffic.
 */
static void settle_openers(dh_session *s, uint32_t now_ms) {
    uint8_t out[MAX_BYTES];
    for (int i = 0; i < 2; i++) {
        const size_t len = tick(s, now_ms, out, sizeof out);
        /* Stop at anything else rather than ticking past it. A caller with an
           alert already pending draws that instead, and it stays owed — but
           settling blindly would encode and discard whatever the ladder put
           first, so a reordering would be swallowed here instead of failing
           in the test that cares. */
        if (len == 0 || (out[0] != DH_MSG_CLIP_POLICY && out[0] != DH_MSG_DEVICE_DROPS)) break;
        dh_session_note_owed_sent(s, out[0]);
    }
}

/* A complete authenticated frame, built the way a helper would build one. */
static size_t make_tagged(uint8_t type, const uint8_t key[DH_SESSION_KEY_SIZE], uint64_t counter,
                          const uint8_t *body, size_t body_len, uint8_t *out, size_t cap) {
    const size_t payload_len = DH_FRAME_AUTH_PREFIX_SIZE + body_len;
    if (cap < DH_FRAME_HEADER_SIZE + payload_len) return 0;

    out[0] = type;
    out[1] = 0;
    out[2] = (uint8_t)(payload_len & 0xFFu);
    out[3] = (uint8_t)(payload_len >> 8);

    size_t written = 0;
    if (dh_auth_wrap(key, type, 0, counter, body, body_len, out + DH_FRAME_HEADER_SIZE,
                     cap - DH_FRAME_HEADER_SIZE, &written) != DH_AUTH_OK)
        return 0;
    return DH_FRAME_HEADER_SIZE + written;
}

/* The body of a decoded frame — what sits behind the authentication prefix. */
static bool decoded_body(const uint8_t *frame, size_t len, dh_frame_view *view,
                         const uint8_t **body, size_t *body_len) {
    size_t consumed = 0;
    if (dh_frame_decode(frame, len, view, &consumed) != DH_FRAME_OK || consumed != len)
        return false;
    if (!dh_msg_is_authenticated(view->hdr.type)) {
        *body = view->payload;
        *body_len = view->hdr.len;
        return true;
    }
    if (view->hdr.len < DH_FRAME_AUTH_PREFIX_SIZE) return false;
    *body = view->payload + DH_FRAME_AUTH_PREFIX_SIZE;
    *body_len = (size_t)view->hdr.len - DH_FRAME_AUTH_PREFIX_SIZE;
    return true;
}

/* Does this frame carry a good tag under `key`, and the counter expected? */
static bool authenticates(const uint8_t *frame, size_t len, const uint8_t key[DH_SESSION_KEY_SIZE],
                          uint64_t expected_counter) {
    dh_frame_view v;
    size_t consumed = 0;
    if (dh_frame_decode(frame, len, &v, &consumed) != DH_FRAME_OK) return false;

    uint64_t counter = 0;
    if (!dh_auth_peek_counter(v.payload, v.hdr.len, &counter) || counter != expected_counter)
        return false;

    dh_auth_counter state;
    dh_auth_counter_init(&state);
    const uint8_t *body = NULL;
    size_t body_len = 0;
    return dh_auth_open(key, &v.hdr, v.payload, &state, &body, &body_len) == DH_AUTH_OK;
}

/* Does this frame carry a good tag under `key`, whatever its counter? Used
   where the counter depends on how much other traffic went out first. */
static bool authenticates_somehow(const uint8_t *frame, size_t len,
                                  const uint8_t key[DH_SESSION_KEY_SIZE]) {
    dh_frame_view v;
    size_t consumed = 0;
    if (dh_frame_decode(frame, len, &v, &consumed) != DH_FRAME_OK) return false;

    dh_auth_counter state;
    dh_auth_counter_init(&state);
    const uint8_t *body = NULL;
    size_t body_len = 0;
    return dh_auth_open(key, &v.hdr, v.payload, &state, &body, &body_len) == DH_AUTH_OK;
}

/* -------------------------------------------------------------- the codecs */

/*
 * Every session-band vector decodes to the fields docs/protocol.md names, and
 * re-encodes to the identical bytes. The codec *is* the wire format's
 * definition, so anything that survives this file but not this test is a
 * definition that disagrees with the gate.
 */
static void test_the_codecs_round_trip_the_golden_frames(void) {
    uint8_t out[MAX_BYTES];
    size_t out_len = 0;
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;

    const struct vector *hello_v = find("hello_mac");
    if (hello_v && decoded_body(hello_v->f[0], hello_v->len[0], &v, &body, &body_len)) {
        dh_hello h;
        CHECK(dh_hello_decode(body, body_len, &h), "hello_mac", "decode failed");
        CHECK(h.proto_version == DH_PROTO_VERSION, "hello_mac", "wrong protocol version");
        CHECK(h.os == DH_OS_MAC, "hello_mac", "wrong platform");
        CHECK(h.build_type == DH_BUILD_RELEASE, "hello_mac", "wrong build type");
        CHECK(h.channel_count == 1, "hello_mac", "wrong requested channel count");
        CHECK(h.max_chunk == 1024, "hello_mac", "wrong requested max chunk");
        CHECK(memcmp(h.helper_key_id, helper_key_id, DH_KEY_ID_SIZE) == 0, "hello_mac",
              "the key id is not the published helper key's");
        CHECK(memcmp(h.helper_nonce, helper_nonce, DH_NONCE_SIZE) == 0, "hello_mac",
              "wrong helper nonce");

        CHECK(dh_hello_encode(&h, k_hello, 0, out, sizeof out, &out_len) == DH_FRAME_OK,
              "hello_mac", "re-encode failed");
        CHECK(out_len == hello_v->len[0] && memcmp(out, hello_v->f[0], out_len) == 0, "hello_mac",
              "re-encode mismatch");
    }

    const struct vector *ack_v = find("hello_ack_ok");
    if (ack_v && decoded_body(ack_v->f[0], ack_v->len[0], &v, &body, &body_len)) {
        dh_hello_ack a;
        CHECK(dh_hello_ack_decode(body, body_len, &a), "hello_ack_ok", "decode failed");
        CHECK(a.proto_version == DH_PROTO_VERSION, "hello_ack_ok", "wrong protocol version");
        CHECK(a.channel_count == 1 && a.max_chunk == 1024, "hello_ack_ok",
              "wrong effective negotiation");
        CHECK(memcmp(a.board_nonce, board_nonce, DH_NONCE_SIZE) == 0, "hello_ack_ok",
              "wrong board nonce");

        CHECK(dh_hello_ack_encode(&a, k_b2h, 0, out, sizeof out, &out_len) == DH_FRAME_OK,
              "hello_ack_ok", "re-encode failed");
        CHECK(out_len == ack_v->len[0] && memcmp(out, ack_v->f[0], out_len) == 0, "hello_ack_ok",
              "re-encode mismatch");
    }

    /* The clipboard policy, both forms. There is no encoder to round-trip
       against — the board writes the byte inside dh_session_tick — so what is
       gated here is the decode: a flags byte read the wrong way round would
       disable the direction the user left on. */
    const struct {
        const char *name;
        uint8_t expected;
    } policies[] = {
        {"clip_policy_both", (uint8_t)(DH_CLIP_MAY_SEND | DH_CLIP_MAY_RECEIVE)},
        {"clip_policy_receive_only", (uint8_t)DH_CLIP_MAY_RECEIVE},
    };
    for (size_t i = 0; i < sizeof policies / sizeof policies[0]; i++) {
        const struct vector *p_v = find(policies[i].name);
        if (!p_v || !decoded_body(p_v->f[0], p_v->len[0], &v, &body, &body_len)) continue;
        CHECK(v.hdr.type == DH_MSG_CLIP_POLICY, policies[i].name, "wrong message type");
        uint8_t flags = 0;
        CHECK(dh_clip_policy_decode(body, body_len, &flags), policies[i].name, "decode failed");
        CHECK(flags == policies[i].expected, policies[i].name, "wrong flags");
    }

    /* The seven drop totals (#133). Decode only, like the policy above: the
       board writes them inside dh_session_tick and there is no encoder to
       round-trip against. Each value in the vector differs, so a field read
       out of order names the wrong seam — and naming the wrong seam sends
       whoever reads it to fix the wrong thing. */
    const struct vector *drops_v = find("device_drops");
    if (drops_v && decoded_body(drops_v->f[0], drops_v->len[0], &v, &body, &body_len)) {
        CHECK(v.hdr.type == DH_MSG_DEVICE_DROPS, "device_drops", "wrong message type");
        dh_device_drops d;
        CHECK(dh_device_drops_decode(body, body_len, &d), "device_drops", "decode failed");
        CHECK(d.reports == 1 && d.inbound == 2 && d.outq == 3 && d.unsent == 4 && d.orphans == 5 &&
                  d.truncated == 6 && d.relay_q == 7,
              "device_drops", "the first seven totals decoded in the wrong order");
        /* #142's two are appended, so the seven above keep their offsets and an
           older helper reading this frame loses the reading rather than the
           session. Pinned here so a later field cannot be slipped in front. */
        CHECK(d.outq_priority == 8 && d.outq_bad_header == 9, "device_drops",
              "the band split did not decode at the end of the body");
        CHECK(d.frames_in == 10 && d.reports_in == 11 && d.frames_refused == 12, "device_drops",
              "the inbound chain did not decode at the end of the body");
    }

    /* The untagged band: pairing and the two refusals. */
    const struct vector *req_v = find("pair_request");
    if (req_v && decoded_body(req_v->f[0], req_v->len[0], &v, &body, &body_len)) {
        dh_pair_request r;
        CHECK(dh_pair_request_decode(body, body_len, &r), "pair_request", "decode failed");
        CHECK(dh_pair_request_encode(&r, out, sizeof out, &out_len) == DH_FRAME_OK, "pair_request",
              "re-encode failed");
        CHECK(out_len == req_v->len[0] && memcmp(out, req_v->f[0], out_len) == 0, "pair_request",
              "re-encode mismatch");
    }

    const struct vector *grant_v = find("pair_grant");
    if (grant_v && decoded_body(grant_v->f[0], grant_v->len[0], &v, &body, &body_len)) {
        dh_pair_grant g;
        CHECK(dh_pair_grant_decode(body, body_len, &g), "pair_grant", "decode failed");
        CHECK(dh_pair_grant_encode(&g, out, sizeof out, &out_len) == DH_FRAME_OK, "pair_grant",
              "re-encode failed");
        CHECK(out_len == grant_v->len[0] && memcmp(out, grant_v->f[0], out_len) == 0, "pair_grant",
              "re-encode mismatch");
    }

    static const struct {
        const char *name;
        uint8_t reason;
    } refusals[] = {
        {"pair_refused_no_window", DH_PAIR_REFUSED_NO_WINDOW},
        {"pair_refused_registered", DH_PAIR_REFUSED_ALREADY_REGISTERED},
    };
    for (size_t i = 0; i < sizeof refusals / sizeof refusals[0]; i++) {
        const struct vector *r_v = find(refusals[i].name);
        if (!r_v || !decoded_body(r_v->f[0], r_v->len[0], &v, &body, &body_len)) continue;
        dh_pair_refused r;
        CHECK(dh_pair_refused_decode(body, body_len, &r), refusals[i].name, "decode failed");
        CHECK(r.reason == refusals[i].reason, refusals[i].name, "wrong reason");
        CHECK(dh_pair_refused_encode(&r, out, sizeof out, &out_len) == DH_FRAME_OK,
              refusals[i].name, "re-encode failed");
        CHECK(out_len == r_v->len[0] && memcmp(out, r_v->f[0], out_len) == 0, refusals[i].name,
              "re-encode mismatch");
    }

    static const struct {
        const char *name;
        uint8_t status;
    } hello_refusals[] = {
        {"hello_refused_version", DH_HELLO_REFUSED_VERSION_INCOMPATIBLE},
        {"hello_refused_unpaired", DH_HELLO_REFUSED_UNPAIRED},
    };
    for (size_t i = 0; i < sizeof hello_refusals / sizeof hello_refusals[0]; i++) {
        const struct vector *r_v = find(hello_refusals[i].name);
        if (!r_v || !decoded_body(r_v->f[0], r_v->len[0], &v, &body, &body_len)) continue;
        dh_hello_refused r;
        CHECK(dh_hello_refused_decode(body, body_len, &r), hello_refusals[i].name,
              "decode failed");
        CHECK(r.status == hello_refusals[i].status, hello_refusals[i].name, "wrong status");
        CHECK(r.proto_version == DH_PROTO_VERSION, hello_refusals[i].name,
              "the board's own version is not in the refusal");
        CHECK(dh_hello_refused_encode(&r, out, sizeof out, &out_len) == DH_FRAME_OK,
              hello_refusals[i].name, "re-encode failed");
        CHECK(out_len == r_v->len[0] && memcmp(out, r_v->f[0], out_len) == 0,
              hello_refusals[i].name, "re-encode mismatch");
    }

    /* Status 1 is reserved and must never appear on the wire: it was v1's
       auth_failed, and leaving the value as a hole is what stops a v2 status
       being misread as the message that manufactured the chord trap (#108). */
    for (size_t i = 0; i < vector_count; i++) {
        if (strncmp(vectors[i].name, "hello_refused", 13) != 0) continue;
        const struct vector *r_v = &vectors[i];
        if (!decoded_body(r_v->f[0], r_v->len[0], &v, &body, &body_len)) continue;
        dh_hello_refused r;
        if (!dh_hello_refused_decode(body, body_len, &r)) continue;
        CHECK(r.status != DH_HELLO_REFUSED_RESERVED, r_v->name,
              "a vector carries v1's auth_failed status");
    }
}

/* --------------------------------------------------------- the hello exchange */

/* The board's answer to the golden hello is the golden ack, byte for byte. */
static void test_the_board_answers_the_golden_hello(void) {
    const struct vector *hello_v = find("hello_mac");
    const struct vector *ack_v = find("hello_ack_ok");
    if (!hello_v || !ack_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    CHECK(!s.present, "hello", "a helper was present before any hello");

    uint8_t reply[DH_SESSION_REPLY_MAX];
    const size_t reply_len = feed(&s, hello_v->f[0], hello_v->len[0], 5000, reply, sizeof reply);

    CHECK(reply_len == ack_v->len[0] && memcmp(reply, ack_v->f[0], reply_len) == 0, "hello",
          "the answer to hello_mac is not hello_ack_ok");
    CHECK(s.present, "hello", "no session after a good hello");
    CHECK(s.peer_os == DH_OS_MAC, "hello", "platform not recorded");
    CHECK(s.channel_count == DH_SESSION_CHANNEL_COUNT, "hello", "channel count not negotiated");
    CHECK(s.max_chunk == 1024, "hello", "chunk size not negotiated");

    /* The session keys are the published ones, which is what makes every
       frame after this one comparable with the vectors too. */
    CHECK(memcmp(s.k_h2b, k_h2b, DH_SESSION_KEY_SIZE) == 0, "hello", "k_h2b is not the published key");
    CHECK(memcmp(s.k_b2h, k_b2h, DH_SESSION_KEY_SIZE) == 0, "hello", "k_b2h is not the published key");

    /* The nonce is spent, and the caller is told it owes another: reusing one
       would reuse every session key derived from it. */
    CHECK(dh_session_needs_nonce(&s), "hello", "the board would reuse its session nonce");
}

/*
 * A hello whose tag does not verify draws **nothing at all**.
 *
 * This is the fix for #108, measured on hardware: v1 answered
 * DH_HELLO_AUTH_FAILED, that answer reached every attached client because it is
 * an input report, and the real helper displayed "Not paired — press the config
 * chord". One frame was enough and the state was sticky. Answering is acting,
 * so there is no answer to overhear and the sequence has no first step.
 */
static void test_a_hello_that_does_not_authenticate_is_answered_with_silence(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);

    uint8_t forged[MAX_BYTES];
    memcpy(forged, hello_v->f[0], hello_v->len[0]);
    forged[DH_FRAME_HEADER_SIZE + DH_FRAME_COUNTER_SIZE] ^= 0x01u; /* one bit of the tag */

    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, forged, hello_v->len[0], 1000, reply, sizeof reply) == 0, "silence",
          "a hello with a bad tag drew an answer");
    CHECK(!s.present, "silence", "a hello with a bad tag started a session");

    /* And it is counted, as the stronger signal it is: something named the
       registered helper's key id and could not produce its tag. */
    CHECK(s.refused_in_window == 1, "silence", "a failed tag was not counted");
    CHECK(s.refused_as_registered, "silence",
          "a forged hello naming the registered key was counted as an ordinary stray frame");
}

/*
 * The two refusals that *are* sent, and why they are safe to overhear: neither
 * can be provoked into saying anything false. Both echo the caller's
 * correlation value, so a listener's refusal is one the real helper discards.
 */
static void test_the_two_refusals_echo_the_callers_correlation(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    if (!decoded_body(hello_v->f[0], hello_v->len[0], &v, &body, &body_len)) return;
    dh_hello golden;
    if (!dh_hello_decode(body, body_len, &golden)) return;

    uint8_t frame[MAX_BYTES];
    uint8_t reply[DH_SESSION_REPLY_MAX];
    uint8_t encoded[DH_HELLO_LEN];

    /* 1. A version this board does not implement. First, because a board
          cannot verify a tag under rules it does not know — so this one is
          answered even though its tag was never checked. */
    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);

    dh_hello wrong_version = golden;
    wrong_version.proto_version = DH_PROTO_VERSION + 1u;
    wrong_version.correlation = 0x0102030405060708ull;
    size_t len = 0;
    CHECK(dh_hello_encode(&wrong_version, k_hello, 0, frame, sizeof frame, &len) == DH_FRAME_OK,
          "refusal", "encode failed");

    size_t reply_len = feed(&s, frame, len, 1000, reply, sizeof reply);
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "refusal",
          "a version mismatch drew nothing");
    CHECK(v.hdr.type == DH_MSG_HELLO_REFUSED, "refusal", "the reply is not a hello_refused");
    dh_hello_refused refused;
    CHECK(dh_hello_refused_decode(body, body_len, &refused), "refusal", "refusal decode failed");
    CHECK(refused.status == DH_HELLO_REFUSED_VERSION_INCOMPATIBLE, "refusal",
          "a version mismatch was not reported as one");
    CHECK(refused.correlation == wrong_version.correlation, "refusal",
          "the refusal does not echo the caller's correlation value");
    CHECK(!s.present, "refusal", "an incompatible helper was admitted to a session");

    /* 2. A board with no registration. There is no secret to prove and the
          honest remedy really is the config chord. */
    an_unpaired_board(&s);
    dh_hello unpaired = golden;
    unpaired.correlation = 0x1122334455667788ull;
    CHECK(dh_hello_encode(&unpaired, k_hello, 0, frame, sizeof frame, &len) == DH_FRAME_OK,
          "refusal", "encode failed");

    reply_len = feed(&s, frame, len, 1000, reply, sizeof reply);
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "refusal",
          "an unpaired board drew nothing");
    CHECK(v.hdr.type == DH_MSG_HELLO_REFUSED, "refusal", "the reply is not a hello_refused");
    CHECK(dh_hello_refused_decode(body, body_len, &refused), "refusal", "refusal decode failed");
    CHECK(refused.status == DH_HELLO_REFUSED_UNPAIRED, "refusal",
          "an unpaired board did not say so");
    CHECK(refused.correlation == unpaired.correlation, "refusal",
          "the refusal does not echo the caller's correlation value");
    CHECK(!s.present, "refusal", "an unpaired board started a session");

    /* Neither refusal is liveness: both are untagged, so anything attached to
       the channel can provoke one, and a frame nothing proved must not hold a
       session open. */
    CHECK(s.last_seen_ms == 0, "refusal", "a refused hello refreshed a liveness deadline");
    (void)encoded;
}

/*
 * Registered — but for somebody else. The board says `unpaired`, because per
 * key id is what the question means (#117).
 *
 * "Does this board hold a registration?" and "does it hold one for the key this
 * hello names?" differ exactly when a helper's identity changed underneath it:
 * a cleared Application Support, an enclave blob that will not decode on a new
 * machine (#112), a home directory restored onto a second Mac, or the board
 * moved to a computer the first one had already registered. Board-wide, all of
 * those fell through to the tag check, failed it, and drew silence — and
 * silence takes the helper round the reconnect loop rather than to `notPaired`,
 * the one state that sends a PAIR_REQUEST. The chord then had nothing to
 * provision, against ADR-0008's "recovery is one chord press".
 */
static void test_a_registration_for_someone_else_is_refused_as_unpaired(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    if (!decoded_body(hello_v->f[0], hello_v->len[0], &v, &body, &body_len)) return;
    dh_hello golden;
    if (!dh_hello_decode(body, body_len, &golden)) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);

    /* A second helper, holding neither the registered key nor the secret one
       ECDH against it produced — so its hello is tagged under a key of its own
       and could never pass the board's tag check. */
    dh_hello stranger = golden;
    stranger.correlation = 0x99AABBCCDDEEFF00ull;
    for (size_t i = 0; i < DH_KEY_ID_SIZE; i++) stranger.helper_key_id[i] = (uint8_t)(0xA0u + i);

    uint8_t foreign_k_hello[DH_SESSION_KEY_SIZE];
    for (size_t i = 0; i < sizeof foreign_k_hello; i++) foreign_k_hello[i] = (uint8_t)(0x5Au ^ i);

    uint8_t frame[MAX_BYTES];
    size_t len = 0;
    CHECK(dh_hello_encode(&stranger, foreign_k_hello, 0, frame, sizeof frame, &len) == DH_FRAME_OK,
          "other key", "encode failed");

    uint8_t reply[DH_SESSION_REPLY_MAX];
    const size_t reply_len = feed(&s, frame, len, 1000, reply, sizeof reply);

    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "other key",
          "a board registered to somebody else drew nothing, so the chord cannot rescue the helper");
    CHECK(v.hdr.type == DH_MSG_HELLO_REFUSED, "other key", "the reply is not a hello_refused");

    dh_hello_refused refused;
    CHECK(dh_hello_refused_decode(body, body_len, &refused), "other key", "refusal decode failed");
    CHECK(refused.status == DH_HELLO_REFUSED_UNPAIRED, "other key",
          "a board that does not know this key did not say unpaired");
    CHECK(refused.correlation == stranger.correlation, "other key",
          "the refusal does not echo the caller's correlation value");
    CHECK(!s.present, "other key", "an unknown helper was admitted to a session");
    CHECK(s.last_seen_ms == 0, "other key", "a refused hello refreshed a liveness deadline");

    /* Not a listener signal, for the same reason the board-wide refusal never
       was one: this is the honest recovery path, and an unpaired helper
       retrying it must not manufacture an alert about itself. The signal that
       matters — a hello naming the *registered* key id and failing its tag —
       is untouched, because only those reach the tag check now. */
    CHECK(s.refused_in_window == 0, "other key",
          "an honest unpaired helper was counted towards the listener alert");
    CHECK(!s.refused_as_registered, "other key",
          "a hello naming an unregistered key was counted as an impersonation");
}

/*
 * A v1 hello is answered, and answered with the version refusal.
 *
 * This is the shape the *shipped* macOS helper sends until #112, so it is the
 * one wrong hello a v2 board is certain to receive. Its payload is 23 bytes —
 * 7 fixed plus a 16-byte bearer token — where a v2 hello's is 63, so a board
 * that gated on "long enough to hold an authentication prefix" before deciding
 * the version would answer it with nothing at all. Silence is what v2 reserves
 * for a *failed tag*, and it would be the wrong thing to say here: the tag was
 * never the problem and the honest remedy is a newer helper.
 */
static void test_a_v1_hello_is_refused_on_the_version_not_answered_with_silence(void) {
    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);

    /* The v1 hello_mac golden frame, frozen at the commit that moved the board
       to v2 — the same bytes tools/macos-checks/probe_manufactured_chord_trap
       and the helper's BindingTests carry. */
    static const uint8_t v1_hello[] = {
        0x01, 0x00, 0x17, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04,
        0xEF, 0xBE, 0xAD, 0xDE, 0xEF, 0xBE, 0xAD, 0xDE,
        0xEF, 0xBE, 0xAD, 0xDE, 0xEF, 0xBE, 0xAD, 0xDE,
    };

    uint8_t reply[DH_SESSION_REPLY_MAX];
    const size_t reply_len = feed(&s, v1_hello, sizeof v1_hello, 1000, reply, sizeof reply);

    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "v1 hello",
          "a v1 hello drew no answer at all");
    CHECK(v.hdr.type == DH_MSG_HELLO_REFUSED, "v1 hello", "the reply is not a hello_refused");

    dh_hello_refused refused;
    CHECK(dh_hello_refused_decode(body, body_len, &refused), "v1 hello", "refusal decode failed");
    CHECK(refused.status == DH_HELLO_REFUSED_VERSION_INCOMPATIBLE, "v1 hello",
          "a v1 hello was not refused on the version");
    CHECK(refused.proto_version == DH_PROTO_VERSION, "v1 hello",
          "the refusal does not carry the board's own version");
    /* Nobody's correlation value, because a frame this build cannot parse has
       none to echo — and a helper acts only on an answer carrying its own. */
    CHECK(refused.correlation == 0, "v1 hello", "the refusal echoes a correlation it never read");
    CHECK(!s.present, "v1 hello", "a v1 hello started a session");
    CHECK(s.refused_in_window == 0, "v1 hello",
          "an old helper was counted towards the listener alert");
}

/* A helper's ack carries the effective values, clamped to what the board has. */
static void test_negotiation_clamps_to_what_the_board_has(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    if (!decoded_body(hello_v->f[0], hello_v->len[0], &v, &body, &body_len)) return;
    dh_hello golden;
    if (!dh_hello_decode(body, body_len, &golden)) return;

    static const struct {
        const char *what;
        uint8_t channels;
        uint16_t chunk;
        uint8_t want_channels;
        uint16_t want_chunk;
    } cases[] = {
        {"over-asking is clamped, not refused", 3, 4096, DH_SESSION_CHANNEL_COUNT,
         DH_SESSION_MAX_CHUNK},
        {"asking for less is honoured, not raised", 1, 512, 1, 512},
        {"asking for nothing is floored to a usable session", 0, 0, 1, DH_SESSION_MIN_CHUNK},
    };

    uint8_t frame[MAX_BYTES];
    uint8_t reply[DH_SESSION_REPLY_MAX];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dh_session s;
        a_paired_board(&s, DH_BUILD_RELEASE);

        dh_hello h = golden;
        h.channel_count = cases[i].channels;
        h.max_chunk = cases[i].chunk;
        size_t len = 0;
        CHECK(dh_hello_encode(&h, k_hello, 0, frame, sizeof frame, &len) == DH_FRAME_OK,
              "negotiate", "encode failed");

        const size_t reply_len = feed(&s, frame, len, 1000, reply, sizeof reply);
        CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), cases[i].what, "no ack");
        dh_hello_ack a;
        CHECK(dh_hello_ack_decode(body, body_len, &a), cases[i].what, "ack decode failed");
        CHECK(a.channel_count == cases[i].want_channels, cases[i].what, "wrong channel count");
        CHECK(a.max_chunk == cases[i].want_chunk, cases[i].what, "wrong chunk size");
    }

    /* The ceiling docs/protocol.md puts on the negotiated chunk holds whatever
       a helper asks for, because a sealed chunk spends 64 bytes of the payload
       maximum on overhead. */
    CHECK(DH_SESSION_MAX_CHUNK <= DH_SESSION_CHUNK_CEILING, "negotiate",
          "the board offers a chunk that leaves no room for a sealed frame");
}

/* A development build compiles the board's checks out (#44), and says so. */
static void test_a_development_build_needs_no_registration(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    dh_session_init(&s, DH_BUILD_DEVELOPMENT);
    dh_session_stage_nonce(&s, board_nonce);
    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);

    uint8_t reply[DH_SESSION_REPLY_MAX];
    const size_t reply_len = feed(&s, hello_v->f[0], hello_v->len[0], 1000, reply, sizeof reply);

    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "dev", "no answer");
    CHECK(v.hdr.type == DH_MSG_HELLO_ACK, "dev", "a development build refused an unpaired helper");
    dh_hello_ack a;
    CHECK(dh_hello_ack_decode(body, body_len, &a), "dev", "ack decode failed");
    CHECK(a.build_type == DH_BUILD_DEVELOPMENT, "dev",
          "a development build did not identify itself");
    CHECK(s.present, "dev", "a development build did not start a session");
}

/* ------------------------------------------------------ per-frame authentication */

/*
 * The acceptance criterion, and the one that would fail on v1's code: a board
 * holding a live authenticated session still refuses a bulk frame that does
 * not carry a good tag. v1 gated relay on dh_session_may_relay — one flag for
 * the whole board — so any process writing into the shared endpoint could push
 * bulk into a session it never authenticated (#34, #95).
 */
static void test_an_unauthenticated_bulk_frame_is_not_relayed(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], 1000, reply, sizeof reply) > 0, "per frame",
          "no session to begin with");
    CHECK(s.present, "per frame", "no session to begin with");

    const uint8_t chunk_body[12] = {1, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD};

    /* The real helper's frame is relayed. */
    uint8_t good[MAX_BYTES];
    const size_t good_len =
        make_tagged(DH_MSG_CLIP_CHUNK, k_h2b, 0, chunk_body, sizeof chunk_body, good, sizeof good);
    dh_frame_view v;
    size_t consumed = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(dh_frame_decode(good, good_len, &v, &consumed) == DH_FRAME_OK, "per frame",
          "the test frame is malformed");
    CHECK(dh_session_authenticate(&s, &v, 1010, &body, &body_len) == DH_AUTH_OK, "per frame",
          "an authenticated bulk frame was refused");
    CHECK(body_len == sizeof chunk_body && memcmp(body, chunk_body, body_len) == 0, "per frame",
          "the relayed body is not what the helper sent");

    /* Anything else's is not — the session is live throughout. */
    uint8_t forged[MAX_BYTES];
    memcpy(forged, good, good_len);
    forged[DH_FRAME_HEADER_SIZE + DH_FRAME_COUNTER_SIZE + 3] ^= 0x40u; /* the tag */
    CHECK(dh_frame_decode(forged, good_len, &v, &consumed) == DH_FRAME_OK, "per frame",
          "the forged frame is malformed");
    CHECK(dh_session_authenticate(&s, &v, 1020, &body, &body_len) == DH_AUTH_ERR_TAG, "per frame",
          "a bulk frame with a bad tag was relayed into a live session");
    CHECK(s.present, "per frame", "refusing one frame ended the session");
    CHECK(s.refused_in_window >= 1, "per frame", "a refused bulk frame was not counted");
}

/* Replay of a captured frame is refused: the counter must be strictly greater
   than the highest accepted under that key. */
static void test_a_replayed_frame_is_refused(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], 1000, reply, sizeof reply) > 0, "replay",
          "no session to begin with");

    const uint8_t chunk_body[12] = {0};
    uint8_t captured[MAX_BYTES];
    const size_t captured_len = make_tagged(DH_MSG_CLIP_CHUNK, k_h2b, 0, chunk_body,
                                            sizeof chunk_body, captured, sizeof captured);

    dh_frame_view v;
    size_t consumed = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(dh_frame_decode(captured, captured_len, &v, &consumed) == DH_FRAME_OK, "replay",
          "the captured frame is malformed");
    CHECK(dh_session_authenticate(&s, &v, 1010, &body, &body_len) == DH_AUTH_OK, "replay",
          "the first copy was refused");

    /* Byte-identical, tag and all — which is exactly what a listener records
       and re-sends. */
    CHECK(dh_frame_decode(captured, captured_len, &v, &consumed) == DH_FRAME_OK, "replay",
          "the replayed frame is malformed");
    CHECK(dh_session_authenticate(&s, &v, 1020, &body, &body_len) == DH_AUTH_ERR_COUNTER, "replay",
          "a byte-identical replay was accepted");

    /* A gap is not a replay: the board's outbound path is a short bounded
       queue and a frame it cannot take is a silent loss, so gaps happen in
       normal operation and are not evidence of an attack. */
    uint8_t later[MAX_BYTES];
    const size_t later_len =
        make_tagged(DH_MSG_CLIP_CHUNK, k_h2b, 7, chunk_body, sizeof chunk_body, later, sizeof later);
    CHECK(dh_frame_decode(later, later_len, &v, &consumed) == DH_FRAME_OK, "replay", "malformed");
    CHECK(dh_session_authenticate(&s, &v, 1030, &body, &body_len) == DH_AUTH_OK, "replay",
          "a counter gap was refused as a replay");
}

/*
 * A frame arriving from the peer board is tagged under *this* board's key with
 * *this* board's counter. The tag is per hop: the far helper's means nothing
 * here, so what crosses the inter-board link carries no prefix at all.
 */
static void test_a_relayed_frame_is_tagged_for_this_boards_helper(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);

    /* No session, no key to tag under — and nobody to send it to. */
    const uint8_t chunk_body[12] = {2, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
    uint8_t bare[MAX_BYTES];
    size_t bare_len = 0;
    CHECK(dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, chunk_body, sizeof chunk_body, bare, sizeof bare,
                          &bare_len) == DH_FRAME_OK,
          "re-tag", "encode failed");

    dh_frame_view v;
    size_t consumed = 0;
    CHECK(dh_frame_decode(bare, bare_len, &v, &consumed) == DH_FRAME_OK, "re-tag", "malformed");

    uint8_t out[MAX_BYTES];
    size_t out_len = 0;
    CHECK(dh_session_emit_relayed(&s, &v, out, sizeof out, &out_len) != DH_FRAME_OK, "re-tag",
          "a frame was relayed to a helper with no session");

    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], 1000, reply, sizeof reply) > 0, "re-tag",
          "no session");

    /* The ack was counter 0, so the first relayed frame is counter 1 and the
       next is 2 — one space per key, shared by everything this board emits. */
    CHECK(dh_session_emit_relayed(&s, &v, out, sizeof out, &out_len) == DH_FRAME_OK, "re-tag",
          "a relayed frame was not tagged");
    CHECK(authenticates(out, out_len, k_b2h, 1), "re-tag",
          "the relayed frame does not authenticate under k_b2h at counter 1");

    const uint8_t *body = NULL;
    size_t body_len = 0;
    dh_frame_view tagged;
    CHECK(decoded_body(out, out_len, &tagged, &body, &body_len), "re-tag", "malformed");
    CHECK(tagged.hdr.type == DH_MSG_CLIP_CHUNK, "re-tag", "the relayed type changed");
    CHECK(body_len == sizeof chunk_body && memcmp(body, chunk_body, body_len) == 0, "re-tag",
          "the relayed payload changed");

    CHECK(dh_session_emit_relayed(&s, &v, out, sizeof out, &out_len) == DH_FRAME_OK, "re-tag",
          "a second relayed frame was refused");
    CHECK(authenticates(out, out_len, k_b2h, 2), "re-tag", "the counter did not advance");
}

/* ------------------------------------------------------------------ liveness */

/*
 * Liveness is carried by traffic that **authenticates**. Under v1 the deadline
 * measured "something is writing", and on a shared endpoint a second writer
 * would hold the board's view of the helper alive after the real helper had
 * stopped (#95).
 */
static void test_only_authenticated_traffic_is_liveness(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 100000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "liveness",
          "no session");
    settle_openers(&s, now);

    /* A helper's beat, properly tagged, keeps the session indefinitely — and
       is not answered. The board's own beat is idle-gated, so a direction kept
       busy by note_sent emits none of its own. */
    uint64_t counter = 0;
    for (int i = 0; i < 10; i++) {
        now += DH_SESSION_HEARTBEAT_MS;
        dh_session_note_sent(&s, now);

        uint8_t beat[MAX_BYTES];
        const size_t beat_len =
            make_tagged(DH_MSG_HEARTBEAT, k_h2b, counter++, NULL, 0, beat, sizeof beat);
        CHECK(tick(&s, now, reply, sizeof reply) == 0, "liveness",
              "marked absent, or beat into a direction that was not idle");
        CHECK(feed(&s, beat, beat_len, now, reply, sizeof reply) == 0, "liveness",
              "a heartbeat drew a reply");
        CHECK(s.present, "liveness", "not present while beating");
        CHECK(s.last_seen_ms == now, "liveness", "an authenticated beat was not liveness");
    }

    /* A beat that does not authenticate is not a sign of life. This is the
       whole of v2's narrowing, and it is the case v1 could not express. */
    const uint32_t honest = s.last_seen_ms;
    now += DH_SESSION_HEARTBEAT_MS;
    uint8_t forged[MAX_BYTES];
    size_t forged_len = make_tagged(DH_MSG_HEARTBEAT, k_h2b, counter, NULL, 0, forged, sizeof forged);
    forged[DH_FRAME_HEADER_SIZE + DH_FRAME_COUNTER_SIZE] ^= 0x80u;
    CHECK(feed(&s, forged, forged_len, now, reply, sizeof reply) == 0, "liveness",
          "a forged beat drew a reply");
    CHECK(s.last_seen_ms == honest, "liveness",
          "a frame that did not authenticate held the session alive");

    /* And a board→helper type arriving from the helper is not liveness either:
       a listener writing into the shared endpoint can send any of them. */
    now += DH_SESSION_HEARTBEAT_MS;
    uint8_t wrong_way[MAX_BYTES];
    const size_t wrong_len =
        make_tagged(DH_MSG_DEVICE_HEARTBEAT, k_b2h, 99, NULL, 0, wrong_way, sizeof wrong_way);
    CHECK(feed(&s, wrong_way, wrong_len, now, reply, sizeof reply) == 0, "liveness",
          "a board-to-helper frame drew a reply");
    CHECK(s.last_seen_ms == honest, "liveness",
          "a frame this layer cannot act on refreshed the deadline");

    /* Silence still ends it, and the eviction *is* the frame. */
    now = honest + DH_SESSION_ABSENT_MS;
    const size_t end_len = tick(&s, now, reply, sizeof reply);
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(decoded_body(reply, end_len, &v, &body, &body_len), "liveness",
          "the timeout announced nothing");
    CHECK(v.hdr.type == DH_MSG_SESSION_END && body_len == 1 &&
              body[0] == DH_SESSION_END_LIVENESS_TIMEOUT,
          "liveness", "the timeout did not announce the eviction");
    CHECK(!s.present, "liveness", "still present after the timeout");
    CHECK(tick(&s, now + DH_SESSION_HEARTBEAT_MS, reply, sizeof reply) == 0, "liveness",
          "absence announced twice, or a beat without a session");
}

/* The board's beat fills an idle direction and nothing else, and it is tagged
   like everything else it sends. */
static void test_the_board_beats_only_into_an_idle_direction(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 500000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "idle",
          "no session");
    settle_openers(&s, now);

    /* The ack is itself traffic, so the direction is not idle yet. */
    now += DH_SESSION_HEARTBEAT_MS - 1;
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle", "beat before the direction was idle");

    now += 1;
    const size_t beat_len = tick(&s, now, reply, sizeof reply);
    CHECK(beat_len > 0, "idle", "an idle interval drew no beat");
    /* Counter 3, not 1: the ack spent 0, the clipboard policy this session owed
       its helper (#52) spent 1, and the baseline drop totals (#133) spent 2. */
    CHECK(authenticates(reply, beat_len, k_b2h, 3), "idle",
          "the board's beat is not tagged under k_b2h at the next counter");
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle", "beat twice in one interval");

    /* No session, no beat — its absence is what makes the helper's timeout
       mean something. */
    dh_session_drop(&s);
    now += DH_SESSION_HEARTBEAT_MS * 4;
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle", "beat with no session to keep alive");
}

static void test_liveness_survives_the_clock_wrapping(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint8_t reply[DH_SESSION_REPLY_MAX];

    const uint32_t before_wrap = UINT32_MAX - (DH_SESSION_HEARTBEAT_MS / 2);
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], before_wrap, reply, sizeof reply) > 0, "wrap",
          "no session");
    settle_openers(&s, before_wrap);

    const uint32_t after_wrap = before_wrap + DH_SESSION_HEARTBEAT_MS; /* wraps */
    CHECK(tick(&s, after_wrap, reply, sizeof reply) > 0, "wrap",
          "the idle timer did not survive the wrap");
    CHECK(s.present, "wrap", "marked absent across the wrap");

    /* A beat is not an eviction, so the check is on what the frame *is*
       rather than on whether one was emitted at all. */
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    size_t len = tick(&s, before_wrap + DH_SESSION_ABSENT_MS - 1, reply, sizeof reply);
    CHECK(len == 0 || (decoded_body(reply, len, &v, &body, &body_len) &&
                       v.hdr.type != DH_MSG_SESSION_END),
          "wrap", "marked absent early across the wrap");
    CHECK(s.present, "wrap", "marked absent early across the wrap");

    len = tick(&s, before_wrap + DH_SESSION_ABSENT_MS + 1, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len) && v.hdr.type == DH_MSG_SESSION_END,
          "wrap", "not marked absent across the wrap");
    CHECK(!s.present, "wrap", "the session outlived its deadline across the wrap");
}

/* ----------------------------------------------------------- listener alert */

/*
 * A rate, not an event — the shape #94 established for `reconnectingRepeatedly`,
 * because no single refused frame can say what a rate says. The threshold is a
 * firmware constant, not a wire-format fact, so the wire carries both the
 * window and the count.
 */
/*
 * Run one listener window with `refusals` forged frames in it, keeping the
 * session alive with the helper's own beats — which is the realistic shape: a
 * listener writes junk into the shared endpoint while the helper it is sitting
 * beside works normally. Hands back whatever the board emitted once the window
 * closed.
 */
static size_t measure_a_window(dh_session *s, uint32_t *now, uint64_t *beat_counter,
                               unsigned refusals, uint8_t *out, size_t cap) {
    const uint8_t chunk_body[12] = {0};
    uint8_t forged[MAX_BYTES];
    const size_t forged_len = make_tagged(DH_MSG_CLIP_CHUNK, k_h2b, 1, chunk_body,
                                          sizeof chunk_body, forged, sizeof forged);
    forged[DH_FRAME_HEADER_SIZE + DH_FRAME_COUNTER_SIZE] ^= 0x20u;

    dh_frame_view v;
    size_t consumed = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    for (unsigned i = 0; i < refusals; i++) {
        CHECK(dh_frame_decode(forged, forged_len, &v, &consumed) == DH_FRAME_OK, "window",
              "the forged frame is malformed");
        (void)dh_session_authenticate(s, &v, *now, &body, &body_len);
    }

    /* Stops one step short of the window, so every tick inside the loop is one
       the alert must not fire on. */
    const uint32_t started = *now;
    while ((uint32_t)((*now + DH_SESSION_HEARTBEAT_MS) - started) < DH_LISTENER_WINDOW_MS) {
        *now += DH_SESSION_HEARTBEAT_MS;

        uint8_t beat[MAX_BYTES];
        const size_t beat_len =
            make_tagged(DH_MSG_HEARTBEAT, k_h2b, (*beat_counter)++, NULL, 0, beat, sizeof beat);
        CHECK(feed(s, beat, beat_len, *now, out, cap) == 0, "window",
              "a heartbeat drew a reply");

        /* Keep the board's own beat out of the way, so anything a tick emits
           is the alert and not a heartbeat. */
        dh_session_note_sent(s, *now);
        CHECK(tick(s, *now, out, cap) == 0, "window",
              "something was sent before the window it measures had closed");
    }

    *now = started + DH_LISTENER_WINDOW_MS;
    uint8_t last_beat[MAX_BYTES];
    const size_t last_len = make_tagged(DH_MSG_HEARTBEAT, k_h2b, (*beat_counter)++, NULL, 0,
                                        last_beat, sizeof last_beat);
    CHECK(feed(s, last_beat, last_len, *now, out, cap) == 0, "window",
          "a heartbeat drew a reply");
    dh_session_note_sent(s, *now);
    return tick(s, *now, out, cap);
}

/*
 * A rate, not an event — the shape #94 established for `reconnectingRepeatedly`,
 * because no single refused frame can say what a rate says. The threshold is a
 * firmware constant, not a wire-format fact, so the wire carries both the
 * window and the count.
 */
static void test_a_rate_of_refused_frames_is_reported_to_the_helper(void) {
    const struct vector *hello_v = find("hello_mac");
    const struct vector *alert_v = find("listener_alert");
    if (!hello_v || !alert_v) return;

    /* The published vector is this firmware's own numbers: 4 refused frames in
       a 10 s window. A vector that drifted from the constants would gate
       nothing. */
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    if (decoded_body(alert_v->f[0], alert_v->len[0], &v, &body, &body_len)) {
        dh_listener_alert golden;
        CHECK(dh_listener_alert_decode(body, body_len, &golden), "alert", "decode failed");
        CHECK(golden.window_ms == DH_LISTENER_WINDOW_MS, "alert",
              "the golden window is not this firmware's");
        CHECK(golden.refused == DH_LISTENER_THRESHOLD, "alert",
              "the golden count is not this firmware's threshold");
    }

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 200000;
    uint64_t beats = 0;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "alert",
          "no session");
    settle_openers(&s, now);

    const size_t alert_len =
        measure_a_window(&s, &now, &beats, DH_LISTENER_THRESHOLD, reply, sizeof reply);
    CHECK(decoded_body(reply, alert_len, &v, &body, &body_len), "alert", "no alert was sent");
    CHECK(v.hdr.type == DH_MSG_LISTENER_ALERT, "alert", "the reply is not a listener alert");
    dh_listener_alert reported;
    CHECK(dh_listener_alert_decode(body, body_len, &reported), "alert", "alert decode failed");
    CHECK(reported.window_ms == DH_LISTENER_WINDOW_MS, "alert", "the alert names the wrong window");
    CHECK(reported.refused == DH_LISTENER_THRESHOLD, "alert", "the alert counts the wrong number");
    CHECK(authenticates_somehow(reply, alert_len, k_b2h), "alert", "the alert is not tagged");
    CHECK(s.present, "alert", "reporting a listener ended the session");

    /*
     * The measurement is not released by encoding it. The outbound queue's
     * priority band holds one frame, and the tick that first has a session to
     * report to is the one right after the HELLO_ACK went into that band — so
     * the copy this tick produced is exactly the one likely to be refused.
     * Nothing follows an alert, so a refused copy that had already been marked
     * sent would be a measurement destroyed.
     */
    now += DH_SESSION_HEARTBEAT_MS;
    dh_session_note_sent(&s, now);
    const size_t again = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, again, &v, &body, &body_len) &&
              v.hdr.type == DH_MSG_LISTENER_ALERT,
          "alert", "an alert the queue refused was thrown away");
    CHECK(dh_listener_alert_decode(body, body_len, &reported) &&
              reported.refused == DH_LISTENER_THRESHOLD,
          "alert", "the retried alert lost the count it was measured with");

    /* Once something has taken it, it is reported once and not again. */
    dh_session_note_owed_sent(&s, DH_MSG_LISTENER_ALERT);
    now += DH_SESSION_HEARTBEAT_MS;
    dh_session_note_sent(&s, now);
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "alert", "the alert was reported twice");
}

/*
 * Below the threshold a window says nothing. A single corrupt report — a bus
 * glitch, a helper restarted mid-frame — is not a listener, and an alert that
 * fired on one would be the sort of alarm a user learns to ignore.
 */
static void test_a_count_below_the_threshold_is_not_reported(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 400000;
    uint64_t beats = 0;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "threshold",
          "no session");
    settle_openers(&s, now);

    CHECK(measure_a_window(&s, &now, &beats, DH_LISTENER_THRESHOLD - 1u, reply, sizeof reply) == 0,
          "threshold", "a count below the threshold was reported");
    CHECK(s.present, "threshold", "the session did not survive the window");
}

/*
 * A helper still beating into a session the board has already evicted is the
 * most ordinary event on this channel — #107 measured 586 teardowns in sixteen
 * hours — and it must not look like a listener. Frames that arrive with no
 * session fail for a reason that is not authentication, and are not counted.
 */
static void test_an_ordinary_liveness_timeout_is_not_a_listener(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 600000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "timeout",
          "no session");
    settle_openers(&s, now);

    /* The board evicts for silence. */
    now += DH_SESSION_ABSENT_MS;
    CHECK(tick(&s, now, reply, sizeof reply) > 0, "timeout", "the session was not evicted");
    CHECK(!s.present, "timeout", "still present after the timeout");

    /* The helper has not found out yet and goes on beating, properly tagged. */
    dh_frame_view v;
    size_t consumed = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    for (unsigned i = 0; i < DH_LISTENER_THRESHOLD * 2u; i++) {
        now += DH_SESSION_HEARTBEAT_MS;
        uint8_t beat[MAX_BYTES];
        const size_t beat_len = make_tagged(DH_MSG_HEARTBEAT, k_h2b, i, NULL, 0, beat, sizeof beat);
        CHECK(dh_frame_decode(beat, beat_len, &v, &consumed) == DH_FRAME_OK, "timeout",
              "malformed");
        (void)dh_session_authenticate(&s, &v, now, &body, &body_len);
    }

    CHECK(s.refused_in_window == 0, "timeout",
          "an ordinary liveness timeout was counted as a listener");
    CHECK(!s.alert_pending, "timeout", "a liveness timeout raised a listener alert");
}

/*
 * A board with no session has nobody to tell, so it keeps what it measured and
 * reports it on the next session that authenticates, carrying the window it
 * was measured over. A purely passive listener stays undetectable — ADR-0008
 * says so, and that is accepted.
 */
static void test_a_measurement_outlives_the_session_it_was_taken_in(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 800000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "pending",
          "no session");
    settle_openers(&s, now);

    /* Enough refusals to trip, then the session dies before the window it was
       measured in has closed. */
    const uint8_t chunk_body[12] = {0};
    uint8_t forged[MAX_BYTES];
    const size_t forged_len = make_tagged(DH_MSG_CLIP_CHUNK, k_h2b, 1, chunk_body,
                                          sizeof chunk_body, forged, sizeof forged);
    forged[DH_FRAME_HEADER_SIZE + DH_FRAME_COUNTER_SIZE] ^= 0x20u;

    dh_frame_view v;
    size_t consumed = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    for (unsigned i = 0; i < DH_LISTENER_THRESHOLD; i++) {
        CHECK(dh_frame_decode(forged, forged_len, &v, &consumed) == DH_FRAME_OK, "pending",
              "malformed");
        (void)dh_session_authenticate(&s, &v, now, &body, &body_len);
    }

    now += DH_SESSION_ABSENT_MS;
    CHECK(tick(&s, now, reply, sizeof reply) > 0, "pending", "the session was not evicted");
    CHECK(!s.present, "pending", "still present after the timeout");

    /* The window closes with nobody to tell. */
    now += DH_LISTENER_WINDOW_MS;
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "pending",
          "an alert was sent with nobody to tell");
    CHECK(s.alert_pending, "pending", "the measurement was thrown away with the session");

    /* The helper reconnects, and is told about the window measured before it
       got here. */
    dh_session_stage_nonce(&s, board_nonce);
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "pending",
          "no new session");
    settle_openers(&s, now);
    const size_t alert_len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, alert_len, &v, &body, &body_len), "pending",
          "the pending alert was never sent");
    CHECK(v.hdr.type == DH_MSG_LISTENER_ALERT, "pending", "the reply is not a listener alert");
    dh_listener_alert reported;
    CHECK(dh_listener_alert_decode(body, body_len, &reported), "pending", "decode failed");
    CHECK(reported.refused == DH_LISTENER_THRESHOLD, "pending",
          "the alert lost the count it was measured with");

    dh_session_note_owed_sent(&s, DH_MSG_LISTENER_ALERT);
    dh_session_note_sent(&s, now);

    /* The alert outranks the baseline drop totals, so they are still owed
       behind it — settled here so the silence below is about the alert. */
    settle_openers(&s, now);
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "pending", "the alert was reported twice");
}

/* ------------------------------------------------------------------ pairing */

/*
 * The pairing exchange, end to end: only public halves cross, the answer
 * echoes the caller's correlation value, and the first registration closes the
 * window.
 */
static void test_pairing_hands_over_a_public_key_and_closes_the_window(void) {
    const struct vector *req_v = find("pair_request");
    if (!req_v) return;

    dh_session s;
    an_unpaired_board(&s);

    uint8_t reply[DH_SESSION_REPLY_MAX];
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;

    /* Outside a window, the asker is told there is no window rather than left
       waiting on an answer that is not coming. */
    size_t reply_len = feed(&s, req_v->f[0], req_v->len[0], 1000, reply, sizeof reply);
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "pairing",
          "a request outside a window drew nothing");
    CHECK(v.hdr.type == DH_MSG_PAIR_REFUSED, "pairing", "the reply is not a pair_refused");
    dh_pair_refused refused;
    CHECK(dh_pair_refused_decode(body, body_len, &refused), "pairing", "decode failed");
    CHECK(refused.reason == DH_PAIR_REFUSED_NO_WINDOW, "pairing", "the wrong reason was given");

    /* The chord. */
    dh_pair_open_window(&pairing, 2000);
    reply_len = feed(&s, req_v->f[0], req_v->len[0], 2000, reply, sizeof reply);
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "pairing", "no grant");
    CHECK(v.hdr.type == DH_MSG_PAIR_GRANT, "pairing", "the reply is not a pair_grant");

    dh_pair_grant grant;
    CHECK(dh_pair_grant_decode(body, body_len, &grant), "pairing", "grant decode failed");

    dh_pair_request asked;
    const uint8_t *req_body = NULL;
    size_t req_body_len = 0;
    dh_frame_view req_view;
    CHECK(decoded_body(req_v->f[0], req_v->len[0], &req_view, &req_body, &req_body_len), "pairing",
          "the request vector is malformed");
    CHECK(dh_pair_request_decode(req_body, req_body_len, &asked), "pairing",
          "the request vector will not decode");
    CHECK(grant.correlation == asked.correlation, "pairing",
          "the grant does not echo the caller's correlation value");
    CHECK(memcmp(grant.board_public, dh_pair_public_key(&pairing), DH_P256_PUBLIC_SIZE) == 0,
          "pairing", "the grant does not carry this board's public key");

    /* And the shared secret both ends now hold is the published one, reached
       without either private half ever appearing on the wire. */
    CHECK(memcmp(dh_pair_shared_secret(&pairing), shared_secret, DH_P256_SHARED_SIZE) == 0,
          "pairing", "the pairing did not reach the published shared secret");

    /* The window is single-shot. A listener that wins the race is registered
       and the helper is not — and the helper is told which of those happened,
       which is the detection signal #34 asked for and never got. */
    reply_len = feed(&s, req_v->f[0], req_v->len[0], 2001, reply, sizeof reply);
    CHECK(decoded_body(reply, reply_len, &v, &body, &body_len), "pairing",
          "a second request drew nothing");
    CHECK(v.hdr.type == DH_MSG_PAIR_REFUSED, "pairing", "the reply is not a pair_refused");
    CHECK(dh_pair_refused_decode(body, body_len, &refused), "pairing", "decode failed");
    CHECK(refused.reason == DH_PAIR_REFUSED_ALREADY_REGISTERED, "pairing",
          "a claimed window was reported as no window at all");

    /* The freshly paired helper now gets a session. */
    const struct vector *hello_v = find("hello_mac");
    if (hello_v) {
        dh_session_stage_nonce(&s, board_nonce);
        CHECK(feed(&s, hello_v->f[0], hello_v->len[0], 3000, reply, sizeof reply) > 0, "pairing",
              "a freshly paired helper was refused");
        CHECK(s.present, "pairing", "a freshly paired helper got no session");
    }
}

/*
 * The trap #109 found while writing the spec: a v2 PAIR_GRANT is 76 bytes and
 * the board built every reply in a 64-byte buffer. dh_frame_encode refuses
 * rather than truncating and the caller queues only on success, so a board
 * built on the old buffer simply never answers a pairing request — no error
 * anywhere, and nothing to say why pairing does not work.
 */
static void test_a_pair_grant_does_not_fit_the_buffer_v1_used(void) {
    const struct vector *req_v = find("pair_request");
    if (!req_v) return;

    dh_session s;
    an_unpaired_board(&s);
    dh_pair_open_window(&pairing, 1000);

    uint8_t too_small[64];
    size_t len = 0;
    dh_frame_view v;
    size_t consumed = 0;
    CHECK(dh_frame_decode(req_v->f[0], req_v->len[0], &v, &consumed) == DH_FRAME_OK, "buffer",
          "malformed");
    CHECK(dh_session_on_frame(&s, &pairing, &v, 1000, too_small, sizeof too_small, &len) !=
              DH_FRAME_OK,
          "buffer", "a 76-byte grant was written into 64 bytes");

    CHECK(DH_SESSION_REPLY_MAX >= DH_FRAME_HEADER_SIZE + DH_PAIR_GRANT_LEN, "buffer",
          "the stated reply maximum cannot hold a pair grant");
}

/* An eviction the board knows about is announced rather than left to the
   helper's timeout — an optimisation over it, never a substitute. */
static void test_an_eviction_the_board_knows_about_is_announced(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], 700000, reply, sizeof reply) > 0, "end",
          "no session");

    size_t out_len = 0;
    CHECK(dh_session_end(&s, DH_SESSION_END_UNPAIRED, reply, sizeof reply, &out_len) ==
              DH_FRAME_OK,
          "end", "ending on a wipe failed");

    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(decoded_body(reply, out_len, &v, &body, &body_len), "end", "the announcement is not a frame");
    CHECK(v.hdr.type == DH_MSG_SESSION_END && body_len == 1 && body[0] == DH_SESSION_END_UNPAIRED,
          "end", "a wipe did not announce the end");
    /* Tagged under the key the session had, which is why the frame is built
       before the session is dropped. */
    CHECK(authenticates(reply, out_len, k_b2h, 1), "end", "the announcement is not tagged");
    CHECK(!s.present, "end", "the announcement left the session up");

    /* Nothing to end, nothing to say. */
    out_len = 0;
    CHECK(dh_session_end(&s, DH_SESSION_END_PROTOCOL_ERROR, reply, sizeof reply, &out_len) ==
              DH_FRAME_OK,
          "end", "ending a session that never started was an error");
    CHECK(out_len == 0, "end", "announced the end of a session that never started");
}

/*
 * The clipboard direction policy (#52). Every session is told one, whether or
 * not it differs from the last session's — a helper that has just arrived has
 * been told nothing, and one that assumed a default would be assuming it about
 * a board it has never spoken to.
 */
static void test_every_session_is_told_the_clipboard_policy(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 400000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "policy",
          "no session");

    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    size_t len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len), "policy",
          "a fresh session sent no policy");
    CHECK(v.hdr.type == DH_MSG_CLIP_POLICY, "policy", "the first frame is not the policy");

    uint8_t flags = 0;
    CHECK(dh_clip_policy_decode(body, body_len, &flags), "policy", "decode failed");
    CHECK(flags == (DH_CLIP_MAY_SEND | DH_CLIP_MAY_RECEIVE), "policy",
          "a board that was told nothing did not report both directions allowed");

    /* Until something takes the frame, it is still owed: a helper never told is
       a helper working from whatever it last heard, with nothing following to
       correct it. */
    CHECK(tick(&s, now, reply, sizeof reply) > 0, "policy",
          "a policy the queue refused was thrown away");
    dh_session_note_owed_sent(&s, DH_MSG_CLIP_POLICY);
    dh_session_note_sent(&s, now);
    /* The baseline drop totals (#133) follow the policy on a fresh session;
       settled here so the silence below is about the policy. */
    settle_openers(&s, now);
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "policy", "the policy was sent twice");

    /* The config page writes a toggle. The live session hears about it. */
    dh_session_set_clip_policy(&s, DH_CLIP_MAY_RECEIVE);
    len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len), "policy",
          "a changed toggle was not sent to the live session");
    CHECK(v.hdr.type == DH_MSG_CLIP_POLICY, "policy", "the change is not a policy frame");
    CHECK(dh_clip_policy_decode(body, body_len, &flags) && flags == DH_CLIP_MAY_RECEIVE, "policy",
          "the change carried the wrong flags");
    dh_session_note_owed_sent(&s, DH_MSG_CLIP_POLICY);
    dh_session_note_sent(&s, now);

    /* Setting the same value again is not a change, so it is not traffic. A
       board calls this every pass; a frame per pass would fill the link. */
    dh_session_set_clip_policy(&s, DH_CLIP_MAY_RECEIVE);
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "policy",
          "an unchanged toggle was sent anyway");

    /* A second helper arrives after the first is gone. It has been told
       nothing, so it is told — and it is told what the board holds now, not
       the default. */
    dh_session_drop(&s);
    dh_session_stage_nonce(&s, board_nonce);
    now += 1000;
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "policy",
          "no second session");
    len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len) && v.hdr.type == DH_MSG_CLIP_POLICY,
          "policy", "the second session was not told the policy");
    CHECK(dh_clip_policy_decode(body, body_len, &flags) && flags == DH_CLIP_MAY_RECEIVE, "policy",
          "the second session was told the default rather than what is set");
}

/*
 * The seven drop totals reach the helper over the channel, which is the whole
 * point of #133: on the config page they could only ever read zero, because
 * the page is reachable only in config mode and config mode is entered by
 * rebooting the board that holds them.
 *
 * What is gated here is the three things that make the reading trustworthy —
 * every session gets a baseline even when nothing has dropped, an unchanged
 * reading is not traffic, and a changed one cannot be sent faster than the
 * queue can take it.
 */
static void test_a_session_is_told_what_the_board_has_dropped(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 400000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "drops",
          "no session");

    /* The policy goes first and is released, so what follows is the drops. */
    size_t len = tick(&s, now, reply, sizeof reply);
    dh_frame_view v;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(decoded_body(reply, len, &v, &body, &body_len) && v.hdr.type == DH_MSG_CLIP_POLICY,
          "drops", "the policy did not come first");
    dh_session_note_owed_sent(&s, DH_MSG_CLIP_POLICY);
    dh_session_note_sent(&s, now);

    /*
     * A board that has dropped nothing still says so. "No drops" and "the
     * board has said nothing" are the two readings a stall has to tell apart,
     * and without a baseline they look identical — which is exactly how a row
     * of zeros on the config page was read as evidence three times.
     */
    len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len), "drops",
          "a fresh session sent no drop totals");
    CHECK(v.hdr.type == DH_MSG_DEVICE_DROPS, "drops", "the second frame is not the drop totals");
    dh_device_drops got;
    CHECK(dh_device_drops_decode(body, body_len, &got), "drops", "decode failed");
    CHECK(got.reports == 0 && got.relay_q == 0, "drops",
          "a board that has dropped nothing did not report zeros");
    dh_session_note_owed_sent(&s, DH_MSG_DEVICE_DROPS);
    dh_session_note_sent(&s, now);

    /* Setting the same reading again is not a change, so it is not traffic. A
       board calls this every pass at 1000 Hz. */
    const dh_device_drops zero = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    dh_session_set_drops(&s, &zero);
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "drops",
          "an unchanged reading was sent anyway");

    /* A seam loses a frame. The helper hears about it — but not before the
       rate limit allows, because the outbound queue is short on purpose. */
    const dh_device_drops one = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    dh_session_set_drops(&s, &one);
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "drops",
          "a second reading went out inside one interval");

    now += DH_SESSION_HEARTBEAT_MS;
    len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len) && v.hdr.type == DH_MSG_DEVICE_DROPS,
          "drops", "a changed reading never reached the helper");
    CHECK(dh_device_drops_decode(body, body_len, &got) && got.truncated == 1, "drops",
          "the changed reading carried the wrong totals");
    dh_session_note_owed_sent(&s, DH_MSG_DEVICE_DROPS);
    dh_session_note_sent(&s, now);

    /*
     * A second helper arrives. It is owed a baseline of its own even though
     * the totals have not moved since the last one was told — it was told
     * nothing, and the totals it inherits are the board's, not the session's.
     */
    dh_session_drop(&s);
    dh_session_stage_nonce(&s, board_nonce);
    now += 1000;
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "drops",
          "no second session");
    len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len) && v.hdr.type == DH_MSG_CLIP_POLICY,
          "drops", "the second session was not told the policy first");
    dh_session_note_owed_sent(&s, DH_MSG_CLIP_POLICY);
    dh_session_note_sent(&s, now);

    len = tick(&s, now, reply, sizeof reply);
    CHECK(decoded_body(reply, len, &v, &body, &body_len) && v.hdr.type == DH_MSG_DEVICE_DROPS,
          "drops", "the second session was told no drop totals");
    CHECK(dh_device_drops_decode(body, body_len, &got) && got.truncated == 1, "drops",
          "the second session was told zeros rather than what the board holds");
}

/*
 * A board that is dropping frames still beats.
 *
 * The drop reading's rate limit is exactly the beat's interval, so a reading
 * that charged the idle timer would displace the beat for as long as anything
 * kept moving — and the helper's quiet detector keys on beats, not on traffic.
 * It would report the beat gone for the whole duration of the fault the
 * reading exists to describe: a diagnostic manufacturing a false reading,
 * which is #133's own mistake one layer up.
 */
static void test_a_board_that_is_dropping_frames_still_beats(void) {
    const struct vector *hello_v = find("hello_mac");
    if (!hello_v) return;

    dh_session s;
    a_paired_board(&s, DH_BUILD_RELEASE);
    uint32_t now = 700000;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    CHECK(feed(&s, hello_v->f[0], hello_v->len[0], now, reply, sizeof reply) > 0, "beat under drops",
          "no session");
    settle_openers(&s, now);

    /* A seam losing a frame a second, for well past the helper's deadline. The
       helper keeps beating throughout, so nothing here is an eviction. */
    dh_device_drops moving = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned beats = 0;
    uint64_t helper_counter = 0;
    for (unsigned second = 0; second < 6; second++) {
        moving.outq++;
        dh_session_set_drops(&s, &moving);
        now += DH_SESSION_HEARTBEAT_MS;

        uint8_t from_helper[MAX_BYTES];
        const size_t from_helper_len = make_tagged(DH_MSG_HEARTBEAT, k_h2b, helper_counter++, NULL,
                                                   0, from_helper, sizeof from_helper);
        CHECK(feed(&s, from_helper, from_helper_len, now, reply, sizeof reply) == 0,
              "beat under drops", "a helper heartbeat drew a reply");

        /* Two ticks in the same millisecond, because one pass emits one frame
           and the board runs this loop at 1000 Hz. */
        for (int i = 0; i < 2; i++) {
            const size_t len = tick(&s, now, reply, sizeof reply);
            if (len == 0) continue;
            dh_session_note_owed_sent(&s, reply[0]);
            if (reply[0] == DH_MSG_DEVICE_HEARTBEAT) beats++;
        }
    }

    CHECK(beats == 6, "beat under drops",
          "a board reporting drops every interval stopped beating");
}

/*
 * Two toggles named by direction, four helpers that only know "me" and "the
 * other computer". This is the whole of that translation, and getting it
 * backwards on one board would disable the wrong direction — visibly wrong at
 * the desk and invisible in any test that only ever looks at board A.
 */
static void test_the_direction_toggles_map_onto_each_board(void) {
    const uint8_t both = DH_CLIP_MAY_SEND | DH_CLIP_MAY_RECEIVE;
    CHECK(dh_clip_policy_for(0, false, false) == both, "directions",
          "board A blocked something with both toggles off");
    CHECK(dh_clip_policy_for(1, false, false) == both, "directions",
          "board B blocked something with both toggles off");

    /* A→B off: A may not send, B may not receive. Neither loses the other
       direction. */
    CHECK(dh_clip_policy_for(0, true, false) == DH_CLIP_MAY_RECEIVE, "directions",
          "blocking A to B did not stop board A sending");
    CHECK(dh_clip_policy_for(1, true, false) == DH_CLIP_MAY_SEND, "directions",
          "blocking A to B did not stop board B receiving");

    CHECK(dh_clip_policy_for(0, false, true) == DH_CLIP_MAY_SEND, "directions",
          "blocking B to A did not stop board A receiving");
    CHECK(dh_clip_policy_for(1, false, true) == DH_CLIP_MAY_RECEIVE, "directions",
          "blocking B to A did not stop board B sending");

    CHECK(dh_clip_policy_for(0, true, true) == 0, "directions", "board A kept a direction");
    CHECK(dh_clip_policy_for(1, true, true) == 0, "directions", "board B kept a direction");
}

int main(int argc, char **argv) {
    const char *frames = argc > 1 ? argv[1] : DH_TEST_VECTORS;
    const char *primitives = argc > 2 ? argv[2] : DH_PRIMITIVE_VECTORS;

    if (!load_vectors(primitives) || !load_vectors(frames)) return 1;
    if (!load_session_material()) {
        printf("FAIL the published session material would not load\n");
        return 1;
    }

    test_the_codecs_round_trip_the_golden_frames();
    test_the_board_answers_the_golden_hello();
    test_a_hello_that_does_not_authenticate_is_answered_with_silence();
    test_the_two_refusals_echo_the_callers_correlation();
    test_a_registration_for_someone_else_is_refused_as_unpaired();
    test_a_v1_hello_is_refused_on_the_version_not_answered_with_silence();
    test_negotiation_clamps_to_what_the_board_has();
    test_a_development_build_needs_no_registration();
    test_an_unauthenticated_bulk_frame_is_not_relayed();
    test_a_replayed_frame_is_refused();
    test_a_relayed_frame_is_tagged_for_this_boards_helper();
    test_only_authenticated_traffic_is_liveness();
    test_the_board_beats_only_into_an_idle_direction();
    test_liveness_survives_the_clock_wrapping();
    test_a_rate_of_refused_frames_is_reported_to_the_helper();
    test_a_count_below_the_threshold_is_not_reported();
    test_an_ordinary_liveness_timeout_is_not_a_listener();
    test_a_measurement_outlives_the_session_it_was_taken_in();
    test_pairing_hands_over_a_public_key_and_closes_the_window();
    test_a_pair_grant_does_not_fit_the_buffer_v1_used();
    test_an_eviction_the_board_knows_about_is_announced();
    test_every_session_is_told_the_clipboard_policy();
    test_a_session_is_told_what_the_board_has_dropped();
    test_a_board_that_is_dropping_frames_still_beats();
    test_the_direction_toggles_map_onto_each_board();

    if (failures) {
        printf("%d session check(s) failed\n", failures);
        return 1;
    }
    printf("session tests passed\n");
    return 0;
}
