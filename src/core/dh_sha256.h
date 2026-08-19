/*
 * deskhopplus shared core — SHA-256, HMAC-SHA256 and HKDF-SHA256 (#110).
 *
 * Three of the four primitives the board runs, per ADR-0008 and
 * docs/protocol.md v2. The fourth is P-256 (dh_p256.h). There is no AEAD
 * here and there is not meant to be: the clipboard seal is AES-256-GCM
 * between the two helpers, so the firmware never links a cipher at all.
 *
 * Written out rather than pulled in because the RP2040 has no hash
 * accelerator and this core links against nothing: it compiles into the
 * firmware, into the host suite, and into both helpers' bindings.
 *
 * Pure C11: no allocation, no I/O, no clock, no entropy source. Gated by
 * test-vectors/primitives.txt, whose SHA-256, HMAC and HKDF vectors are
 * FIPS 180-4's, RFC 4231's and RFC 5869's published answers.
 */

#ifndef DH_SHA256_H_
#define DH_SHA256_H_

#include <stddef.h>
#include <stdint.h>

#define DH_SHA256_DIGEST_SIZE 32u
#define DH_SHA256_BLOCK_SIZE 64u

typedef struct {
    uint32_t state[8];
    uint64_t bits;                       /* message length so far, in bits */
    uint8_t block[DH_SHA256_BLOCK_SIZE]; /* the partial block not yet compressed */
    size_t have;
} dh_sha256_ctx;

void dh_sha256_init(dh_sha256_ctx *c);
void dh_sha256_update(dh_sha256_ctx *c, const uint8_t *data, size_t len);
void dh_sha256_final(dh_sha256_ctx *c, uint8_t out[DH_SHA256_DIGEST_SIZE]);

/* One-shot, for the common case. */
void dh_sha256(const uint8_t *data, size_t len, uint8_t out[DH_SHA256_DIGEST_SIZE]);

/*
 * HMAC-SHA256. Streaming, because the frame tag covers three pieces that are
 * never contiguous in memory — the wire header, the counter, and a body that
 * points into a receive buffer — and copying a 4 KB payload to tag it would
 * be a second copy of every clipboard chunk.
 */
typedef struct {
    dh_sha256_ctx inner;
    uint8_t opad[DH_SHA256_BLOCK_SIZE];
} dh_hmac_sha256_ctx;

void dh_hmac_sha256_init(dh_hmac_sha256_ctx *c, const uint8_t *key, size_t key_len);
void dh_hmac_sha256_update(dh_hmac_sha256_ctx *c, const uint8_t *data, size_t len);
void dh_hmac_sha256_final(dh_hmac_sha256_ctx *c, uint8_t out[DH_SHA256_DIGEST_SIZE]);

void dh_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                    uint8_t out[DH_SHA256_DIGEST_SIZE]);

/*
 * HKDF-SHA256, extract and expand in one call — nothing in this protocol
 * derives twice from one PRK, so there is no reason to expose the halves.
 *
 * out_len must be at most DH_HKDF_MAX_OUTPUT — the expand counter is one
 * byte — and a larger request writes nothing at all. Every key here is 32
 * bytes. A NULL salt or info means zero length, which is RFC 5869 case 3.
 */
#define DH_HKDF_MAX_OUTPUT (255u * DH_SHA256_DIGEST_SIZE)

void dh_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                    const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

#endif /* DH_SHA256_H_ */
