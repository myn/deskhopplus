#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
swift build
bin="$(swift build --show-bin-path)"
swiftc -I "$bin/Modules" -I "$bin/DHCore.build" \
    helpers/macos/Sources/deskhop-helper/MenuBar.swift \
    helpers/macos/Sources/deskhop-helper/LaunchAtLogin.swift \
    helpers/macos/Tests/menu-tests/main.swift \
    "$bin"/DeskhopChannel.build/*.swift.o "$bin"/DHCore.build/*.o \
    -o "$bin/menu-tests"
"$bin/menu-tests"
