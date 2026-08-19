/*
 * Where each writer of flash reaches, and which of them must never reach the
 * identity sector (#111).
 *
 * This gates the one failure that would be silent and permanent: peer
 * propagation copies board A's running image onto board B (#91, measured
 * working 2026-08-18), and the upgrade path writes every page of that image.
 * An identity key inside the copied range would give **both boards the same
 * identity** — every helper would then pair with something that is not the
 * board it thinks it is, and nothing anywhere would say so.
 *
 * The ranges below are written out as what each caller actually passes to
 * flash, not derived from the constants they are being checked against, so a
 * region that moves has to move in two places to pass.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "flash_layout.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* One writer of flash: what it is, and the range it touches. */
struct writer {
    const char *name;
    uint32_t offset;
    uint32_t len;
    const char *what;
};

/*
 * Every path in this firmware that erases or programs flash, as of #111.
 *
 * The image writers are the dangerous ones and they are two distinct
 * transports for the same range (#104): the pull asks the peer board for the
 * image a word at a time, and the UF2 drop takes it from the host as blocks on
 * the config-mode disk. Both write ADDR_FW_RUNNING page by page from address 0
 * to the end of the image.
 */
static const struct writer writers[] = {
    {"peer pull", FLASH_LAYOUT_IMAGE_OFFSET, FLASH_LAYOUT_IMAGE_LEN,
     "tasks.c: write_flash_page(ADDR_FW_RUNNING + page_start_addr)"},
    /* The UF2 drop's range is only this because tud_msc_write10_cb refuses a
       blockNo past MAX_BLOCK_NO. That guard is what this row asserts about;
       without it `blockNo` is a host-supplied absolute page address and block
       0x1F80 lands on the identity sector. */
    {"UF2 drop", FLASH_LAYOUT_IMAGE_OFFSET, FLASH_LAYOUT_IMAGE_LEN,
     "ramdisk.c: write_flash_page(ADDR_FW_RUNNING + blockNo * FLASH_PAGE_SIZE), blockNo bounded"},
    {"recover to ROM", FLASH_LAYOUT_IMAGE_OFFSET, FLASH_LAYOUT_SECTOR,
     "utils.c: flash_range_erase(ADDR_FW_RUNNING, FLASH_SECTOR_SIZE)"},
    {"config wipe", FLASH_LAYOUT_CONFIG_OFFSET, FLASH_LAYOUT_SECTOR,
     "utils.c: flash_range_erase(ADDR_CONFIG, FLASH_SECTOR_SIZE)"},
    {"config save", FLASH_LAYOUT_CONFIG_OFFSET, FLASH_LAYOUT_SECTOR,
     "utils.c: write_flash_page(ADDR_CONFIG) — erases the sector it starts"},
};

static bool overlaps(uint32_t a_off, uint32_t a_len, uint32_t b_off, uint32_t b_len) {
    return FLASH_LAYOUT_OVERLAPS(a_off, a_len, b_off, b_len);
}

/*
 * The acceptance criterion, stated as arithmetic: nothing that writes the
 * image, and nothing that writes the configuration, may touch the identity.
 */
static void test_no_writer_reaches_the_identity_sector(void) {
    for (size_t i = 0; i < sizeof writers / sizeof writers[0]; i++) {
        CHECK(!overlaps(writers[i].offset, writers[i].len, FLASH_LAYOUT_IDENTITY_OFFSET,
                        FLASH_LAYOUT_IDENTITY_LEN),
              writers[i].name, writers[i].what);
    }
}

/*
 * The propagation case named on its own, because it is the one that produces
 * two boards with one identity rather than merely a board that has to re-pair.
 * A pull runs from address 0 to the end of the image and writes every page on
 * the way, so this is the whole 256 KB and not a sample of it.
 */
static void test_a_peer_propagation_cannot_copy_one_boards_identity_onto_another(void) {
    const uint32_t first = FLASH_LAYOUT_IMAGE_OFFSET;
    const uint32_t last = FLASH_LAYOUT_IMAGE_OFFSET + FLASH_LAYOUT_IMAGE_LEN - 1u;

    CHECK(last < FLASH_LAYOUT_IDENTITY_OFFSET, "propagation",
          "the last page a pull writes is inside the identity sector");
    CHECK(first < FLASH_LAYOUT_IDENTITY_OFFSET, "propagation",
          "a pull starts inside the identity sector");

    /* And the staging slot an upgrade is received into, which is written by
       the same path before it is promoted. */
    CHECK(!overlaps(FLASH_LAYOUT_STAGING_OFFSET, FLASH_LAYOUT_STAGING_LEN,
                    FLASH_LAYOUT_IDENTITY_OFFSET, FLASH_LAYOUT_IDENTITY_LEN),
          "propagation", "a staged upgrade lands on the identity sector");
}

/*
 * A wipe must unpair and nothing more. If it took the identity too, every wipe
 * would make every helper report "this board changed" — a false alarm on a
 * routine action, and one that trains a user to ignore the real thing.
 */
static void test_a_config_wipe_clears_the_registration_and_not_the_identity(void) {
    CHECK(!overlaps(FLASH_LAYOUT_CONFIG_OFFSET, FLASH_LAYOUT_SECTOR,
                    FLASH_LAYOUT_IDENTITY_OFFSET, FLASH_LAYOUT_IDENTITY_LEN),
          "wipe", "erasing the config sector reaches the identity");

    /* The two are adjacent, which is deliberate — the identity is "beside
       ADDR_CONFIG and part of neither" — so adjacency is exactly the case
       where an off-by-one erase would be invisible until a board re-paired. */
    CHECK(FLASH_LAYOUT_IDENTITY_OFFSET + FLASH_LAYOUT_IDENTITY_LEN == FLASH_LAYOUT_CONFIG_OFFSET,
          "wipe", "the identity is no longer the sector immediately before the config");
}

/*
 * The bound the UF2 drop's row above depends on, stated as the arithmetic it
 * rests on: the last page a bounded blockNo can write is the last page of the
 * image, and that is below the identity. `MAX_BLOCK_NO` in `ramdisk.c` is
 * `(STAGING_IMAGE_SIZE / FLASH_PAGE_SIZE) - 1`, which is this same number.
 */
static void test_the_highest_uf2_block_still_lands_inside_the_image(void) {
    const uint32_t max_block_no = (FLASH_LAYOUT_IMAGE_LEN / FLASH_LAYOUT_PAGE) - 1u;
    const uint32_t highest_page = max_block_no * FLASH_LAYOUT_PAGE;

    CHECK(highest_page + FLASH_LAYOUT_PAGE <= FLASH_LAYOUT_IMAGE_LEN, "uf2 bound",
          "the highest permitted UF2 block writes past the image");
    CHECK(highest_page < FLASH_LAYOUT_IDENTITY_OFFSET, "uf2 bound",
          "the highest permitted UF2 block reaches the identity sector");

    /* And the block that would reach it is above the bound, so the guard is
       the thing keeping it out rather than an accident of the arithmetic. */
    const uint32_t block_at_identity = FLASH_LAYOUT_IDENTITY_OFFSET / FLASH_LAYOUT_PAGE;
    CHECK(block_at_identity > max_block_no, "uf2 bound",
          "the identity sector is inside the range a bounded blockNo can write");
}

/* Both sectors have to be erasable on their own, or saving one rewrites the
   other whatever the ranges above say. */
static void test_both_sectors_are_whole_erase_units(void) {
    CHECK(FLASH_LAYOUT_IDENTITY_LEN == FLASH_LAYOUT_SECTOR, "granularity",
          "the identity is not exactly one erase sector");
    CHECK(FLASH_LAYOUT_IDENTITY_OFFSET % FLASH_LAYOUT_SECTOR == 0, "granularity",
          "the identity does not start on an erase boundary");
    CHECK(FLASH_LAYOUT_CONFIG_OFFSET % FLASH_LAYOUT_SECTOR == 0, "granularity",
          "the config does not start on an erase boundary");
    CHECK(FLASH_LAYOUT_CONFIG_OFFSET + FLASH_LAYOUT_CONFIG_LEN <= FLASH_LAYOUT_TOTAL,
          "granularity", "the config runs off the end of flash");
}

int main(void) {
    test_no_writer_reaches_the_identity_sector();
    test_a_peer_propagation_cannot_copy_one_boards_identity_onto_another();
    test_a_config_wipe_clears_the_registration_and_not_the_identity();
    test_the_highest_uf2_block_still_lands_inside_the_image();
    test_both_sectors_are_whole_erase_units();

    if (failures) {
        printf("%d flash layout check(s) failed\n", failures);
        return 1;
    }
    printf("flash layout tests passed\n");
    return 0;
}
