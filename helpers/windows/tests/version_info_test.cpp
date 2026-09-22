/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

/*
 * The exe's own properties (#237): what Explorer's Details tab and Task
 * Manager read. Checked on the built exe, whose path
 * CMake passes as the one argument, so a macro the resource compiler left
 * unexpanded fails here and not only on someone's screen.
 *
 * The version is a literal on purpose, like the menu tests: move it with
 * src/core/dh_version.h.
 */

namespace {

int failures = 0;

/* One string from the US English, Unicode table the .rc writes. */
void check_string(const std::vector<char> &block, const char *name, const char *expected) {
    char query[64];
    std::snprintf(query, sizeof query, "\\StringFileInfo\\040904B0\\%s", name);
    void *value = nullptr;
    UINT length = 0;
    if (!VerQueryValueA(block.data(), query, &value, &length) || length == 0) {
        std::fprintf(stderr, "FAIL: %s is missing\n", name);
        ++failures;
        return;
    }
    if (std::strcmp(static_cast<const char *>(value), expected) != 0) {
        std::fprintf(stderr, "FAIL: %s is \"%s\", expected \"%s\"\n", name,
                     static_cast<const char *>(value), expected);
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: version_info_test <path to deskhop-helper.exe>\n");
        return 2;
    }
    DWORD size = GetFileVersionInfoSizeA(argv[1], nullptr);
    if (size == 0) {
        std::fprintf(stderr, "FAIL: %s carries no VERSIONINFO\n", argv[1]);
        return 1;
    }
    std::vector<char> block(size);
    if (!GetFileVersionInfoA(argv[1], 0, size, block.data())) {
        std::fprintf(stderr, "FAIL: could not read the VERSIONINFO of %s\n", argv[1]);
        return 1;
    }

    check_string(block, "FileDescription", "DeskHopPlus Helper");
    check_string(block, "ProductName", "DeskHopPlus");
    check_string(block, "FileVersion", "1.0");
    check_string(block, "ProductVersion", "1.0");

    /* The numeric versions, 1.0.0.0; Explorer shows the file one. */
    VS_FIXEDFILEINFO *fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueA(block.data(), "\\", reinterpret_cast<void **>(&fixed), &length) ||
        fixed->dwFileVersionMS != MAKELONG(0, 1) || fixed->dwFileVersionLS != 0 ||
        fixed->dwProductVersionMS != MAKELONG(0, 1) || fixed->dwProductVersionLS != 0) {
        std::fprintf(stderr, "FAIL: the fixed versions are not 1.0.0.0\n");
        ++failures;
    }

    if (failures == 0) std::printf("version_info: all passed\n");
    return failures == 0 ? 0 : 1;
}
