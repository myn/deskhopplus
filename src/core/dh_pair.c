/*
 * deskhopplus shared core — pairing and the device-held secret (#46).
 * See dh_pair.h.
 */

#include "dh_pair.h"

#include <string.h>

void dh_pair_init(dh_pair *p, const uint8_t *stored) {
    memset(p, 0, sizeof *p);
    if (stored != NULL) {
        memcpy(p->secret, stored, DH_PAIR_SECRET_LEN);
        p->provisioned = true;
    }
}

void dh_pair_open_window(dh_pair *p, const uint8_t *fresh_secret, uint32_t now_ms) {
    memcpy(p->secret, fresh_secret, DH_PAIR_SECRET_LEN);
    p->provisioned = true;
    p->window_open = true;
    p->window_opened_ms = now_ms;
}

bool dh_pair_window_open(const dh_pair *p, uint32_t now_ms) {
    if (!p->window_open)
        return false;
    /* Unsigned difference: a wrapping millisecond counter is arithmetic, not
       a window that never closes. */
    return (uint32_t)(now_ms - p->window_opened_ms) < DH_PAIR_WINDOW_MS;
}

void dh_pair_tick(dh_pair *p, uint32_t now_ms) {
    if (p->window_open && !dh_pair_window_open(p, now_ms))
        p->window_open = false;
}

bool dh_pair_authenticate(const dh_pair *p, const uint8_t *token, size_t len) {
    if (!p->provisioned || token == NULL || len != DH_PAIR_SECRET_LEN)
        return false;

    /*
     * Constant time in the secret's length. Every byte is compared and the
     * differences accumulated, so the time taken says nothing about how many
     * leading bytes were right — which a process holding the channel could
     * otherwise use to guess the secret one byte at a time.
     */
    uint8_t diff = 0;
    for (size_t i = 0; i < DH_PAIR_SECRET_LEN; i++)
        diff |= (uint8_t)(p->secret[i] ^ token[i]);

    return diff == 0;
}

bool dh_pair_grant(dh_pair *p, uint32_t now_ms, uint8_t *out) {
    if (!dh_pair_window_open(p, now_ms))
        return false;

    memcpy(out, p->secret, DH_PAIR_SECRET_LEN);
    p->grants++;
    return true;
}
