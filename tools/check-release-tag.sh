#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

# Does a release tag name the version in src/core/dh_version.h? (#203)
#
#   ./tools/check-release-tag.sh v1.0
#
# The release job runs this before it downloads or publishes anything, so a
# tag pushed without moving the header (or the other way round) stops there
# rather than shipping files whose menus and printouts say a different number.
# The tag is two parts, v<MAJOR>.<MINOR>, the same as the firmware's own
# printout (#197 decision 12).
#
# Run by CI and by hand, so the check a developer can run is the one that
# gates the release rather than an approximation of it.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

tag="${1:?usage: $0 <tag>}"
header=src/core/dh_version.h

# The same two lines CMakeLists.txt parses.
major="$(sed -n 's/^#define DH_VERSION_MAJOR \([0-9][0-9]*\).*/\1/p' "$header")"
minor="$(sed -n 's/^#define DH_VERSION_MINOR \([0-9][0-9]*\).*/\1/p' "$header")"
[ -n "$major" ] && [ -n "$minor" ] \
    || { echo "could not read DH_VERSION_MAJOR/MINOR from $header" >&2; exit 1; }

want="v$major.$minor"
if [ "$tag" != "$want" ]; then
    printf 'tag %s does not name the release: %s says %s\n' "$tag" "$header" "$want" >&2
    exit 1
fi
echo "tag $tag names the release in $header"
