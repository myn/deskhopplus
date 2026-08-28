#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DH_KEYBOARD_REPORT_LENGTH 8u

/* Packet markers are part of the inter-board protocol. Keep protocol.h's
   named packet types pinned to these values. */
#define DH_KEYBOARD_PHYSICAL_PACKET    1u
#define DH_KEYBOARD_SYNTHESIZED_PACKET 28u

typedef enum {
    DH_KEYBOARD_PHYSICAL,
    DH_KEYBOARD_SYNTHESIZED,
} dh_keyboard_provenance;

uint8_t dh_keyboard_transport_encode(dh_keyboard_provenance provenance,
                                     const uint8_t report[DH_KEYBOARD_REPORT_LENGTH],
                                     uint8_t payload[DH_KEYBOARD_REPORT_LENGTH]);

bool dh_keyboard_transport_decode(uint8_t packet_type,
                                  const uint8_t payload[DH_KEYBOARD_REPORT_LENGTH],
                                  dh_keyboard_provenance *provenance,
                                  const uint8_t **report);
