/*
 * The peer board's firmware version, tracked from its heartbeats (#89).
 * See peer_fw.h for why this is not inside the UART handler.
 */

#include "peer_fw.h"

void peer_fw_record(peer_fw_t *peer, uint16_t version, uint64_t now_us) {
    peer->version     = version;
    peer->heard_at_us = now_us;
}

void peer_fw_expire(peer_fw_t *peer, uint64_t now_us) {
    if (peer->version == PEER_FW_UNKNOWN)
        return;

    /* A timestamp behind the one recorded is not elapsed time. The subtraction
       is unsigned, so treating it as such would read as centuries and forget a
       peer that is sitting right there. */
    if (now_us <= peer->heard_at_us)
        return;

    if (now_us - peer->heard_at_us >= PEER_FW_STALE_US)
        peer->version = PEER_FW_UNKNOWN;
}
