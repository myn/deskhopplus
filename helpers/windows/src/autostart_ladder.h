#pragma once
/*
 * The autostart ladder's *decisions*, with no Win32 in them (#86).
 *
 * This split is the point of the slice. Autostart on a managed laptop fails in
 * ways that are not predictable from the outside — a logon task the policy
 * refuses, a run key an agent strips, a startup folder that is redirected —
 * so the helper tries three mechanisms in order and takes the first that
 * works. That logic is the code most likely to be wrong on a machine nobody
 * can reproduce, and none of it needs a registry to be worth checking.
 *
 * The platform calls sit behind this in autostart.h and are not reached by
 * tests, which is the same line ChannelTransport.swift draws on the other
 * helper.
 *
 * Header-only and dependency-free so the test executable links nothing
 * (ADR-0006).
 */

#include <string>
#include <vector>

namespace deskhop::autostart {

/*
 * The order is not arbitrary.
 *
 * A logon task is first because it is the only one that survives a policy
 * that strips per-user run entries, and the only one that can be given a
 * delay. The run key is second: cheap, per-user, needs nothing. The startup
 * folder is last because it is the most visible to the user and the most
 * easily "tidied up" by something else — which makes it the best fallback and
 * the worst first choice.
 */
enum class Mechanism { None = 0, LogonTask = 1, RunKey = 2, StartupFolder = 3 };

inline const char *name(Mechanism mechanism) {
    switch (mechanism) {
    case Mechanism::LogonTask: return "logon task";
    case Mechanism::RunKey: return "run-key entry";
    case Mechanism::StartupFolder: return "startup folder shortcut";
    case Mechanism::None: break;
    }
    return "none";
}

/* The ladder, in the order attempted. */
inline const std::vector<Mechanism> &ladder() {
    static const std::vector<Mechanism> order{Mechanism::LogonTask, Mechanism::RunKey,
                                              Mechanism::StartupFolder};
    return order;
}

/*
 * What is remembered between runs.
 *
 * `exe_path` is the location the registered entry names — the helper's *own*
 * location at the time it was written, never an install path, because a
 * portable exe moves.
 */
struct Record {
    /* What the user asked for, which is not the same as what took. */
    bool enabled{false};
    /* Which mechanism took, or None when every one of them refused. */
    Mechanism mechanism{Mechanism::None};
    std::string exe_path;
    /* A launch carrying the entry's own argument has been seen at least once.
       This is the half of verification a readback cannot give. */
    bool confirmed{false};
};

/* One rung's outcome. */
struct Attempt {
    Mechanism mechanism{Mechanism::None};
    bool registered{false};
};

/*
 * Which rung to try next, given the ones already refused. `None` when the
 * ladder is exhausted — which is logged and nothing more: the helper is an
 * enhancement, never a dependency, and a manually-launched one is fully
 * functional.
 */
inline Mechanism next_attempt(const std::vector<Mechanism> &refused) {
    for (Mechanism rung : ladder()) {
        bool already = false;
        for (Mechanism r : refused) already = already || r == rung;
        if (!already) return rung;
    }
    return Mechanism::None;
}

/* The first rung that took, in ladder order — not in the order the attempts
   happen to be listed, so a caller that reports them out of order still gets
   the same answer. */
inline Mechanism first_success(const std::vector<Attempt> &attempts) {
    for (Mechanism rung : ladder())
        for (const Attempt &attempt : attempts)
            if (attempt.mechanism == rung && attempt.registered) return rung;
    return Mechanism::None;
}

/*
 * Fold a ladder run into the record. `exe_path` is where the helper is running
 * from now, which is what any entry written during this run names.
 *
 * `confirmed` is cleared, deliberately: a *new* entry has never been seen to
 * fire, so carrying the old proof forward would let a mechanism that was
 * verified last month vouch for one registered a minute ago.
 */
inline Record after_enabling(const std::vector<Attempt> &attempts, const std::string &exe_path) {
    Record record;
    record.enabled = true;
    record.mechanism = first_success(attempts);
    record.exe_path = record.mechanism == Mechanism::None ? std::string{} : exe_path;
    record.confirmed = false;
    return record;
}

/*
 * A launch happened. `carried_autostart_argument` is the evidence that it was
 * a real autostarted one: the entry the ladder writes launches the helper with
 * an argument the user's own shortcut does not have, so a launch carrying it
 * came from the mechanism and not from a double-click.
 *
 * This is the second half of verification, and it can only ever be answered on
 * a *later* logon. "The entry exists" and "the entry fired" are different
 * claims, and only the second one is what "enabled" is supposed to mean.
 */
inline Record note_launch(const Record &before, bool carried_autostart_argument) {
    Record record = before;
    if (carried_autostart_argument && record.mechanism != Mechanism::None) record.confirmed = true;
    return record;
}

enum class Verification {
    /* The user has not asked for it. */
    NotEnabled,
    /* Asked for, and every rung refused. Logged; nothing is shown. */
    NotRegistered,
    /* An entry is there and reads back, but no autostarted launch has been
       seen yet. This is the honest state on the day it is switched on. */
    RegisteredNotYetProven,
    /* Both halves: the entry reads back, and a launch carrying its argument
       has been seen. */
    Confirmed,
};

/*
 * Verification is two-part on purpose. A readback proves only that something
 * wrote a value; a managed laptop can leave a run key sitting there and
 * refuse to act on it at logon, which reads back perfectly and never starts
 * anything.
 */
inline Verification verify(const Record &record, bool entry_reads_back) {
    if (!record.enabled) return Verification::NotEnabled;
    if (record.mechanism == Mechanism::None || !entry_reads_back)
        return Verification::NotRegistered;
    return record.confirmed ? Verification::Confirmed : Verification::RegisteredNotYetProven;
}

/*
 * Does the recorded entry still name where the helper is actually running
 * from? A portable exe moves, and an entry pointing at where it used to be is
 * an autostart that silently stopped working.
 *
 * The self-heal this enables is what makes a three-mechanism ladder safe for a
 * file the user can drag somewhere else.
 */
inline bool needs_rewrite(const Record &record, const std::string &current_exe_path) {
    if (!record.enabled || record.mechanism == Mechanism::None) return false;
    return record.exe_path != current_exe_path;
}

/*
 * What disabling has to remove: whichever rung took, and only that one. The
 * others were never written, and deleting a startup shortcut this helper did
 * not create would be reaching into something that is not ours.
 */
inline Mechanism to_remove(const Record &record) { return record.mechanism; }

/* The record after a successful disable — nothing left behind, including the
   proof, which belonged to an entry that no longer exists. */
inline Record after_disabling() { return Record{}; }

} // namespace deskhop::autostart
