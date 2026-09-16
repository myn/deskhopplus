/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "file_read.h"

#include <windows.h>
#include <share.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using deskhop::FileRead;
using deskhop::read_offered;

/*
 * The read of one lazy file at the offered length (#182, #181).
 *
 * Each test writes a real temporary file. It offers the file at one length,
 * changes the file, and then checks only what the read gives back. No
 * clipboard and no service: the rule is the read function's alone.
 */

namespace {

int failures = 0;

#define CHECK(condition, message)                                                                  \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            std::fprintf(stderr, "FAIL: %s\n", message);                                           \
            ++failures;                                                                             \
        }                                                                                           \
    } while (false)

/* A fresh temporary file that holds `bytes`. */
std::wstring temporary_file(const std::vector<uint8_t> &bytes) {
    wchar_t directory[MAX_PATH]{};
    GetTempPathW(MAX_PATH, directory);
    wchar_t path[MAX_PATH]{};
    GetTempFileNameW(directory, L"dhr", 0, path);
    HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    CloseHandle(handle);
    return path;
}

const std::vector<uint8_t> kOffered = {'f', 'i', 'r', 's', 't', ' ', 's', 'i',
                                       'x', 't', 'e', 'e', 'n', ' ', 'b', 'y'};

void test_a_grown_file_is_sent_at_the_offered_length() {
    const std::wstring path = temporary_file(kOffered);
    HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    const char more[] = "tes and then some more";
    DWORD wrote = 0;
    WriteFile(handle, more, sizeof more - 1, &wrote, nullptr);
    CloseHandle(handle);

    std::vector<uint8_t> out;
    const FileRead result = read_offered(path, 16, out);
    DeleteFileW(path.c_str());
    CHECK(result.outcome == FileRead::Outcome::Read, "a grown file is read");
    CHECK(out == kOffered, "the first offered-length bytes are what is sent");
    CHECK(result.size_now == 38, "the size now is reported so the log can say how much it grew");
}

void test_a_shrunk_file_fails_with_its_size_now() {
    const std::wstring path = temporary_file(kOffered);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    SetFilePointer(handle, 5, nullptr, FILE_BEGIN);
    SetEndOfFile(handle);
    CloseHandle(handle);

    std::vector<uint8_t> out;
    const FileRead result = read_offered(path, 16, out);
    DeleteFileW(path.c_str());
    CHECK(result.outcome == FileRead::Outcome::Shrank, "a file shorter than offered fails");
    CHECK(result.size_now == 5, "the failure carries the size now");
    CHECK(out.empty(), "a failed file leaves the payload empty");
}

void test_a_missing_file_fails_at_open() {
    const std::wstring path = temporary_file({});
    DeleteFileW(path.c_str());

    std::vector<uint8_t> out;
    const FileRead result = read_offered(path, 16, out);
    CHECK(result.outcome == FileRead::Outcome::OpenFailed, "a missing file fails at open");
    CHECK(result.error == ERROR_FILE_NOT_FOUND, "the failure carries the Win32 error");
}

/* The helper's own log is held open by its writer, as `_wfsopen(L"a",
   _SH_DENYWR)` opens it. A read that does not share write fails against that
   writer with ERROR_SHARING_VIOLATION, which is how helper.log was lost in
   the #182 report. */
void test_a_file_held_open_by_its_writer_is_read() {
    const std::wstring path = temporary_file(kOffered);
    FILE *writer = _wfsopen(path.c_str(), L"a", _SH_DENYWR);

    std::vector<uint8_t> out;
    const FileRead result = read_offered(path, 16, out);
    if (writer != nullptr) std::fclose(writer);
    DeleteFileW(path.c_str());
    CHECK(writer != nullptr, "the fixture's writer opened the file");
    CHECK(result.outcome == FileRead::Outcome::Read, "a file held open by its writer is read");
    CHECK(out == kOffered, "the bytes are the offered length");
}

/* A size-0 file is a real offer, and must not read as a file that shrank. */
void test_an_empty_file_is_sent() {
    const std::wstring path = temporary_file({});

    std::vector<uint8_t> out;
    const FileRead result = read_offered(path, 0, out);
    DeleteFileW(path.c_str());
    CHECK(result.outcome == FileRead::Outcome::Read, "an empty file is read");
    CHECK(out.empty(), "an empty file is sent as no bytes");
}

}  // namespace

int main() {
    test_a_grown_file_is_sent_at_the_offered_length();
    test_a_shrunk_file_fails_with_its_size_now();
    test_a_missing_file_fails_at_open();
    test_a_file_held_open_by_its_writer_is_read();
    test_an_empty_file_is_sent();
    if (failures > 0) {
        std::fprintf(stderr, "%d file_read check(s) failed\n", failures);
        return 1;
    }
    std::printf("file_read tests passed\n");
    return 0;
}
