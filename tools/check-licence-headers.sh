#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

# Does every source file state its licence? (#198)
#
#   ./tools/check-licence-headers.sh
#
# A file written for this fork carries both lines of the header below. An
# upstream file keeps DeskHop's own notice; the three upstream scripts that
# never had one carry the "Modified by" line GPLv3 §5(a) asks for. Vendored
# trees are not ours to mark, and misc/crc32.py is upstream, unchanged, and
# never had a notice.
#
# ponytail: an upstream notice alone passes, so a later edit to an unchanged
# upstream file is not made to add its "Modified by" line. Enforcing that
# needs the v0.78 tag, which CI's shallow checkout does not have.
#
# Run by CI and by hand, so the check a developer can run is the one that
# gates the build rather than an approximation of it.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

missing=()
while IFS= read -r -d '' f; do
    grep -q 'Copyright (c) 20[0-9][0-9] Hrvoje Cavrak' "$f" && continue
    grep -q 'Modified by Derek Reynolds, 2026, for deskhopplus\.' "$f" && continue
    grep -q 'SPDX-License-Identifier: GPL-3.0-only' "$f" \
        && grep -q 'Copyright (c) 2026 Derek Reynolds' "$f" && continue
    missing+=("$f")
done < <(git ls-files -z -co --exclude-standard -- '*.c' '*.h' '*.cpp' '*.swift' '*.sh' '*.py' \
             ':!pico-sdk' ':!Pico-PIO-USB' ':!src/core/micro-ecc' ':!misc/crc32.py')

if [ "${#missing[@]}" -gt 0 ]; then
    printf 'No licence header:\n' >&2
    printf '  %s\n' "${missing[@]}" >&2
    printf '\nA new file starts with both lines, in its comment style, after any shebang:\n  SPDX-License-Identifier: GPL-3.0-only\n  Copyright (c) 2026 Derek Reynolds\n' >&2
    exit 1
fi
echo "every source file states its licence"
