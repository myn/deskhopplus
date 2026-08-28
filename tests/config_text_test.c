/* TDD specification for issue #23's public config-text parsing seam. */

#include <stdio.h>
#include <string.h>

#include "dh_config_text.h"

static int failures;
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);         \
        }                                                                       \
    } while (0)

static void test_key_names_cover_the_config_vocabulary_and_round_trip(void) {
    const struct {
        const char *name;
        uint8_t usage;
    } examples[] = {{"a", 0x04},          {"z", 0x1d},       {"1", 0x1e},
                    {"0", 0x27},          {"f1", 0x3a},      {"f12", 0x45},
                    {"lctrl", 0xe0},      {"rgui", 0xe7},    {"semicolon", 0x33},
                    {"printscreen", 0x46}, {"kp_divide", 0x54}, {"kp_period", 0x63}};

    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        uint8_t usage = 0;
        CHECK(dh_key_name_parse(examples[i].name, strlen(examples[i].name), &usage));
        CHECK(usage == examples[i].usage);
        CHECK(strcmp(dh_key_name(examples[i].usage), examples[i].name) == 0);
    }

    for (unsigned usage = 1; usage <= UINT8_MAX; ++usage) {
        const char *name = dh_key_name((uint8_t)usage);
        if (name) {
            uint8_t parsed = 0;
            CHECK(dh_key_name_parse(name, strlen(name), &parsed));
            CHECK(parsed == usage);
        }
    }

    uint8_t untouched = 0x5a;
    CHECK(!dh_key_name_parse("notakey", 7, &untouched));
    CHECK(untouched == 0x5a);
    CHECK(dh_key_name(0x02) == NULL);
}

static void test_overrides_are_bounded_transactional_and_identify_errors(void) {
    dh_key_override_t overrides[2] = {{0xaa, 0xbb}, {0xcc, 0xdd}};
    size_t count = 99;
    dh_config_text_error_t error;

    CHECK(dh_config_text_parse_overrides("capslock=lctrl\na = b\n", overrides, 2, &count, &error));
    CHECK(count == 2);
    CHECK(overrides[0].from == 0x39 && overrides[0].to == 0xe0);
    CHECK(overrides[1].from == 0x04 && overrides[1].to == 0x05);

    const dh_key_override_t before[2] = {{overrides[0].from, overrides[0].to},
                                         {overrides[1].from, overrides[1].to}};
    count = 17;
    CHECK(!dh_config_text_parse_overrides("a=b\nnope=c\n", overrides, 2, &count, &error));
    CHECK(error.code == DH_CONFIG_TEXT_UNKNOWN_KEY && error.line == 2);
    CHECK(strcmp(error.token, "nope") == 0);
    CHECK(count == 17 && memcmp(overrides, before, sizeof(before)) == 0);

    CHECK(!dh_config_text_parse_overrides("a=b\nc=d\ne=f\n", overrides, 2, &count, &error));
    CHECK(error.code == DH_CONFIG_TEXT_CAPACITY && error.line == 3);
    CHECK(count == 17 && memcmp(overrides, before, sizeof(before)) == 0);
}

static void test_passthrough_and_chords_reject_malformed_or_oversized_input(void) {
    uint8_t passthrough[3] = {0xaa, 0xbb, 0xcc};
    size_t count = 8;
    dh_config_text_error_t error;

    CHECK(dh_config_text_parse_keys("capslock, f1\nkp_enter", passthrough, 3, &count, &error));
    CHECK(count == 3 && passthrough[0] == 0x39 && passthrough[1] == 0x3a &&
          passthrough[2] == 0x58);

    const uint8_t before[3] = {passthrough[0], passthrough[1], passthrough[2]};
    count = 8;
    CHECK(!dh_config_text_parse_keys("a,b,c,d", passthrough, 3, &count, &error));
    CHECK(error.code == DH_CONFIG_TEXT_CAPACITY);
    CHECK(count == 8 && memcmp(passthrough, before, sizeof(before)) == 0);

    CHECK(!dh_config_text_parse_keys("a,,b", passthrough, 3, &count, &error));
    CHECK(error.code == DH_CONFIG_TEXT_EXPECTED_KEY);
    CHECK(count == 8 && memcmp(passthrough, before, sizeof(before)) == 0);

    dh_key_chord_t chord = {.keys = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, .key_count = 9};
    const dh_key_chord_t chord_before = chord;
    CHECK(dh_config_text_parse_chord("ralt+k", &chord, &error));
    CHECK(chord.key_count == 2 && chord.keys[0] == 0xe6 && chord.keys[1] == 0x0e);

    chord = chord_before;
    CHECK(!dh_config_text_parse_chord("a+b+c+d+e+f+g", &chord, &error));
    CHECK(error.code == DH_CONFIG_TEXT_CAPACITY);
    CHECK(memcmp(&chord, &chord_before, sizeof(chord)) == 0);

    CHECK(!dh_config_text_parse_chord("lctrl++k", &chord, &error));
    CHECK(error.code == DH_CONFIG_TEXT_EXPECTED_KEY);
    CHECK(memcmp(&chord, &chord_before, sizeof(chord)) == 0);
}

int main(void) {
    test_key_names_cover_the_config_vocabulary_and_round_trip();
    test_overrides_are_bounded_transactional_and_identify_errors();
    test_passthrough_and_chords_reject_malformed_or_oversized_input();
    if (failures == 0)
        printf("config_text_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
