/*
 * The autostart ladder's decisions (#86): which mechanism took, whether the
 * recorded path still matches, what the marker proves, and what disabling has
 * to remove.
 *
 * These run on any machine, which is the whole reason the decisions were split
 * out of the Win32 calls: this is the logic most likely to be wrong on a
 * managed laptop nobody can reproduce, and verifying it by hope is not
 * verifying it.
 *
 * Style follows tests/helper_test.c and mkroamer's reconnect_policy_test.cpp —
 * an assertion macro, a main, a printed failure line, a non-zero exit. No
 * framework and no new dependency (ADR-0006).
 */

#include <cstdio>
#include <string>
#include <vector>

#include "autostart_ladder.h"

using namespace deskhop::autostart;

static int failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, (what));      \
        }                                                                    \
    } while (0)

static Attempt refused(Mechanism m) { return Attempt{m, false}; }
static Attempt took(Mechanism m) { return Attempt{m, true}; }

/* The order is the decision: a logon task survives policy that strips run
   entries, and the startup folder is the most easily tidied away by something
   else — so it is the fallback, never the first choice. */
static void ladder_is_attempted_in_order() {
    CHECK(next_attempt({}) == Mechanism::LogonTask, "the first rung is the logon task");
    CHECK(next_attempt({Mechanism::LogonTask}) == Mechanism::RunKey,
          "a refused logon task falls to the run key");
    CHECK(next_attempt({Mechanism::LogonTask, Mechanism::RunKey}) == Mechanism::StartupFolder,
          "a refused run key falls to the startup folder");
    CHECK(next_attempt({Mechanism::LogonTask, Mechanism::RunKey, Mechanism::StartupFolder}) ==
              Mechanism::None,
          "an exhausted ladder has nothing left to try");
}

static void the_first_success_wins() {
    CHECK(first_success({took(Mechanism::LogonTask)}) == Mechanism::LogonTask,
          "the logon task taking is the answer");
    CHECK(first_success({refused(Mechanism::LogonTask), took(Mechanism::RunKey)}) ==
              Mechanism::RunKey,
          "the run key takes when the logon task refused");
    CHECK(first_success({refused(Mechanism::LogonTask), refused(Mechanism::RunKey),
                         took(Mechanism::StartupFolder)}) == Mechanism::StartupFolder,
          "the startup folder is reached when both above it refused");
    CHECK(first_success({refused(Mechanism::LogonTask), refused(Mechanism::RunKey),
                         refused(Mechanism::StartupFolder)}) == Mechanism::None,
          "every rung refusing leaves no mechanism");

    /* Ladder order, not report order: a caller that lists its attempts the
       other way round must still get the rung that ranks first. */
    CHECK(first_success({took(Mechanism::StartupFolder), took(Mechanism::LogonTask)}) ==
              Mechanism::LogonTask,
          "the highest rung that took wins regardless of report order");
}

/* Every rung refusing is logged and nothing more. The helper stays fully
   functional and the record says plainly that nothing is registered. */
static void an_exhausted_ladder_registers_nothing() {
    const Record record = after_enabling({refused(Mechanism::LogonTask),
                                          refused(Mechanism::RunKey),
                                          refused(Mechanism::StartupFolder)},
                                         "C:\\Users\\d\\deskhop-helper.exe");
    CHECK(record.enabled, "the user's request is remembered even when nothing took");
    CHECK(record.mechanism == Mechanism::None, "no mechanism is claimed");
    CHECK(record.exe_path.empty(), "no path is recorded for an entry that was never written");
    CHECK(verify(record, true) == Verification::NotRegistered,
          "a readback cannot vouch for an entry that does not exist");
}

/* "The entry exists" and "the entry fired" are different claims. */
static void verification_needs_both_halves() {
    Record record = after_enabling({took(Mechanism::LogonTask)}, "C:\\tools\\deskhop-helper.exe");

    CHECK(verify(record, true) == Verification::RegisteredNotYetProven,
          "a fresh entry has never been seen to fire");
    CHECK(verify(record, false) == Verification::NotRegistered,
          "an entry that no longer reads back is not registered, whatever was recorded");

    /* A launch that did not carry the entry's argument is a double-click, and
       proves nothing about the mechanism. */
    record = note_launch(record, false);
    CHECK(verify(record, true) == Verification::RegisteredNotYetProven,
          "a manual launch is not evidence the entry fired");

    record = note_launch(record, true);
    CHECK(verify(record, true) == Verification::Confirmed,
          "a launch carrying the entry's argument is the second half of the proof");

    /* Even confirmed, a vanished entry is not registered — something removed
       it since, and last month's proof does not cover that. */
    CHECK(verify(record, false) == Verification::NotRegistered,
          "proof does not survive the entry being removed");
}

static void a_launch_proves_nothing_when_nothing_is_registered() {
    Record record;
    record.enabled = true;
    record.mechanism = Mechanism::None;
    record = note_launch(record, true);
    CHECK(!record.confirmed, "an argument cannot confirm a mechanism that was never registered");
}

/* Re-enabling writes a new entry, which has never fired. Carrying the old
   proof forward would let a mechanism verified last month vouch for one
   registered a minute ago. */
static void re_enabling_clears_the_proof() {
    Record record = after_enabling({took(Mechanism::RunKey)}, "C:\\tools\\deskhop-helper.exe");
    record = note_launch(record, true);
    CHECK(record.confirmed, "the run key entry was proven");

    const Record again = after_enabling({took(Mechanism::StartupFolder)},
                                        "C:\\tools\\deskhop-helper.exe");
    CHECK(again.mechanism == Mechanism::StartupFolder, "the new mechanism is recorded");
    CHECK(!again.confirmed, "a newly written entry starts unproven");
}

/* A portable exe moves, and an entry naming where it used to be is an
   autostart that silently stopped working. */
static void a_moved_exe_needs_the_entry_rewritten() {
    const Record record = after_enabling({took(Mechanism::RunKey)},
                                         "C:\\Users\\d\\Downloads\\deskhop-helper.exe");

    CHECK(!needs_rewrite(record, "C:\\Users\\d\\Downloads\\deskhop-helper.exe"),
          "an entry naming where the helper runs from needs nothing");
    CHECK(needs_rewrite(record, "C:\\tools\\deskhop-helper.exe"),
          "an entry naming somewhere else must be rewritten");

    Record disabled;
    CHECK(!needs_rewrite(disabled, "C:\\tools\\deskhop-helper.exe"),
          "there is nothing to rewrite when autostart is off");

    Record nothing_took = after_enabling({refused(Mechanism::LogonTask),
                                          refused(Mechanism::RunKey),
                                          refused(Mechanism::StartupFolder)},
                                         "C:\\tools\\deskhop-helper.exe");
    CHECK(!needs_rewrite(nothing_took, "C:\\elsewhere\\deskhop-helper.exe"),
          "there is nothing to rewrite when no entry was ever written");
}

/* Disabling removes whichever rung took, and only that one — the others were
   never written, and a startup shortcut this helper did not create is not ours
   to delete. */
static void disabling_removes_exactly_what_took() {
    const Record run_key = after_enabling({refused(Mechanism::LogonTask),
                                           took(Mechanism::RunKey)},
                                          "C:\\tools\\deskhop-helper.exe");
    CHECK(to_remove(run_key) == Mechanism::RunKey, "the run key is what gets removed");

    const Record none = after_enabling({refused(Mechanism::LogonTask), refused(Mechanism::RunKey),
                                        refused(Mechanism::StartupFolder)},
                                       "C:\\tools\\deskhop-helper.exe");
    CHECK(to_remove(none) == Mechanism::None, "nothing is removed when nothing was written");

    const Record after = after_disabling();
    CHECK(!after.enabled, "disabling leaves autostart off");
    CHECK(after.mechanism == Mechanism::None, "disabling leaves no mechanism behind");
    CHECK(after.exe_path.empty(), "disabling leaves no path behind");
    CHECK(!after.confirmed, "disabling leaves no proof behind");
    CHECK(verify(after, true) == Verification::NotEnabled,
          "a readback of somebody else's entry does not re-enable ours");
}

/* Autostart is off until the user asks for it. A portable exe that silently
   writes a logon task on first run is a surprise nobody asked for. */
static void autostart_is_off_until_asked_for() {
    const Record fresh;
    CHECK(!fresh.enabled, "a fresh record is off");
    CHECK(fresh.mechanism == Mechanism::None, "a fresh record claims no mechanism");
    CHECK(verify(fresh, true) == Verification::NotEnabled,
          "an entry that happens to read back does not mean the user asked for one");
}

int main() {
    autostart_is_off_until_asked_for();
    ladder_is_attempted_in_order();
    the_first_success_wins();
    an_exhausted_ladder_registers_nothing();
    verification_needs_both_halves();
    a_launch_proves_nothing_when_nothing_is_registered();
    re_enabling_clears_the_proof();
    a_moved_exe_needs_the_entry_rewritten();
    disabling_removes_exactly_what_took();

    if (failures == 0) std::printf("autostart_ladder_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
