/*
 * deskhopplus shared core — pairing and the device-held secret (#46).
 *
 * The threat this answers is stated precisely in #34: the channel is a data
 * bridge between two machines whose isolation is the premise of the project.
 * The firmware relays opaquely, so any local process that opened the channel
 * could push bytes to the other computer, where a helper writes files and
 * sets the clipboard. Reading the clipboard in transit is the lesser half.
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
