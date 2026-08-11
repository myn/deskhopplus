#!/usr/bin/env bash
#
# The build, in one place, because none of its tools are on PATH on this
# machine and every session was rediscovering that.
#
#   ./tools/build.sh            firmware, helper, and both test suites
#   ./tools/build.sh fw         firmware only
#   ./tools/build.sh helper     the macOS helper and its tests
#   ./tools/build.sh tests      both host test suites, no firmware
#   ./tools/build.sh clean      remove build/ and tests/build/
#
# It always ends by printing the flashable artifact, its timestamp, and the
# version stamped into it. That last number is the one that matters: a peer
# board pulls firmware only when the peer reports a *strictly newer* version,
# so a build carrying the version already running leaves the second board on
# the old firmware, silently and with no error anywhere. See
# docs/verification/vendor-hid-interface.md.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

target="${1:-all}"

bold=$'\033[1m'; red=$'\033[31m'; green=$'\033[32m'; yellow=$'\033[33m'; off=$'\033[0m'
say()  { printf '%s==>%s %s\n' "$bold" "$off" "$*"; }
warn() { printf '%s==>%s %s\n' "$yellow" "$off" "$*"; }
die()  { printf '%s==>%s %s\n' "$red" "$off" "$*" >&2; exit 1; }

# ---------------------------------------------------------------- toolchain

# Both live outside PATH: cmake ships inside the app bundle, and the Arm
# toolchain installs under /Applications from the cask. Globbed rather than
# pinned so a toolchain upgrade does not silently fall back to a system gcc.
need_toolchain() {
    if ! command -v cmake >/dev/null 2>&1; then
        [ -x /Applications/CMake.app/Contents/bin/cmake ] \
            || die "no cmake. Install CMake.app, or put cmake on PATH."
        PATH="/Applications/CMake.app/Contents/bin:$PATH"
    fi

    if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        local found
        found="$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | sort -V | tail -1 || true)"
        [ -n "$found" ] \
            || die "no arm-none-eabi-gcc. brew install --cask gcc-arm-embedded
    (its .pkg step needs sudo in your own terminal — it fails headless)"
        PATH="$found:$PATH"
    fi

    export PATH
}

# ----------------------------------------------------------------- firmware

# The artifact's mtime before the build, so the report can distinguish "just
# rebuilt" from "already current". An unchanged timestamp is the honest result
# of an incremental build with no changed inputs -- but it looks identical to a
# build that never ran, which is a bad thing to leave ambiguous right before
# someone flashes a board.
uf2_stamp() { stat -f '%m' build/deskhop.uf2 2>/dev/null || echo none; }
before=""

build_fw() {
    need_toolchain
    before="$(uf2_stamp)"
    say "firmware → build/ (cmake)  [$(arm-none-eabi-gcc -dumpversion), $(cmake --version | head -1)]"

    # Reconfigure every time. It is cheap, and a stale cache is exactly how a
    # changed VERSION_MINOR fails to reach the compile flags.
    cmake -S . -B build >/dev/null
    cmake --build build -j"$(sysctl -n hw.ncpu)"
}

# ------------------------------------------------------------------- helper

build_helper() {
    command -v swift >/dev/null 2>&1 || die "no swift. Install the Command Line Tools."
    # .build/ is SwiftPM's own default, not a choice made here, and not the
    # same tree as cmake's build/. Named in full every time because the two
    # differ by one character and only one of them holds the flashable image.
    say "macOS helper → .build/ (swiftpm — not build/)"
    swift build
}

test_helper() {
    command -v swift >/dev/null 2>&1 || die "no swift. Install the Command Line Tools."
    say "helper tests"
    swift run channel-tests
}

test_core() {
    need_toolchain
    say "C core tests"
    cmake -S tests -B tests/build >/dev/null
    cmake --build tests/build >/dev/null
    ctest --test-dir tests/build --output-on-failure
}

# ------------------------------------------------------- what you will flash

# The point of the whole script. Reads the version back out of the .crc the
# build stamps rather than out of CMakeLists.txt, so it reports what is in the
# artifact and not what someone intended to be in it.
report() {
    local uf2=build/deskhop.uf2 crc=build/deskhop.crc
    [ -f "$uf2" ] || { warn "no firmware artifact"; return 0; }

    printf '\n%s────────────────────────────────────────────────────%s\n' "$bold" "$off"
    printf '%sFLASH THIS%s  %s\n' "$green" "$off" "$repo/$uf2"

    local freshness=""
    if [ -n "$before" ]; then
        if [ "$before" = "$(uf2_stamp)" ]; then
            freshness="  ← unchanged: no inputs changed, already current"
        else
            freshness="  ← rebuilt just now"
        fi
    fi
    printf '  built    %s%s\n' "$(date -r "$uf2" '+%Y-%m-%d %H:%M:%S')" "$freshness"

    if [ -f "$crc" ]; then
        local v
        v="$(python3 - "$crc" <<'PY'
import struct, sys
magic, version, crc = struct.unpack('<IHI', open(sys.argv[1], 'rb').read()[:10])
print(f"{version}  (v{version // 1000}.{version % 1000 - 100}, crc {crc:#010x})")
PY
)"
        printf '  version  %s\n' "$v"
        printf '\n  A board upgrades its peer only from a %sstrictly newer%s version.\n' "$bold" "$off"
        printf '  If this number is not above what the boards run, bump VERSION_MINOR\n'
        printf '  in CMakeLists.txt — otherwise only the board you flash changes.\n'
    fi

    # The two output trees differ by one character. Say which is which here,
    # so nobody goes looking for the image in the helper's directory.
    if [ -d .build ]; then
        printf '\n  %sbuild/%s   cmake, firmware — the .uf2 above\n' "$bold" "$off"
        printf '  %s.build/%s  swiftpm, the macOS helper — nothing to flash\n' "$bold" "$off"
    fi
    printf '%s────────────────────────────────────────────────────%s\n' "$bold" "$off"
}

# --------------------------------------------------------------------- main

case "$target" in
    all)    build_fw; build_helper; test_helper; test_core; report ;;
    fw)     build_fw; report ;;
    helper) build_helper; test_helper ;;
    tests)  test_helper; test_core ;;
    clean)  say "removing build/ tests/build/ .build/"; rm -rf build tests/build .build ;;
    *)      die "unknown target '$target' — one of: all fw helper tests clean" ;;
esac
