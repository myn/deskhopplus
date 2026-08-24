#pragma once
/*
 * This helper's half of the clipboard seal (#113, ADR-0008): AES-256-GCM,
 * from CNG.
 *
 * Everything that decides *what* is sealed, under which key and with which
 * nonce is in the shared core (`dh_seal.h`), for the reason the session
 * machine is there too: a rule both helpers must agree on cannot have two
 * implementations to drift between. All that is left for a platform is the
 * cipher, and ADR-0008 chose AES-256-GCM because both platforms already have
 * it — so nothing here is hand-written crypto, and the firmware links no
 * cipher at all.
 *
 * bcrypt is already on ADR-0006's list of inbox libraries, for
 * BCryptGenRandom; this uses the same one.
 *
 * ChaCha20-Poly1305 would have been the other candidate and was rejected for
 * exactly this file: CNG only offers it on Windows 11, and #84 targets a
 * managed laptop.
 */

#include "dh_seal.h"

namespace deskhop {

/*
 * The cipher, as the seam `dh_seal.h` takes. The returned pointer is valid for
 * the life of the process, and the callbacks behind it are safe to use from
 * any thread — each call makes and destroys its own key handle.
 *
 * Returns nullptr when CNG will not give up an AES-GCM provider, which is a
 * machine that cannot run the seal at all rather than one that should fall
 * back to sending a clipboard payload in clear.
 */
const dh_seal_aead *seal_aead();

} /* namespace deskhop */
