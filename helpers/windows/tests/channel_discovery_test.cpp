/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include <cstdio>

#include "channel_identity.h"

using namespace deskhop;

static int failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

int main() {
    CHECK(classify_collection(kVendorId, kProductId, kUsagePage, kUsage) ==
          Collection::NormalChannel);
    CHECK(classify_collection(kVendorId, kProductId, kUsagePage, kUsage + 1) ==
          Collection::NormalChannel);
    CHECK(classify_collection(kConfigVendorId, kConfigProductId, kUsagePage, 0x10) ==
          Collection::ConfigApi);
    CHECK(classify_collection(kConfigVendorId, kConfigProductId, kUsagePage, kUsage) ==
          Collection::ConfigChannel);
    CHECK(classify_collection(kConfigVendorId, kConfigProductId, kUsagePage, kUsage + 1) ==
          Collection::None);

    /* The API can appear first: presence is config mode, then the helper
       channel's arrival asks the session to acquire it. */
    auto first = discover(Mode::None, 0, 0, 1, false);
    CHECK(first.mode == Mode::Config && first.appeared && !first.disappeared);
    auto second = discover(first.mode, 0, 1, 1, true);
    CHECK(second.mode == Mode::Config && second.appeared);

    /* Or the channel can appear first: the later API collection must not
       announce another appearance and retire the live session. */
    first = discover(Mode::None, 0, 1, 0, true);
    CHECK(first.mode == Mode::Config && first.appeared);
    second = discover(first.mode, 0, 1, 1, false);
    CHECK(second.mode == Mode::Config && !second.appeared && !second.disappeared);
    CHECK(!discover(Mode::Config, 0, 0, 1, false).disappeared);

    /* Normal mode still requires its two channels. During a reboot, stale
       collections from the old identity do not mix into the new set. */
    CHECK(discover(Mode::None, 2, 0, 0, true).mode == Mode::Normal);
    CHECK(discover(Mode::Normal, 2, 0, 0, true).appeared);
    CHECK(discover(Mode::Normal, 2, 1, 1, false).mode == Mode::Normal);
    CHECK(discover(Mode::Normal, 0, 1, 1, true).mode == Mode::Config);
    CHECK(discover(Mode::Config, 2, 1, 1, false).mode == Mode::Config);
    CHECK(discover(Mode::Config, 2, 0, 0, true).mode == Mode::Normal);

    auto gone = discover(Mode::Config, 0, 0, 0, false);
    CHECK(gone.mode == Mode::None && gone.disappeared && !gone.appeared);

    if (failures == 0) std::printf("channel discovery: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
