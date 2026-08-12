/*
 * The peer board's firmware version, as tracked from its heartbeats (#89).
 *
 * Three ways to be wrong, all invisible from the board itself: reporting a
 * version for a peer that has gone, forgetting one that is still there, and
 * rendering "never heard from" as though it were a version.
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
        peer_fw_record(&peer, 182, SECONDS(10));
        CHECK(peer.version == 182, "heard", "a heartbeat makes it known");
    }

    /* Fresh right up to the threshold: three heartbeat intervals, two missed
       beats plus one of grace. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(10));

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
        peer_fw_record(&peer, 182, SECONDS(0));

        for (int second = 1; second <= 10; second++) {
            peer_fw_expire(&peer, SECONDS(second));
            peer_fw_record(&peer, 182, SECONDS(second));
        }

        peer_fw_expire(&peer, SECONDS(10));
        CHECK(peer.version == 182, "refresh", "a talking peer is never forgotten");
    }

    /* A version left reading as current after the peer has gone answers "are
       both boards on the same build?" with a confident lie. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(0));

        peer_fw_expire(&peer, SECONDS(2));
        CHECK(peer.version == 182, "quiet", "two seconds is not yet gone");

        peer_fw_expire(&peer, SECONDS(4));
        CHECK(peer.version == PEER_FW_UNKNOWN, "quiet", "a silent peer is forgotten");
    }

    /* A peer upgraded under us reports the new version, and it replaces the
       old rather than being ignored as a duplicate. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 181, SECONDS(0));
        peer_fw_record(&peer, 182, SECONDS(1));
        CHECK(peer.version == 182, "upgraded", "the newer version replaces it");
    }

    /* A timestamp behind the one recorded is not elapsed time. Unsigned
       subtraction would read it as centuries and forget a peer that is there. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(100));

        peer_fw_expire(&peer, SECONDS(1));
        CHECK(peer.version == 182, "backwards", "a past timestamp expires nothing");
    }

    if (failures == 0)
        printf("peer_fw_test: all checks passed\n");

    return failures == 0 ? 0 : 1;
}
