/*
 * The end-to-end clipboard seal (#113, ADR-0008).
 *
 * Four claims, in the order the ticket makes them:
 *
 *   1. a payload captured on the wire is not readable without both helpers'
 *      keys;
 *   2. neither board holds a key that opens one — read out of the board's own
 *      state rather than asserted;
 *   3. fidelity survives sealing: the delivered bytes are byte-identical and
 *      CRC32-verified;
 *   4. no build can turn the seal off.
 *
 * The bytes are gated by test-vectors/frames.txt, which an independent Python
 * implementation produced (tools/gen-frame-vectors.py) from published key
 * material. Reproducing `clip_offer_text2` and `clip_chunk_hi` exactly is what
 * says the ECDH, the HKDF, the nonce and the AAD all agree with the other
 * implementation and not merely with themselves.
 *
 * The cipher underneath is aes_gcm_ref.c — the suite's own AES-256-GCM, gated
 * first, below, against NIST's published test cases. See its header for why it
 * is in tests/ and not in the core.
 *
 * Style follows the rest of the harness: an assertion macro, a main, a printed
 * failure line, a non-zero exit — no framework.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "aes_gcm_ref.h"
#include "dh_auth.h"
#include "dh_clip.h"
#include "dh_crc32.h"
#include "dh_frame.h"
#include "dh_pair.h"
#include "dh_seal.h"
#include "dh_session.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* ------------------------------------------------------------------ loading */

#define MAX_VECTORS 64
#define MAX_FIELDS 12
#define MAX_FIELD_BYTES 512

struct vector {
    char name[64];
    size_t fields;
    uint8_t f[MAX_FIELDS][MAX_FIELD_BYTES];
    size_t len[MAX_FIELDS];
};

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `<name> | <hex> | <hex> ...`, one vector per line, comments on '#'. Same
   format as auth_test.c reads, and both vector files are in it. */
static size_t load_vectors(const char *path, struct vector *out, size_t cap) {
    FILE *file = fopen(path, "r");
    if (!file) {
        ++failures;
        printf("FAIL cannot open %s\n", path);
        return 0;
    }
    char line[16384];
    size_t n = 0;
    while (fgets(line, sizeof line, file)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        if (n >= cap) {
            ++failures;
            printf("FAIL vector capacity (%zu) exceeded in %s — raise MAX_VECTORS\n", cap, path);
            break;
        }
        struct vector *v = &out[n];
        size_t name_len = 0;
        for (char *q = p; q < bar && name_len + 1 < sizeof v->name; q++)
            if (!isspace((unsigned char)*q)) v->name[name_len++] = *q;
        v->name[name_len] = '\0';

        v->fields = 0;
        int bad = 0;
        for (char *q = bar; *q && v->fields < MAX_FIELDS;) {
            size_t idx = v->fields++;
            v->len[idx] = 0;
            int hi = -1;
            for (q++; *q && *q != '|'; q++) {
                if (isspace((unsigned char)*q)) continue;
                int nib = hex_nibble((unsigned char)*q);
                if (nib < 0) { bad = 1; break; }
                if (hi < 0) {
                    hi = nib;
                } else {
                    if (v->len[idx] >= MAX_FIELD_BYTES) { bad = 1; break; }
                    v->f[idx][v->len[idx]++] = (uint8_t)((hi << 4) | nib);
                    hi = -1;
                }
            }
            if (bad || hi != -1) { bad = 1; break; }
            while (*q && *q != '|') q++;
        }
        if (bad) {
            ++failures;
            printf("FAIL bad hex in vector %s (%s)\n", v->name, path);
            continue;
        }
        n++;
    }
    fclose(file);
    return n;
}

static struct vector primitives[MAX_VECTORS];
static struct vector frames[MAX_VECTORS];
static size_t primitive_count, frame_count;

static const struct vector *find(const struct vector *v, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0) return &v[i];
    ++failures;
    printf("FAIL vector %s missing\n", name);
    return NULL;
}

static void check_bytes(const uint8_t *got, const uint8_t *want, size_t len, const char *name,
                        const char *what) {
    if (memcmp(got, want, len) == 0) return;
    ++failures;
    printf("FAIL [%s] %s\n  got  ", name, what);
    for (size_t i = 0; i < len; i++) printf("%02x", got[i]);
    printf("\n  want ");
    for (size_t i = 0; i < len; i++) printf("%02x", want[i]);
    printf("\n");
}

/* The body of a golden frame: past the 4-byte header and the 24-byte
   authentication prefix, which belong to the hop and not to the seal. */
static const uint8_t *golden_body(const char *name, size_t *len) {
    const struct vector *v = find(frames, frame_count, name);
    if (!v || v->len[0] < DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE) {
        *len = 0;
        return NULL;
    }
    *len = v->len[0] - DH_FRAME_HEADER_SIZE - DH_FRAME_AUTH_PREFIX_SIZE;
    return v->f[0] + DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE;
}

/* ------------------------------------------------------------- the material */

/*
 * Everything the seal needs, taken from the two vector files rather than
 * copied into this file: the two ephemeral private keys, the two nonces (which
 * are the HKDF salt, in that order), the expected key, and the seal id, which
 * is read out of the golden SEAL_STALE body.
 */
static uint8_t offer_private[DH_P256_PRIVATE_SIZE];
static uint8_t accept_private[DH_P256_PRIVATE_SIZE];
static uint8_t offer_nonce[DH_NONCE_SIZE];
static uint8_t accept_nonce[DH_NONCE_SIZE];
static uint8_t expected_key[DH_SEAL_KEY_SIZE];
static uint32_t seal_id;

/* The session material session_test.c uses, for the board hop below. */
static uint8_t helper_private[DH_P256_PRIVATE_SIZE];
static uint8_t board_private[DH_P256_PRIVATE_SIZE];
static uint8_t board_nonce[DH_NONCE_SIZE];
static uint8_t shared_secret[DH_P256_SHARED_SIZE];
static uint8_t helper_key_id[DH_KEY_ID_SIZE];

static bool load_material(void) {
    const struct vector *offer = find(primitives, primitive_count, "p256_pub_seal_offer");
    const struct vector *accept = find(primitives, primitive_count, "p256_pub_seal_accept");
    const struct vector *hkdf = find(primitives, primitive_count, "hkdf_seal");
    const struct vector *session = find(primitives, primitive_count, "session_material");
    if (!offer || !accept || !hkdf || !session) return false;
    if (hkdf->len[1] != 2u * DH_NONCE_SIZE || hkdf->len[3] != DH_SEAL_KEY_SIZE) {
        ++failures;
        printf("FAIL hkdf_seal does not carry the two nonces and a 32-byte key\n");
        return false;
    }

    memcpy(offer_private, offer->f[0], sizeof offer_private);
    memcpy(accept_private, accept->f[0], sizeof accept_private);
    memcpy(offer_nonce, hkdf->f[1], DH_NONCE_SIZE);
    memcpy(accept_nonce, hkdf->f[1] + DH_NONCE_SIZE, DH_NONCE_SIZE);
    memcpy(expected_key, hkdf->f[3], sizeof expected_key);

    size_t stale_len = 0;
    const uint8_t *stale = golden_body("seal_stale", &stale_len);
    if (!stale || !dh_seal_decode_stale(stale, stale_len, &seal_id)) return false;

    if (session->fields < 8) return false;
    memcpy(helper_private, session->f[0], sizeof helper_private);
    memcpy(board_private, session->f[1], sizeof board_private);
    memcpy(board_nonce, session->f[3], sizeof board_nonce);
    memcpy(shared_secret, session->f[4], sizeof shared_secret);

    uint8_t helper_public[DH_P256_PUBLIC_SIZE];
    if (!dh_p256_public_from_private(helper_private, helper_public)) return false;
    dh_p256_key_id(helper_public, helper_key_id);
    return true;
}

/* The two ends of one seal, established the way the two helpers establish it:
   an offer across, an accept back. */
static void a_live_seal(dh_seal_tx *tx, dh_seal_rx *rx) {
    dh_seal_tx_init(tx);
    dh_seal_rx_init(rx);

    uint8_t offer[DH_SEAL_EXCHANGE_LEN], accept[DH_SEAL_EXCHANGE_LEN];
    size_t offer_len = 0, accept_len = 0;
    CHECK(dh_seal_tx_offer(tx, seal_id, offer_private, offer_nonce, offer, sizeof offer,
                           &offer_len) == DH_SEAL_OK,
          "exchange", "the offer was not built");
    CHECK(dh_seal_rx_offered(rx, offer, offer_len, accept_private, accept_nonce, accept,
                             sizeof accept, &accept_len) == DH_SEAL_OK,
          "exchange", "the offer was not accepted");
    CHECK(dh_seal_tx_accepted(tx, accept, accept_len) == DH_SEAL_OK, "exchange",
          "the accept was refused");
}

/* The two encoders, with the body length they wrote — 0 on any refusal, and
   the reason in *rc for the tests that are about a refusal. */
static size_t sealed_offer(dh_seal_tx *tx, const dh_clip_offer *offer, uint8_t *out, size_t cap,
                           dh_seal_result *rc) {
    size_t len = 0;
    const dh_seal_result r = dh_seal_encode_offer(tx, aes_gcm_ref_aead(), offer, out, cap, &len);
    if (rc) *rc = r;
    return r == DH_SEAL_OK ? len : 0;
}

static size_t sealed_chunk(dh_seal_tx *tx, const dh_clip_chunk *chunk, uint8_t *out, size_t cap,
                           dh_seal_result *rc) {
    size_t len = 0;
    const dh_seal_result r = dh_seal_encode_chunk(tx, aes_gcm_ref_aead(), chunk, out, cap, &len);
    if (rc) *rc = r;
    return r == DH_SEAL_OK ? len : 0;
}

/* ------------------------------------------------------------- the cipher */

/*
 * NIST's published GCM test cases 13-16, which are also what
 * tools/gen-frame-vectors.py checks its own implementation against. Both
 * answer to the document rather than to each other, which is the only way two
 * implementations agreeing means anything.
 *
 * 15 and 16 are the ones that matter: several blocks, a partial trailing
 * block, and additional data. Every sealed vector in frames.txt is short, so
 * without them the counter and the GHASH padding would be untested.
 */
static size_t unhex(const char *hex, uint8_t *out) {
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2)
        out[n++] = (uint8_t)((hex_nibble(p[0]) << 4) | hex_nibble(p[1]));
    return n;
}

static void test_reference_cipher(void) {
    uint8_t key[32], nonce[12], plain[64], aad[32], cipher[64], tag[16], back[64];

    memset(key, 0, sizeof key);
    memset(nonce, 0, sizeof nonce);

    aes_gcm_ref_seal(key, nonce, NULL, 0, NULL, 0, cipher, tag);
    size_t want_len = unhex("530f8afbc74536b9a963b4f1c4cb738b", plain);
    check_bytes(tag, plain, want_len, "gcm_case_13", "tag");

    memset(plain, 0, 16);
    aes_gcm_ref_seal(key, nonce, NULL, 0, plain, 16, cipher, tag);
    uint8_t want[64];
    want_len = unhex("cea7403d4d606b6e074ec5d3baf39d18", want);
    check_bytes(cipher, want, want_len, "gcm_case_14", "ciphertext");
    want_len = unhex("d0d1c8a799996bf0265b98b5d48ab919", want);
    check_bytes(tag, want, want_len, "gcm_case_14", "tag");

    unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", key);
    unhex("cafebabefacedbaddecaf888", nonce);
    const size_t plain_len =
        unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
              "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
              plain);

    aes_gcm_ref_seal(key, nonce, NULL, 0, plain, plain_len, cipher, tag);
    want_len = unhex("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                     "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
                     want);
    check_bytes(cipher, want, want_len, "gcm_case_15", "ciphertext");
    want_len = unhex("b094dac5d93471bdec1a502270e3cc6c", want);
    check_bytes(tag, want, want_len, "gcm_case_15", "tag");

    CHECK(aes_gcm_ref_open(key, nonce, NULL, 0, cipher, plain_len, tag, back), "gcm_case_15",
          "the tag did not verify on the way back");
    check_bytes(back, plain, plain_len, "gcm_case_15", "plaintext");

    const size_t aad_len = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
    aes_gcm_ref_seal(key, nonce, aad, aad_len, plain, 60, cipher, tag);
    want_len = unhex("76fc6ece0f4e1768cddf8853bb2d551b", want);
    check_bytes(tag, want, want_len, "gcm_case_16", "tag");

    /* Additional data is authenticated, not encrypted: one bit of it changed
       is a message that no longer opens. */
    aad[0] ^= 0x01u;
    CHECK(!aes_gcm_ref_open(key, nonce, aad, aad_len, cipher, 60, tag, back), "gcm_case_16",
          "edited additional data still opened");
}

/* --------------------------------------------------------- the exchange */

static void test_the_exchange_matches_the_wire(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    dh_seal_tx_init(&tx);
    dh_seal_rx_init(&rx);

    uint8_t offer[DH_SEAL_EXCHANGE_LEN], accept[DH_SEAL_EXCHANGE_LEN];
    size_t offer_len = 0, accept_len = 0;

    CHECK(dh_seal_tx_offer(&tx, seal_id, offer_private, offer_nonce, offer, sizeof offer,
                           &offer_len) == DH_SEAL_OK,
          "seal_offer", "not built");
    size_t want_len = 0;
    const uint8_t *want = golden_body("seal_offer", &want_len);
    if (want) {
        CHECK(offer_len == want_len, "seal_offer", "wrong length");
        check_bytes(offer, want, want_len, "seal_offer", "body");
    }

    CHECK(dh_seal_rx_offered(&rx, offer, offer_len, accept_private, accept_nonce, accept,
                             sizeof accept, &accept_len) == DH_SEAL_OK,
          "seal_accept", "not built");
    want = golden_body("seal_accept", &want_len);
    if (want) {
        CHECK(accept_len == want_len, "seal_accept", "wrong length");
        check_bytes(accept, want, want_len, "seal_accept", "body");
    }

    CHECK(dh_seal_tx_accepted(&tx, accept, accept_len) == DH_SEAL_OK, "seal_accept", "refused");

    /* Both ends derived the published key, from opposite sides of one ECDH. */
    check_bytes(tx.key, expected_key, DH_SEAL_KEY_SIZE, "k_seal", "the offerer's key");
    check_bytes(rx.key, expected_key, DH_SEAL_KEY_SIZE, "k_seal", "the accepter's key");

    /* The private half has done its one job and is not kept afterwards. */
    uint8_t zeros[DH_P256_PRIVATE_SIZE] = {0};
    check_bytes(tx.eph_private, zeros, sizeof zeros, "k_seal",
                "the ephemeral private key outlived the exchange");

    uint8_t stale[DH_SEAL_STALE_LEN];
    CHECK(dh_seal_encode_stale(seal_id, stale, sizeof stale) == (int)DH_SEAL_STALE_LEN,
          "seal_stale", "not built");
    want = golden_body("seal_stale", &want_len);
    if (want) check_bytes(stale, want, want_len, "seal_stale", "body");
}

static void test_an_accept_this_end_did_not_ask_for(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    dh_seal_tx_init(&tx);
    dh_seal_rx_init(&rx);

    uint8_t offer[DH_SEAL_EXCHANGE_LEN], accept[DH_SEAL_EXCHANGE_LEN];
    size_t offer_len = 0, accept_len = 0;
    (void)dh_seal_tx_offer(&tx, seal_id, offer_private, offer_nonce, offer, sizeof offer,
                           &offer_len);
    (void)dh_seal_rx_offered(&rx, offer, offer_len, accept_private, accept_nonce, accept,
                             sizeof accept, &accept_len);

    accept[0] ^= 0xffu; /* a different seal_id: some other exchange's answer */
    CHECK(dh_seal_tx_accepted(&tx, accept, accept_len) == DH_SEAL_ERR_UNKNOWN_ID, "exchange",
          "an accept for another seal was taken");
    CHECK(!tx.live, "exchange", "an accept for another seal made a key");

    accept[0] ^= 0xffu;
    CHECK(dh_seal_tx_accepted(&tx, accept, accept_len - 1) == DH_SEAL_ERR_MALFORMED, "exchange",
          "a short accept was taken");
}

/* ------------------------------------------------------- the sealed bytes */

/*
 * The two messages that carry the user's bytes, rebuilt from the same inputs
 * the golden vectors were generated from: one seal, counter 0 for the offer and
 * 1 for the chunk, since they come off the same key in that order.
 */
static void test_sealed_messages_match_the_wire(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    const dh_clip_offer offer = {.id = 1, .kind = 0, .total = 2, .meta = NULL, .meta_len = 0};
    uint8_t body[DH_FRAME_MAX_PAYLOAD];
    size_t n = sealed_offer(&tx, &offer, body, sizeof body, NULL);
    CHECK(n > 0, "clip_offer_text2", "the offer was not sealed");

    size_t want_len = 0;
    const uint8_t *want = golden_body("clip_offer_text2", &want_len);
    if (want && n > 0) {
        CHECK(n == want_len, "clip_offer_text2", "wrong length");
        check_bytes(body, want, want_len < n ? want_len : n, "clip_offer_text2", "body");
    }

    /* And back: the receiving end reads what was put in. */
    uint8_t plain[DH_FRAME_MAX_PAYLOAD];
    dh_clip_offer read;
    CHECK(dh_seal_open_offer(&rx, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
              DH_SEAL_OK,
          "clip_offer_text2", "the offer did not open");
    CHECK(read.id == 1 && read.kind == 0 && read.total == 2 && read.meta_len == 0,
          "clip_offer_text2", "the offer opened to something else");

    const uint8_t data[2] = {'h', 'i'};
    const dh_clip_chunk chunk = {
        .id = 2, .seq = 0, .crc32 = dh_crc32(data, sizeof data), .data = data, .data_len = 2};
    n = sealed_chunk(&tx, &chunk, body, sizeof body, NULL);
    CHECK(n > 0, "clip_chunk_hi", "the chunk was not sealed");

    want = golden_body("clip_chunk_hi", &want_len);
    if (want && n > 0) {
        CHECK(n == want_len, "clip_chunk_hi", "wrong length");
        check_bytes(body, want, want_len < n ? want_len : n, "clip_chunk_hi", "body");
    }

    dh_clip_chunk read_chunk;
    CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), body, n, plain, sizeof plain,
                             &read_chunk) == DH_SEAL_OK,
          "clip_chunk_hi", "the chunk did not open");
    CHECK(read_chunk.id == 2 && read_chunk.seq == 0 && read_chunk.data_len == 2, "clip_chunk_hi",
          "the chunk header opened to something else");
    CHECK(memcmp(read_chunk.data, data, 2) == 0, "clip_chunk_hi", "the bytes changed");
    CHECK(read_chunk.crc32 == dh_crc32(read_chunk.data, read_chunk.data_len), "clip_chunk_hi",
          "the CRC32 inside the seal does not match the bytes it covers");
}

/* AC 1: not readable without the key. */
static void test_a_captured_payload_is_not_readable(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    /* Something worth reading, and long enough to show up if it were there. */
    static const uint8_t secret[] = "the-clipboard-of-the-other-computer";
    const dh_clip_chunk chunk = {.id = 7,
                                 .seq = 0,
                                 .crc32 = dh_crc32(secret, sizeof secret),
                                 .data = secret,
                                 .data_len = (uint16_t)sizeof secret};
    uint8_t body[DH_FRAME_MAX_PAYLOAD];
    const size_t n = sealed_chunk(&tx, &chunk, body, sizeof body, NULL);
    CHECK(n > 0, "unreadable", "the chunk was not sealed");
    if (n == 0) return;

    /* The plaintext is not on the wire anywhere in the frame. */
    bool found = false;
    for (size_t i = 0; i + sizeof secret <= n; i++)
        if (memcmp(body + i, secret, sizeof secret) == 0) found = true;
    CHECK(!found, "unreadable", "the payload is on the wire in clear");

    uint8_t plain[DH_FRAME_MAX_PAYLOAD];
    dh_clip_chunk read;

    /* A holder of the right seal id and the wrong key gets nothing. */
    dh_seal_rx wrong = rx;
    wrong.key[0] ^= 0x01u;
    CHECK(dh_seal_open_chunk(&wrong, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
              DH_SEAL_ERR_AUTH,
          "unreadable", "a payload opened under the wrong key");

    /* And a holder of no key at all is told which seal it is missing, so it can
       say so rather than guess. */
    dh_seal_rx none;
    dh_seal_rx_init(&none);
    CHECK(dh_seal_open_chunk(&none, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
              DH_SEAL_ERR_UNKNOWN_ID,
          "unreadable", "an unknown seal was not reported as one");

    uint32_t named = 0;
    CHECK(dh_seal_peek_id(DH_MSG_CLIP_CHUNK, body, n, &named) && named == seal_id, "unreadable",
          "the seal id could not be read from the clear head");
}

/* The clear head is the AAD, so a relay that edits it breaks the seal. */
static void test_the_clear_head_is_bound_to_the_payload(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    const uint8_t data[4] = {1, 2, 3, 4};
    const dh_clip_chunk chunk = {
        .id = 9, .seq = 3, .crc32 = dh_crc32(data, sizeof data), .data = data, .data_len = 4};
    uint8_t body[DH_FRAME_MAX_PAYLOAD], plain[DH_FRAME_MAX_PAYLOAD];
    const size_t n = sealed_chunk(&tx, &chunk, body, sizeof body, NULL);
    CHECK(n > 0, "aad", "the chunk was not sealed");
    if (n == 0) return;

    dh_clip_chunk read;
    /* Every clear field, and one ciphertext byte: each one edited alone. */
    const size_t offsets[] = {0, 4, 12, n - 1};
    const char *what[] = {"the transfer id", "the sequence number", "the seal counter",
                          "the tag"};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        body[offsets[i]] ^= 0x01u;
        CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
                  DH_SEAL_ERR_AUTH,
              "aad", what[i]);
        body[offsets[i]] ^= 0x01u;
    }
    CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
              DH_SEAL_OK,
          "aad", "the unedited frame stopped opening");
}

/* ------------------------------------------------------ what a board holds */

/*
 * Every 32-byte window of the board's state, tried as a seal key. This is the
 * ticket's second criterion read out of the firmware rather than asserted: if a
 * board held anything that opened a payload, a window of its state would be it.
 */
static bool a_board_holds_a_key_that_opens(const uint8_t *state, size_t state_len,
                                           const uint8_t *body, size_t body_len) {
    uint32_t named = 0;
    if (!dh_seal_peek_id(DH_MSG_CLIP_CHUNK, body, body_len, &named)) return false;

    uint8_t plain[DH_FRAME_MAX_PAYLOAD];
    dh_clip_chunk read;
    for (size_t i = 0; i + DH_SEAL_KEY_SIZE <= state_len; i++) {
        dh_seal_rx probe;
        dh_seal_rx_init(&probe);
        probe.live = true;
        probe.seal_id = named;
        memcpy(probe.key, state + i, DH_SEAL_KEY_SIZE);
        if (dh_seal_open_chunk(&probe, aes_gcm_ref_aead(), body, body_len, plain, sizeof plain,
                               &read) == DH_SEAL_OK)
            return true;
    }
    return false;
}

/* A board that has paired with the published helper key and negotiated a
   session with it, so that it is holding everything a board ever holds. */
static void relay_through_a_board(uint8_t build_type, const uint8_t *sealed, size_t sealed_len,
                                  const char *name) {
    const struct vector *hello = find(frames, frame_count, "hello_mac");
    if (!hello) return;

    static dh_pair pairing;
    dh_session board;
    dh_session_init(&board, build_type);
    dh_session_stage_nonce(&board, board_nonce);
    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);
    dh_pair_set_registration(&pairing, helper_key_id, shared_secret);

    dh_frame_view v;
    size_t consumed = 0;
    uint8_t reply[DH_SESSION_REPLY_MAX];
    size_t reply_len = 0;
    CHECK(dh_frame_decode(hello->f[0], hello->len[0], &v, &consumed) == DH_FRAME_OK, name,
          "the golden hello is malformed");
    CHECK(dh_session_on_frame(&board, &pairing, &v, 1000, reply, sizeof reply, &reply_len) ==
              DH_FRAME_OK,
          name, "the board refused the hello");

    /* A sealed chunk arriving from this board's own helper, tagged for the hop
       under the session key, verified and then re-tagged toward the far helper
       — the whole of what a board does with a clipboard payload. */
    uint8_t k_h2b[DH_SESSION_KEY_SIZE], k_b2h[DH_SESSION_KEY_SIZE];
    uint8_t helper_nonce[DH_NONCE_SIZE];
    const struct vector *material = find(primitives, primitive_count, "session_material");
    if (!material) return;
    memcpy(helper_nonce, material->f[2], sizeof helper_nonce);
    dh_auth_derive_session_keys(shared_secret, helper_nonce, board_nonce, k_h2b, k_b2h);

    uint8_t framed[DH_FRAME_MAX_SIZE];
    size_t framed_len = 0;
    CHECK(dh_auth_frame(DH_MSG_CLIP_CHUNK, 0, k_h2b, 1, sealed, sealed_len, framed, sizeof framed,
                        &framed_len) == DH_FRAME_OK,
          name, "the hop frame was not built");
    CHECK(dh_frame_decode(framed, framed_len, &v, &consumed) == DH_FRAME_OK, name,
          "the hop frame is malformed");

    const uint8_t *body = NULL;
    size_t body_len = 0;
    CHECK(dh_session_authenticate(&board, &v, 1010, &body, &body_len) == DH_AUTH_OK, name,
          "the board could not authenticate the hop");
    CHECK(body_len == sealed_len && body && memcmp(body, sealed, sealed_len) == 0, name,
          "the board read something other than the sealed body");

    /* The far board's side of the same frame: the hop tag is stripped at the
       first board and a fresh one written by the second, so what crosses the
       inter-board link is the header and the sealed body. */
    static uint8_t bare[DH_FRAME_MAX_SIZE];
    size_t bare_len = 0;
    CHECK(dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, sealed, sealed_len, bare, sizeof bare,
                          &bare_len) == DH_FRAME_OK,
          name, "the inter-board frame was not built");
    CHECK(dh_frame_decode(bare, bare_len, &v, &consumed) == DH_FRAME_OK, name,
          "the inter-board frame is malformed");

    static uint8_t relayed[DH_FRAME_MAX_SIZE];
    size_t relayed_len = 0;
    CHECK(dh_session_emit_relayed(&board, &v, relayed, sizeof relayed, &relayed_len) ==
              DH_FRAME_OK,
          name, "the board would not relay the sealed chunk");

    /* What the board relayed is what it was given, byte for byte, behind a new
       hop tag: opaque relay. */
    CHECK(relayed_len == DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + sealed_len, name,
          "the relayed frame changed size");
    check_bytes(relayed + DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE, sealed, sealed_len,
                name, "the relayed payload changed");

    CHECK(!a_board_holds_a_key_that_opens((const uint8_t *)&board, sizeof board, sealed,
                                          sealed_len),
          name, "a window of the board's session state opens the payload");
    CHECK(!a_board_holds_a_key_that_opens((const uint8_t *)&pairing, sizeof pairing, sealed,
                                          sealed_len),
          name, "a window of the board's pairing state opens the payload");

    /* And the key itself is nowhere in either, which is the same claim read the
       other way round. */
    bool found = false;
    for (size_t i = 0; i + DH_SEAL_KEY_SIZE <= sizeof board; i++)
        if (memcmp((const uint8_t *)&board + i, expected_key, DH_SEAL_KEY_SIZE) == 0) found = true;
    for (size_t i = 0; i + DH_SEAL_KEY_SIZE <= sizeof pairing; i++)
        if (memcmp((const uint8_t *)&pairing + i, expected_key, DH_SEAL_KEY_SIZE) == 0)
            found = true;
    CHECK(!found, name, "the seal key is in the board's state");
}

/* AC 2 and AC 4: no board holds a key that opens a payload — including a
   development board, which skips its own authentication entirely (#44) and
   still has nothing to open a payload with. The exemption is the board's, and
   the board is not a party to this key. */
static void test_no_board_can_read_a_payload(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    static const uint8_t secret[] = "the-clipboard-of-the-other-computer";
    const dh_clip_chunk chunk = {.id = 4,
                                 .seq = 0,
                                 .crc32 = dh_crc32(secret, sizeof secret),
                                 .data = secret,
                                 .data_len = (uint16_t)sizeof secret};
    static uint8_t sealed[DH_FRAME_MAX_PAYLOAD];
    const size_t n = sealed_chunk(&tx, &chunk, sealed, sizeof sealed, NULL);
    CHECK(n > 0, "board", "the chunk was not sealed");
    if (n == 0) return;

    relay_through_a_board(DH_BUILD_RELEASE, sealed, n, "board");
    relay_through_a_board(DH_BUILD_DEVELOPMENT, sealed, n, "development board");
}

/*
 * A cipher that refuses everything, which is the only way to reach the one
 * path where a payload has been staged in the caller's buffer and the cipher
 * then failed. Nothing else produces it, and it is the path that would leave a
 * clipboard sitting in clear if it were left half-written.
 */
static bool refuse_seal(void *ctx, const uint8_t key[DH_SEAL_KEY_SIZE],
                        const uint8_t nonce[DH_SEAL_NONCE_SIZE], const uint8_t *aad,
                        size_t aad_len, const uint8_t *plain, size_t plain_len,
                        uint8_t *cipher_out, uint8_t tag_out[DH_SEAL_TAG_SIZE]) {
    (void)ctx; (void)key; (void)nonce; (void)aad; (void)aad_len; (void)plain; (void)plain_len;
    (void)cipher_out; (void)tag_out;
    return false;
}

static bool refuse_open(void *ctx, const uint8_t key[DH_SEAL_KEY_SIZE],
                        const uint8_t nonce[DH_SEAL_NONCE_SIZE], const uint8_t *aad,
                        size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                        const uint8_t tag[DH_SEAL_TAG_SIZE], uint8_t *plain_out) {
    (void)ctx; (void)key; (void)nonce; (void)aad; (void)aad_len; (void)cipher; (void)cipher_len;
    (void)tag; (void)plain_out;
    return false;
}

static const dh_seal_aead refusing_aead = {NULL, refuse_seal, refuse_open};

static void test_a_refused_cipher_leaves_nothing(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    static const uint8_t secret[] = "the-clipboard-of-the-other-computer";
    const dh_clip_chunk chunk = {.id = 8,
                                 .seq = 0,
                                 .crc32 = dh_crc32(secret, sizeof secret),
                                 .data = secret,
                                 .data_len = (uint16_t)sizeof secret};

    uint8_t body[DH_FRAME_MAX_PAYLOAD];
    memset(body, 0, sizeof body);
    size_t written = 0;
    CHECK(dh_seal_encode_chunk(&tx, &refusing_aead, &chunk, body, sizeof body, &written) ==
              DH_SEAL_ERR_AUTH,
          "refused cipher", "a refusing cipher still reported success");
    CHECK(written == 0, "refused cipher", "a refused encode reported a length");

    bool found = false;
    for (size_t i = 0; i + sizeof secret <= sizeof body; i++)
        if (memcmp(body + i, secret, sizeof secret) == 0) found = true;
    CHECK(!found, "refused cipher", "a refused encode left the payload in clear");
}

/* AC 4, the other half: there is no path through this layer that puts a
   payload on the wire in clear, so nothing has to be switched on. */
static void test_there_is_no_unsealed_path(void) {
    dh_seal_tx tx;
    dh_seal_tx_init(&tx);

    const uint8_t data[4] = {1, 2, 3, 4};
    const dh_clip_chunk chunk = {
        .id = 1, .seq = 0, .crc32 = dh_crc32(data, sizeof data), .data = data, .data_len = 4};
    const dh_clip_offer offer = {.id = 1, .kind = 0, .total = 4, .meta = NULL, .meta_len = 0};

    uint8_t body[DH_FRAME_MAX_PAYLOAD];
    memset(body, 0xAA, sizeof body);
    dh_seal_result rc = DH_SEAL_OK;
    (void)sealed_chunk(&tx, &chunk, body, sizeof body, &rc);
    CHECK(rc == DH_SEAL_ERR_NO_SEAL, "no unsealed path", "a chunk was encoded with no seal");
    (void)sealed_offer(&tx, &offer, body, sizeof body, &rc);
    CHECK(rc == DH_SEAL_ERR_NO_SEAL, "no unsealed path", "an offer was encoded with no seal");

    /* Refused, not half-written: nothing of the payload reached the buffer. */
    bool found = false;
    for (size_t i = 0; i + sizeof data <= sizeof body; i++)
        if (memcmp(body + i, data, sizeof data) == 0) found = true;
    CHECK(!found, "no unsealed path", "a refused encode left payload in the buffer");
}

/* --------------------------------------------------------------- fidelity */

/* AC 3: the delivered bytes are byte-identical and the CRC32 covers them. The
   seal changes the wire, never the content. */
static void test_fidelity_survives_the_seal(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    static uint8_t data[DH_SEAL_MAX_CHUNK_DATA];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 7u + (i >> 8));

    const dh_clip_chunk chunk = {.id = 3,
                                 .seq = 11,
                                 .crc32 = dh_crc32(data, sizeof data),
                                 .data = data,
                                 .data_len = (uint16_t)sizeof data};
    static uint8_t body[DH_FRAME_MAX_PAYLOAD];
    const size_t n = sealed_chunk(&tx, &chunk, body, sizeof body, NULL);
    CHECK(n > 0, "fidelity", "the largest chunk did not fit");
    CHECK(n + DH_FRAME_AUTH_PREFIX_SIZE == DH_FRAME_MAX_PAYLOAD, "fidelity",
          "the largest chunk does not fill a frame exactly");

    static uint8_t plain[DH_FRAME_MAX_PAYLOAD];
    dh_clip_chunk read;
    CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
              DH_SEAL_OK,
          "fidelity", "the largest chunk did not open");
    CHECK(read.data_len == sizeof data, "fidelity", "the length changed");
    check_bytes(read.data, data, sizeof data, "fidelity", "the bytes changed");
    CHECK(read.crc32 == dh_crc32(read.data, read.data_len), "fidelity",
          "the CRC32 does not cover the delivered bytes");

    /* One byte more than the ceiling is refused rather than truncated. */
    const dh_clip_chunk oversize = {.id = 3,
                                    .seq = 12,
                                    .crc32 = 0,
                                    .data = data,
                                    .data_len = (uint16_t)(sizeof data + 1u)};
    dh_seal_result rc = DH_SEAL_OK;
    (void)sealed_chunk(&tx, &oversize, body, sizeof body, &rc);
    CHECK(rc == DH_SEAL_ERR_BUFFER, "fidelity", "a chunk past the ceiling was sealed anyway");
}

/* The ceiling docs/protocol.md states, and the board clamps a helper's request
   to, is exactly what this layer's overhead leaves. Stated in two places
   because the board must not need this header; checked here so they cannot
   drift apart. */
static void test_the_chunk_ceiling_is_the_seal_arithmetic(void) {
    CHECK((unsigned)DH_SEAL_MAX_CHUNK_DATA == DH_SESSION_CHUNK_CEILING, "ceiling",
          "DH_SESSION_CHUNK_CEILING no longer matches what the seal leaves");
    CHECK(DH_SEAL_CHUNK_OVERHEAD == 40, "ceiling", "a sealed chunk's overhead changed");
    CHECK(DH_SEAL_OFFER_OVERHEAD == 43, "ceiling", "a sealed offer's overhead changed");
}

/* ------------------------------------------------------------ housekeeping */

/* Each message spends one counter value, and never the same one twice — which
   is what makes the nonce safe without a nonce on the wire. A retransmitted
   chunk is therefore re-sealed, not resent. */
static void test_the_counter_advances_once_per_message(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    const uint8_t data[2] = {'h', 'i'};
    const dh_clip_chunk chunk = {
        .id = 5, .seq = 0, .crc32 = dh_crc32(data, sizeof data), .data = data, .data_len = 2};

    uint8_t first[DH_FRAME_MAX_PAYLOAD], again[DH_FRAME_MAX_PAYLOAD];
    const size_t a = sealed_chunk(&tx, &chunk, first, sizeof first, NULL);
    CHECK(tx.counter == 1, "counter", "the counter did not advance");
    const size_t b = sealed_chunk(&tx, &chunk, again, sizeof again, NULL);
    CHECK(tx.counter == 2, "counter", "the counter did not advance twice");
    CHECK(a == b && a > 0, "counter", "the same chunk sealed to a different length");
    CHECK(a > 0 && memcmp(first, again, a) != 0, "counter",
          "the same chunk sealed twice to the same bytes — a reused nonce");

    /* Both open: a retransmission is an ordinary sealed frame. */
    uint8_t plain[DH_FRAME_MAX_PAYLOAD];
    dh_clip_chunk read;
    CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), again, b, plain, sizeof plain, &read) ==
              DH_SEAL_OK,
          "counter", "the re-sealed chunk did not open");
}

/*
 * The ordinary recovery when one end's session went away and the other's did
 * not: the receiver holds no key for the seal it is being sent, says so, and
 * the sender offers a new one. #107 measured 586 teardowns in sixteen hours,
 * so this is a routine path and not an exceptional one.
 */
static void test_a_stale_seal_is_recovered(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    const uint8_t data[2] = {'h', 'i'};
    const dh_clip_chunk chunk = {
        .id = 6, .seq = 0, .crc32 = dh_crc32(data, sizeof data), .data = data, .data_len = 2};
    uint8_t body[DH_FRAME_MAX_PAYLOAD], plain[DH_FRAME_MAX_PAYLOAD];
    const size_t n = sealed_chunk(&tx, &chunk, body, sizeof body, NULL);
    CHECK(n > 0, "stale", "the chunk was not sealed");
    if (n == 0) return;

    /* The receiver's session ended: it drops every seal it held. */
    dh_seal_rx_init(&rx);
    dh_clip_chunk read;
    CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), body, n, plain, sizeof plain, &read) ==
              DH_SEAL_ERR_UNKNOWN_ID,
          "stale", "a dropped seal still opened a payload");

    uint32_t named = 0;
    CHECK(dh_seal_peek_id(DH_MSG_CLIP_CHUNK, body, n, &named), "stale",
          "the seal id could not be read");

    uint8_t stale[DH_SEAL_STALE_LEN];
    CHECK(dh_seal_encode_stale(named, stale, sizeof stale) == (int)DH_SEAL_STALE_LEN, "stale",
          "SEAL_STALE was not built");
    uint32_t decoded = 0;
    CHECK(dh_seal_decode_stale(stale, sizeof stale, &decoded) && decoded == named, "stale",
          "SEAL_STALE did not round trip");

    /* A stale naming some other seal changes nothing; this one discards it. */
    CHECK(!dh_seal_tx_stale(&tx, decoded ^ 0xffffffffu), "stale",
          "a stale for another seal discarded this one");
    CHECK(tx.live, "stale", "a stale for another seal discarded this one");
    CHECK(dh_seal_tx_stale(&tx, decoded), "stale", "the named seal was not discarded");
    CHECK(!tx.live, "stale", "the named seal survived");
    dh_seal_result rc = DH_SEAL_OK;
    (void)sealed_chunk(&tx, &chunk, body, sizeof body, &rc);
    CHECK(rc == DH_SEAL_ERR_NO_SEAL, "stale", "a discarded seal still sealed");

    /* A fresh offer, and the pair is working again. */
    a_live_seal(&tx, &rx);
    CHECK(sealed_chunk(&tx, &chunk, body, sizeof body, NULL) > 0, "stale",
          "the new seal does not work");
}

/* A malformed sealed body is malformed, not unauthenticated: there is no tag
   in it that failed. */
static void test_short_bodies(void) {
    dh_seal_tx tx;
    dh_seal_rx rx;
    a_live_seal(&tx, &rx);

    uint8_t body[DH_FRAME_MAX_PAYLOAD], plain[DH_FRAME_MAX_PAYLOAD];
    memset(body, 0, sizeof body);
    dh_clip_chunk chunk;
    dh_clip_offer offer;

    CHECK(dh_seal_open_chunk(&rx, aes_gcm_ref_aead(), body, DH_CLIP_CHUNK_HEAD_LEN - 1, plain,
                             sizeof plain, &chunk) == DH_SEAL_ERR_MALFORMED,
          "short", "a body too short for its head was not called malformed");
    CHECK(dh_seal_open_offer(&rx, aes_gcm_ref_aead(), body, DH_CLIP_OFFER_HEAD_LEN - 1, plain,
                             sizeof plain, &offer) == DH_SEAL_ERR_MALFORMED,
          "short", "a body too short for its head was not called malformed");

    uint32_t named = 0;
    CHECK(!dh_seal_peek_id(DH_MSG_CLIP_DONE, body, sizeof body, &named), "short",
          "a message that carries no seal named one");
}

int main(int argc, char **argv) {
    const char *primitives_path = argc > 1 ? argv[1] : DH_PRIMITIVE_VECTORS;
    const char *frames_path = argc > 2 ? argv[2] : DH_TEST_VECTORS;

    primitive_count = load_vectors(primitives_path, primitives, MAX_VECTORS);
    frame_count = load_vectors(frames_path, frames, MAX_VECTORS);

    test_reference_cipher(); /* first: everything below trusts it */

    if (load_material()) {
        test_the_exchange_matches_the_wire();
        test_an_accept_this_end_did_not_ask_for();
        test_sealed_messages_match_the_wire();
        test_a_captured_payload_is_not_readable();
        test_the_clear_head_is_bound_to_the_payload();
        test_no_board_can_read_a_payload();
        test_the_counter_advances_once_per_message();
        test_a_stale_seal_is_recovered();
        test_a_refused_cipher_leaves_nothing();
        test_short_bodies();
        test_fidelity_survives_the_seal();
    }
    test_there_is_no_unsealed_path();
    test_the_chunk_ceiling_is_the_seal_arithmetic();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("seal_test ok (%zu primitive vectors, %zu frames)\n", primitive_count, frame_count);
    return 0;
}
