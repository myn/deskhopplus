/*
 * The peer board's firmware version, as tracked from its heartbeats (#89).
 *
 * Four ways to be wrong, all invisible from the board itself: reporting a
 * version for a peer that has gone, forgetting one that is still there,
 * rendering "never heard from" as though it were a version, and -- since #91
 * put two builds at one version number -- calling two different images a
 * match because their versions agree.
 *
 * Split out of handlers.c for the same reason config_store.c was split out of
 * the flash path (#74): the arithmetic is pure, and behind a UART handler
 * nothing could reach it.
 *
 * Style follows config_test.c: a named assertion macro, a main, a printed
 * failure line, a non-zero exit — no framework.
 */

#include <stdio.h>

#include "peer_fw.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

#define SECONDS(n) ((uint64_t)(n) * 1000000ull)

/* Two image checksums. Only their difference matters -- these stand in for
   two builds carrying the same version number, which is the case #91 created
   and the reason this struct holds a checksum at all. */
#define CRC_A 0xc26fdcc0u
#define CRC_B 0x5a5a1234u

int main(void) {
    /* An unplugged peer and a peer on the same build must not look alike. */
    {
        peer_fw_t peer = {0};
        CHECK(peer.version == PEER_FW_UNKNOWN, "cold", "nothing heard is unknown");

        /* Expiring an absence neither resurrects a version nor underflows the
           subtraction against a zero timestamp. */
        peer_fw_expire(&peer, SECONDS(3600));
        CHECK(peer.version == PEER_FW_UNKNOWN, "cold", "expiry leaves it unknown");
    }

    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, CRC_A, SECONDS(10));
        CHECK(peer.version == 182, "heard", "a heartbeat makes it known");
    }

    /* Fresh right up to the threshold: three heartbeat intervals, two missed
       beats plus one of grace. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, CRC_A, SECONDS(10));

        peer_fw_expire(&peer, SECONDS(10) + PEER_FW_STALE_US - 1);
        CHECK(peer.version == 182, "window", "still known one tick short");

        peer_fw_expire(&peer, SECONDS(10) + PEER_FW_STALE_US);
        CHECK(peer.version == PEER_FW_UNKNOWN, "window", "forgotten on the threshold");
    }

    /* Every heartbeat refreshes it, so a peer that keeps talking is never
       forgotten however long it has been up. Without the refresh this would
       expire on the strength of the first beat alone. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, CRC_A, SECONDS(0));

        for (int second = 1; second <= 10; second++) {
            peer_fw_expire(&peer, SECONDS(second));
            peer_fw_record(&peer, 182, CRC_A, SECONDS(second));
        }

        peer_fw_expire(&peer, SECONDS(10));
        CHECK(peer.version == 182, "refresh", "a talking peer is never forgotten");
    }

    /* A version left reading as current after the peer has gone answers "are
       both boards on the same build?" with a confident lie. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, CRC_A, SECONDS(0));

        peer_fw_expire(&peer, SECONDS(2));
        CHECK(peer.version == 182, "quiet", "two seconds is not yet gone");

        peer_fw_expire(&peer, SECONDS(4));
        CHECK(peer.version == PEER_FW_UNKNOWN, "quiet", "a silent peer is forgotten");
    }

    /* A peer upgraded under us reports the new version, and it replaces the
       old rather than being ignored as a duplicate. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 181, CRC_A, SECONDS(0));
        peer_fw_record(&peer, 182, CRC_A, SECONDS(1));
        CHECK(peer.version == 182, "upgraded", "the newer version replaces it");
    }

    /* The case the version cannot describe, and the whole reason the checksum
       is tracked: since #91 two boards can hold *different images at the same
       version*. A tracker that kept only the version would report a match. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 194, CRC_A, SECONDS(0));

        peer_fw_record(&peer, 194, CRC_B, SECONDS(1));
        CHECK(peer.version == 194, "same version", "the version is unchanged");
        CHECK(peer.checksum == CRC_B, "same version", "the new image replaces it");
    }

    /* A checksum outliving the version it belongs to would be worse than no
       checksum: it would name a build for a peer that is no longer there. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 194, CRC_A, SECONDS(0));

        peer_fw_expire(&peer, SECONDS(4));
        CHECK(peer.version == PEER_FW_UNKNOWN, "expiry", "the version is forgotten");
        CHECK(peer.checksum == 0, "expiry", "and the checksum goes with it");
    }

    /* A timestamp behind the one recorded is not elapsed time. Unsigned
       subtraction would read it as centuries and forget a peer that is there. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, CRC_A, SECONDS(100));

        peer_fw_expire(&peer, SECONDS(1));
        CHECK(peer.version == 182, "backwards", "a past timestamp expires nothing");
    }

    if (failures == 0)
        printf("peer_fw_test: all checks passed\n");

    return failures == 0 ? 0 : 1;
}
