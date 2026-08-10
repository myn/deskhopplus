/*
 * Tests for the transmit-queue overflow accounting (#43): a failed enqueue is
 * counted and reported to the caller; a successful one changes nothing.
 */

#include <stdio.h>

#include "dh_txq.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                   \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++failures;                                                           \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what));   \
        }                                                                         \
    } while (0)

int main(void) {
    /* A successful enqueue passes through and counts nothing. */
    {
        dh_txq_stats stats = {0};
        CHECK(dh_txq_track(&stats, true), "ok", "successful enqueue reported as failed");
        CHECK(stats.dropped == 0, "ok", "successful enqueue was counted as a drop");
    }

    /* A failed enqueue is reported to the caller and counted. */
    {
        dh_txq_stats stats = {0};
        CHECK(!dh_txq_track(&stats, false), "drop", "failed enqueue reported as success");
        CHECK(stats.dropped == 1, "drop", "failed enqueue not counted");
        dh_txq_track(&stats, false);
        dh_txq_track(&stats, true);
        dh_txq_track(&stats, false);
        CHECK(stats.dropped == 3, "drop", "drop count wrong after mixed results");
    }

    /* The counter saturates instead of wrapping to zero. */
    {
        dh_txq_stats stats = {.dropped = UINT32_MAX - 1};
        dh_txq_track(&stats, false);
        CHECK(stats.dropped == UINT32_MAX, "saturate", "did not reach saturation");
        CHECK(!dh_txq_track(&stats, false), "saturate", "result changed at saturation");
        CHECK(stats.dropped == UINT32_MAX, "saturate", "counter wrapped past UINT32_MAX");
    }

    if (failures == 0)
        printf("txq_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
