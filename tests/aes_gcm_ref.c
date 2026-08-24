/*
 * A reference AES-256-GCM for the host suite. See aes_gcm_ref.h for why it
 * lives in tests/ rather than in the core. Straight from NIST SP 800-38D, with
 * the 96-bit nonce case only.
 */

#include "aes_gcm_ref.h"

#include <string.h>

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab,
    0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
    0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71,
    0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6,
    0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
    0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45,
    0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44,
    0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
    0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49,
    0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
    0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
    0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1,
    0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb,
    0x16};

static uint8_t xtime(uint8_t b) {
    return (uint8_t)((b & 0x80u) ? ((uint8_t)(b << 1) ^ 0x1bu) : (uint8_t)(b << 1));
}

/* AES-256: 8 key words in, 15 round keys out. */
static void expand(const uint8_t key[32], uint8_t rk[15][16]) {
    uint8_t w[60][4];
    memcpy(w, key, 32);

    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint8_t t[4];
        memcpy(t, w[i - 1], 4);
        if (i % 8 == 0) {
            const uint8_t first = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[first];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; j++)
                t[j] = SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++)
            w[i][j] = (uint8_t)(w[i - 8][j] ^ t[j]);
    }
    for (int r = 0; r < 15; r++)
        memcpy(rk[r], w[r * 4], 16);
}

static void encrypt_block(const uint8_t rk[15][16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++)
        s[i] = (uint8_t)(in[i] ^ rk[0][i]);

    for (int round = 1; round < 15; round++) {
        for (int i = 0; i < 16; i++)
            s[i] = SBOX[s[i]];

        /* ShiftRows: the state is column-major, so row r of column c is byte
           4c+r and row r rotates left by r columns. */
        for (int i = 0; i < 16; i++)
            t[i] = s[4 * (((i / 4) + (i % 4)) % 4) + (i % 4)];
        memcpy(s, t, 16);

        if (round != 14) {
            for (int c = 0; c < 4; c++) {
                const uint8_t *col = s + 4 * c;
                for (int r = 0; r < 4; r++) {
                    const uint8_t a = col[r], b = col[(r + 1) % 4];
                    t[4 * c + r] = (uint8_t)(xtime(a) ^ xtime(b) ^ b ^ col[(r + 2) % 4] ^
                                             col[(r + 3) % 4]);
                }
            }
            memcpy(s, t, 16);
        }

        for (int i = 0; i < 16; i++)
            s[i] ^= rk[round][i];
    }
    memcpy(out, s, 16);
}

/* Multiplication in GF(2^128), bit by bit — the definition, not a fast one. */
static void gf_mul(uint8_t z[16], const uint8_t x[16], const uint8_t y[16]) {
    uint8_t v[16], acc[16] = {0};
    memcpy(v, y, 16);

    for (int i = 0; i < 128; i++) {
        if ((x[i / 8] >> (7 - (i % 8))) & 1u) {
            for (int j = 0; j < 16; j++)
                acc[j] ^= v[j];
        }
        const int carry = v[15] & 1;
        for (int j = 15; j > 0; j--)
            v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1u) << 7));
        v[0] = (uint8_t)(v[0] >> 1);
        if (carry)
            v[0] ^= 0xe1u;
    }
    memcpy(z, acc, 16);
}

/* GHASH one run of data into `y`, zero-padding a short final block. */
static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        uint8_t block[16] = {0};
        const size_t n = (len - i < 16) ? len - i : 16;
        memcpy(block, data + i, n);
        for (int j = 0; j < 16; j++)
            y[j] ^= block[j];
        uint8_t z[16];
        gf_mul(z, y, h);
        memcpy(y, z, 16);
    }
}

static void put_be64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * (7 - i))) & 0xffu);
}

/* The counter half of GCM: J0 is the 96-bit nonce with a 1, and each block is
   encrypted under the 32-bit counter incremented first. */
static void gctr(const uint8_t rk[15][16], const uint8_t j0[16], const uint8_t *in, size_t len,
                 uint8_t *out) {
    uint8_t counter[16];
    memcpy(counter, j0, 16);

    for (size_t i = 0; i < len; i += 16) {
        for (int j = 15; j >= 12; j--) {
            if (++counter[j] != 0)
                break;
        }
        uint8_t stream[16];
        encrypt_block(rk, counter, stream);
        const size_t n = (len - i < 16) ? len - i : 16;
        for (size_t j = 0; j < n; j++)
            out[i + j] = (uint8_t)(in[i + j] ^ stream[j]);
    }
}

/* The hash subkey and the first counter block, which both halves need. */
static void gcm_setup(const uint8_t key[32], const uint8_t nonce[12], uint8_t rk[15][16],
                      uint8_t h[16], uint8_t j0[16]) {
    expand(key, rk);

    memset(h, 0, 16);
    encrypt_block(rk, h, h);

    memcpy(j0, nonce, 12);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
}

static void gcm_tag(const uint8_t rk[15][16], const uint8_t h[16], const uint8_t j0[16],
                    const uint8_t *aad, size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                    uint8_t tag_out[16]) {
    uint8_t y[16] = {0};
    ghash(y, h, aad, aad_len);
    ghash(y, h, cipher, cipher_len);

    uint8_t lengths[16];
    put_be64(lengths, (uint64_t)aad_len * 8u);
    put_be64(lengths + 8, (uint64_t)cipher_len * 8u);
    ghash(y, h, lengths, sizeof lengths);

    uint8_t mask[16];
    encrypt_block(rk, j0, mask);
    for (int i = 0; i < 16; i++)
        tag_out[i] = (uint8_t)(y[i] ^ mask[i]);
}

void aes_gcm_ref_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                      size_t aad_len, const uint8_t *plain, size_t plain_len, uint8_t *cipher_out,
                      uint8_t tag_out[16]) {
    uint8_t rk[15][16], h[16], j0[16];
    gcm_setup(key, nonce, rk, h, j0);
    gctr(rk, j0, plain, plain_len, cipher_out);
    gcm_tag(rk, h, j0, aad, aad_len, cipher_out, plain_len, tag_out);
}

bool aes_gcm_ref_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                      size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                      const uint8_t tag[16], uint8_t *plain_out) {
    uint8_t rk[15][16], h[16], j0[16];
    gcm_setup(key, nonce, rk, h, j0);

    /* The tag first, over the ciphertext as it arrived: a frame that does not
       authenticate must leave no plaintext behind, and `plain_out` is allowed
       to be the ciphertext buffer itself. */
    uint8_t computed[16];
    gcm_tag(rk, h, j0, aad, aad_len, cipher, cipher_len, computed);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff = (uint8_t)(diff | (computed[i] ^ tag[i]));
    if (diff != 0)
        return false;

    gctr(rk, j0, cipher, cipher_len, plain_out);
    return true;
}

static bool aead_seal(void *ctx, const uint8_t key[DH_SEAL_KEY_SIZE],
                      const uint8_t nonce[DH_SEAL_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
                      const uint8_t *plain, size_t plain_len, uint8_t *cipher_out,
                      uint8_t tag_out[DH_SEAL_TAG_SIZE]) {
    (void)ctx;
    aes_gcm_ref_seal(key, nonce, aad, aad_len, plain, plain_len, cipher_out, tag_out);
    return true;
}

static bool aead_open(void *ctx, const uint8_t key[DH_SEAL_KEY_SIZE],
                      const uint8_t nonce[DH_SEAL_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
                      const uint8_t *cipher, size_t cipher_len, const uint8_t tag[DH_SEAL_TAG_SIZE],
                      uint8_t *plain_out) {
    (void)ctx;
    return aes_gcm_ref_open(key, nonce, aad, aad_len, cipher, cipher_len, tag, plain_out);
}

const dh_seal_aead *aes_gcm_ref_aead(void) {
    static const dh_seal_aead aead = {NULL, aead_seal, aead_open};
    return &aead;
}
