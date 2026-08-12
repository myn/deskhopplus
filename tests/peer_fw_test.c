/*
 * The peer board's firmware version, as tracked from its heartbeats (#89).
 *
 * The decision under test is small but has three ways to be wrong, and all
 * three are invisible from the board itself: reporting a version for a peer
 * that has gone away, forgetting one that is still there, and rendering
 * "never heard from" as though it were a version. The last is the reason
 * unknown is zero rather than a plausible-looking number — the config UI
 * derives major and minor as (value - 100) / 1000 and (value - 100) % 1000,
 * so anything it is handed gets displayed as *something*, and only a value
 * the UI special-cases reads as an absence.
 *
 * Split out of handlers.c for the same reason config_store.c was split out
 * of the flash path (#74): the arithmetic is pure, and behind a UART handler
 * nothing could reach it.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdio.h>

#include "peer_fw.h"

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++failures;                                                        \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                      \
    } while (0)

#define SECONDS(n) ((uint64_t)(n) * 1000000ull)

int main(void) {
    /* A board that has heard nothing has no peer version, and saying so is
       the point: an unplugged peer and a peer on the same build must not
       look alike. */
    {
        peer_fw_t peer = {0};
        CHECK(peer.version == PEER_FW_UNKNOWN);

        /* Expiring an absence neither resurrects a version nor underflows the
           subtraction against a zero timestamp. */
        peer_fw_expire(&peer, SECONDS(3600));
        CHECK(peer.version == PEER_FW_UNKNOWN);
    }

    /* A heartbeat carrying a version is what makes it known. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(10));
        CHECK(peer.version == 182);
    }

    /* Fresh right up to the threshold. Three heartbeat intervals, the same
       discipline the channel uses for its own liveness (ADR-0004) — two
       missed beats plus one of grace. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(10));

        peer_fw_expire(&peer, SECONDS(10) + PEER_FW_STALE_US - 1);
        CHECK(peer.version == 182);

        peer_fw_expire(&peer, SECONDS(10) + PEER_FW_STALE_US);
        CHECK(peer.version == PEER_FW_UNKNOWN);
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
        CHECK(peer.version == 182);
    }

    /* A peer that goes quiet is forgotten rather than left reading as
       current — the failure that makes a stale version worse than none,
       because it answers "are both boards on the same build?" with a
       confident lie. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(0));

        peer_fw_expire(&peer, SECONDS(2));
        CHECK(peer.version == 182);

        peer_fw_expire(&peer, SECONDS(4));
        CHECK(peer.version == PEER_FW_UNKNOWN);
    }

    /* A peer upgraded under us reports the new version, and it replaces the
       old rather than being ignored as a duplicate. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 181, SECONDS(0));
        peer_fw_record(&peer, 182, SECONDS(1));
        CHECK(peer.version == 182);
    }

    /* A timestamp behind the one already recorded must not expire anything.
       The subtraction is unsigned, so treating it as elapsed time would read
       as an enormous interval and forget a peer that is present. */
    {
        peer_fw_t peer = {0};
        peer_fw_record(&peer, 182, SECONDS(100));

        peer_fw_expire(&peer, SECONDS(1));
        CHECK(peer.version == 182);
    }

    if (failures == 0)
        printf("peer_fw_test: all checks passed\n");

    return failures == 0 ? 0 : 1;
}
