/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "file_read.h"

namespace deskhop {

FileRead read_offered(const std::wstring &path, uint64_t offered, std::vector<uint8_t> &out) {
    /*
     * Share write as well as read (#182). A log that is still growing is held
     * open by its writer. Win32 refuses an open that does not share write
     * while a writer holds the file: ERROR_SHARING_VIOLATION. The helper's own
     * log is one such file, and a share-read open of it failed at this step.
     */
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {FileRead::Outcome::OpenFailed, 0, GetLastError()};

    /* Measured for the log line only. The read below never depends on it. */
    LARGE_INTEGER now{};
    if (!GetFileSizeEx(handle, &now)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        return {FileRead::Outcome::ReadFailed, 0, error};
    }
    const uint64_t size_now = static_cast<uint64_t>(now.QuadPart);

    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(offered));
    uint8_t *cursor = out.data() + at;
    uint64_t left = offered;
    while (left > 0) {
        const DWORD ask = left > 0x100000u ? 0x100000u : static_cast<DWORD>(left);
        DWORD got = 0;
        if (!ReadFile(handle, cursor, ask, &got, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            out.resize(at);
            return {FileRead::Outcome::ReadFailed, size_now, error};
        }
        /* End of file before the offered length: the file shrank. */
        if (got == 0) {
            CloseHandle(handle);
            out.resize(at);
            return {FileRead::Outcome::Shrank, size_now, 0};
        }
        cursor += got;
        left -= got;
    }
    CloseHandle(handle);
    return {FileRead::Outcome::Read, size_now, 0};
}

}  // namespace deskhop
