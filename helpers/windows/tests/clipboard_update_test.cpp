/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include <cstdio>

#include "clipboard_update.h"

using deskhop::clipboard_update_is_external;
using deskhop::prefetched_image_is_current;
using deskhop::ClipboardReads;
using deskhop::ClipboardSend;
using deskhop::SlowRead;
using deskhop::select_clipboard_send;

int main() {
    int failures = 0;
#define CHECK(condition, message)                    \
    do {                                             \
        if (!(condition)) {                          \
            ++failures;                              \
            std::printf("FAIL %s\n", (message));    \
        }                                            \
    } while (0)

    CHECK(!clipboard_update_is_external(6320, 6318, 3612416, 3612416),
          "a delayed-format close was mistaken for an external copy");
    CHECK(!clipboard_update_is_external(6318, 6318, 0, 3612416),
          "the exact self sequence was mistaken for an external copy");
    CHECK(clipboard_update_is_external(6321, 6318, 462350, 3612416),
          "a different owner's copy was mistaken for the helper's write");
    CHECK(prefetched_image_is_current(6343, 6343),
          "an unchanged clipboard discarded its prefetched image");
    CHECK(!prefetched_image_is_current(6343, 6364),
          "a newer local copy was overwritten by a prefetched image");


    /* What one copy sends when it holds text, a picture, or both (#195). Zero
       bytes is "not there", which is also what a read that came back empty
       is. The limit is 100 in these, not the real threshold, so the case on
       each side of it is a number and not the constant asserted to itself. */
    CHECK(select_clipboard_send(5, 0, 100) == ClipboardSend::Text, "text alone was not sent as text");
    CHECK(select_clipboard_send(0, 50, 100) == ClipboardSend::Image,
          "a picture alone was not sent as a picture");
    CHECK(select_clipboard_send(5, 95, 100) == ClipboardSend::Bundle,
          "text and a picture that fit together were not bundled");
    CHECK(select_clipboard_send(5, 96, 100) == ClipboardSend::Text,
          "text beside a picture too big to bundle did not travel alone");
    CHECK(select_clipboard_send(0, 0, 100) == ClipboardSend::Nothing,
          "an empty read sent something");

    /* One read at a time (#329): 6400 is a slow copy, 6401 a second copy
       made while it rendered. */
    {
        ClipboardReads reads(6399);
        CHECK(reads.update(6400, true), "an external copy did not start a read");
        CHECK(!reads.update(6401, true), "a second read started while one was in flight");
        reads.read_done();
        CHECK(reads.update(6401, true), "a copy made during a read was lost when it ended");
        reads.read_done();
        CHECK(!reads.update(6401, true), "a read ending with no new copy read again");
    }
    {
        ClipboardReads reads(6399);
        CHECK(reads.update(6400, true), "an external copy did not start a read");
        CHECK(!reads.update(6400, true), "two updates for one copy read it twice");
        reads.read_done();
        CHECK(!reads.update(6401, false), "the helper's own write was read as a copy");
        CHECK(!reads.update(6401, true), "the helper's own write was read on a later update");
        CHECK(reads.update(6402, true), "an external copy after an own write was not read");
    }

    /* A read that waits past 5 s warns once, and its end gives one total line
       only then (#330). Started near the 32-bit wrap: the clock is
       GetTickCount64 cut to 32 bits. */
    {
        SlowRead slow;
        CHECK(!slow.warn(1000), "an idle watch warned");
        slow.start(4294964000u);
        CHECK(!slow.warn(4294964000u + 5000u), "a read warned at exactly 5 s");
        CHECK(slow.warn(2001u), "a read past 5 s across the clock wrap did not warn");
        CHECK(slow.waited(2001u) == 5297u, "the wait across the clock wrap was wrong");
        CHECK(!slow.warn(9000u), "a slow read warned twice");
        CHECK(slow.end(), "a read that warned gave no total line");
        CHECK(!slow.warn(20000u), "an ended read warned");
    }
    {
        SlowRead slow;
        slow.start(1000);
        CHECK(!slow.warn(1250), "a fast read warned");
        CHECK(!slow.end(), "a fast read gave a total line");
        slow.start(30000);
        CHECK(!slow.warn(30250), "a new read inherited the last one's warning");
    }

    if (failures != 0) return 1;
    std::printf("clipboard update tests passed\n");
    return 0;
}
