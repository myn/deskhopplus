/*
 * deskhopplus shared core — SHA-256, HMAC-SHA256 and HKDF-SHA256 (#110).
 * See dh_sha256.h. FIPS 180-4, RFC 2104 and RFC 5869; gated against those
 * documents' own published answers in test-vectors/primitives.txt.
 */

#include "dh_sha256.h"

#include <string.h>

/* FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the
   cube roots of the first sixty-four primes. */
static const uint32_t DH_SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (uint32_t)((x >> n) | (x << (32u - n)));
}

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void dh_sha256_compress(uint32_t state[8], const uint8_t block[DH_SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4u);
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + DH_SHA256_K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void dh_sha256_init(dh_sha256_ctx *c) {
    /* FIPS 180-4 section 5.3.3: the fractional parts of the square roots of
       the first eight primes. */
    c->state[0] = 0x6a09e667u;
    c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u;
    c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu;
    c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu;
    c->state[7] = 0x5be0cd19u;
    c->bits = 0;
    c->have = 0;
    memset(c->block, 0, sizeof c->block);
}

void dh_sha256_update(dh_sha256_ctx *c, const uint8_t *data, size_t len) {
    c->bits += (uint64_t)len * 8u;

    if (c->have > 0) {
        size_t want = DH_SHA256_BLOCK_SIZE - c->have;
        size_t take = len < want ? len : want;
        memcpy(c->block + c->have, data, take);
        c->have += take;
        data += take;
        len -= take;
        if (c->have < DH_SHA256_BLOCK_SIZE)
            return;
        dh_sha256_compress(c->state, c->block);
        c->have = 0;
    }

    while (len >= DH_SHA256_BLOCK_SIZE) {
        dh_sha256_compress(c->state, data);
        data += DH_SHA256_BLOCK_SIZE;
        len -= DH_SHA256_BLOCK_SIZE;
    }

    if (len > 0) {
        memcpy(c->block, data, len);
        c->have = len;
    }
}

void dh_sha256_final(dh_sha256_ctx *c, uint8_t out[DH_SHA256_DIGEST_SIZE]) {
    uint64_t bits = c->bits;

    /* One 0x80 byte, then zeros, then the length in the last 8 bytes. When
       the length no longer fits, the padding runs into a second block —
       which is the boundary the sha256_pad_* vectors sit either side of. */
    static const uint8_t one = 0x80u;
    static const uint8_t zeros[DH_SHA256_BLOCK_SIZE] = {0};
    dh_sha256_update(c, &one, 1);
    c->bits = bits; /* padding is not message length */

    size_t pad = (c->have <= 56u) ? (56u - c->have) : (120u - c->have);
    if (pad > 0) {
        dh_sha256_update(c, zeros, pad);
        c->bits = bits;
    }

    uint8_t length[8];
    for (unsigned i = 0; i < 8; i++)
        length[i] = (uint8_t)(bits >> (56u - i * 8u));
    dh_sha256_update(c, length, sizeof length);

    for (unsigned i = 0; i < 8; i++)
        store_be32(out + i * 4u, c->state[i]);
}

void dh_sha256(const uint8_t *data, size_t len, uint8_t out[DH_SHA256_DIGEST_SIZE]) {
    dh_sha256_ctx c;
    dh_sha256_init(&c);
    dh_sha256_update(&c, data, len);
    dh_sha256_final(&c, out);
}

void dh_hmac_sha256_init(dh_hmac_sha256_ctx *c, const uint8_t *key, size_t key_len) {
    uint8_t block[DH_SHA256_BLOCK_SIZE];
    memset(block, 0, sizeof block);

    /* RFC 2104: a key longer than the block is hashed first, a shorter one is
       zero-padded. So an empty key and a key of block-length zeros are the
       same key, which is why RFC 5869's absent salt needs no special case. */
    if (key_len > DH_SHA256_BLOCK_SIZE)
        dh_sha256(key, key_len, block);
    else if (key_len > 0)
        memcpy(block, key, key_len);

    uint8_t ipad[DH_SHA256_BLOCK_SIZE];
    for (size_t i = 0; i < DH_SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (uint8_t)(block[i] ^ 0x36u);
        c->opad[i] = (uint8_t)(block[i] ^ 0x5cu);
    }

    dh_sha256_init(&c->inner);
    dh_sha256_update(&c->inner, ipad, sizeof ipad);
}

void dh_hmac_sha256_update(dh_hmac_sha256_ctx *c, const uint8_t *data, size_t len) {
    dh_sha256_update(&c->inner, data, len);
}

void dh_hmac_sha256_final(dh_hmac_sha256_ctx *c, uint8_t out[DH_SHA256_DIGEST_SIZE]) {
    uint8_t inner[DH_SHA256_DIGEST_SIZE];
    dh_sha256_final(&c->inner, inner);

    dh_sha256_ctx outer;
    dh_sha256_init(&outer);
    dh_sha256_update(&outer, c->opad, sizeof c->opad);
    dh_sha256_update(&outer, inner, sizeof inner);
    dh_sha256_final(&outer, out);
}

void dh_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                    uint8_t out[DH_SHA256_DIGEST_SIZE]) {
    dh_hmac_sha256_ctx c;
    dh_hmac_sha256_init(&c, key, key_len);
    dh_hmac_sha256_update(&c, data, len);
    dh_hmac_sha256_final(&c, out);
}

void dh_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                    const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len) {
    /* Extract. RFC 5869: the salt is the HMAC key and the input keying
       material is the message, which reads backwards and is correct. */
    uint8_t prk[DH_SHA256_DIGEST_SIZE];
    dh_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    /* Expand. T(0) is empty; T(n) = HMAC(prk, T(n-1) || info || n). */
    uint8_t block[DH_SHA256_DIGEST_SIZE];
    size_t have = 0;
    uint8_t counter = 0;
    while (have < out_len) {
        counter++;
        dh_hmac_sha256_ctx c;
        dh_hmac_sha256_init(&c, prk, sizeof prk);
        if (counter > 1)
            dh_hmac_sha256_update(&c, block, sizeof block);
        if (info_len > 0)
            dh_hmac_sha256_update(&c, info, info_len);
        dh_hmac_sha256_update(&c, &counter, 1);
        dh_hmac_sha256_final(&c, block);

        size_t take = out_len - have;
        if (take > sizeof block)
            take = sizeof block;
        memcpy(out + have, block, take);
        have += take;
    }
}
