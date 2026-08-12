/*
 * Configuration validation on the host (#74).
 *
 * The defect this exists to prevent shipped because nothing could reach it.
 * save_config and load_config both took the CRC over
 * `sizeof(config_t) - sizeof(uint32_t)`, which excludes the checksum only
 * while the checksum is the last four bytes of the struct. #46's secret took
 * config_t from 136 to 160 bytes and left four bytes of padding *behind* the
 * checksum, so the CRC started covering the checksum field itself. Save and
 * load then hashed different content, the comparison could never match, and
 * every boot fell back to defaults — losing the pairing secret and every
 * other setting with it.
 *
 * It was invisible to tests because the arithmetic was identically wrong in
 * both directions and both lived behind flash I/O. The decision is now split
 * out (config_store.h) so the arithmetic can be tested without a device, and
 * the interesting case is the *tail*: a checksum that stops short still
 * round-trips perfectly, so a round-trip test alone proves nothing. Every
 * byte ahead of the checksum has to be shown to matter.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdio.h>
#include <string.h>

#include "config_store.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* A config with something in every field, so a checksum that skips a region
   has something to miss there. Deliberately not default_config: that lives in
   the firmware image, and a field left at zero is a field a short checksum
   gets away with. */
static config_t a_populated_config(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof cfg);

    uint8_t *raw = (uint8_t *)&cfg;
    for (size_t i = 0; i < sizeof cfg; i++)
        raw[i] = (uint8_t)(0x40u + (i * 7u));

    cfg.magic_header = CONFIG_MAGIC_HEADER;
    cfg.version = CURRENT_CONFIG_VERSION;
    config_seal(&cfg);
    return cfg;
}

static void test_a_sealed_config_validates(void) {
    config_t cfg = a_populated_config();
    CHECK(config_is_valid(&cfg), "roundtrip", "a freshly sealed config did not validate");

    /* Through a byte buffer, as flash delivers it. */
    uint8_t stored[sizeof(config_t)];
    memcpy(stored, &cfg, sizeof stored);
    config_t loaded;
    memcpy(&loaded, stored, sizeof loaded);
    CHECK(config_is_valid(&loaded), "roundtrip", "a config did not survive a byte-for-byte copy");
}

/*
 * The heart of #74. Every byte ahead of the checksum must be covered, so
 * corrupting any one of them must be caught — including the tail, which is
 * where the padding that broke this actually sat. A checksum whose range
 * stops short passes a round-trip test and fails this one.
 */
static void test_every_payload_byte_is_covered(void) {
    const size_t payload = offsetof(config_t, checksum);

    /* Every byte, magic and version included. Those two would also be caught
       by their own checks, and that is fine: the criterion is that a
       disturbed byte anywhere ahead of the checksum is refused, not which
       check refuses it. Skipping them would leave the front of the struct
       untested against a checksum that started late. */
    for (size_t i = 0; i < payload; i++) {
        config_t cfg = a_populated_config();
        uint8_t *raw = (uint8_t *)&cfg;
        raw[i] ^= 0xFFu;

        if (config_is_valid(&cfg)) {
            ++failures;
            printf("FAIL %s:%d [coverage] byte %zu of %zu is outside the checksum\n", __FILE__,
                   __LINE__, i, payload);
            return; /* one report is enough; they would all be the same defect */
        }
    }
}

/* The reserved words exist precisely to absorb padding that would otherwise
   sit behind the checksum (#46/#74). They are the last thing the CRC covers,
   so they are the first thing a short range drops. */
static void test_the_reserved_tail_is_covered(void) {
    config_t cfg = a_populated_config();
    cfg._reserved[1] ^= 0xFFFFFFFFu;
    CHECK(!config_is_valid(&cfg), "tail",
          "the last word before the checksum is outside the checksum");

    config_t other = a_populated_config();
    other._reserved[0] ^= 0x1u;
    CHECK(!config_is_valid(&other), "tail", "a single bit in the reserved tail went unnoticed");
}

static void test_the_checksum_itself_is_checked(void) {
    config_t cfg = a_populated_config();
    cfg.checksum ^= 0x1u;
    CHECK(!config_is_valid(&cfg), "checksum", "a corrupted checksum was accepted");
}

/*
 * Magic and version are separate refusals, and both matter: a wiped sector
 * reads as 0xFF everywhere and must not be mistaken for a config, and a
 * config written by an older firmware must not be read with this one's
 * field layout.
 */
static void test_magic_and_version_are_refused_distinctly(void) {
    config_t wrong_magic = a_populated_config();
    wrong_magic.magic_header ^= 0x1u;
    config_seal(&wrong_magic); /* seal it, so only the magic is wrong */
    CHECK(!config_is_valid(&wrong_magic), "magic", "a config with the wrong magic was accepted");

    config_t wrong_version = a_populated_config();
    wrong_version.version = CURRENT_CONFIG_VERSION + 1u;
    config_seal(&wrong_version);
    CHECK(!config_is_valid(&wrong_version), "version",
          "a config from another firmware version was accepted");

    /* An erased sector: every byte 0xFF. */
    config_t erased;
    memset(&erased, 0xFF, sizeof erased);
    CHECK(!config_is_valid(&erased), "erased", "an erased flash sector was read as a config");

    /* And a zeroed one, which is what a fresh page reads as on some parts. */
    config_t zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    CHECK(!config_is_valid(&zeroed), "zeroed", "a zeroed sector was read as a config");
}

/*
 * Sealing is what save_config does and validating is what load_config does,
 * so the two must agree by construction. Sealing twice must also be stable —
 * save_config stamps the checksum into the live config, and a second save
 * without an intervening edit must not invalidate the first.
 */
static void test_sealing_is_idempotent(void) {
    config_t cfg = a_populated_config();
    const uint32_t first = cfg.checksum;
    config_seal(&cfg);
    CHECK(cfg.checksum == first, "idempotent", "sealing an unchanged config changed its checksum");
    CHECK(config_is_valid(&cfg), "idempotent", "sealing twice invalidated the config");
}

int main(void) {
    test_a_sealed_config_validates();
    test_every_payload_byte_is_covered();
    test_the_reserved_tail_is_covered();
    test_the_checksum_itself_is_checked();
    test_magic_and_version_are_refused_distinctly();
    test_sealing_is_idempotent();

    if (failures) {
        printf("%d config check(s) failed\n", failures);
        return 1;
    }
    printf("config tests passed\n");
    return 0;
}
