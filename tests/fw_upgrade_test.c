/*
 * Whether a firmware upgrade being received is still alive (#90).
 *
 * The defect this guards was invisible from the board: an upgrade that
 * stopped left `upgrade_in_progress` set for good, and the heartbeat, the
 * config-mode timeout and the config-mode LED all sat behind that flag. The
 * board looked healthy while being unreachable, and only a power cycle got it
 * back.
 *
 * Three ways to be wrong: never noticing a stall, calling a healthy transfer
 * stalled (which costs a manual reflash once flash has been written), and
 * mistaking "no upgrade at all" for a stalled one.
 *
 * Split out of tasks.c and handlers.c for the same reason peer_fw.c was split
 * out of the UART handler (#89): behind a task the decision could not be
 * reached.
 *
 * Style follows peer_fw_test.c: a named assertion macro, a main, a printed
 * failure line, a non-zero exit — no framework.
 */

#include <stdio.h>

#include "fw_upgrade.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

#define SECONDS(n) ((uint32_t)((n) * 1000000u))

int main(void) {
    /* A board with no upgrade running is the common case, and it must never
       look stalled — the caller asks unconditionally, once a second, forever. */
    {
        fw_upgrade_state_t fw = {0};
        CHECK(!fw_upgrade_stalled(&fw, SECONDS(3600)), "idle", "no upgrade is not a stall");
    }

    /* A transfer that has just begun has a full window ahead of it. Without
       the start being stamped, a zero timestamp would make every upgrade look
       stalled the moment it passed the window since boot. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true};
        fw_upgrade_progress(&fw, SECONDS(100));

        CHECK(!fw_upgrade_stalled(&fw, SECONDS(100)), "start", "a fresh upgrade is not stalled");
    }

    /* Alive right up to the threshold, abandoned on it. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true};
        fw_upgrade_progress(&fw, SECONDS(10));

        CHECK(!fw_upgrade_stalled(&fw, SECONDS(10) + FW_UPGRADE_STALL_US - 1),
              "window", "still alive one tick short");

        CHECK(fw_upgrade_stalled(&fw, SECONDS(10) + FW_UPGRADE_STALL_US),
              "window", "stalled on the threshold");
    }

    /* Every step refreshes it, so a transfer that keeps advancing is never
       abandoned however long the whole image takes — and a full pull is
       256 KB four bytes at a time, so it takes a long while. Without the
       refresh this would give up on the strength of the first step alone. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true};
        fw_upgrade_progress(&fw, SECONDS(0));

        for (int second = 1; second <= 300; second++) {
            CHECK(!fw_upgrade_stalled(&fw, SECONDS(second)), "refresh", "an advancing pull survives");
            fw_upgrade_progress(&fw, SECONDS(second));
        }
    }

    /* The microsecond counter is 32-bit and wraps every 71 minutes, and this
       one has to be — it crosses cores, where a 64-bit read could tear. A
       transfer straddling the wrap must not read as stalled: the subtraction
       is unsigned so that the difference stays right when now_us is the
       smaller number. */
    {
        const uint32_t before_wrap = 0xFFFFFFFFu - SECONDS(1);

        fw_upgrade_state_t fw = {.upgrade_in_progress = true};
        fw_upgrade_progress(&fw, before_wrap);

        /* Two seconds later the counter has wrapped past zero. */
        CHECK(!fw_upgrade_stalled(&fw, before_wrap + SECONDS(2)),
              "wrap", "a live transfer survives the wrap");

        CHECK(fw_upgrade_stalled(&fw, before_wrap + SECONDS(31)),
              "wrap", "and a stalled one is still caught across it");
    }

    /* The reported hang: a transfer stops and nothing ever moves it again.
       The caller acts on this by clearing upgrade_in_progress, which is what
       lets the config-mode timeout resume and what lets the pull be started
       again by the next heartbeat from the newer peer board. Since a restart
       rewrites every page, that is also how a half-written image is repaired
       (abandon_firmware_upgrade). */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true};
        fw_upgrade_progress(&fw, SECONDS(0));

        CHECK(!fw_upgrade_stalled(&fw, SECONDS(29)), "hang", "twenty-nine seconds is not yet gone");
        CHECK(fw_upgrade_stalled(&fw, SECONDS(31)), "hang", "a silent transfer is abandoned");

        /* Fourteen minutes was the observed hang, and it never ended. */
        CHECK(fw_upgrade_stalled(&fw, SECONDS(14 * 60)), "hang", "and stays abandoned");

        /* Acting on it is the caller's job, but the point of acting is that
           asking again then answers no, so the board is free to retry. */
        fw.upgrade_in_progress = false;
        CHECK(!fw_upgrade_stalled(&fw, SECONDS(14 * 60)), "hang", "abandoning ends the stall");
    }

    /* A dropped request or response is the common failure, not a rare one:
       uart_tx_queue is shared with mouse traffic, and neither end retransmits.
       Measured on hardware — moving the cursor during a pull lost responses
       repeatedly, and each loss cost a full stall window and a restart. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true,
                                 .source              = FW_UPGRADE_SOURCE_PULL,
                                 .byte_done           = true};
        CHECK(!fw_upgrade_request_lost(&fw, SECONDS(100)), "outstanding",
              "nothing owed to us is not a lost request");

        /* request_byte clears byte_done and stamps the time together. */
        fw.byte_done        = false;
        fw.requested_at_us  = SECONDS(10);

        CHECK(!fw_upgrade_request_lost(&fw, SECONDS(10) + FW_UPGRADE_REREQUEST_US - 1),
              "outstanding", "still waiting one tick short");

        CHECK(fw_upgrade_request_lost(&fw, SECONDS(10) + FW_UPGRADE_REREQUEST_US),
              "outstanding", "asked again on the threshold");

        /* Asking again restarts the wait rather than re-firing every pass. */
        fw.requested_at_us = SECONDS(10) + FW_UPGRADE_REREQUEST_US;
        CHECK(!fw_upgrade_request_lost(&fw, SECONDS(10) + FW_UPGRADE_REREQUEST_US + 1),
              "outstanding", "re-asking restarts the wait");
    }

    {
        fw_upgrade_state_t fw = {.byte_done = false};
        CHECK(!fw_upgrade_request_lost(&fw, SECONDS(100)), "outstanding",
              "no upgrade owes us nothing");
    }

    /* #104. A UF2 drop sets the same in-progress flag the pull runs on and
       leaves byte_done clear from start to finish — which is precisely the
       state re-requesting was added to act on. Nothing but the transport tells
       the two apart, and before it was recorded every one of these asked the
       peer board for a word, and the peer board answered. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true,
                                 .source              = FW_UPGRADE_SOURCE_DROP,
                                 .byte_done           = false};
        fw_upgrade_progress(&fw, SECONDS(0));

        CHECK(!fw_upgrade_may_pull(&fw), "phantom", "a drop is not a pull");

        /* However long the host leaves it sitting there. */
        for (uint32_t us = FW_UPGRADE_REREQUEST_US; us <= SECONDS(300);
             us += FW_UPGRADE_REREQUEST_US)
            CHECK(!fw_upgrade_request_lost(&fw, us), "phantom",
                  "a drop never asks the peer board for anything");
    }

    /* The transport is not enough on its own: the pull may act only while its
       own transfer is actually running. A cleared flag with the source left
       behind is what abandoning looks like mid-write. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = false,
                                 .source              = FW_UPGRADE_SOURCE_PULL,
                                 .byte_done           = false};

        CHECK(!fw_upgrade_may_pull(&fw), "phantom", "a pull that is over may not act");
        CHECK(!fw_upgrade_request_lost(&fw, SECONDS(100)), "phantom",
              "and owes nothing once it is over");
    }

    /* A drop is still a transfer, though, so the backstop still covers it: the
       stall is about the transfer going quiet, not about who was sending it.
       This is the case #92's criterion 1 needs, and it could not be reached
       while a phantom pull kept the drop looking alive. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true,
                                 .source              = FW_UPGRADE_SOURCE_DROP};
        fw_upgrade_progress(&fw, SECONDS(0));

        CHECK(!fw_upgrade_stalled(&fw, SECONDS(29)), "phantom", "a live drop is not stalled");
        CHECK(fw_upgrade_stalled(&fw, SECONDS(31)), "phantom", "a quiet drop is abandoned too");
    }

    /* The backstop has to survive the retry. Re-requesting is not progress, so
       a board asking into a dead link still reaches the stall window and gets
       abandoned — otherwise the retry would replace the failure it was added
       to make rare. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true,
                                 .source              = FW_UPGRADE_SOURCE_PULL,
                                 .byte_done           = false};
        fw_upgrade_progress(&fw, SECONDS(0));
        fw.requested_at_us = SECONDS(0);

        for (uint32_t us = FW_UPGRADE_REREQUEST_US; us < FW_UPGRADE_STALL_US;
             us += FW_UPGRADE_REREQUEST_US) {
            CHECK(fw_upgrade_request_lost(&fw, us), "deadlink", "keeps asking");
            CHECK(!fw_upgrade_stalled(&fw, us), "deadlink", "and is not yet abandoned");
            fw.requested_at_us = us; /* what request_byte does */
        }

        CHECK(fw_upgrade_stalled(&fw, FW_UPGRADE_STALL_US), "deadlink",
              "a dead link is still abandoned in the end");
    }

    /* Giving up is a restart while the peer board is there, and ROM recovery
       only when it is not. Both of the hardware brickings on 2026-08-12 were
       stalls with the peer board present and heartbeating throughout — a
       rebooting board and a busy link, both of which another attempt fixes. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true, .image_dirty = true};

        CHECK(!fw_upgrade_must_recover(&fw, true), "recover",
              "a peer board that is there can repair us");

        CHECK(fw_upgrade_must_recover(&fw, false), "recover",
              "a peer board that has gone cannot");
    }

    /* Nothing written means nothing to repair, so the board is free to wait
       however long it takes — there is no half-written image to protect
       anyone from. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true, .image_dirty = false};

        CHECK(!fw_upgrade_must_recover(&fw, true), "recover", "a clean image just retries");
        CHECK(!fw_upgrade_must_recover(&fw, false), "recover",
              "a clean image retries even with no peer board");
    }

    /* However many times it takes. The policy this replaced spent one restart
       and then went to ROM, which is what put a board in BOOTSEL twice for
       what were dropped packets. */
    {
        fw_upgrade_state_t fw = {.upgrade_in_progress = true, .image_dirty = true};

        for (int attempt = 0; attempt < 100; attempt++)
            CHECK(!fw_upgrade_must_recover(&fw, true), "patience",
                  "retrying never runs out while the peer board is there");
    }

    /* Which only holds because one interval fits inside the other many times
       over. If these ever converge, the retry stops being a retry. */
    CHECK(FW_UPGRADE_REREQUEST_US * 10 < FW_UPGRADE_STALL_US, "backstop",
          "many attempts fit inside the stall window");

    /* There is deliberately no "a timestamp behind the recorded one stalls
       nothing" case here, unlike peer_fw_test.c. Under wrap arithmetic the two
       are the same thing: a caller passing an earlier time and a counter that
       has wrapped past 2^32 are indistinguishable from inside this function,
       and the wrap is the one that actually happens. Rejecting the smaller
       number would break the case above. The only caller reads a monotonic
       hardware counter, so there is no earlier time to defend against.

       Both the microsecond counter and this window are unsigned 32-bit, so
       neither the constant nor a full-window delta can overflow. */
    CHECK(FW_UPGRADE_STALL_US < 0xFFFFFFFFu / 2, "range",
          "the window stays well inside half the wrap");

    if (failures == 0)
        printf("fw_upgrade_test: all checks passed\n");

    return failures == 0 ? 0 : 1;
}
