/*
 * deskhopplus shared core — the board's identity, and the one helper key it
 * registers (#111, ADR-0008).
 *
 * The threat this answers is stated precisely in #34: the channel is a data
 * bridge between two machines whose isolation is the premise of the project.
 * The firmware relays opaquely, so any local process that opened the channel
 * could push bytes to the other computer, where a helper writes files and
 * sets the clipboard.
 *
 * ---------------------------------------------------------------------------
 * WHAT REPLACED THE BEARER TOKEN
 *
 * Until #111 this file held a 16-byte secret that the device handed to a
 * helper during a pairing window and the helper sent back verbatim in every
 * hello. Two properties made that unsound, and both were measured rather than
 * argued (#95, 2026-08-13):
 *
 *   1. On macOS the channel is not exclusive. A second
 *      kIOHIDOptionsTypeSeizeDevice open succeeds and every client receives
 *      every report, so a secret that crosses the wire is a secret anything
 *      running as the user can take — with no filesystem access and no
 *      permission of any kind.
 *   2. Rotation therefore did not recover a stolen pairing, it re-issued it.
 *      A listener still attached when the user pressed the chord simply
 *      received the replacement. The documented remedy handed the thief the
 *      new secret.
 *
 * ADR-0008 replaced it with a key pair on each side. **Only public halves
 * cross, at any point, ever**, so there is nothing in the exchange to take:
 *
 *   - The **board's identity** is a P-256 key pair generated on first boot
 *     into its *own flash sector*, beside ADDR_CONFIG and ADDR_FW_RUNNING and
 *     part of neither. It survives a config wipe, a firmware update, and the
 *     peer propagation that copies one board's image onto the other (#91) —
 *     an identity inside the image would give both boards the same identity.
 *   - The **registration** is exactly one helper public key, plus the 32-byte
 *     shared secret from one ECDH against it. It lives in the configuration
 *     and a config wipe clears it, because wiping is how a user unpairs. The
 *     identity is not the registration's to take with it: if a wipe took the
 *     identity too, every wipe would make every helper report "this board
 *     changed" — a false alarm on a routine action.
 *
 * The asymmetric work happens **here and nowhere else**. One ECDH at pairing
 * yields the long-term shared secret; every session afterwards is a hash
 * (dh_auth.h). On this chip an ECDH is 133.4 ms, measured on board A under
 * #110, and src/main.c runs six jobs on core 0 in one cooperative loop with no
 * preemption — so it is paid during the pairing window, where the board has
 * just rebooted for the chord and already stalls to write flash.
 *
 * What this does not fix, stated because each was accepted rather than
 * overlooked: there is **no forward secrecy** — flash plus recorded traffic
 * reads the traffic — and a listener that **wins the single-shot pairing
 * race** is registered while the real helper is not. The second is awkward and
 * it is *visible*, which is the detection signal #34 asked for and never got.
 * ---------------------------------------------------------------------------
 *
 * Pure C11: no I/O, no clock, no entropy source. A private key is 32 bytes the
 * caller hands in, exactly as the fresh secret used to be, because a core that
 * cannot be tested deterministically is a core whose security property cannot
 * be tested at all.
 */

#ifndef DH_PAIR_H_
#define DH_PAIR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_p256.h"

/*
 * How long a window stays open. "About a minute" (#42): long enough to cover
 * the config-mode reboot and a helper's reconnection backoff, short enough
 * that it is not meaningfully open to whatever starts next.
 */
#define DH_PAIR_WINDOW_MS 60000u

typedef enum {
    DH_PAIR_OK = 0,
    /* No window is open, and none closed on someone else inside the minute
       the user pressed for. docs/protocol.md: PAIR_REFUSED(no_window). */
    DH_PAIR_ERR_NO_WINDOW = -1,
    /* The window was single-shot and something else took it. This is the
       failure that used to be silent: a listener could be provisioned
       *alongside* the helper, and now it is provisioned *instead* of it and
       the helper is told so. PAIR_REFUSED(already_registered). */
    DH_PAIR_ERR_ALREADY_REGISTERED = -2,
    /* Not a point on P-256, or the ECDH refused it. A pairing request is
       hostile input — anything attached to the channel can send one. */
    DH_PAIR_ERR_BAD_KEY = -3,
    /* This board has no key pair of its own, so it has no private half to
       agree with. Only reachable before first boot's generation. */
    DH_PAIR_ERR_NO_IDENTITY = -4,
} dh_pair_result;

typedef struct {
    /* The board's own, from its own flash sector. */
    uint8_t identity_private[DH_P256_PRIVATE_SIZE];
    uint8_t identity_public[DH_P256_PUBLIC_SIZE];
    bool has_identity;

    /* The registration, from the configuration. Exactly one helper key: one
       board serves one computer, and several registrations would make "which
       one is legitimate?" unanswerable and eviction meaningless. */
    uint8_t helper_key_id[DH_KEY_ID_SIZE];
    uint8_t shared_secret[DH_P256_SHARED_SIZE];
    bool registered;

    bool window_open;
    /* A window this board closed by registering someone, still inside its own
       minute. What tells no_window from already_registered. */
    bool window_claimed;
    uint32_t window_opened_ms;

    uint32_t registrations; /* since boot, for diagnostics */
} dh_pair;

/* No identity and no registration: what a board holds before flash is read. */
void dh_pair_init(dh_pair *p);

/*
 * Adopt the board's identity — the 32 bytes read back from the identity
 * sector, or freshly drawn on first boot. Derives and keeps the public half.
 *
 * False when those bytes are not a scalar in [1, n-1]; draw again. Random
 * bytes fail about once in 2^32 draws, which is rare enough to be a retry and
 * far too common to be an assertion.
 */
bool dh_pair_set_identity(dh_pair *p, const uint8_t private_key[DH_P256_PRIVATE_SIZE]);

static inline bool dh_pair_has_identity(const dh_pair *p) { return p->has_identity; }

/* The 64 raw bytes a PAIR_GRANT carries, X || Y big-endian. NULL without an
   identity, so a caller cannot hand out an uninitialised key. */
const uint8_t *dh_pair_public_key(const dh_pair *p);

/*
 * Adopt a registration read back from the configuration: the registered
 * helper's key id and the shared secret one ECDH already produced. No
 * asymmetric work — this is a boot path, not a pairing.
 */
void dh_pair_set_registration(dh_pair *p, const uint8_t key_id[DH_KEY_ID_SIZE],
                              const uint8_t shared_secret[DH_P256_SHARED_SIZE]);

/*
 * Unpair. The configuration holding the registration was wiped, which is how
 * a user revokes a paired machine — so the registration goes with it *now*
 * rather than staying live in RAM until the next power-up (#75). The identity
 * is untouched: it is the board's, not the pairing's.
 */
void dh_pair_clear_registration(dh_pair *p);

static inline bool dh_pair_registered(const dh_pair *p) { return p->registered; }

/*
 * Is this the key id the board registered? The comparison is constant time:
 * an early return would leak how much of a guess was right, one byte at a
 * time, to a process that can retry as fast as it likes.
 *
 * A key id names a key without carrying one, so this is not authentication —
 * it decides whether the sender *claims* to be the registered helper, which is
 * what makes the listener count in DH_MSG_LISTENER_ALERT mean something rather
 * than counting every stray frame alike. The tag is what proves it.
 */
bool dh_pair_is_registered_key(const dh_pair *p, const uint8_t key_id[DH_KEY_ID_SIZE]);

/* The pairing-time shared secret every session key is derived from. NULL when
   nothing is registered, so there is no key to derive under. */
const uint8_t *dh_pair_shared_secret(const dh_pair *p);

/*
 * A chord press: config mode was entered or left, and the user has a minute.
 *
 * Deliberately does **not** unpair. v1 rotated the secret on every window,
 * because a leaked bearer token stayed valid until the configuration was
 * wiped. There is no leaked token now — only public halves cross — so a chord
 * press that nobody pairs against leaves the existing registration exactly as
 * it was, and an accidental press costs the user nothing. Re-pairing replaces
 * the registration, which is what makes a stolen pairing recoverable.
 */
void dh_pair_open_window(dh_pair *p, uint32_t now_ms);

/* Wrap-safe. */
bool dh_pair_window_open(const dh_pair *p, uint32_t now_ms);

/* Closes a window that has run its minute, and forgets that a window inside
   that minute was claimed — after it, the honest answer to a pairing request
   is "no window" again rather than "someone else took it". */
void dh_pair_tick(dh_pair *p, uint32_t now_ms);

/*
 * Register this helper public key, inside an open window: one ECDH against
 * the board's private half, the 32-byte result kept as the shared secret, and
 * the window **closed** — the first registration is the only one.
 *
 * This is the board's only asymmetric work and it takes 133.4 ms on the chip.
 * Call it where a stall is expected, never from a USB callback.
 *
 * The caller persists the registration afterwards: this core has no flash.
 */
dh_pair_result dh_pair_register(dh_pair *p, uint32_t now_ms,
                                const uint8_t helper_public[DH_P256_PUBLIC_SIZE]);

/*
 * Why a pairing request that is not being granted is being refused, for the
 * PAIR_REFUSED a board owes the asker. Never called on DH_PAIR_OK.
 */
dh_pair_result dh_pair_refusal(const dh_pair *p, uint32_t now_ms);

#endif /* DH_PAIR_H_ */
