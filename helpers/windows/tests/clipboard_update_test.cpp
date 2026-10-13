/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include <cstdio>

#include "clipboard_update.h"

using deskhop::clipboard_update_is_external;
using deskhop::prefetched_image_is_current;
using deskhop::ClipboardReads;
using deskhop::ClipboardSend;
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

    if (failures != 0) return 1;
    std::printf("clipboard update tests passed\n");
    return 0;
}
