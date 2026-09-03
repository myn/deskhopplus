#include "file_store.h"

#include <shlobj.h>

#include <cstring>
#include <cwchar>
#include <set>

#include "file_naming.h"

namespace deskhop {

namespace {

std::wstring utf8_to_wide(const std::string &text) {
    if (text.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    if (chars <= 0) return {};
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    return out;
}

/* Everything under `directory`, and the directory itself. `SHFileOperation`
   would do it in one call and is deliberately not used: it can put up UI, and
   a background helper must never do that. */
void remove_tree(const std::wstring &directory) {
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = directory + L"\\" + name;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) remove_tree(path);
            else DeleteFileW(path.c_str());
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(directory.c_str());
}

} // namespace

FileStore::FileStore(std::wstring root) : root_(std::move(root)) {}

std::wstring FileStore::default_root() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) return L"deskhopplus";
    std::wstring path(buffer, length);
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + L"deskhopplus";
}

void FileStore::collect_garbage() {
    if (GetFileAttributesW(root_.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    /* Newest by the counter in the name, which is what `write` builds it from:
       the tick count, then a per-run sequence. Compared as numbers rather than
       as text, so "10" sorts after "9". */
    std::vector<std::wstring> sets;
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((root_ + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        sets.push_back(name);
    } while (FindNextFileW(search, &found));
    FindClose(search);

    std::wstring newest;
    unsigned long long newest_order[2] = {0, 0};
    for (const std::wstring &name : sets) {
        unsigned long long order[2] = {0, 0};
        const size_t dash = name.find(L'-');
        order[0] = std::wcstoull(name.substr(0, dash).c_str(), nullptr, 10);
        if (dash != std::wstring::npos)
            order[1] = std::wcstoull(name.substr(dash + 1).c_str(), nullptr, 10);
        if (newest.empty() || order[0] > newest_order[0] ||
            (order[0] == newest_order[0] && order[1] > newest_order[1])) {
            newest = name;
            newest_order[0] = order[0];
            newest_order[1] = order[1];
        }
    }

    unsigned removed = 0;
    for (const std::wstring &name : sets) {
        if (name == newest) continue;
        remove_tree(root_ + L"\\" + name);
        removed++;
    }
    if (removed > 0 && log)
        log(std::to_string(removed) +
            " set(s) of files left by a previous run were removed; the newest was kept in case "
            "it is still on the clipboard");
}

bool FileStore::write(const FileDelivery &delivery, Written &out) {
    sequence_++;
    const std::wstring directory = root_ + L"\\" + std::to_wstring(GetTickCount64()) + L"-" +
                                   std::to_wstring(sequence_);
    if (SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (log) log("a directory for the arriving files could not be made");
        return false;
    }

    std::vector<std::wstring> paths;
    std::set<std::string> used;
    size_t at = 0;
    for (const FileEntry &file : delivery.files) {
        const size_t size = static_cast<size_t>(file.size);
        if (at + size > delivery.bytes.size()) {
            if (log) log("the arriving files did not add up to the payload; nothing was written");
            remove_tree(directory);
            return false;
        }
        const std::wstring path = directory + L"\\" + utf8_to_wide(unused_file_name(file.name, used));
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (log) log("a file that arrived could not be created; the whole set was discarded");
            remove_tree(directory);
            return false;
        }
        size_t left = size;
        const uint8_t *from = delivery.bytes.data() + at;
        bool ok = true;
        while (left > 0) {
            const DWORD ask = left > 0x100000u ? 0x100000u : static_cast<DWORD>(left);
            DWORD wrote = 0;
            if (!WriteFile(handle, from, ask, &wrote, nullptr) || wrote == 0) {
                ok = false;
                break;
            }
            from += wrote;
            left -= wrote;
        }
        CloseHandle(handle);
        if (!ok) {
            if (log) log("a file that arrived could not be written; the whole set was discarded");
            remove_tree(directory);
            return false;
        }
        paths.push_back(path);
        at += size;
    }

    if (paths.empty()) {
        remove_tree(directory);
        return false;
    }
    out.directory = directory;
    out.paths = std::move(paths);
    return true;
}

} // namespace deskhop
