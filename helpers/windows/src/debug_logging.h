/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once
/*
 * Debug logging's decisions, with no Win32 in them (#271), split the way
 * autostart_ladder.h is (ADR-0006). main.cpp owns the tray tick and the real
 * _wfsopen; this owns the setting and what to do with helper.log at start.
 *
 * The setting is a machine-local preference, the deliberate exception to the
 * device holding every setting (see Debug logging in GLOSSARY.md). It is a
 * marker file beside `autostart`: present is on, absent is off, so a machine
 * that ran an older helper starts off like a fresh one.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace deskhop::debug_logging {

/* The log trim: a helper.log bigger than this at start is emptied. */
inline constexpr std::uintmax_t kTrimBytes = 5u * 1024u * 1024u;

inline std::filesystem::path marker(const std::filesystem::path &directory) {
    return directory / "debug-logging";
}

inline bool is_on(const std::filesystem::path &directory) {
    std::error_code ignored;
    return std::filesystem::exists(marker(directory), ignored);
}

/* Saves the choice. False when the marker could not be written or removed. */
inline bool set(const std::filesystem::path &directory, bool on) {
    if (on) return static_cast<bool>(std::ofstream(marker(directory)));
    std::error_code error;
    std::filesystem::remove(marker(directory), error);
    return !error;
}

enum class LogStart { DoNotOpen, Empty, Append };

/* What to do with helper.log at start. `size` is 0 when there is no file.
   Off never touches the file, so a log kept on purpose is never emptied. */
inline LogStart at_start(bool on, std::uintmax_t size) {
    if (!on) return LogStart::DoNotOpen;
    return size > kTrimBytes ? LogStart::Empty : LogStart::Append;
}

} // namespace deskhop::debug_logging
