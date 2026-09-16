/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace deskhop {

/*
 * The read of one lazy file, at the moment the paste side asks for it (#182,
 * ADR-0011 amendment).
 *
 * The offer's total is the length that will be sent. `read_offered` appends
 * exactly that many bytes to `out`. A file that grew since the copy gives its
 * first offered-length bytes. A file that shrank cannot give them, so the read
 * fails and `out` is as it was. The size now is returned with the outcome, so
 * the caller can log how much the file grew.
 *
 * A failed open or read carries GetLastError (#181). The caller logs it with
 * the file's name and the step, so the log names the cause: a locked file, a
 * cloud placeholder or a missing file.
 *
 * A unit of its own, and not a lambda in clipboard.cpp, so a test can reach it
 * with a real file and no clipboard.
 */
struct FileRead {
    enum class Outcome { Read, Shrank, OpenFailed, ReadFailed };
    Outcome outcome;
    /* The file's length at request time. Set for Read and Shrank. */
    uint64_t size_now;
    /* GetLastError of the step that failed. Set for OpenFailed and ReadFailed. */
    DWORD error;
};

FileRead read_offered(const std::wstring &path, uint64_t offered, std::vector<uint8_t> &out);

}  // namespace deskhop
