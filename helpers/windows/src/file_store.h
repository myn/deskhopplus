#pragma once
/*
 * Where files that arrive from the other computer are written (#56).
 *
 * Copying a file puts a *reference* on the clipboard, not its contents, so
 * pasting one that came over the link means there has to be a real file at a
 * real path to point at. This is that path — and because these are files
 * nobody asked to keep, it is a temporary directory that is swept when the
 * helper starts, rather than a folder in the user's own space that would slowly
 * fill up with things they never chose to save (#42, story 11).
 *
 * Emptied on **start** rather than on a timer or at exit, deliberately. A
 * helper that crashes never runs an exit path, and a timer would delete a file
 * the user is still working on.
 *
 * **The newest set survives that sweep**, and the reason is the crash. A
 * CF_HDROP reference is a list of paths, and a path outlives the process that
 * put it there — so a helper that crashed and was restarted seconds after
 * files arrived would delete the very files the user is about to paste and
 * leave a reference pointing at nothing. Keeping one set costs a bounded
 * amount of disk and is not accumulation: every run leaves at most one
 * behind.
 *
 * ---------------------------------------------------------------------------
 * NOTHING PARTIAL IS EVER LEFT BEHIND
 *
 * A transfer that fails delivers nothing at all — the transfer machine
 * discards an incomplete payload rather than handing it over — so this is only
 * ever called with every byte in hand. What can still fail is the writing, and
 * a set half-written to disk would put working references to truncated files
 * on the clipboard. So a failure removes the whole directory and reports
 * nothing, which is the same rule one layer up.
 *
 * The macOS twin is FileStore.swift. What is written twice is the platform
 * call; the *rule* about colliding names is `file_naming.h`, shared with it in
 * spirit and with the same wording, because a helper that renames differently
 * on each side is a delivery that does not round-trip.
 */

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "clip_service.h"

namespace deskhop {

class FileStore {
  public:
    struct Written {
        std::wstring directory;
        std::vector<std::wstring> paths;
    };

    /* Diagnostics, never shown to the user. */
    std::function<void(const std::string &)> log;

    /* `%TEMP%\deskhopplus` unless a caller names somewhere else, which only a
       test does. */
    explicit FileStore(std::wstring root = default_root());
    static std::wstring default_root();

    /* Throw away what previous runs left, keeping only the newest set — see
       above for why that one is kept. Called once, at startup. */
    void collect_garbage();

    /* Write a delivered set and say where each file landed. False when
       anything went wrong, having removed whatever had been written. */
    bool write(const FileDelivery &delivery, Written &out);

  private:
    std::wstring root_;
    /* Distinguishes two sets that arrive inside the same tick. */
    unsigned long sequence_{0};
};

} // namespace deskhop
