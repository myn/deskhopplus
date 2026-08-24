/*
 * A reference AES-256-GCM, for the host tests only (#113).
 *
 * It is here and not in src/core/ on purpose. ADR-0008 put the seal's cipher on
 * the platform — CryptoKit on macOS, CNG on Windows — so that neither helper
 * ships a hand-written AES and the firmware links none at all. The C suite
 * still has to be able to open what the core sealed, so it carries its own
 * plain implementation, gated by the published NIST GCM test cases in
 * seal_test.c before anything else uses it.
 *
 * Correctness only: no attempt at constant time, and nothing here should ever
 * be linked into something that ships.
 */

#ifndef AES_GCM_REF_H_
#define AES_GCM_REF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_seal.h"

/* 96-bit nonce, 128-bit tag — the only shape docs/protocol.md uses.
   `cipher_out` may alias `plain`, and `plain_out` may alias `cipher`. */
void aes_gcm_ref_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                      size_t aad_len, const uint8_t *plain, size_t plain_len, uint8_t *cipher_out,
                      uint8_t tag_out[16]);

/* False when the tag does not verify, and then nothing is written to
   `plain_out`. */
bool aes_gcm_ref_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                      size_t aad_len, const uint8_t *cipher, size_t cipher_len,
                      const uint8_t tag[16], uint8_t *plain_out);

/* The same two, as the seam dh_seal.h takes. */
const dh_seal_aead *aes_gcm_ref_aead(void);

#endif /* AES_GCM_REF_H_ */
