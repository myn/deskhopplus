#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

#
# make-app-bundle.sh must turn a bare binary into a .app that macOS opens as
# an app (#206): Finder runs a plain Mach-O in a Terminal window, and that is
# what the v1.0 zip gave a stranger on first run. The .app is what Gatekeeper
# and launchd see, so the checks here are theirs: plutil for the plist,
# codesign for the ad-hoc seal, and one run of the binary from inside the
# .app, which an arm64 kernel refuses when the seal is missing. The same
# checks run again on what unzip gives back, because the zip is what ships.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# A stand-in helper: any binary that exits 0. Compiled here rather than copied
# from /bin so the seal being tested is ours, not Apple's.
printf 'int main(void) { return 0; }\n' >"$tmp/t.c"
cc "$tmp/t.c" -o "$tmp/deskhop-helper"

./tools/make-app-bundle.sh "$tmp/deskhop-helper" "$tmp/out"

app="$tmp/out/deskhopplus-helper.app"
plist="$app/Contents/Info.plist"

fail=0
check() {
    local got
    if ! got="$("$@" 2>&1)"; then echo "FAIL: $*"; echo "$got"; fail=1; fi
}
key() {
    local got; got="$(/usr/libexec/PlistBuddy -c "Print $1" "$plist" 2>/dev/null || true)"
    [ "$got" = "$2" ] || { echo "FAIL: $1 is '$got', want '$2'"; fail=1; }
}
verify_app() {
    check test -x "$1/Contents/MacOS/deskhopplus-helper"
    check codesign --verify --deep --strict "$1"
    check "$1/Contents/MacOS/deskhopplus-helper"
}

verify_app "$app"
check plutil -lint "$plist"
key CFBundleExecutable deskhopplus-helper
key CFBundleIdentifier com.deskhopplus.helper
key CFBundlePackageType APPL
# The literal on purpose, like the menu tests: it moves with dh_version.h.
key CFBundleShortVersionString 1.0
key LSUIElement true
# The icon Finder shows (#208): the plist names it, and the file is inside.
key CFBundleIconFile deskhop
check test -s "$app/Contents/Resources/deskhop.icns"
# Not grep -q: it closes the pipe on the first match, codesign dies of SIGPIPE,
# and pipefail turns a good seal into a failure now and then.
codesign -dv "$app" 2>&1 | grep 'Signature=adhoc' >/dev/null || { echo "FAIL: not ad-hoc signed"; fail=1; }

mkdir "$tmp/unzipped" && unzip -q "$tmp/out/deskhopplus-helper-macos.zip" -d "$tmp/unzipped"
verify_app "$tmp/unzipped/deskhopplus-helper.app"

[ "$fail" -eq 0 ] && echo "app bundle tests passed"
exit "$fail"
