/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once
/*
 * The wording, which is the half of every output the shared core deliberately
 * does not carry (#80, #85). `dh_helper` emits a code and its numbers —
 * `DH_NOTE_LISTENER_DETECTED, a = 4, b = 10000` — and each helper says it in
 * its own words; a Windows tray tooltip and a macOS menu bar item are not one
 * string table living in C.
 *
 * The session state, config-chord permission, and bulk permission come from
 * the core. The Windows presence view below also uses the transport mode to
 * clear a stale connected config label during the core's USB-noise debounce.
 * The chord predicate carries a security property (#34) and is called,
 * never re-read.
 *
 * The rule the note codes exist to protect: **a note never loses its
 * numbers**. A rate reported without the rate is what let #94 run for two
 * days, so every case that has an `a` or a `b` spends it.
 *
 * The macOS twins are HelperState.swift and HelperNotes.swift.
 */

#include <cstdint>
#include <optional>
#include <string>

#include "dh_helper.h"

namespace deskhop::words {

/* The tray's current state, including a config label still held by the
   shared core during its USB-noise debounce. */
dh_helper_state presence_state(dh_helper_state state, bool config_present, bool live);

/* Update the Windows presence at a live-session edge; the shared core still
   debounces ordinary USB noise. */
std::optional<dh_helper_state> session_edge_presence(dh_helper_state state,
                                                     bool config_present,
                                                     bool was_live, bool live);

/* The greyed first row of the tray menu: this helper's name and release, from
   the one version the firmware and both helpers share (#199). */
std::string release_row();

/*
 * What the user is told, in words. Empty for DH_HELPER_QUIET, which shows
 * nothing at all: a device that disappears for a moment is ordinary, and
 * config mode is something the user did on purpose.
 *
 * A state this helper has no words for also comes back empty, and says so
 * through `state_is_known` rather than silently reading as quiet — a state
 * added to the core and left out here would otherwise be shown to nobody
 * (#119).
 */
std::string state_message(dh_helper_state state);

/* Whether `state_message` has words for this state at all. */
bool state_is_known(dh_helper_state state);

/*
 * Whether this state names a remedy the user can act on, and so earns a
 * balloon rather than a silent tooltip change. Ordinary reconnection is not an
 * event worth interrupting anyone for (#85).
 */
bool state_names_a_remedy(dh_helper_state state);

/*
 * The three looks the presence's icon can take (#208). The shape carries the
 * state and the words stay one hover away: #38's "in words, not a colour to
 * interpret" holds because a look never replaces the tooltip.
 *
 *   Paired     — the solid glyph. Connected.
 *   Off        — the outlined glyph. Looking, absent, or in config mode: the
 *                device is not there to talk to, and none of it is a fault.
 *   Attention  — the glyph with a badge. Every state that names something to
 *                go and do, the reconnect rate (check the link), and a file
 *                question waiting on this computer's user.
 */
enum class Look { Paired, Off, Attention };

Look look(dh_helper_state state, bool question_waiting);

/*
 * The whole tooltip, by priority: a waiting question, then a receive with its
 * percent, then a send, then the state. What the user can act on comes before
 * what the device is doing. `question_summary` is empty when nothing is
 * offered; `total` of zero means nothing is arriving.
 */
std::string tooltip(dh_helper_state state, const std::string &question_summary,
                    uint64_t received, uint64_t total, bool sending);

/* Integer arithmetic, and truncating rather than rounding — the same spelling
   as `MenuBar.size` on the other computer, so the two ends quote one transfer
   at one size. */
std::string size_text(uint64_t bytes);

/* One log line for one note. `transport_reason` is the platform's own
   description of a write that failed, which the core has no field for — it
   takes the failure, not the sentence. */
std::string note_line(dh_helper_note note, int32_t a, int32_t b,
                      const std::string &transport_reason);

} // namespace deskhop::words
