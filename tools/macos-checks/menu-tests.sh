#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

set -euo pipefail
cd "$(dirname "$0")/../.."
swift build
bin="$(swift build --show-bin-path)"
# The build's own module cache, not swiftc's default one: a header added to
# src/core is invisible to a stale cached DHCore, and swift build keeps this
# one current.
swiftc -module-cache-path "$bin/ModuleCache" -I "$bin/Modules" -I "$bin/DHCore.build" \
    helpers/macos/Sources/deskhop-helper/MenuBar.swift \
    helpers/macos/Sources/deskhop-helper/LaunchAtLogin.swift \
    helpers/macos/Tests/menu-tests/main.swift \
    "$bin"/DeskhopChannel.build/*.swift.o "$bin"/DHCore.build/*.o \
    -o "$bin/menu-tests"
"$bin/menu-tests"
