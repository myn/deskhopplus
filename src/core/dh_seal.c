/*
 * The end-to-end clipboard seal (#113, ADR-0008). Layouts: docs/protocol.md,
 * "The sealed clipboard payload"; test-vectors/frames.txt is the gate.
 *
 * The cipher is the platform's, reached through dh_seal_aead. Everything that
 * decides *what* is sealed, under which key, and with which nonce is here, so
 * that the two helpers cannot drift apart on it.
 */

#include "dh_seal.h"

#include <string.h>

#include "dh_sha256.h"

/* The one HKDF label for this key, and the reason the seal key can never
   collide with a session key derived from the same primitive. */
static const uint8_t seal_info[] = {'d', 'e', 's', 'k', 'h', 'o', 'p', 'p', 'l',
                                    'u', 's', '/', '2', ' ', 's', 'e', 'a', 'l'};

/* The largest sealed body that still leaves room for the hop's authentication
   prefix inside one frame payload. */
#define SEAL_BODY_MAX (DH_FRAME_MAX_PAYLOAD - DH_FRAME_AUTH_PREFIX_SIZE)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/*
 * nonce = seal_counter (u64, little-endian) || 00 00 00 00
 *
 * Twelve bytes, always: the counter is unique under a one-directional key, so
 * the four zeros are structure rather than padding to be filled in later.
 */
static void seal_nonce(uint64_t counter, uint8_t out[DH_SEAL_NONCE_SIZE]) {
    for (size_t i = 0; i < 8; i++)
        out[i] = (uint8_t)((counter >> (8u * i)) & 0xff);
    memset(out + 8, 0, DH_SEAL_NONCE_SIZE - 8u);
}

/* k_seal = HKDF(ECDH(mine, theirs), nonce_offer || nonce_accept, "…/2 seal") */
static bool derive(const uint8_t eph_private[DH_P256_PRIVATE_SIZE],
                   const uint8_t peer_public[DH_P256_PUBLIC_SIZE],
                   const uint8_t nonce_offer[DH_NONCE_SIZE],
                   const uint8_t nonce_accept[DH_NONCE_SIZE], uint8_t key[DH_SEAL_KEY_SIZE]) {
    uint8_t shared[DH_P256_SHARED_SIZE];
    if (!dh_p256_ecdh(eph_private, peer_public, shared))
        return false;

    uint8_t salt[2u * DH_NONCE_SIZE];
    memcpy(salt, nonce_offer, DH_NONCE_SIZE);
    memcpy(salt + DH_NONCE_SIZE, nonce_accept, DH_NONCE_SIZE);

    dh_hkdf_sha256(shared, sizeof shared, salt, sizeof salt, seal_info, sizeof seal_info, key,
                   DH_SEAL_KEY_SIZE);
    memset(shared, 0, sizeof shared);
    memset(salt, 0, sizeof salt);
    return true;
}

/* seal_id:u32 nonce:16 eph_pubkey:64 — the body both halves of the exchange
   carry, differing only in which end drew what is in it. */
static int encode_exchange(uint32_t seal_id, const uint8_t nonce[DH_NONCE_SIZE],
                           const uint8_t public_key[DH_P256_PUBLIC_SIZE], uint8_t *out,
                           size_t cap) {
    if (cap < (size_t)DH_SEAL_EXCHANGE_LEN)
        return -1;
    put32(out, seal_id);
    memcpy(out + DH_SEAL_ID_SIZE, nonce, DH_NONCE_SIZE);
    memcpy(out + DH_SEAL_ID_SIZE + DH_NONCE_SIZE, public_key, DH_P256_PUBLIC_SIZE);
    return (int)DH_SEAL_EXCHANGE_LEN;
}

void dh_seal_tx_init(dh_seal_tx *tx) { memset(tx, 0, sizeof *tx); }

void dh_seal_rx_init(dh_seal_rx *rx) { memset(rx, 0, sizeof *rx); }

dh_seal_result dh_seal_tx_offer(dh_seal_tx *tx, uint32_t seal_id,
                                const uint8_t eph_private[DH_P256_PRIVATE_SIZE],
                                const uint8_t nonce[DH_NONCE_SIZE], uint8_t *out, size_t cap,
                                size_t *out_len) {
    uint8_t public_key[DH_P256_PUBLIC_SIZE];
    if (!dh_p256_public_from_private(eph_private, public_key))
        return DH_SEAL_ERR_KEY;

    const int n = encode_exchange(seal_id, nonce, public_key, out, cap);
    if (n < 0)
        return DH_SEAL_ERR_BUFFER;

    /* Whatever this end held is gone from here: the offer supersedes it, and a
       key kept beside a fresh offer is a key nothing will ever open again. */
    dh_seal_tx_init(tx);
    tx->offered = true;
    tx->seal_id = seal_id;
    memcpy(tx->nonce, nonce, DH_NONCE_SIZE);
    memcpy(tx->eph_private, eph_private, DH_P256_PRIVATE_SIZE);

    *out_len = (size_t)n;
    return DH_SEAL_OK;
}

dh_seal_result dh_seal_tx_accepted(dh_seal_tx *tx, const uint8_t *body, size_t len) {
    if (len != (size_t)DH_SEAL_EXCHANGE_LEN)
        return DH_SEAL_ERR_MALFORMED;
    if (!tx->offered)
        return DH_SEAL_ERR_NO_SEAL;
    if (get32(body) != tx->seal_id)
        return DH_SEAL_ERR_UNKNOWN_ID;

    const uint8_t *peer_nonce = body + DH_SEAL_ID_SIZE;
    const uint8_t *peer_public = body + DH_SEAL_ID_SIZE + DH_NONCE_SIZE;
    if (!derive(tx->eph_private, peer_public, tx->nonce, peer_nonce, tx->key))
        return DH_SEAL_ERR_KEY;

    /* The ephemeral private half has done its one job. Holding it afterwards
       would keep material on the heap that adds nothing this end can still
       use. */
    memset(tx->eph_private, 0, sizeof tx->eph_private);
    tx->offered = false;
    tx->live = true;
    tx->counter = 0;
    return DH_SEAL_OK;
}

dh_seal_result dh_seal_rx_offered(dh_seal_rx *rx, const uint8_t *body, size_t len,
                                  const uint8_t eph_private[DH_P256_PRIVATE_SIZE],
                                  const uint8_t nonce[DH_NONCE_SIZE], uint8_t *out, size_t cap,
                                  size_t *out_len) {
    if (len != (size_t)DH_SEAL_EXCHANGE_LEN)
        return DH_SEAL_ERR_MALFORMED;

    uint8_t public_key[DH_P256_PUBLIC_SIZE];
    if (!dh_p256_public_from_private(eph_private, public_key))
        return DH_SEAL_ERR_KEY;

    const uint32_t seal_id = get32(body);
    const uint8_t *peer_nonce = body + DH_SEAL_ID_SIZE;
    const uint8_t *peer_public = body + DH_SEAL_ID_SIZE + DH_NONCE_SIZE;

    uint8_t key[DH_SEAL_KEY_SIZE];
    if (!derive(eph_private, peer_public, peer_nonce, nonce, key))
        return DH_SEAL_ERR_KEY;

    const int n = encode_exchange(seal_id, nonce, public_key, out, cap);
    if (n < 0) {
        memset(key, 0, sizeof key);
        return DH_SEAL_ERR_BUFFER;
    }

    rx->live = true;
    rx->seal_id = seal_id;
    memcpy(rx->key, key, sizeof rx->key);
    memset(key, 0, sizeof key);

    *out_len = (size_t)n;
    return DH_SEAL_OK;
}

bool dh_seal_tx_stale(dh_seal_tx *tx, uint32_t seal_id) {
    if ((!tx->live && !tx->offered) || tx->seal_id != seal_id)
        return false;
    dh_seal_tx_init(tx);
    return true;
}

int dh_seal_encode_stale(uint32_t seal_id, uint8_t *out, size_t cap) {
    if (cap < (size_t)DH_SEAL_STALE_LEN)
        return -1;
    put32(out, seal_id);
    return (int)DH_SEAL_STALE_LEN;
}

bool dh_seal_decode_stale(const uint8_t *body, size_t len, uint32_t *seal_id) {
    if (len != (size_t)DH_SEAL_STALE_LEN)
        return false;
    *seal_id = get32(body);
    return true;
}

bool dh_seal_peek_id(uint8_t type, const uint8_t *body, size_t len, uint32_t *seal_id) {
    if (type == DH_MSG_CLIP_OFFER) {
        dh_clip_offer_head head;
        if (!dh_clip_decode_offer_head(body, len, &head))
            return false;
        *seal_id = head.seal_id;
        return true;
    }
    if (type == DH_MSG_CLIP_CHUNK) {
        dh_clip_chunk_head head;
        if (!dh_clip_decode_chunk_head(body, len, &head))
            return false;
        *seal_id = head.seal_id;
        return true;
    }
    return false;
}

/*
 * Seal a message that is already staged as `head_len` clear bytes at the front
 * of `out` and `plain_len` plaintext bytes behind them. The head is the AAD, so
 * editing an id, a sequence number or a counter in flight stops the frame
 * opening; the tag lands last.
 */
static dh_seal_result seal_staged(dh_seal_tx *tx, const dh_seal_aead *aead, uint8_t *out,
                                  size_t head_len, size_t plain_len, size_t *out_len) {
    uint8_t nonce[DH_SEAL_NONCE_SIZE];
    seal_nonce(tx->counter, nonce);

    uint8_t *cipher = out + head_len;
    if (!aead->seal(aead->ctx, tx->key, nonce, out, head_len, cipher, plain_len, cipher,
                    cipher + plain_len)) {
        /* The plaintext was staged in the caller's buffer for the cipher to
           encrypt in place, so a cipher that failed leaves it sitting there in
           clear. Nothing may send it, and nothing should be able to find it. */
        memset(cipher, 0, plain_len);
        return DH_SEAL_ERR_AUTH;
    }

    tx->counter++;
    *out_len = head_len + plain_len + DH_SEAL_TAG_SIZE;
    return DH_SEAL_OK;
}

dh_seal_result dh_seal_encode_offer(dh_seal_tx *tx, const dh_seal_aead *aead,
                                    const dh_clip_offer *offer, uint8_t *out, size_t cap,
                                    size_t *out_len) {
    *out_len = 0;
    if (!tx->live)
        return DH_SEAL_ERR_NO_SEAL;

    const size_t body_len = (size_t)DH_SEAL_OFFER_OVERHEAD + offer->meta_len;
    if (cap < body_len || body_len > SEAL_BODY_MAX)
        return DH_SEAL_ERR_BUFFER;

    const dh_clip_offer_head head = {
        .id = offer->id,
        .seal_id = tx->seal_id,
        .seal_counter = tx->counter,
    };
    if (dh_clip_encode_offer_head(&head, out, cap) < 0)
        return DH_SEAL_ERR_BUFFER;

    const int plain_len = dh_clip_encode_offer_plain(offer, out + DH_CLIP_OFFER_HEAD_LEN,
                                                     cap - DH_CLIP_OFFER_HEAD_LEN);
    if (plain_len < 0)
        return DH_SEAL_ERR_BUFFER;

    return seal_staged(tx, aead, out, DH_CLIP_OFFER_HEAD_LEN, (size_t)plain_len, out_len);
}

dh_seal_result dh_seal_encode_chunk(dh_seal_tx *tx, const dh_seal_aead *aead,
                                    const dh_clip_chunk *chunk, uint8_t *out, size_t cap,
                                    size_t *out_len) {
    *out_len = 0;
    if (!tx->live)
        return DH_SEAL_ERR_NO_SEAL;

    const size_t body_len = (size_t)DH_SEAL_CHUNK_OVERHEAD + chunk->data_len;
    if (cap < body_len || body_len > SEAL_BODY_MAX)
        return DH_SEAL_ERR_BUFFER;

    const dh_clip_chunk_head head = {
        .id = chunk->id,
        .seq = chunk->seq,
        .seal_id = tx->seal_id,
        .seal_counter = tx->counter,
    };
    if (dh_clip_encode_chunk_head(&head, out, cap) < 0)
        return DH_SEAL_ERR_BUFFER;

    const int plain_len = dh_clip_encode_chunk_plain(chunk, out + DH_CLIP_CHUNK_HEAD_LEN,
                                                     cap - DH_CLIP_CHUNK_HEAD_LEN);
    if (plain_len < 0)
        return DH_SEAL_ERR_BUFFER;

    return seal_staged(tx, aead, out, DH_CLIP_CHUNK_HEAD_LEN, (size_t)plain_len, out_len);
}

/*
 * The half of opening that both messages share: check this end holds the named
 * key, then decrypt the ciphertext behind the head into the caller's buffer.
 *
 * The counter is a nonce here and not a replay window. Replay is answered a
 * layer down on each hop (dh_auth's counter) and a layer up by the transfer's
 * received-set, which ignores a chunk it already holds.
 */
static dh_seal_result open_staged(const dh_seal_rx *rx, const dh_seal_aead *aead,
                                  const uint8_t *body, size_t len, size_t head_len,
                                  uint32_t named_seal_id, uint64_t counter, uint8_t *plain,
                                  size_t plain_cap, size_t *plain_len) {
    if (len < head_len + DH_SEAL_TAG_SIZE)
        return DH_SEAL_ERR_MALFORMED;
    if (!rx->live || rx->seal_id != named_seal_id)
        return DH_SEAL_ERR_UNKNOWN_ID;

    const size_t cipher_len = len - head_len - DH_SEAL_TAG_SIZE;
    if (cipher_len > plain_cap)
        return DH_SEAL_ERR_BUFFER;

    uint8_t nonce[DH_SEAL_NONCE_SIZE];
    seal_nonce(counter, nonce);

    if (!aead->open(aead->ctx, rx->key, nonce, body, head_len, body + head_len, cipher_len,
                    body + head_len + cipher_len, plain))
        return DH_SEAL_ERR_AUTH;

    *plain_len = cipher_len;
    return DH_SEAL_OK;
}

dh_seal_result dh_seal_open_offer(const dh_seal_rx *rx, const dh_seal_aead *aead,
                                  const uint8_t *body, size_t len, uint8_t *plain,
                                  size_t plain_cap, dh_clip_offer *out) {
    dh_clip_offer_head head;
    if (!dh_clip_decode_offer_head(body, len, &head))
        return DH_SEAL_ERR_MALFORMED;

    size_t plain_len = 0;
    const dh_seal_result rc = open_staged(rx, aead, body, len, DH_CLIP_OFFER_HEAD_LEN,
                                          head.seal_id, head.seal_counter, plain, plain_cap,
                                          &plain_len);
    if (rc != DH_SEAL_OK)
        return rc;

    if (!dh_clip_decode_offer_plain(plain, plain_len, out))
        return DH_SEAL_ERR_MALFORMED;
    out->id = head.id;
    return DH_SEAL_OK;
}

dh_seal_result dh_seal_open_chunk(const dh_seal_rx *rx, const dh_seal_aead *aead,
                                  const uint8_t *body, size_t len, uint8_t *plain,
                                  size_t plain_cap, dh_clip_chunk *out) {
    dh_clip_chunk_head head;
    if (!dh_clip_decode_chunk_head(body, len, &head))
        return DH_SEAL_ERR_MALFORMED;

    size_t plain_len = 0;
    const dh_seal_result rc = open_staged(rx, aead, body, len, DH_CLIP_CHUNK_HEAD_LEN,
                                          head.seal_id, head.seal_counter, plain, plain_cap,
                                          &plain_len);
    if (rc != DH_SEAL_OK)
        return rc;

    if (!dh_clip_decode_chunk_plain(plain, plain_len, out))
        return DH_SEAL_ERR_MALFORMED;
    out->id = head.id;
    out->seq = head.seq;
    return DH_SEAL_OK;
}
