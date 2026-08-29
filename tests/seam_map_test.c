#include <stdint.h>
#include <stdio.h>

#include "dh_seam_map.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL seam_map: %s\n", message); failures++; } \
} while (0)

/* Independent translation of mkroamer's edge_map.h oracle. */
static uint16_t mkroamer_cross(uint16_t position, uint16_t start, uint16_t end) {
    const double fraction = (double)(position - start) / (double)(end - start);
    return (uint16_t)(fraction * 65535.0 + 0.5);
}

static uint16_t mkroamer_entry(uint16_t position, uint16_t start, uint16_t end) {
    return (uint16_t)(start + ((double)position / 65535.0) * (end - start) + 0.5);
}

static void test_cross_and_entry_match_mkroamer(void) {
    const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY] = {
        {.screen_index = 1, .start = 0, .end = 30000},
        {.screen_index = 2, .start = 12000, .end = 65535},
    };
    const uint16_t shared[] = {0, 1, 15000, 29999, 30000};

    for (unsigned i = 0; i < sizeof shared / sizeof shared[0]; i++) {
        dh_seam_hit_t hit;
        CHECK(dh_seam_cross(ranges, 1, shared[i], &hit),
              "configured source position did not cross");
        CHECK(hit.segment == 0, "crossing selected the wrong segment");
        CHECK(hit.position == mkroamer_cross(shared[i], 0, 30000),
              "crossing differs from mkroamer's normalized edge position");
    }

    for (uint32_t position = 0; position <= 65535; position += 4095) {
        uint16_t entry;
        CHECK(dh_seam_entry(ranges, 1, (uint16_t)position, &entry),
              "configured target segment did not accept entry");
        CHECK(entry == mkroamer_entry((uint16_t)position, 12000, 65535),
              "entry differs from mkroamer's edge position mapping");
    }
}

static void test_gaps_boundaries_and_degenerate_ranges_match_mkroamer(void) {
    const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY] = {
        {.screen_index = 1, .start = 0, .end = 10000},
        {.screen_index = 1, .start = 10000, .end = 20000},
        {.screen_index = 2, .start = 30000, .end = 40000},
        {.screen_index = 3, .start = 50000, .end = 50000},
    };
    dh_seam_hit_t hit;
    uint16_t entry;

    CHECK(dh_seam_cross(ranges, 1, 10000, &hit) && hit.segment == 1 && hit.position == 0,
          "a shared boundary did not belong to the later segment");
    CHECK(!dh_seam_cross(ranges, 2, 25000, &hit),
          "a gap between configured positions crossed");
    CHECK(!dh_seam_cross(ranges, 4, 35000, &hit),
          "an unconfigured screen index crossed");
    CHECK(!dh_seam_cross(ranges, 3, 50000, &hit),
          "a degenerate source range was treated as configured");
    CHECK(!dh_seam_entry(ranges, 3, 40000, &entry),
          "a degenerate target range was treated as configured");
}

static void test_an_empty_map_is_distinct_from_a_configured_map(void) {
    const dh_seam_range_t empty[DH_SEAM_RANGE_CAPACITY] = {0};
    const dh_seam_range_t configured[DH_SEAM_RANGE_CAPACITY] = {
        {.screen_index = 1, .start = 0, .end = 0},
    };
    CHECK(!dh_seam_map_is_configured(empty), "an empty map was treated as configured");
    CHECK(!dh_seam_map_is_configured(configured),
          "a single-point partial range was treated as configured");
}

static void test_partial_slots_degrade_without_disabling_paired_enforcement(void) {
    const dh_seam_range_t source[DH_SEAM_RANGE_CAPACITY] = {
        {.screen_index = 1, .start = 0, .end = 20000},
        {.screen_index = 2, .start = 30000, .end = 40000},
    };
    const dh_seam_range_t partial_target[DH_SEAM_RANGE_CAPACITY] = {
        {0},
        {.screen_index = 3, .start = 10000, .end = 20000},
    };
    dh_seam_entry_t entry;

    CHECK(dh_seam_resolve_crossing(source, partial_target, 1, 4, 4, 10000, &entry) ==
              DH_SEAM_CROSSING_LEGACY,
          "a one-sided segment trapped the cursor before its peer was configured");
    CHECK(dh_seam_resolve_crossing(source, partial_target, 2, 4, 4, 35000, &entry) ==
              DH_SEAM_CROSSING_MAPPED && entry.screen_index == 3,
          "a paired segment did not map while another slot was partial");
    CHECK(dh_seam_resolve_crossing(source, partial_target, 4, 4, 4, 35000, &entry) ==
              DH_SEAM_CROSSING_BLOCKED,
          "a monitor without a range crossed after a paired map became active");
}

static void test_nonexistent_screens_do_not_activate_range_enforcement(void) {
    const dh_seam_range_t source[DH_SEAM_RANGE_CAPACITY] = {
        {.screen_index = 1, .start = 0, .end = 20000},
    };
    const dh_seam_range_t target[DH_SEAM_RANGE_CAPACITY] = {
        {.screen_index = 255, .start = 0, .end = 20000},
    };
    dh_seam_entry_t entry;

    CHECK(dh_seam_resolve_crossing(source, target, 2, 2, 2, 30000, &entry) ==
              DH_SEAM_CROSSING_LEGACY,
          "a nonexistent target screen activated enforcement for another monitor");
}

int main(void) {
    test_cross_and_entry_match_mkroamer();
    test_gaps_boundaries_and_degenerate_ranges_match_mkroamer();
    test_an_empty_map_is_distinct_from_a_configured_map();
    test_partial_slots_degrade_without_disabling_paired_enforcement();
    test_nonexistent_screens_do_not_activate_range_enforcement();
    if (failures) return 1;
    printf("seam_map_test: all checks passed\n");
    return 0;
}
