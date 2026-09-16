/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * The bundle a kind-3 CLIP_OFFER carries (#195): more than one representation
 * of one copy, so the pasting application picks the one it wants.
 *
 * The expected bytes are written by hand rather than produced by the packer,
 * so a change of layout fails here instead of agreeing with itself.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dh_bundle.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL bundle: %s\n", message); failures++; } \
} while (0)

static bool part_is(const dh_bundle_part *part, uint8_t kind, const void *bytes, uint32_t len) {
    return part->kind == kind && part->len == len && memcmp(part->bytes, bytes, len) == 0;
}

static void test_text_and_png_round_trip(void) {
    static const uint8_t png[] = {0x89, 'P', 'N', 'G'};
    const dh_bundle_part out[] = {
        {.kind = DH_BUNDLE_PART_TEXT, .bytes = (const uint8_t *)"hi", .len = 2},
        {.kind = DH_BUNDLE_PART_PNG, .bytes = png, .len = sizeof png},
    };
    uint8_t packed[64];
    const size_t written = dh_bundle_pack(out, 2, packed, sizeof packed);

    /* part_kind:u8 len:u32 (little-endian) bytes, twice. */
    static const uint8_t expected[] = {0, 2, 0, 0, 0, 'h', 'i',
                                       1, 4, 0, 0, 0, 0x89, 'P', 'N', 'G'};
    CHECK(written == sizeof expected && memcmp(packed, expected, sizeof expected) == 0,
          "text and a PNG did not pack to the layout docs/protocol.md states");

    dh_bundle bundle;
    CHECK(dh_bundle_unpack(packed, written, &bundle), "the packed bundle would not unpack");
    CHECK(bundle.count == 2, "two parts did not unpack to two parts");
    CHECK(part_is(&bundle.parts[0], DH_BUNDLE_PART_TEXT, "hi", 2) &&
              part_is(&bundle.parts[1], DH_BUNDLE_PART_PNG, png, sizeof png),
          "the parts did not survive the round trip in order");
}

/*
 * The boundary. A length arrives from the other computer and would otherwise
 * be read past the end of the buffer it arrived in.
 */
static void test_a_part_that_runs_past_the_payload_is_refused(void) {
    static const uint8_t overrun[] = {0, 9, 0, 0, 0, 'h', 'i'}; /* says 9, holds 2 */
    static const uint8_t short_head[] = {0, 2, 0};              /* not even a length */
    static const uint8_t after_a_good_part[] = {0, 2, 0, 0, 0, 'h', 'i', 1, 1, 0, 0, 0};
    dh_bundle bundle;
    CHECK(!dh_bundle_unpack(overrun, sizeof overrun, &bundle),
          "a part longer than the payload was accepted");
    CHECK(!dh_bundle_unpack(short_head, sizeof short_head, &bundle),
          "a part header cut short was accepted");
    CHECK(!dh_bundle_unpack(after_a_good_part, sizeof after_a_good_part, &bundle),
          "an overrun after a good part was accepted");
}

/* A newer far helper adding RTF must not cost this one the text beside it. */
static void test_an_unknown_part_kind_is_skipped_not_refused(void) {
    static const uint8_t payload[] = {7, 3, 0, 0, 0, 'r', 't', 'f', 0, 2, 0, 0, 0, 'h', 'i'};
    dh_bundle bundle;
    CHECK(dh_bundle_unpack(payload, sizeof payload, &bundle),
          "a bundle with an unknown part beside a known one was refused");
    CHECK(bundle.count == 1 && part_is(&bundle.parts[0], DH_BUNDLE_PART_TEXT, "hi", 2),
          "the known part was not the one kept");
}

/* A bundle of nothing has nothing to deliver, as an empty file list has. */
static void test_a_bundle_with_nothing_to_write_is_refused(void) {
    static const uint8_t only_unknown[] = {7, 1, 0, 0, 0, 'x'};
    dh_bundle bundle;
    CHECK(!dh_bundle_unpack((const uint8_t *)"", 0, &bundle), "an empty payload was accepted");
    CHECK(!dh_bundle_unpack(only_unknown, sizeof only_unknown, &bundle),
          "a bundle of only unknown parts was accepted");
    CHECK(dh_bundle_pack(NULL, 0, (uint8_t[8]){0}, 8) == 0, "a bundle of no parts packed");
}

static void test_more_parts_than_the_bundle_holds_is_refused(void) {
    uint8_t payload[(DH_BUNDLE_PARTS_MAX + 1) * 6];
    for (unsigned i = 0; i <= DH_BUNDLE_PARTS_MAX; i++) {
        const uint8_t part[] = {0, 1, 0, 0, 0, 'a'};
        memcpy(payload + i * sizeof part, part, sizeof part);
    }
    dh_bundle bundle;
    CHECK(!dh_bundle_unpack(payload, sizeof payload, &bundle),
          "a bundle with more parts than the decoder holds was accepted");
}

static void test_packing_refuses_a_buffer_it_would_overrun(void) {
    const dh_bundle_part part = {.kind = DH_BUNDLE_PART_TEXT, .bytes = (const uint8_t *)"hello",
                                 .len = 5};
    uint8_t packed[8];
    CHECK(dh_bundle_packed_len(&part, 1) == 10, "the packed length is not header plus bytes");
    CHECK(dh_bundle_pack(&part, 1, packed, sizeof packed) == 0,
          "packing into too small a buffer reported success");
}

int main(void) {
    test_text_and_png_round_trip();
    test_a_part_that_runs_past_the_payload_is_refused();
    test_an_unknown_part_kind_is_skipped_not_refused();
    test_a_bundle_with_nothing_to_write_is_refused();
    test_more_parts_than_the_bundle_holds_is_refused();
    test_packing_refuses_a_buffer_it_would_overrun();

    if (failures == 0) printf("bundle: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
