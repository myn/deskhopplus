#include "autostart.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace deskhop {

namespace {

using autostart::Mechanism;

/* One name, used by all three mechanisms, so a leftover from one is
   recognisable as ours whichever rung wrote it. */
constexpr wchar_t kEntryName[] = L"deskhopplus-helper";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring module_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) return std::wstring(buffer.data(), written);
        buffer.resize(buffer.size() * 2); /* the manifest declares longPathAware */
    }
}

std::string narrow(const std::wstring &text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

/* `"C:\path\deskhop-helper.exe" --autostart` — quoted, because a portable exe
   the user put under Program Files or in a folder with a space in its name is
   the ordinary case, not the exotic one. */
std::wstring command_line(const std::wstring &exe) {
    return L"\"" + exe + L"\" " + kAutostartArgument;
}

/*
 * Two paths naming the same file. Windows filesystems are case-insensitive and
 * the shell hands back its own normalisation of a link target rather than the
 * string SetPath was given — so an exact comparison can report that a working
 * entry names somewhere else, which would have the self-heal rewrite a correct
 * shortcut on every start and clear its proof each time.
 *
 * This does not settle resolution differences (a junction, a mapped drive);
 * see #122, which waits on a machine where the third rung is actually reached.
 */
bool same_path(const std::wstring &a, const std::wstring &b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring startup_link_path() {
    PWSTR folder = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder) != S_OK) return {};
    std::wstring path = std::wstring(folder) + L"\\" + kEntryName + L".lnk";
    CoTaskMemFree(folder);
    return path;
}

/*
 * Run a console tool with no window and wait for it.
 *
 * The logon task is written with schtasks.exe rather than through the Task
 * Scheduler COM API. Both are inbox and neither needs administrator rights for
 * a per-user task; schtasks is a fraction of the code, and this helper's whole
 * claim is that it is small enough to read. CREATE_NO_WINDOW because a console
 * flashing at logon is exactly the sort of thing autostart must not do.
 */
bool run_quietly(std::wstring command, DWORD &exit_code) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return false;

    WaitForSingleObject(process.hProcess, 20000);
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = ~0u;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

bool run_quietly(std::wstring command) {
    DWORD exit_code = 0;
    return run_quietly(std::move(command), exit_code);
}

std::wstring run_key_value() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    wchar_t value[1024] = {0};
    constexpr size_t kCapacity = sizeof(value) / sizeof(wchar_t);
    /* One character short of the buffer, so there is always somewhere to put a
       terminator: RegQueryValueExW does not promise a REG_SZ carries one, and
       a full-length value written by other software would otherwise be read
       past the end of the array. */
    DWORD size = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, kEntryName, nullptr, &type,
                                            reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return {};

    size_t chars = size / sizeof(wchar_t);
    if (chars >= kCapacity) chars = kCapacity - 1;
    value[chars] = L'\0'; /* a no-op when the stored value already had one */
    return value;
}

/* The shortcut's target, so a readback checks what the link points at rather
   than only that a file with the right name exists. */
std::wstring link_target(const std::wstring &link) {
    IShellLinkW *shell_link = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&shell_link)) != S_OK)
        return {};

    std::wstring target;
    IPersistFile *file = nullptr;
    if (shell_link->QueryInterface(IID_PPV_ARGS(&file)) == S_OK) {
        if (file->Load(link.c_str(), STGM_READ) == S_OK) {
            wchar_t path[MAX_PATH] = {0};
            if (shell_link->GetPath(path, MAX_PATH, nullptr, 0) == S_OK) target = path;
        }
        file->Release();
    }
    shell_link->Release();
    return target;
}

bool write_startup_link(const std::wstring &link, const std::wstring &exe) {
    IShellLinkW *shell_link = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&shell_link)) != S_OK)
        return false;

    bool saved = false;
    shell_link->SetPath(exe.c_str());
    shell_link->SetArguments(kAutostartArgument);
    shell_link->SetDescription(L"deskhopplus helper");
    IPersistFile *file = nullptr;
    if (shell_link->QueryInterface(IID_PPV_ARGS(&file)) == S_OK) {
        saved = file->Save(link.c_str(), TRUE) == S_OK;
        file->Release();
    }
    shell_link->Release();
    return saved;
}

} // namespace

Autostart::Autostart(std::wstring state_directory, Log log)
    : state_directory_(std::move(state_directory)), log_(std::move(log)) {
    record_path_ = state_directory_ + L"\\autostart";
    exe_path_ = module_path();
}

void Autostart::note(const std::string &message) const {
    if (log_) log_(message);
}

void Autostart::start(bool launched_by_autostart) {
    load();

    /*
     * The second half of verification, and it can only ever be answered on a
     * later logon than the one the entry was written on. The record is saved
     * because that is what carries the proof forward — "the entry exists" is a
     * readback, "the entry fired" is this.
     */
    const autostart::Record noted = autostart::note_launch(record_, launched_by_autostart);
    if (noted.confirmed != record_.confirmed) {
        record_ = noted;
        save();
        note("autostart confirmed: this launch came from the " +
             std::string(autostart::name(record_.mechanism)));
    }

    /* A portable exe moves. An entry naming where it used to be is an
       autostart that silently stopped working, so it is rewritten in place
       rather than waiting for the user to notice. */
    if (autostart::needs_rewrite(record_, narrow(exe_path_))) {
        note("the helper has moved since autostart was set up; rewriting the " +
             std::string(autostart::name(record_.mechanism)));
        const Mechanism previous = record_.mechanism;
        remove(previous);
        /* Both halves again, for the reason enable() states: a rewrite that
           policy silently discards would otherwise be recorded as a working
           entry at the new path, and the ladder would never fall through to a
           rung that does work. */
        if (write(previous) && reads_back(previous)) {
            record_.exe_path = narrow(exe_path_);
            /* A rewritten entry has never been seen to fire. */
            record_.confirmed = false;
            save();
        } else {
            remove(previous); /* take back whatever a half-done rewrite left */
            /* The rung that worked before will not take now. Run the whole
               ladder again rather than leaving an entry pointing nowhere. */
            note("rewriting the " + std::string(autostart::name(previous)) +
                 " failed; trying the ladder again");
            enable();
        }
    }
}

autostart::Verification Autostart::status() const {
    return autostart::verify(record_, record_.mechanism != Mechanism::None &&
                                          reads_back(record_.mechanism));
}

void Autostart::enable() {
    std::vector<autostart::Attempt> attempts;
    std::vector<Mechanism> refused;

    for (Mechanism rung = autostart::next_attempt(refused); rung != Mechanism::None;
         rung = autostart::next_attempt(refused)) {
        /* Both halves of "it took": the call succeeded *and* the entry reads
           back. A mechanism that reports success and leaves nothing behind is
           what a managed laptop's policy actually looks like from here.
           
           Rolled back when only the first half held. Otherwise a rung whose
           write survived but whose readback did not would sit there
           unrecorded, while the record named whichever rung took afterwards —
           and disabling, which removes only the recorded one, would leave it
           behind for good. */
        bool took = write(rung);
        if (took && !reads_back(rung)) {
            remove(rung);
            took = false;
        }
        attempts.push_back(autostart::Attempt{rung, took});
        note(std::string(took ? "autostart registered: " : "autostart refused: ") +
             autostart::name(rung));
        if (took) break;
        refused.push_back(rung);
    }

    record_ = autostart::after_enabling(attempts, narrow(exe_path_));
    save();

    if (record_.mechanism == Mechanism::None) {
        /*
         * Logged and nothing more. The helper is an enhancement, never a
         * dependency, and a manually-launched one is fully functional — so
         * there is nothing here worth interrupting the user about.
         */
        note("every autostart mechanism was refused; the helper still works when started by "
             "hand");
    }
}

void Autostart::disable() {
    const Mechanism registered = autostart::to_remove(record_);
    if (registered != Mechanism::None) {
        remove(registered);
        note("autostart removed: " + std::string(autostart::name(registered)));
    }
    record_ = autostart::after_disabling();
    save();
}

bool Autostart::write(Mechanism mechanism) {
    if (exe_path_.empty()) return false;

    switch (mechanism) {
    case Mechanism::LogonTask:
        /* /F replaces an existing task rather than failing on it, which is
           what makes the rewrite-on-move path idempotent. /RL LIMITED is
           explicit: this must never be the thing that asks for elevation. */
        return run_quietly(L"schtasks.exe /Create /F /SC ONLOGON /RL LIMITED /TN \"" +
                           std::wstring(kEntryName) + L"\" /TR \"\\\"" + exe_path_ +
                           L"\\\" " + kAutostartArgument + L"\"");

    case Mechanism::RunKey: {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                            &key, nullptr) != ERROR_SUCCESS)
            return false;
        const std::wstring value = command_line(exe_path_);
        const LSTATUS status = RegSetValueExW(
            key, kEntryName, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    case Mechanism::StartupFolder: {
        const std::wstring link = startup_link_path();
        return !link.empty() && write_startup_link(link, exe_path_);
    }

    case Mechanism::None:
        break;
    }
    return false;
}

bool Autostart::reads_back(Mechanism mechanism) const {
    switch (mechanism) {
    case Mechanism::LogonTask: {
        DWORD exit_code = 0;
        return run_quietly(L"schtasks.exe /Query /TN \"" + std::wstring(kEntryName) + L"\"",
                           exit_code);
    }
    case Mechanism::RunKey: {
        const std::wstring value = run_key_value();
        /* The value must still name where the helper is running from — an
           entry pointing at the old location reads back as *present* and is
           exactly the failure the self-heal exists for. */
        return !value.empty() && same_path(value, command_line(exe_path_));
    }
    case Mechanism::StartupFolder: {
        const std::wstring link = startup_link_path();
        return !link.empty() && same_path(link_target(link), exe_path_);
    }
    case Mechanism::None:
        break;
    }
    return false;
}

void Autostart::remove(Mechanism mechanism) {
    switch (mechanism) {
    case Mechanism::LogonTask:
        run_quietly(L"schtasks.exe /Delete /F /TN \"" + std::wstring(kEntryName) + L"\"");
        break;
    case Mechanism::RunKey: {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, kEntryName);
            RegCloseKey(key);
        }
        break;
    }
    case Mechanism::StartupFolder: {
        const std::wstring link = startup_link_path();
        if (!link.empty()) DeleteFileW(link.c_str());
        break;
    }
    case Mechanism::None:
        break;
    }
}

bool Autostart::launched_by_autostart() {
    const std::wstring line = GetCommandLineW();
    return line.find(kAutostartArgument) != std::wstring::npos;
}

/* The record is four fields of plain text. Nothing here is secret — it says
   which mechanism took and where the helper was — so it is readable by the
   user who owns it, which is the right property for something they may need to
   explain to their own IT department. */
void Autostart::load() {
    record_ = autostart::Record{};

    FILE *file = nullptr;
    if (_wfopen_s(&file, record_path_.c_str(), L"rb") != 0 || !file) return;

    char line[1024];
    while (std::fgets(line, sizeof line, file)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        const size_t split = text.find('=');
        if (split == std::string::npos) continue;
        const std::string key = text.substr(0, split);
        const std::string value = text.substr(split + 1);

        if (key == "enabled") record_.enabled = value == "1";
        else if (key == "confirmed") record_.confirmed = value == "1";
        else if (key == "exe") record_.exe_path = value;
        else if (key == "mechanism") {
            const int raw = std::atoi(value.c_str());
            record_.mechanism = raw >= static_cast<int>(Mechanism::LogonTask) &&
                                        raw <= static_cast<int>(Mechanism::StartupFolder)
                                    ? static_cast<Mechanism>(raw)
                                    : Mechanism::None;
        }
    }
    std::fclose(file);
}

void Autostart::save() const {
    /* The directory is the secret store's, and on a first run that turns
       autostart on before anything has been paired it may not exist yet. */
    SHCreateDirectoryExW(nullptr, state_directory_.c_str(), nullptr);

    FILE *file = nullptr;
    if (_wfopen_s(&file, record_path_.c_str(), L"wb") != 0 || !file) {
        note("could not write the autostart record; the setting will not survive a restart");
        return;
    }
    std::fprintf(file, "enabled=%d\nmechanism=%d\nconfirmed=%d\nexe=%s\n", record_.enabled ? 1 : 0,
                 static_cast<int>(record_.mechanism), record_.confirmed ? 1 : 0,
                 record_.exe_path.c_str());
    std::fclose(file);
}

} // namespace deskhop
