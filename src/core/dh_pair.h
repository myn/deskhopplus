/*
 * deskhopplus shared core — pairing and the device-held secret (#46).
 *
 * The threat this answers is stated precisely in #34: the channel is a data
 * bridge between two machines whose isolation is the premise of the project.
 * The firmware relays opaquely, so any local process that opened the channel
 * could push bytes to the other computer, where a helper writes files and
 * sets the clipboard. Reading the clipboard in transit is the lesser half.
 *
 * (That last ranking is #34's and does not survive a transport where reads
 * cannot be prevented — see the correction below. Reading is the acquisition
 * step for writing once the credential is on the wire, and the content read
 * is the *other* machine's clipboard, which no local API would hand over.)
 *
 * The control is a secret the device holds and a helper must prove it knows.
 * The secret is never displayed and never typed: it is handed to whichever
 * helper is connected during a window that only a **physical keyboard chord
 * on the device** can open. No remote or background process can open one.
 *
 * Every window rotates the secret (decided here, #34's open question). That
 * makes a stolen pairing recoverable by the same one-keystroke gesture that
 * created it — otherwise a secret leaked to a process that won the
 * exclusivity race stays valid until the configuration is wiped, which costs
 * the user every setting they have. The cost of rotating is that the helper
 * on *this* board re-pairs on the next chord press; the secret is per-board
 * and never syncs, so the other computer's helper is untouched.
 *
 * ---------------------------------------------------------------------------
 * CORRECTION, 2026-08-13 (#95). Everything above holds only where the channel
 * is genuinely exclusive. On **macOS it is not**: a second
 * kIOHIDOptionsTypeSeizeDevice open succeeds, measured, and every client
 * receives every report. Two consequences, both of which read the wrong way
 * from the paragraphs above.
 *
 * 1. The secret is never displayed or typed, but it is **transmitted in the
 *    clear** — it is a bearer token, not a key. PAIR_GRANT carries it as its
 *    payload, and every HELLO carries it back verbatim (dh_session.c). So on
 *    macOS it is obtainable by listening, with no filesystem access and no
 *    permission of any kind. "Never displayed" is not "protected".
 *
 * 2. Rotation does not recover a stolen pairing there — it **re-issues it**.
 *    The reasoning above is conditioned on a leak requiring an attacker to
 *    have won the exclusivity race. Where there is no race, a listener still
 *    attached when the user presses the chord simply receives the new secret
 *    in the PAIR_GRANT. The documented remedy hands the thief the replacement.
 *
 * Windows is unaffected: hidclass.sys refuses the second open, so there is no
 * listener and both properties hold as written. Do not delete this note when
 * the posture is fixed — replace it with what replaced the bearer token.
 *
 * DECIDED, 2026-08-18: ADR-0008. The bearer token is replaced by a key pair
 * per side, a per-frame authentication tag, and a clipboard payload sealed
 * between the two helpers — so nothing secret crosses, at pairing or after.
 * The wire is now written down in full: docs/protocol.md v2 and the regenerated
 * test-vectors/frames.txt (#109). The code here is unchanged until #111 lands;
 * this note stands until then and is what #111 replaces.
 * ---------------------------------------------------------------------------
 *
 * Pure C11: no I/O, no clock, no entropy source. Fresh secrets are handed in
 * by the caller, because a core that cannot be tested deterministically is
 * a core whose security property cannot be tested at all.
 */

#ifndef DH_PAIR_H_
#define DH_PAIR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length of the device-held secret, and of the token a hello carries. */
#define DH_PAIR_SECRET_LEN 16u

/*
 * How long a window stays open. "About a minute" (#42): long enough to cover
 * the config-mode reboot and a helper's reconnection backoff, short enough
 * that it is not meaningfully open to whatever starts next.
 */
#define DH_PAIR_WINDOW_MS 60000u

typedef struct {
    uint8_t secret[DH_PAIR_SECRET_LEN];
    bool provisioned;        /* a secret exists to authenticate against */
    bool window_open;
    uint32_t window_opened_ms;
    uint32_t grants;         /* secrets handed out since boot, for diagnostics */
} dh_pair;

/*
 * stored is the secret read back from flash, or NULL when there is none —
 * a fresh device, or one whose configuration was wiped. Without a secret the
 * device authenticates nobody, and the remedy is one chord press.
 */
void dh_pair_init(dh_pair *p, const uint8_t *stored);

/*
 * A chord press: config mode was entered or left. Rotates to fresh_secret and
 * opens the window. Any previously paired helper is evicted by construction —
 * that is the point of rotating, and it is what makes a stolen pairing
 * recoverable.
 *
 * fresh_secret must be DH_PAIR_SECRET_LEN bytes of real entropy; the core has
 * no opinion about where they come from, and no way to check.
 */
void dh_pair_open_window(dh_pair *p, const uint8_t *fresh_secret, uint32_t now_ms);

/* Wrap-safe; closes a window that has run its minute. */
bool dh_pair_window_open(const dh_pair *p, uint32_t now_ms);
void dh_pair_tick(dh_pair *p, uint32_t now_ms);

/*
 * Does this token authenticate? False whenever the device holds no secret,
 * the length is wrong, or the bytes differ. The comparison is constant time
 * in the secret's length: an early return would leak how much of a guess was
 * right, one byte at a time, to a process that can retry as fast as it likes.
 */
bool dh_pair_authenticate(const dh_pair *p, const uint8_t *token, size_t len);

/*
 * Hand the secret to a helper, if a window is open. Writes DH_PAIR_SECRET_LEN
 * bytes to out and returns true; returns false — and writes nothing — outside
 * a window, which is the whole of "pairing requests are refused".
 */
bool dh_pair_grant(dh_pair *p, uint32_t now_ms, uint8_t *out);

#endif /* DH_PAIR_H_ */
