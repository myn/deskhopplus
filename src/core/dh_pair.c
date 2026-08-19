/*
 * deskhopplus shared core — the board's identity and its one registration.
 * See dh_pair.h.
 */

#include "dh_pair.h"

#include <string.h>

#include "dh_auth.h" /* dh_auth_bytes_equal — the same constant-time compare */

void dh_pair_init(dh_pair *p) {
    memset(p, 0, sizeof *p);
}

bool dh_pair_set_identity(dh_pair *p, const uint8_t private_key[DH_P256_PRIVATE_SIZE]) {
    uint8_t public_key[DH_P256_PUBLIC_SIZE];
    if (!dh_p256_public_from_private(private_key, public_key))
        return false;

    memcpy(p->identity_private, private_key, DH_P256_PRIVATE_SIZE);
    memcpy(p->identity_public, public_key, DH_P256_PUBLIC_SIZE);
    p->has_identity = true;
    return true;
}

const uint8_t *dh_pair_public_key(const dh_pair *p) {
    return p->has_identity ? p->identity_public : NULL;
}

void dh_pair_set_registration(dh_pair *p, const uint8_t key_id[DH_KEY_ID_SIZE],
                              const uint8_t shared_secret[DH_P256_SHARED_SIZE]) {
    memcpy(p->helper_key_id, key_id, DH_KEY_ID_SIZE);
    memcpy(p->shared_secret, shared_secret, DH_P256_SHARED_SIZE);
    p->registered = true;
}

void dh_pair_clear_registration(dh_pair *p) {
    memset(p->helper_key_id, 0, sizeof p->helper_key_id);
    memset(p->shared_secret, 0, sizeof p->shared_secret);
    p->registered = false;
}

bool dh_pair_is_registered_key(const dh_pair *p, const uint8_t key_id[DH_KEY_ID_SIZE]) {
    if (!p->registered || key_id == NULL)
        return false;
    return dh_auth_bytes_equal(p->helper_key_id, key_id, DH_KEY_ID_SIZE);
}

const uint8_t *dh_pair_shared_secret(const dh_pair *p) {
    return p->registered ? p->shared_secret : NULL;
}

void dh_pair_open_window(dh_pair *p, uint32_t now_ms) {
    p->window_open = true;
    p->window_claimed = false;
    p->window_opened_ms = now_ms;
}

bool dh_pair_window_open(const dh_pair *p, uint32_t now_ms) {
    if (!p->window_open)
        return false;
    /* Unsigned difference: a wrapping millisecond counter is arithmetic, not
       a window that never closes. */
    return (uint32_t)(now_ms - p->window_opened_ms) < DH_PAIR_WINDOW_MS;
}

/* Has the minute the user pressed for run out? True whether the window was
   claimed by a registration or simply expired unused — both stop being the
   user's window at the same moment. */
static bool minute_elapsed(const dh_pair *p, uint32_t now_ms) {
    return (uint32_t)(now_ms - p->window_opened_ms) >= DH_PAIR_WINDOW_MS;
}

void dh_pair_tick(dh_pair *p, uint32_t now_ms) {
    if (p->window_open && !dh_pair_window_open(p, now_ms))
        p->window_open = false;

    if (p->window_claimed && minute_elapsed(p, now_ms))
        p->window_claimed = false;
}

dh_pair_result dh_pair_refusal(const dh_pair *p, uint32_t now_ms) {
    /* Claimed beats no_window while the minute lasts: the user pressed the
       chord, something registered, and telling the helper "no window" would
       hide that its window was taken. After the minute the honest answer is
       no_window again — which is also what window_claimed's expiry is for. */
    if (p->window_claimed && !minute_elapsed(p, now_ms))
        return DH_PAIR_ERR_ALREADY_REGISTERED;
    return DH_PAIR_ERR_NO_WINDOW;
}

dh_pair_result dh_pair_register(dh_pair *p, uint32_t now_ms,
                                const uint8_t helper_public[DH_P256_PUBLIC_SIZE]) {
    if (!dh_pair_window_open(p, now_ms))
        return dh_pair_refusal(p, now_ms);

    if (!p->has_identity)
        return DH_PAIR_ERR_NO_IDENTITY;

    /* The one ECDH. dh_p256_ecdh validates the peer key itself, so a caller
       cannot skip the check on input anything attached to the channel can
       send. 133.4 ms on this chip (#110) — see the header for where it may
       be called from. */
    uint8_t shared_secret[DH_P256_SHARED_SIZE];
    if (!dh_p256_ecdh(p->identity_private, helper_public, shared_secret))
        return DH_PAIR_ERR_BAD_KEY;

    uint8_t key_id[DH_KEY_ID_SIZE];
    dh_p256_key_id(helper_public, key_id);
    dh_pair_set_registration(p, key_id, shared_secret);

    /*
     * The window is single-shot: the first registration closes it. A listener
     * can no longer be provisioned silently *alongside* the helper — if it
     * wins the race it is registered and the helper is not, and the helper is
     * told so. That failure is awkward and it is visible, which is the whole
     * trade (#34, ADR-0008).
     */
    p->window_open = false;
    p->window_claimed = true;
    p->registrations++;
    return DH_PAIR_OK;
}
