#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

# Wrap the Mac helper binary in a .app bundle (#206).
#
#   ./tools/make-app-bundle.sh <binary> <out-dir>
#
# Writes <out-dir>/deskhopplus-helper.app and, around it, the zip a user
# downloads, <out-dir>/deskhopplus-helper-macos.zip. Versioned from
# src/core/dh_version.h like everything else that shows a number (#199).
#
# A .app is a folder of a fixed shape: Contents/Info.plist and the binary
# under Contents/MacOS. Finder opens a plain Mach-O in a Terminal window and a
# .app as an app, which is the whole point; LSUIElement keeps it out of the
# Dock as well, on top of the .accessory policy main.swift sets.
#
# Ad-hoc signed (`-s -`): a seal with no identity behind it, so no Developer
# Program and no notarization (#197 decision 5). Gatekeeper still refuses the
# download the first time and the README's Open Anyway step stays. The seal is
# for the arm64 kernel, which refuses to run an unsealed binary at all, and it
# covers every slice of the universal binary and the plist alike. Nothing is
# nested (no frameworks, no helpers), so signing the .app signs it all.
#
# The zip is made here too, so CI ships and the test checks one artefact.
# Info-ZIP keeps the mode bits, and the seal lives inside the files, so what
# a user unzips still verifies and runs.
#
# Run by the helper CI job on the release binary, and by hand on a local build
# to try the .app before a tag.

set -euo pipefail

binary="${1:?usage: $0 <binary> <out-dir>}"
out="${2:?usage: $0 <binary> <out-dir>}"
header="$(dirname "${BASH_SOURCE[0]}")/../src/core/dh_version.h"

# The same two lines CMakeLists.txt and check-release-tag.sh parse.
major="$(sed -n 's/^#define DH_VERSION_MAJOR \([0-9][0-9]*\).*/\1/p' "$header")"
minor="$(sed -n 's/^#define DH_VERSION_MINOR \([0-9][0-9]*\).*/\1/p' "$header")"
[ -n "$major" ] && [ -n "$minor" ] \
    || { echo "could not read DH_VERSION_MAJOR/MINOR from $header" >&2; exit 1; }
version="$major.$minor"

app="$out/deskhopplus-helper.app"
zip="$out/deskhopplus-helper-macos.zip"
rm -rf "$app" "$zip"
mkdir -p "$app/Contents/MacOS"
cp "$binary" "$app/Contents/MacOS/deskhopplus-helper"
chmod +x "$app/Contents/MacOS/deskhopplus-helper"

cat >"$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>deskhopplus-helper</string>
	<key>CFBundleIdentifier</key>
	<string>com.deskhopplus.helper</string>
	<key>CFBundleName</key>
	<string>deskhopplus helper</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$version</string>
	<key>CFBundleVersion</key>
	<string>$version</string>
	<key>LSMinimumSystemVersion</key>
	<string>13.0</string>
	<key>LSUIElement</key>
	<true/>
</dict>
</plist>
PLIST

codesign --force -s - "$app"
(cd "$out" && zip -qr deskhopplus-helper-macos.zip deskhopplus-helper.app)
echo "wrote $app and $zip"
