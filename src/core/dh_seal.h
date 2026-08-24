/*
 * deskhopplus shared core — the end-to-end clipboard seal (#113).
 *
 * ADR-0008's other half. dh_auth.h authenticates one *hop*: helper to board,
 * board to helper, with the tag rewritten at each end. This file encrypts the
 * bulk payload *end to end*, between the two helpers, so that the boards relay
 * ciphertext and hold no key that opens it. That turns CONTEXT.md's **opaque
 * relay** from a discipline into a property: a board relays bytes it could not
 * read even if it wanted to, which matters because ADR-0007 already accepts
 * that a compromised board can flash its peer.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS HERE AND WHAT IS NOT
 *
 * **Here:** the seal exchange, the key agreement over it, the counter that
 * feeds the nonce, and the layout of a sealed CLIP_OFFER and CLIP_CHUNK.
 *
 * **Not here: AES-256-GCM.** ADR-0008 chose it because both helpers already
 * have it — CryptoKit on macOS, CNG on Windows — so the cipher arrives through
 * `dh_seal_aead` and this core links none. The board links none either, and
 * more than that: the board never instantiates this file at all. Its primitive
 * set stays SHA-256, HMAC, HKDF and P-256.
 *
 * **Not here: entropy.** An ephemeral private key and a nonce are bytes the
 * caller draws and hands in, exactly as dh_pair_open_window takes its fresh
 * secret and dh_session stages its nonce. A core that cannot be tested
 * deterministically is a core whose security property cannot be tested at all.
 * ---------------------------------------------------------------------------
 *
 * **One seal per direction, established by the sender.** A helper with bulk to
 * send and no live seal offers one; the receiver accepts. Two independent
 * one-directional seals cost one extra ECDH on each helper — a millisecond on
 * a laptop, no board work at all — and remove the whole question of who wins
 * when both sides offer at once. Each helper therefore holds a dh_seal_tx for
 * what it sends and a dh_seal_rx for what it receives.
 *
 * There is no development-build path through this file, and there must never
 * be one. #44's exemption is the *board's*, and the board is not a party to
 * this key.
 *
 * Pure C11: no allocation, no I/O, no clock. Layouts: docs/protocol.md, "The
 * sealed clipboard payload"; test-vectors/frames.txt is the gate.
 */

#ifndef DH_SEAL_H_
#define DH_SEAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_auth.h"
#include "dh_clip.h"
#include "dh_frame.h"
#include "dh_p256.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

#define DH_SEAL_KEY_SIZE 32u
#define DH_SEAL_NONCE_SIZE 12u /* GCM's 96-bit nonce, and the only width used */
#define DH_SEAL_TAG_SIZE 16u
#define DH_SEAL_ID_SIZE 4u

/*
 * The sizes, as enum constants rather than computed macros, for the reason
 * DH_SESSION_ABSENT_MS is one: a macro built out of other macros does not
 * survive into the bindings, and a helper re-deriving these sums in its own
 * language would be a second copy of the layout, free to drift from the wire.
 */
enum {
    /* SEAL_OFFER and SEAL_ACCEPT share a body: seal_id:u32 nonce:16 pubkey:64 */
    DH_SEAL_EXCHANGE_LEN = DH_SEAL_ID_SIZE + DH_NONCE_SIZE + DH_P256_PUBLIC_SIZE,
    /* SEAL_STALE: seal_id:u32 */
    DH_SEAL_STALE_LEN = DH_SEAL_ID_SIZE,

    /* What a sealed message costs beyond the bytes it carries. */
    DH_SEAL_OFFER_OVERHEAD =
        DH_CLIP_OFFER_HEAD_LEN + DH_CLIP_OFFER_PLAIN_FIXED + DH_SEAL_TAG_SIZE,
    DH_SEAL_CHUNK_OVERHEAD =
        DH_CLIP_CHUNK_HEAD_LEN + DH_CLIP_CHUNK_PLAIN_FIXED + DH_SEAL_TAG_SIZE,

    /*
     * The largest chunk of user data a frame can carry once the seal has been
     * paid for: the payload maximum, less the hop's authentication prefix, less
     * this file's 40 bytes. It is the arithmetic behind
     * DH_SESSION_CHUNK_CEILING, which is what a helper asking for more is
     * clamped to.
     */
    DH_SEAL_MAX_CHUNK_DATA =
        DH_FRAME_MAX_PAYLOAD - DH_FRAME_AUTH_PREFIX_SIZE - DH_SEAL_CHUNK_OVERHEAD,
};

typedef enum {
    DH_SEAL_OK = 0,
    DH_SEAL_ERR_MALFORMED = -1,
    /*
     * A sealed frame naming a seal this end holds no key for. Answered with
     * SEAL_STALE, which is the ordinary recovery when one side's session ended
     * and the other's did not — #107 measured 586 teardowns in sixteen hours,
     * so this is a routine event and not an attack.
     */
    DH_SEAL_ERR_UNKNOWN_ID = -2,
    DH_SEAL_ERR_AUTH = -3,   /* the GCM tag did not verify */
    DH_SEAL_ERR_BUFFER = -4, /* output buffer too small */
    DH_SEAL_ERR_NO_SEAL = -5,
    /* An ephemeral key that is not usable: 32 bytes that are not a scalar in
       [1, n-1] (draw again), or a peer key that is not a point on the curve. */
    DH_SEAL_ERR_KEY = -6,
} dh_seal_result;

/*
 * The cipher, supplied by the platform.
 *
 * `cipher_out` may alias `plain`, and `plain_out` may alias `cipher` — the
 * encoders below seal in place, which is what keeps a 4 KB clipboard chunk from
 * being copied a second time on its way to the wire. An implementation that
 * cannot encrypt in place must read its whole input before writing its output.
 *
 * `open` returns false when the tag does not verify, and must not leave
 * plaintext behind when it does.
 */
typedef struct {
    void *ctx; /* handed back to both callbacks; may be NULL */
    bool (*seal)(void *ctx, const uint8_t key[DH_SEAL_KEY_SIZE],
                 const uint8_t nonce[DH_SEAL_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
                 const uint8_t *plain, size_t plain_len, uint8_t *cipher_out,
                 uint8_t tag_out[DH_SEAL_TAG_SIZE]);
    bool (*open)(void *ctx, const uint8_t key[DH_SEAL_KEY_SIZE],
                 const uint8_t nonce[DH_SEAL_NONCE_SIZE], const uint8_t *aad, size_t aad_len,
                 const uint8_t *cipher, size_t cipher_len, const uint8_t tag[DH_SEAL_TAG_SIZE],
                 uint8_t *plain_out);
} dh_seal_aead;

/*
 * The sending half: this end offered a seal and seals everything it sends under
 * it.
 *
 * `counter` starts at 0 and increases by exactly one per sealed message. Since
 * the key is one-directional and every counter value is used once, no nonce is
 * ever reused under a key — which is the whole reason a retransmitted chunk is
 * re-sealed under a fresh counter rather than resent byte for byte.
 */
typedef struct {
    bool offered; /* an offer is out; no key until the accept arrives */
    bool live;
    uint32_t seal_id;
    uint8_t nonce[DH_NONCE_SIZE];          /* this end's, kept until the accept */
    uint8_t eph_private[DH_P256_PRIVATE_SIZE]; /* wiped once the key exists */
    uint8_t key[DH_SEAL_KEY_SIZE];
    uint64_t counter;
} dh_seal_tx;

/* The receiving half: the peer offered, this end accepted, and this is the key
   that opens what the peer sends. */
typedef struct {
    bool live;
    uint32_t seal_id;
    uint8_t key[DH_SEAL_KEY_SIZE];
} dh_seal_rx;

/*
 * Both halves start empty, and both are emptied again whenever this end's
 * session ends: re-offering is cheap, and the alternative is holding a key
 * whose peer may no longer exist.
 */
void dh_seal_tx_init(dh_seal_tx *tx);
void dh_seal_rx_init(dh_seal_rx *rx);

/*
 * Offer a seal. `eph_private` and `nonce` are freshly drawn by the caller, and
 * `seal_id` is a fresh random u32 that names the key in every frame sealed
 * under it — so a receiver knows which key to try rather than guessing.
 *
 * The ephemeral key is per seal and is **not** this helper's identity key: the
 * identity key on macOS lives in the Secure Enclave and cannot leave it, and a
 * seal needs no long-term identity anyway. What vouches for the peer is the
 * relay itself (docs/protocol.md, "What each board vouches for").
 *
 * Writes the SEAL_OFFER body. DH_SEAL_ERR_KEY means those 32 bytes are not a
 * usable scalar — draw again, which random bytes ask for about once in 2^32.
 */
dh_seal_result dh_seal_tx_offer(dh_seal_tx *tx, uint32_t seal_id,
                                const uint8_t eph_private[DH_P256_PRIVATE_SIZE],
                                const uint8_t nonce[DH_NONCE_SIZE], uint8_t *out, size_t cap,
                                size_t *out_len);

/*
 * The peer's SEAL_ACCEPT. The key exists from here on:
 *
 *   k_seal = HKDF(ikm  = ECDH(ephemeral private, peer ephemeral public),
 *                 salt = nonce_offer || nonce_accept,
 *                 info = "deskhopplus/2 seal")
 *
 * An accept whose seal_id is not the one this end offered is not this
 * exchange's and is ignored (DH_SEAL_ERR_UNKNOWN_ID).
 */
dh_seal_result dh_seal_tx_accepted(dh_seal_tx *tx, const uint8_t *body, size_t len);

/*
 * The peer's SEAL_OFFER: derive the same key from this end and write the
 * SEAL_ACCEPT body that closes the exchange. `eph_private` and `nonce` are this
 * end's, freshly drawn.
 *
 * A second offer replaces whatever this end held. The offerer owns the seal, so
 * an offer arriving while one is live means the peer discarded its own — after
 * a SEAL_STALE, or a session of its own that ended.
 */
dh_seal_result dh_seal_rx_offered(dh_seal_rx *rx, const uint8_t *body, size_t len,
                                  const uint8_t eph_private[DH_P256_PRIVATE_SIZE],
                                  const uint8_t nonce[DH_NONCE_SIZE], uint8_t *out, size_t cap,
                                  size_t *out_len);

/*
 * A SEAL_STALE for this end's outgoing seal: the peer holds no key for it, so
 * the seal is discarded and the next payload waits on a fresh offer. A stale
 * naming some other seal is not this one's business and changes nothing.
 */
bool dh_seal_tx_stale(dh_seal_tx *tx, uint32_t seal_id);

int dh_seal_encode_stale(uint32_t seal_id, uint8_t *out, size_t cap);
bool dh_seal_decode_stale(const uint8_t *body, size_t len, uint32_t *seal_id);

/*
 * Which seal a sealed message names, read from its clear head alone — what a
 * receiver needs to answer SEAL_STALE for a key it does not hold.
 *
 * `type` must be DH_MSG_CLIP_OFFER or DH_MSG_CLIP_CHUNK; false for anything
 * else, and for a body too short to carry the head.
 */
bool dh_seal_peek_id(uint8_t type, const uint8_t *body, size_t len, uint32_t *seal_id);

/*
 * Seal one CLIP_OFFER / CLIP_CHUNK into `out`: head, ciphertext, then the
 * 16-byte GCM tag, with `*out_len` set to the body length. The clear head is
 * the AAD, so a relayed frame whose id, sequence or counter was edited no
 * longer opens.
 *
 * DH_SEAL_ERR_NO_SEAL when this end holds no live seal. There is nothing to
 * fall back to, because falling back would mean putting the payload on the
 * wire in clear.
 */
dh_seal_result dh_seal_encode_offer(dh_seal_tx *tx, const dh_seal_aead *aead,
                                    const dh_clip_offer *offer, uint8_t *out, size_t cap,
                                    size_t *out_len);
dh_seal_result dh_seal_encode_chunk(dh_seal_tx *tx, const dh_seal_aead *aead,
                                    const dh_clip_chunk *chunk, uint8_t *out, size_t cap,
                                    size_t *out_len);

/*
 * Open one received CLIP_OFFER / CLIP_CHUNK. The plaintext lands in `plain`,
 * which the caller owns, and the decoded view points into it — the received
 * body cannot be decrypted in place because it is const, and because a frame
 * that fails to open must leave nothing behind.
 */
dh_seal_result dh_seal_open_offer(const dh_seal_rx *rx, const dh_seal_aead *aead,
                                  const uint8_t *body, size_t len, uint8_t *plain,
                                  size_t plain_cap, dh_clip_offer *out);
dh_seal_result dh_seal_open_chunk(const dh_seal_rx *rx, const dh_seal_aead *aead,
                                  const uint8_t *body, size_t len, uint8_t *plain,
                                  size_t plain_cap, dh_clip_chunk *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_SEAL_H_ */
