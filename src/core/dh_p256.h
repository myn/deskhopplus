/*
 * deskhopplus shared core — P-256 key pairs and ECDH (#110).
 *
 * The curve is not a preference. ADR-0008 records that the macOS helper's
 * key lives in the Secure Enclave, which does P-256 and nothing else, and
 * that a private key a same-user process could read out of a file would undo
 * the whole wire fix. Key storage chose the curve; the board follows it.
 *
 * Public keys are 64 raw bytes, X || Y, big-endian — micro-ecc's native form
 * and CryptoKit's `rawRepresentation`, so no end adds or strips an X9.63
 * `0x04` prefix (docs/protocol.md, "Identities").
 *
 * No allocation, no I/O, no clock, and no entropy source: a private key is
 * 32 bytes the caller hands in, exactly as dh_pair_open_window already takes
 * its fresh secret. A core that cannot be tested deterministically is a core
 * whose security property cannot be tested at all.
 */

#ifndef DH_P256_H_
#define DH_P256_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

#define DH_P256_PRIVATE_SIZE 32u
#define DH_P256_PUBLIC_SIZE 64u
#define DH_P256_SHARED_SIZE 32u

/* A key id names a key on the wire without carrying one: SHA-256(public)[0..8]. */
#define DH_KEY_ID_SIZE 8u

/*
 * The public key for a private key the caller drew.
 *
 * Returns false when those 32 bytes are not a scalar in [1, n-1] — draw
 * again. Random bytes fail this about once in 2^32 draws, which is rare
 * enough to be a retry and far too common to be an assertion.
 */
bool dh_p256_public_from_private(const uint8_t private_key[DH_P256_PRIVATE_SIZE],
                                 uint8_t public_key[DH_P256_PUBLIC_SIZE]);

/*
 * Is this 64 bytes a point on P-256? A public key arrives from the wire, in
 * a pairing request anything attached to the channel can send, so this is a
 * decision about hostile input rather than a check on our own material.
 */
bool dh_p256_public_valid(const uint8_t public_key[DH_P256_PUBLIC_SIZE]);

/*
 * One ECDH. The shared secret is the X coordinate of d·Q, 32 bytes, which is
 * what docs/protocol.md feeds to HKDF as `ss`. The peer key is validated
 * here, so a caller cannot skip it.
 *
 * This is the board's only asymmetric work, and it happens at pairing and
 * nowhere else — every session afterwards is a hash. Time it before putting
 * it anywhere near the 2000 Hz loop in src/main.c.
 */
bool dh_p256_ecdh(const uint8_t private_key[DH_P256_PRIVATE_SIZE],
                  const uint8_t peer_public_key[DH_P256_PUBLIC_SIZE],
                  uint8_t shared_secret[DH_P256_SHARED_SIZE]);

/* SHA-256(public)[0..8] — the id a hello carries in place of a key. */
void dh_p256_key_id(const uint8_t public_key[DH_P256_PUBLIC_SIZE],
                    uint8_t out[DH_KEY_ID_SIZE]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_P256_H_ */
