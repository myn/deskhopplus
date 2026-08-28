#include "dh_keyboard_transport.h"

#include <string.h>

uint8_t dh_keyboard_transport_encode(dh_keyboard_provenance provenance,
                                     const uint8_t report[DH_KEYBOARD_REPORT_LENGTH],
                                     uint8_t payload[DH_KEYBOARD_REPORT_LENGTH]) {
    memcpy(payload, report, DH_KEYBOARD_REPORT_LENGTH);
    return provenance == DH_KEYBOARD_SYNTHESIZED
               ? DH_KEYBOARD_SYNTHESIZED_PACKET
               : DH_KEYBOARD_PHYSICAL_PACKET;
}

bool dh_keyboard_transport_decode(uint8_t packet_type,
                                  const uint8_t payload[DH_KEYBOARD_REPORT_LENGTH],
                                  dh_keyboard_provenance *provenance,
                                  const uint8_t **report) {
    if (packet_type == DH_KEYBOARD_PHYSICAL_PACKET)
        *provenance = DH_KEYBOARD_PHYSICAL;
    else if (packet_type == DH_KEYBOARD_SYNTHESIZED_PACKET)
        *provenance = DH_KEYBOARD_SYNTHESIZED;
    else
        return false;

    *report = payload;
    return true;
}
