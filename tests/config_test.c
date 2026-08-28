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
#include "dh_session.h"

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

/*
 * The clipboard toggles came out of padding, which is what lets them arrive
 * without a CURRENT_CONFIG_VERSION bump — and a version bump costs every user
 * their settings and their pairing (#52).
 *
 * Two things have to hold. A configuration written before these existed has
 * zeros where they now sit, so it must still validate; and those zeros must
 * mean *allowed*, because that is what both toggles default to. A field named
 * for the permission rather than the block would read as "clipboard off in
 * both directions" on every board already in use, which looks exactly like the
 * feature not working.
 */
static void test_the_clipboard_toggles_fit_the_padding(void) {
    /* A configuration sealed before the toggles existed: the bytes they now
       occupy were padding, zeroed by the same initialisation every writer
       used. */
    config_t before;
    memset(&before, 0, sizeof before);
    before.magic_header = CONFIG_MAGIC_HEADER;
    before.version = CURRENT_CONFIG_VERSION;
    before.jump_threshold = 200;
    config_seal(&before);

    CHECK(config_is_valid(&before), "toggles",
          "a configuration written before the toggles existed stopped validating");
    CHECK(before.clip_block_a_to_b == 0 && before.clip_block_b_to_a == 0, "toggles",
          "the toggle bytes did not land on the old padding");

    /* Zero is allowed, which is the default both toggles are specified to
       have. The one place that translation happens is the core's. */
    CHECK(dh_clip_policy_for(0, before.clip_block_a_to_b, before.clip_block_b_to_a) ==
              (DH_CLIP_MAY_SEND | DH_CLIP_MAY_RECEIVE),
          "toggles", "a stored zero did not mean both directions allowed");

    /* And they are inside the checksummed range like everything else, so a
       toggle cannot be flipped in flash without the board noticing. */
    config_t flipped = before;
    flipped.clip_block_a_to_b = 1;
    CHECK(!config_is_valid(&flipped), "toggles",
          "a toggle changed underneath the checksum was accepted");
}

static void test_complete_hotkey_table_fits_the_config_sector(void) {
    CHECK(CONFIG_FLASH_PAGE_COUNT == 2, "hotkeys",
          "the complete hotkey table is not covered by exactly two flash pages");
    CHECK(CONFIG_FLASH_BYTES >= sizeof(config_t), "hotkeys",
          "the persisted config buffer truncates the hotkey table");
    CHECK(CONFIG_FLASH_BYTES <= CONFIG_FLASH_SECTOR_SIZE, "hotkeys",
          "the hotkey table escaped the dedicated config sector");

    config_t cfg = a_populated_config();
    cfg.hotkeys[DH_HOTKEY_ACTION_FW_UPGRADE_B] = (dh_hotkey_t){
        .modifier = 0x22, .keys = {0x05, 0x45}, .key_count = 2,
        .action_id = DH_HOTKEY_ACTION_FW_UPGRADE_B};
    config_seal(&cfg);
    config_t loaded;
    memcpy(&loaded, &cfg, sizeof loaded);
    CHECK(config_is_valid(&loaded), "hotkeys", "a complete table did not survive persistence");
    CHECK(memcmp(loaded.hotkeys, cfg.hotkeys, sizeof cfg.hotkeys) == 0, "hotkeys",
          "a valid binding was lost or truncated on reload");
}

static void test_keymap_profiles_survive_persistence(void) {
    CHECK(CURRENT_CONFIG_VERSION == 13, "keymaps",
          "the stored layout changed without the issue #26 version bump");
    CHECK(sizeof(((output_t *)0)->keymap.overrides) / sizeof(dh_key_override_t) == 32,
          "keymaps", "an output does not hold 32 overrides");
    CHECK(sizeof(((output_t *)0)->keymap.passthrough) == 16,
          "keymaps", "an output does not hold 16 passthrough entries");

    config_t cfg = a_populated_config();
    cfg.output[0].keymap = (dh_keymap_profile_t){
        .overrides = {{0x39, 0xe0}, {0xe1, 0x04}},
        .override_count = 2,
        .passthrough = {0xe3, 0x2a},
        .passthrough_count = 2,
    };
    cfg.output[1].keymap = (dh_keymap_profile_t){
        .overrides = {{0x04, 0x05}}, .override_count = 1,
        .passthrough = {0x39}, .passthrough_count = 1,
    };
    config_seal(&cfg);
    config_t loaded;
    memcpy(&loaded, &cfg, sizeof loaded);

    CHECK(config_is_valid(&loaded), "keymaps", "keymap profiles invalidated persistence");
    CHECK(memcmp(loaded.output[0].keymap.overrides, cfg.output[0].keymap.overrides,
                 sizeof cfg.output[0].keymap.overrides) == 0,
          "keymaps", "output A overrides changed on reload");
    CHECK(memcmp(&loaded.output[1].keymap, &cfg.output[1].keymap,
                 sizeof cfg.output[1].keymap) == 0,
          "keymaps", "output B profile changed on reload");
}

int main(void) {
    test_a_sealed_config_validates();
    test_every_payload_byte_is_covered();
    test_the_reserved_tail_is_covered();
    test_the_checksum_itself_is_checked();
    test_magic_and_version_are_refused_distinctly();
    test_sealing_is_idempotent();
    test_the_clipboard_toggles_fit_the_padding();
    test_complete_hotkey_table_fits_the_config_sector();
    test_keymap_profiles_survive_persistence();

    if (failures) {
        printf("%d config check(s) failed\n", failures);
        return 1;
    }
    printf("config tests passed\n");
    return 0;
}
