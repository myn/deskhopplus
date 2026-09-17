#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

# Re-render the committed icon files from the menu bar's glyph (#208). See
# main.swift for what comes out. Mac only: it draws with AppKit and packs
# the .icns with iconutil. Run from anywhere; writes beside itself.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/../.."
swift build
bin="$(swift build --show-bin-path)"
swiftc -module-cache-path "$bin/ModuleCache" -I "$bin/Modules" -I "$bin/DHCore.build" \
    helpers/macos/Sources/deskhop-helper/MenuBar.swift \
    helpers/macos/Sources/deskhop-helper/LaunchAtLogin.swift \
    helpers/icon/main.swift \
    "$bin"/DeskhopChannel.build/*.swift.o "$bin"/DHCore.build/*.o \
    -o "$bin/render-icons"
"$bin/render-icons" "$here"
