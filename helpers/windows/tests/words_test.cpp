/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * What the presence shows for a state (#208): which of the three looks the
 * icon takes, and what the tooltip says, by priority. The drawing is not
 * here — a test asserts what the user is told, never how it is painted.
 *
 * Runs on any machine: words.cpp has no Win32 in it, which is what lets these
 * decisions be checked before the exe reaches the laptop.
 */

#include <cstdio>
#include <string>

#include "words.h"

using namespace deskhop::words;

static int failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, (what));      \
        }                                                                    \
    } while (0)

int main() {
    CHECK(presence_state(DH_HELPER_CONNECTED_CONFIG_MODE, true, false) ==
              DH_HELPER_DEVICE_IN_CONFIG_MODE, "reassertion after config channel loss");
    CHECK(presence_state(DH_HELPER_CONNECTED_CONFIG_MODE, false, false) ==
              DH_HELPER_DEVICE_ABSENT, "reassertion after config device removal");
    CHECK(presence_state(DH_HELPER_CONNECTED_CONFIG_MODE, false, true) ==
              DH_HELPER_CONNECTED, "reassertion after normal reconnection");
    CHECK(presence_state(DH_HELPER_CONNECTED, true, false) ==
              DH_HELPER_DEVICE_IN_CONFIG_MODE, "reassertion during config entry");
    CHECK(presence_state(DH_HELPER_CONNECTED, false, false) ==
              DH_HELPER_CONNECTED, "ordinary USB noise keeps core debounce");

    /* Clear a stale connected config label on session loss, while the shared
       core keeps its USB-noise grace period. */
    CHECK(session_edge_presence(DH_HELPER_CONNECTED_CONFIG_MODE, true, true, false) ==
              DH_HELPER_DEVICE_IN_CONFIG_MODE, "config session loss with API present");
    CHECK(session_edge_presence(DH_HELPER_CONNECTED_CONFIG_MODE, false, true, false) ==
              DH_HELPER_DEVICE_ABSENT, "config session loss after device removal");
    CHECK(session_edge_presence(DH_HELPER_CONNECTED, true, true, false) ==
              DH_HELPER_DEVICE_IN_CONFIG_MODE, "normal session loss on config entry");
    CHECK(!session_edge_presence(DH_HELPER_CONNECTED, false, true, false),
          "normal session loss keeps core debounce");
    CHECK(session_edge_presence(DH_HELPER_CONNECTED_CONFIG_MODE, true, false, true) ==
              DH_HELPER_CONNECTED_CONFIG_MODE, "config session becomes live");
    CHECK(session_edge_presence(DH_HELPER_CONNECTED_CONFIG_MODE, false, false, true) ==
              DH_HELPER_CONNECTED, "normal session clears stale config label");
    CHECK(!session_edge_presence(DH_HELPER_CONNECTED_CONFIG_MODE, true, false, false),
          "no status change without session edge");
    CHECK(!session_edge_presence(DH_HELPER_RECONNECTING_REPEATEDLY, true, true, false),
          "failure status is not overwritten");

    /* The shape carries the state, and the words stay beside it (#38). */
    CHECK(look(DH_HELPER_CONNECTED, false) == Look::Paired, "paired is the solid glyph");
    CHECK(look(DH_HELPER_QUIET, false) == Look::Off, "looking for the device is off");
    CHECK(look(DH_HELPER_DEVICE_ABSENT, false) == Look::Off, "an absent device is off");
    CHECK(look(DH_HELPER_DEVICE_IN_CONFIG_MODE, false) == Look::Off,
          "config mode is something the user did, so it is off, not attention");
    CHECK(look(DH_HELPER_NOT_PAIRED, false) == Look::Attention, "not paired asks for the chord");
    CHECK(look(DH_HELPER_VERSION_INCOMPATIBLE, false) == Look::Attention,
          "a version mismatch asks for an update");
    CHECK(look(DH_HELPER_LISTENER_DETECTED, false) == Look::Attention,
          "a listener asks to be found and stopped");
    CHECK(look(DH_HELPER_BOARD_IDENTITY_CHANGED, false) == Look::Attention,
          "a changed identity asks for a decision");
    CHECK(look(DH_HELPER_RECONNECTING_REPEATEDLY, false) == Look::Attention,
          "a reconnect rate asks for the cable to be checked");
    CHECK(look(DH_HELPER_CONNECTED, true) == Look::Attention,
          "a file question waiting on this computer's user is attention whatever the state");
    CHECK(look(DH_HELPER_QUIET, true) == Look::Attention, "a question outranks off too");

    /* The tooltip, by priority: the question, then a receive, then a send,
       then the state. What the user can act on comes before what the device
       is doing. */
    CHECK(tooltip(DH_HELPER_CONNECTED, "", 0, 0, false) == "DeskHopPlus — Connected and paired",
          "an idle tooltip names the helper and the state");
    CHECK(tooltip(DH_HELPER_QUIET, "", 0, 0, false) == "DeskHopPlus — Looking for the device",
          "the quiet state now has an icon, so it has words to hover");
    CHECK(tooltip(DH_HELPER_CONNECTED, "", 2200000, 8388608, false)
              == "DeskHopPlus — Receiving 2.0 MB of 8.0 MB — 26%",
          "a receive shows the percent, truncated, the same as the Mac");
    CHECK(tooltip(DH_HELPER_CONNECTED, "", 0, 0, true) == "DeskHopPlus — Sending",
          "a send is named, since the icon does not change for it");
    CHECK(tooltip(DH_HELPER_CONNECTED, "", 4096, 8192, true)
              == "DeskHopPlus — Receiving 4 KB of 8 KB — 50%",
          "a receive outranks a send: it has a number");
    CHECK(tooltip(DH_HELPER_CONNECTED, "photo.jpg — 1.0 MB, about 5 seconds.", 4096, 8192, true)
              == "DeskHopPlus — Files offered: photo.jpg — 1.0 MB, about 5 seconds.",
          "a waiting question outranks everything");
    CHECK(tooltip(DH_HELPER_NOT_PAIRED, "", 0, 0, false)
              == "DeskHopPlus — Not paired — press the config chord on the device",
          "a state with a remedy keeps its remedy");

    /* The size spelling the Mac and Windows quote one transfer at. */
    CHECK(size_text(999) == "999 bytes", "bytes below a KB");
    CHECK(size_text(1536) == "1 KB", "KB truncates");
    CHECK(size_text(1572864) == "1.5 MB", "MB to a tenth");

    if (failures == 0) std::printf("words: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
