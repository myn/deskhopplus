/*
 * The board's stored identity: what sits in the identity sector, whether it is
 * usable, and how one is made valid (#111, ADR-0008).
 *
 * Split out of the flash I/O for the reason #74 paid for: both halves of the
 * config checksum lived inside functions that read memory-mapped flash and
 * called write_flash_page, so neither could run on the host — and when the
 * range they summed became wrong, it was wrong *identically* in both
 * directions, so every round trip still agreed with itself and nothing failed.
 * Every setting was lost on every boot for as long as that lasted. The same
 * arithmetic, for a key the board cannot regenerate without unpairing every
 * helper, is worth a host test.
 *
 * What this is NOT is a second configuration. It holds one thing — the P-256
 * private key this board generated on first boot — and it has no version
 * migration story, because there is nothing here to migrate: a record this
 * firmware cannot read is a board that draws a fresh identity and re-pairs,
 * which is one chord press.
 *
 * Pure C11: no flash, no SDK, no I/O. tests/identity_test.c is the gate.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_p256.h" /* DH_P256_PRIVATE_SIZE */

/* What marks these bytes as an identity at all. An erased sector reads as 0xFF
   everywhere and a fresh one as 0x00, and neither is this. */
#define IDENTITY_MAGIC_HEADER 0x1DEA5EEDu

/*
 * Bumped only if the field layout changes. Bumping it costs every board its
 * identity and therefore its pairing — one chord press per board — so it is a
 * heavier decision than the configuration's version, not a lighter one.
 */
#define CURRENT_IDENTITY_VERSION 1

typedef struct {
    uint32_t magic_header;
    uint32_t version;

    /* The private half. The public half is derived on every boot rather than
       stored: 32 bytes of flash saved is not the point — a stored public key
       is a second copy of the same fact, free to disagree with the first. */
    uint8_t private_key[DH_P256_PRIVATE_SIZE];

    uint32_t _reserved[2];

    /* Keep checksum at the end of the struct. */
    uint32_t checksum;
} identity_t;

/*
 * Asserted rather than trusted, for the reason config_layout.h spells out:
 * both seal and validate sum everything ahead of the checksum, so a field that
 * rounds the struct up and leaves padding *behind* the checksum makes the sum
 * cover the checksum itself, and nothing ever validates again (#74).
 */
_Static_assert(offsetof(identity_t, checksum) == sizeof(identity_t) - sizeof(uint32_t),
               "identity_t: checksum must be last, with no trailing padding (see #74)");
_Static_assert(sizeof(identity_t) <= 256u,
               "identity_t must fit one flash page, which is what save_identity writes");

/* Stamp the checksum so this identity will validate. Idempotent. */
void identity_seal(identity_t *id);

/*
 * May this identity be used? Magic, version and checksum, all three, because
 * they fail differently: a never-written sector fails the magic, a record from
 * another firmware fails the version, and a disturbed one fails the checksum.
 *
 * A caller that gets false generates a fresh key pair and writes it. That is
 * the *only* recovery, and it unpairs every helper registered against the old
 * one — which is why the identity sector is placed where neither a config wipe
 * nor a firmware update nor a peer propagation can reach it (flash_layout.h).
 */
bool identity_is_valid(const identity_t *id);
