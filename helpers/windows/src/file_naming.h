#pragma once
/*
 * What a file is called once it is on this computer's disk (#56).
 *
 * The shared core already guarantees a name is safe to join to a directory
 * path — `dh_file_name_is_clean` is that boundary. What it cannot guarantee is
 * that two names in one delivery are *different*: cleaning maps several
 * spellings onto one, and two files copied from different folders can share a
 * name outright. Either way the second must not quietly overwrite the first,
 * so it is renamed rather than dropped — a transfer that says it delivered
 * three files has to have delivered three.
 *
 * No Win32 here, so the rule is reachable from a test on any machine. The
 * macOS twin is `FileNaming.unused`, and the two must agree: a delivery that
 * round-trips has to come back under the names it went out as.
 */

#include <set>
#include <string>

#include "dh_file_list.h"

namespace deskhop {

/*
 * `name`, or the first suffixed form of it that `used` has not seen. The suffix
 * goes before the extension so the file still opens in whatever the user
 * expects.
 *
 * A leading dot is a hidden file, not an extension: `.gitignore` suffixed as
 * `-2.gitignore` would be a different kind of file.
 */
inline std::string unused_file_name(const std::string &name, std::set<std::string> &used) {
    if (used.insert(name).second) return name;

    const size_t dot = name.rfind('.');
    const bool has_extension = dot != std::string::npos && dot != 0;
    const std::string stem = has_extension ? name.substr(0, dot) : name;
    const std::string extension = has_extension ? name.substr(dot) : std::string();

    for (unsigned attempt = 2; attempt <= DH_FILE_LIST_MAX + 1u; attempt++) {
        const std::string candidate = stem + "-" + std::to_string(attempt) + extension;
        if (used.insert(candidate).second) return candidate;
    }
    /* Unreachable: a list holds at most DH_FILE_LIST_MAX names, so that many
       attempts cannot all collide. Named rather than left to fall through,
       because falling through would return the name unchanged and overwrite
       the file it collided with. */
    const std::string last = stem + "-" + std::to_string(used.size() + 1) + extension;
    used.insert(last);
    return last;
}

} // namespace deskhop
