#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dh_keyboard_transport.h"

static int failures;

#define CHECK(condition, message)                                                        \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, (message));             \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

static void test_physical_report_crosses_with_its_provenance(void) {
    const uint8_t report[DH_KEYBOARD_REPORT_LENGTH] = {0x01, 0x00, 0x04, 0x05, 0, 0, 0, 0};
    uint8_t payload[DH_KEYBOARD_REPORT_LENGTH] = {0};

    const uint8_t packet_type = dh_keyboard_transport_encode(
        DH_KEYBOARD_PHYSICAL, report, payload);

    CHECK(packet_type == DH_KEYBOARD_PHYSICAL_PACKET,
          "physical report used the wrong packet marker");
    CHECK(memcmp(payload, report, sizeof report) == 0,
          "physical report bytes changed in transport");

    dh_keyboard_provenance provenance = DH_KEYBOARD_SYNTHESIZED;
    const uint8_t *decoded = NULL;
    CHECK(dh_keyboard_transport_decode(packet_type, payload, &provenance, &decoded),
          "physical report was not recognized on receipt");
    CHECK(provenance == DH_KEYBOARD_PHYSICAL,
          "physical report arrived as synthesized input");
    CHECK(decoded == payload, "decode copied or replaced the report payload");
}

static void test_synthesized_report_crosses_with_its_provenance(void) {
    const uint8_t lock_report[DH_KEYBOARD_REPORT_LENGTH] = {0x09, 0x00, 0x14, 0, 0, 0, 0, 0};
    uint8_t payload[DH_KEYBOARD_REPORT_LENGTH] = {0};

    const uint8_t packet_type = dh_keyboard_transport_encode(
        DH_KEYBOARD_SYNTHESIZED, lock_report, payload);

    CHECK(packet_type == DH_KEYBOARD_SYNTHESIZED_PACKET,
          "synthesized report used the wrong packet marker");
    CHECK(memcmp(payload, lock_report, sizeof lock_report) == 0,
          "synthesized report bytes changed in transport");

    dh_keyboard_provenance provenance = DH_KEYBOARD_PHYSICAL;
    const uint8_t *decoded = NULL;
    CHECK(dh_keyboard_transport_decode(packet_type, payload, &provenance, &decoded),
          "synthesized report was not recognized on receipt");
    CHECK(provenance == DH_KEYBOARD_SYNTHESIZED,
          "synthesized report arrived as physical input");
    CHECK(decoded == payload, "decode copied or replaced the report payload");
}

int main(void) {
    test_physical_report_crosses_with_its_provenance();
    test_synthesized_report_crosses_with_its_provenance();
    return failures == 0 ? 0 : 1;
}
