#pragma once
/*
 * The Win32 side of autostart (#86) — the three mechanisms, their readbacks,
 * and where the record is kept between runs.
 *
 * Every *decision* is autostart_ladder.h, which has no Win32 in it and is
 * covered by tests. This file is the platform underneath: it does what the
 * ladder says and reports what happened.
 *
 * Opt-in from the tray menu, never automatic on first run. A portable exe that
 * silently writes a logon task is a surprise the user did not ask for.
 *
 * The self-heal is the part that matters for a file the user can drag
 * somewhere else: every entry names the helper's *own* current location
 * (GetModuleFileNameW) and is rewritten whenever it stops matching. mkroamer's
 * autostart.h chose the Startup folder for exactly this reason; the ladder
 * keeps that and adds the fallbacks a managed laptop needs.
 */

#include <windows.h>

#include <functional>
#include <string>

#include "autostart_ladder.h"

namespace deskhop {

/* The argument the registered entry launches the helper with. A launch
   carrying it came from a mechanism this helper wrote; a double-click does
   not, which is what makes it evidence rather than a guess. */
inline constexpr wchar_t kAutostartArgument[] = L"--autostart";

class Autostart {
  public:
    using Log = std::function<void(const std::string &)>;

    /* `state_directory` is where the record file lives — the same
       %LOCALAPPDATA% directory the secret store uses. */
    Autostart(std::wstring state_directory, Log log);

    /* Read the record, note whether *this* launch came from the entry, and
       rewrite the entry if the helper has moved since it was written. */
    void start(bool launched_by_autostart);

    const autostart::Record &record() const { return record_; }
    autostart::Verification status() const;

    /* Run the ladder and record the first rung that took. Every rung refusing
       is logged and nothing more: the helper is an enhancement, never a
       dependency. */
    void enable();
    /* Remove whichever rung took, leaving nothing behind. */
    void disable();

    /* Whether this process was launched by a registered entry. Reads the
       process command line, so it is answerable before anything else runs. */
    static bool launched_by_autostart();

  private:
    bool write(autostart::Mechanism mechanism);
    bool reads_back(autostart::Mechanism mechanism) const;
    void remove(autostart::Mechanism mechanism);

    void load();
    void save() const;
    void note(const std::string &message) const;

    std::wstring state_directory_;
    std::wstring record_path_;
    std::wstring exe_path_;
    Log log_;
    autostart::Record record_;
};

} // namespace deskhop
