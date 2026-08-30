#include <stdio.h>
#include <string.h>

#include "dh_place.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL place: %s\n", message); failures++; } \
} while (0)

int main(void) {
    const dh_place place = {
        .chain_index = 2,
        .chain_direction = 1,
        .border_direction = 4,
        .entry_position = 0x9234,
    };
    uint8_t body[DH_PLACE_BODY_SIZE] = {0};
    CHECK(dh_place_encode(&place, body, sizeof body), "PLACE did not encode");
    const uint8_t expected[] = {2, 1, 4, 0x34, 0x92};
    CHECK(memcmp(body, expected, sizeof expected) == 0, "PLACE wire body changed");

    dh_place decoded = {0};
    CHECK(dh_place_decode(body, sizeof body, &decoded), "PLACE did not decode");
    CHECK(decoded.chain_index == place.chain_index &&
              decoded.chain_direction == place.chain_direction &&
              decoded.border_direction == place.border_direction &&
              decoded.entry_position == place.entry_position,
          "PLACE did not round-trip");
    CHECK(!dh_place_decode(body, sizeof body - 1, &decoded), "short PLACE was accepted");

    const dh_position position = {.query_id = 7, .chain_index = 2, .x = 0x1234, .y = 0xabcd};
    uint8_t pos_body[DH_POSITION_BODY_SIZE] = {0};
    CHECK(dh_position_encode(&position, pos_body, sizeof pos_body),
          "POS_RESPONSE did not encode");
    const uint8_t expected_pos[] = {7, 2, 0x34, 0x12, 0xcd, 0xab};
    CHECK(memcmp(pos_body, expected_pos, sizeof expected_pos) == 0,
          "POS_RESPONSE wire body changed");
    dh_position decoded_position = {0};
    CHECK(dh_position_decode(pos_body, sizeof pos_body, &decoded_position),
          "POS_RESPONSE did not decode");
    CHECK(decoded_position.query_id == position.query_id &&
              decoded_position.chain_index == position.chain_index &&
              decoded_position.x == position.x && decoded_position.y == position.y,
          "POS_RESPONSE did not round-trip");

    const dh_display_rect mac[] = {
        {.x = 1920, .y = 0, .width = 1920, .height = 1080},
        {.x = 0, .y = 0, .width = 1920, .height = 1080},
    };
    dh_place mac_left = {
        .chain_index = 2, .chain_direction = 1, .border_direction = 5,
        .entry_position = 32768,
    };
    dh_place_point point = {0};
    CHECK(dh_place_target(&mac_left, mac, 2, 0, &point),
          "Mac left display was not found from a right-hand primary");
    CHECK(point.display_index == 1 && point.x == 960 && point.y == 1079,
          "Mac left bottom seam placement was wrong");

    const dh_display_rect windows[] = {
        {.x = 0, .y = 0, .width = 2560, .height = 1440},
        {.x = 2560, .y = 0, .width = 2560, .height = 1440},
    };
    dh_place windows_right = {
        .chain_index = 2, .chain_direction = 2, .border_direction = 4,
        .entry_position = 49151,
    };
    CHECK(dh_place_target(&windows_right, windows, 2, 0, &point),
          "Windows right display was not found from a left-hand primary");
    CHECK(point.display_index == 1 && point.x == 4479 && point.y == 0,
          "Windows right top seam placement was wrong");

    if (failures) return 1;
    printf("place_test: all checks passed\n");
    return 0;
}
