/*
 * deskhopplus shared core — P-256 key pairs and ECDH (#110). See dh_p256.h.
 *
 * This file is the only one that compiles micro-ecc. The library is included
 * as source rather than built separately so that its configuration is set
 * once, here, for all three of this core's toolchains — the firmware's
 * arm-none-eabi, the host suite's cc, and MSVC (#82). Three build files
 * carrying the same -D flags would be three places for them to drift, and a
 * drifted curve list is a defect nothing would report until the vectors
 * disagreed.
 *
 * micro-ecc itself is vendored verbatim: see micro-ecc/VENDOR.md, and do not
 * patch those files.
 */

#include "dh_p256.h"

#include <string.h>

#include "dh_sha256.h"

/* One curve, because ADR-0008 chose one curve — the Secure Enclave does P-256
   and nothing else. The other four would be code the board carries and never
   reaches. Compressed points likewise: public keys cross this wire as 64 raw
   bytes, X || Y, and nothing here ever sees a prefix byte. */
#define uECC_SUPPORTS_secp160r1 0
#define uECC_SUPPORTS_secp192r1 0
#define uECC_SUPPORTS_secp224r1 0
#define uECC_SUPPORTS_secp256r1 1
#define uECC_SUPPORTS_secp256k1 0
#define uECC_SUPPORT_COMPRESSED_POINT 0

/*
 * And no entropy source, which takes suppressing rather than configuring.
 *
 * micro-ecc's platform-specific.inc defines a default RNG on every host it
 * knows — /dev/urandom on POSIX, CryptGenRandom on Windows — and uECC.c
 * initialises its global RNG pointer to it. That would put an open(), a
 * read() and a close() inside this core on the host builds, and pull
 * crypt32 into the Windows one against ADR-0006's inbox-only rule, for a
 * function nothing here ever calls: #110 requires the entropy to come from
 * the caller, as dh_pair_open_window already takes its fresh secret.
 *
 * Defining the include guard ahead of the include is what leaves it out.
 * micro-ecc offers no switch for this and must not be patched, so the
 * #error below is the safety net: if a future upstream renames that guard,
 * this stops compiling rather than quietly regaining a file descriptor.
 *
 * The name is a reserved identifier and is spelled that way on purpose — it
 * has to be the one micro-ecc uses, and no other spelling does anything.
 */
#define _UECC_PLATFORM_SPECIFIC_H_

/* Third-party and vendored verbatim, so #82's rule — a defect MSVC finds in
   the core is fixed in the core, never suppressed — cannot apply to it: there
   is nothing here to fix without patching upstream. Level 3 keeps the real
   ones visible. If the Windows build turns warnings into errors and micro-ecc
   trips one, lower this number; do not edit micro-ecc/. */
#if defined(_MSC_VER)
#pragma warning(push, 3)
#endif

#include "micro-ecc/uECC.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(default_RNG_defined)
#error "micro-ecc compiled in a default RNG: this core has no entropy source (#110)"
#endif

/*
 * The RNG pointer is already null with platform-specific.inc left out, so
 * this is the belt to that brace: it holds even if a future micro-ecc gains
 * a default by some other route, and it is what auth_test asserts, because
 * nothing else would notice the difference — the answers would be identical
 * and the file descriptor invisible.
 *
 * What having no RNG gives up is micro-ecc's random-Z blinding of the scalar
 * multiply, which defends against power and EM analysis. That is not the
 * threat here and cannot be: the board's private key sits in an RP2040 flash
 * sector with no secure boot and no flash encryption, so an attacker holding
 * the board reads the key out rather than measuring it. The adversary
 * ADR-0008 works to is a process on the *host*, which sees none of this.
 */
static void no_entropy_source(void) {
    uECC_set_rng(0);
}

bool dh_p256_public_from_private(const uint8_t private_key[DH_P256_PRIVATE_SIZE],
                                 uint8_t public_key[DH_P256_PUBLIC_SIZE]) {
    no_entropy_source();
    return uECC_compute_public_key(private_key, public_key, uECC_secp256r1()) == 1;
}

bool dh_p256_public_valid(const uint8_t public_key[DH_P256_PUBLIC_SIZE]) {
    no_entropy_source();
    return uECC_valid_public_key(public_key, uECC_secp256r1()) == 1;
}

bool dh_p256_ecdh(const uint8_t private_key[DH_P256_PRIVATE_SIZE],
                  const uint8_t peer_public_key[DH_P256_PUBLIC_SIZE],
                  uint8_t shared_secret[DH_P256_SHARED_SIZE]) {
    no_entropy_source();

    /* uECC_shared_secret does not validate the peer key — its own header says
       so. The key arrives in a pairing request that anything attached to the
       channel can send, so the check is not optional and is made here rather
       than left to each caller. */
    if (uECC_valid_public_key(peer_public_key, uECC_secp256r1()) != 1)
        return false;

    return uECC_shared_secret(peer_public_key, private_key, shared_secret,
                              uECC_secp256r1()) == 1;
}

void dh_p256_key_id(const uint8_t public_key[DH_P256_PUBLIC_SIZE], uint8_t out[DH_KEY_ID_SIZE]) {
    uint8_t digest[DH_SHA256_DIGEST_SIZE];
    dh_sha256(public_key, DH_P256_PUBLIC_SIZE, digest);
    memcpy(out, digest, DH_KEY_ID_SIZE);
}
