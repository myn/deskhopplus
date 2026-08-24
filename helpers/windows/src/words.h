#pragma once
/*
 * The wording, which is the half of every output the shared core deliberately
 * does not carry (#80, #85). `dh_helper` emits a code and its numbers —
 * `DH_NOTE_LISTENER_DETECTED, a = 4, b = 10000` — and each helper says it in
 * its own words; a Windows tray tooltip and a macOS menu bar item are not one
 * string table living in C.
 *
 * What is *not* here is any decision. Which state the helper is in, whether
 * the config chord may be offered from it, and whether bulk is allowed are all
 * read off the core, so a second helper cannot answer them differently. The
 * chord predicate in particular carries a security property (#34) and is
 * called, never re-read.
 *
 * The rule the note codes exist to protect: **a note never loses its
 * numbers**. A rate reported without the rate is what let #94 run for two
 * days, so every case that has an `a` or a `b` spends it.
 *
 * The macOS twins are HelperState.swift and HelperNotes.swift.
 */

#include <cstdint>
#include <string>

#include "dh_helper.h"

namespace deskhop::words {

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

/* One log line for one note. `transport_reason` is the platform's own
   description of a write that failed, which the core has no field for — it
   takes the failure, not the sentence. */
std::string note_line(dh_helper_note note, int32_t a, int32_t b,
                      const std::string &transport_reason);

} // namespace deskhop::words
