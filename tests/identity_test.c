/*
 * The board's stored identity: what validates, what does not, and that a
 * round trip through seal and validate keeps the key intact (#111).
 *
 * Small, and deliberately so. What makes it worth having is #74: the same
 * arithmetic for the configuration lived behind write_flash_page, was wrong
 * *identically* in both directions, and therefore agreed with itself on every
 * round trip while every stored config silently failed. The cost there was the
 * user's settings. The cost here is the board's identity, which cannot be
 * regenerated without unpairing every helper registered against it.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdio.h>
#include <string.h>

#include "identity_store.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* An obviously fake key: this file never runs the curve, it only checks what
   surrounds the bytes. */
static identity_t an_identity(void) {
    identity_t id;
    memset(&id, 0, sizeof id);
    id.magic_header = IDENTITY_MAGIC_HEADER;
    id.version = CURRENT_IDENTITY_VERSION;
    for (unsigned i = 0; i < DH_P256_PRIVATE_SIZE; i++)
        id.private_key[i] = (uint8_t)(i + 1u);
    identity_seal(&id);
    return id;
}

static void test_a_sealed_identity_validates_and_keeps_its_key(void) {
    const identity_t id = an_identity();
    CHECK(identity_is_valid(&id), "round trip", "a freshly sealed identity was refused");

    for (unsigned i = 0; i < DH_P256_PRIVATE_SIZE; i++)
        CHECK(id.private_key[i] == (uint8_t)(i + 1u), "round trip", "the key did not survive");
}

/* Sealing an unchanged record twice leaves the same checksum: the checksum
   field is not part of what is summed. */
static void test_sealing_is_idempotent(void) {
    identity_t id = an_identity();
    const uint32_t once = id.checksum;
    identity_seal(&id);
    CHECK(id.checksum == once, "idempotent", "a second seal changed the checksum");
}

/*
 * An erased sector reads as 0xFF everywhere and a never-written one as 0x00.
 * Both are what a board sees before its first boot, and both must fail — a
 * board that read either as a key would derive a public half from it and
 * offer it in a pairing grant.
 */
static void test_an_unwritten_sector_is_refused(void) {
    identity_t erased;
    memset(&erased, 0xFF, sizeof erased);
    CHECK(!identity_is_valid(&erased), "unwritten", "an erased sector was read as an identity");

    identity_t zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    CHECK(!identity_is_valid(&zeroed), "unwritten", "a zeroed sector was read as an identity");
}

/*
 * Magic, version and checksum fail differently and all three are checked, for
 * the reason config_store.h gives: a never-written sector fails the magic, a
 * record from another firmware fails the version, and a disturbed one fails
 * the checksum.
 */
static void test_magic_version_and_checksum_are_each_refused(void) {
    identity_t wrong_magic = an_identity();
    wrong_magic.magic_header ^= 1u;
    identity_seal(&wrong_magic);
    CHECK(!identity_is_valid(&wrong_magic), "magic", "a foreign record was accepted");

    identity_t wrong_version = an_identity();
    wrong_version.version = CURRENT_IDENTITY_VERSION + 1u;
    identity_seal(&wrong_version);
    CHECK(!identity_is_valid(&wrong_version), "version",
          "a record from another firmware version was accepted");

    identity_t disturbed = an_identity();
    disturbed.checksum ^= 1u;
    CHECK(!identity_is_valid(&disturbed), "checksum", "a disturbed record was accepted");
}

/*
 * Every byte ahead of the checksum is covered. A key whose last byte flipped
 * is a different key, and a board that accepted it would hold a private half
 * that no longer matches the public half any helper pinned.
 */
static void test_every_byte_of_the_key_is_covered(void) {
    for (unsigned i = 0; i < DH_P256_PRIVATE_SIZE; i++) {
        identity_t flipped = an_identity();
        flipped.private_key[i] ^= 0x80u;
        CHECK(!identity_is_valid(&flipped), "coverage",
              "a flipped key byte still validated against the stored checksum");
    }

    /* Including the reserved words, which are inside the summed range: they
       are zero today, and a future field taking one of them must invalidate
       the records written before it. */
    identity_t reserved = an_identity();
    reserved._reserved[1] = 0xA5A5A5A5u;
    CHECK(!identity_is_valid(&reserved), "coverage",
          "the reserved words are outside the checksummed range");
}

int main(void) {
    test_a_sealed_identity_validates_and_keeps_its_key();
    test_sealing_is_idempotent();
    test_an_unwritten_sector_is_refused();
    test_magic_version_and_checksum_are_each_refused();
    test_every_byte_of_the_key_is_covered();

    if (failures) {
        printf("%d identity check(s) failed\n", failures);
        return 1;
    }
    printf("identity tests passed\n");
    return 0;
}
