#!/usr/bin/env bash
#
# The build, in one place, because none of its tools are on PATH on this
# machine and every session was rediscovering that.
#
#   ./tools/build.sh            firmware, helper, and both test suites
#   ./tools/build.sh fw         firmware only
#   ./tools/build.sh helper     the macOS helper and its tests
#   ./tools/build.sh tests      both host test suites, no firmware
#   ./tools/build.sh clean      remove build/, tests/build/ and .build/
#
# It always ends by printing what it produced and whether that is what is
# actually deployed, because both artifacts have a way of appearing to build
# fine while changing nothing:
#
#   Firmware -- board B pulls from board A on a strictly newer version, or on
#   an equal version carrying a different image (#91). So a rebuild reaches
#   both boards without a version bump, but a *downgrade* still needs the
#   number to move, and board A never follows B at all. See
#   docs/verification/vendor-hid-interface.md.
#
#   Helper -- launchd holds the binary it started with, so a rebuild is not a
#   deploy. A helper started before a wire-format change ran against newer
#   firmware for two days, failing every frame it was sent, while every rebuild
#   reported success (#93).

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

# ---------------------------------------------------------------- artifacts

# The two things this script produces, and where the agent's job file lives.
helper_bin=.build/release/deskhop-helper
helper_plist_installed="$HOME/Library/LaunchAgents/com.deskhopplus.helper.plist"

# Each artifact's mtime before the build, so the report can distinguish "just
# rebuilt" from "already current". An unchanged timestamp is the honest result
# of an incremental build with no changed inputs -- but it looks identical to a
# build that never ran, which is a bad thing to leave ambiguous right before
# someone flashes a board or restarts an agent.
uf2_stamp() { stat -f '%m' build/deskhop.uf2 2>/dev/null || echo none; }
helper_stamp() { stat -f '%m' "$helper_bin" 2>/dev/null || echo none; }
before=""
helper_before=""

freshness() {
    local was="$1" now="$2"
    [ -n "$was" ] || return 0
    if [ "$was" = "$now" ]; then
        printf '  ← unchanged: no inputs changed, already current'
    else
        printf '  ← rebuilt just now'
    fi
}

# Seconds as something a person can act on. "2d 4h older than the build" is the
# sentence that would have caught #93; a raw epoch delta is not.
elapsed() {
    local s="$1"
    if   [ "$s" -ge 86400 ]; then printf '%dd %dh' "$(( s / 86400 ))" "$(( s % 86400 / 3600 ))"
    elif [ "$s" -ge 3600 ];  then printf '%dh %dm' "$(( s / 3600 ))"  "$(( s % 3600 / 60 ))"
    elif [ "$s" -ge 60 ];    then printf '%dm'     "$(( s / 60 ))"
    else                          printf '%ds'     "$s"
    fi
}

# ----------------------------------------------------------------- firmware

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

# Release, not debug, because release is what gets deployed: the install steps
# in helpers/macos/README.md copy .build/release/deskhop-helper, and the
# LaunchAgent runs whatever that copy left behind. A debug build here meant the
# documented build command could not refresh the documented install (#93) --
# every rebuild landed in .build/debug/ and changed nothing the agent was
# running. The debug tree still appears; `swift run channel-tests` builds it.
build_helper() {
    command -v swift >/dev/null 2>&1 || die "no swift. Install the Command Line Tools."
    helper_before="$(helper_stamp)"
    # .build/ is SwiftPM's own default, not a choice made here, and not the
    # same tree as cmake's build/. Named in full every time because the two
    # differ by one character and only one of them holds the flashable image.
    say "macOS helper → .build/release/ (swiftpm — not build/)"
    swift build -c release
}

# Release, to reuse the build above rather than compiling the whole package a
# second time in debug -- and so the suite that gates the change exercises the
# configuration that actually gets deployed. CI still runs the debug pair
# (`swift build` + `swift run channel-tests`), so both readings are covered.
test_helper() {
    command -v swift >/dev/null 2>&1 || die "no swift. Install the Command Line Tools."
    say "helper tests"
    swift run -c release channel-tests
}

test_core() {
    need_toolchain
    say "C core tests"
    cmake -S tests -B tests/build >/dev/null
    cmake --build tests/build >/dev/null
    ctest --test-dir tests/build --output-on-failure
}

# --------------------------------------------------- what you will flash

# Reads the version back out of the .crc the build stamps rather than out of
# CMakeLists.txt, so it reports what is in the artifact and not what someone
# intended to be in it.
report_fw() {
    local uf2=build/deskhop.uf2 crc=build/deskhop.crc
    [ -f "$uf2" ] || { warn "no firmware artifact"; return 0; }

    printf '%sFLASH THIS%s   %s\n' "$green" "$off" "$repo/$uf2"
    printf '  built     %s%s\n' \
        "$(date -r "$uf2" '+%Y-%m-%d %H:%M:%S')" "$(freshness "$before" "$(uf2_stamp)")"

    if [ -f "$crc" ]; then
        local v
        v="$(python3 - "$crc" <<'PY'
import struct, sys
magic, version, crc = struct.unpack('<IHI', open(sys.argv[1], 'rb').read()[:10])
print(f"{version}  (v{version // 1000}.{version % 1000 - 100}, crc {crc:#010x})")
PY
)"
        printf '  version   %s\n' "$v"
        printf '\n  Board B follows board A on a newer version, or on the %ssame%s version\n' "$bold" "$off"
        printf '  carrying a different image (#91) — so a rebuild propagates as it is.\n'
        printf '  Bump VERSION_MINOR in CMakeLists.txt to flash something %solder%s onto\n' "$bold" "$off"
        printf '  the pair, and to move board A, which never follows B.\n'
    fi
}

# ------------------------------------------------- what the agent is running

# Building the helper is a third of deploying it. The binary also has to be
# copied where the LaunchAgent points, and the agent restarted -- and neither
# of those is visible from a build that succeeded. That gap is #93: a helper
# started before a fix reached the tree ran against newer firmware for two
# days, failing every frame it was sent, ~1.4 teardowns a second, while every
# rebuild reported success and changed nothing.
#
# So report the three things a green build cannot tell you: where the agent
# points, whether anything is there, and whether what is running predates what
# was just built.

# The installed copy wins. Its ProgramArguments is routinely edited away from
# the repo's /usr/local/bin default to avoid sudo (see #70's first sitting),
# and it is the file launchd actually reads.
helper_plist() {
    if [ -f "$helper_plist_installed" ]; then printf '%s' "$helper_plist_installed"
    else printf '%s' helpers/macos/LaunchAgent/com.deskhopplus.helper.plist
    fi
}

# By inode, not by string: the plist may name the same binary by an absolute
# path where this script holds a relative one, or through a symlink.
same_file() {
    local a b
    a="$(stat -f '%d:%i' "$1" 2>/dev/null)" || return 1
    b="$(stat -f '%d:%i' "$2" 2>/dev/null)" || return 1
    [ "$a" = "$b" ]
}

# launchd is the authority on the agent's pid. pgrep cannot distinguish the
# agent from a foreground `swift run deskhop-helper` (README documents running
# one), and docs/verification/helper-channel.md says as much: "Confirm against
# `ps -o ppid=` or `launchctl list`, not `pgrep` alone".
agent_pid() {
    launchctl list com.deskhopplus.helper 2>/dev/null \
        | sed -n 's/.*"PID" = \([0-9]*\);.*/\1/p' | head -1
}

# A plist sitting in ~/Library/LaunchAgents is not a bootstrapped job, and the
# two need different remedies: kickstart restarts a loaded job and fails on one
# launchd has never heard of.
agent_loaded() { launchctl list com.deskhopplus.helper >/dev/null 2>&1; }

# ps pads lstart to a fixed width, and date -j rejects the trailing spaces.
proc_started() {
    local lstart
    lstart="$(ps -o lstart= -p "$1" 2>/dev/null | sed 's/ *$//')" || return 1
    [ -n "$lstart" ] || return 1
    date -j -f '%a %b %e %T %Y' "$lstart" +%s 2>/dev/null
}

report_helper() {
    [ -f "$helper_bin" ] || { warn "no helper artifact at $helper_bin"; return 0; }

    local built; built="$(helper_stamp)"
    printf '\n%sDEPLOY THIS%s  %s\n' "$green" "$off" "$repo/$helper_bin"
    printf '  built     %s%s\n' \
        "$(date -r "$helper_bin" '+%Y-%m-%d %H:%M:%S')" "$(freshness "$helper_before" "$built")"

    # Keyed on the agent being installed, not on any deskhop-helper existing:
    # a developer running one in the foreground has no agent to be stale, and
    # telling them to kickstart a job that was never bootstrapped is a remedy
    # that errors out.
    if [ ! -f "$helper_plist_installed" ]; then
        printf '  agent     not installed — helpers/macos/README.md has the steps\n'
        if pgrep -qx deskhop-helper 2>/dev/null; then
            printf '  running   one in the foreground, which this does not manage\n'
        fi
        return 0
    fi

    local pid; pid="$(agent_pid)"

    local plist target stale=0
    plist="$(helper_plist)"
    # plutil writes its parse errors to stdout, so a bare 2>/dev/null would
    # capture the error text and go on to treat it as a path.
    target="$(plutil -extract ProgramArguments.0 raw -o - "$plist" 2>/dev/null)" || target=""
    case "$target" in /*) ;; *) target="" ;; esac

    if [ -z "$target" ]; then
        printf '  agent     %s%s%s\n' "$yellow" "$plist" "$off"
        printf '            %s↑ no readable ProgramArguments — cannot tell what it runs%s\n' "$yellow" "$off"
        stale=1
    elif [ ! -f "$target" ]; then
        # What #93 actually found: .build/ had been cleaned, so the agent was
        # holding a deleted binary and the path it names did not exist at all.
        printf '  installed %s%s — nothing there%s\n' "$red" "$target" "$off"
        stale=1
    else
        local at delta=""
        at="$(stat -f '%m' "$target")"
        if [ "$at" -lt "$built" ]; then
            delta="  ${yellow}← $(elapsed "$(( built - at ))") older than this build${off}"
            stale=1
        fi
        printf '  installed %s  %s%s\n' "$target" "$(date -r "$target" '+%Y-%m-%d %H:%M:%S')" "$delta"
    fi

    local started run_delta="" not_running=0
    if [ -z "$pid" ]; then
        # An installed agent with no process is its own failure, and a worse
        # one than a stale binary: nothing is running at all.
        printf '  running   %snothing — the agent is installed but not up%s\n' "$red" "$off"
        not_running=1
        stale=1
    else
        started="$(proc_started "$pid" || true)"
        if [ -z "$started" ]; then
            printf '  running   pid %s\n' "$pid"
        else
            # The heart of #93: a process that predates the binary cannot be
            # running it, however green the build was.
            if [ "$started" -lt "$built" ]; then
                run_delta="  ${yellow}← started $(elapsed "$(( built - started ))") before this build${off}"
                stale=1
            fi
            printf '  running   pid %-6s %s%s\n' "$pid" "$(date -r "$started" '+%Y-%m-%d %H:%M:%S')" "$run_delta"
        fi
    fi

    if [ "$stale" = 1 ]; then
        if [ "$not_running" = 1 ]; then
            printf '\n  %sThe agent is not running.%s\n' "$yellow" "$off"
        else
            printf '\n  %sThe agent is not running what you just built.%s\n' "$yellow" "$off"
        fi
        if [ -z "$target" ]; then
            printf '    Check ProgramArguments in %s\n' "$plist"
        else
            # Pointing the agent straight at .build/release is the documented
            # way to avoid sudo, and then there is nothing to copy -- only a
            # restart. Suggesting a cp onto itself would be noise at best.
            same_file "$repo/$helper_bin" "$target" \
                || printf '    cp %s %s\n' "$helper_bin" "$target"
            if agent_loaded; then
                printf '    launchctl kickstart -k gui/$(id -u)/com.deskhopplus.helper\n'
            else
                printf '    launchctl bootstrap gui/$(id -u) %s\n' "$helper_plist_installed"
            fi
        fi
    fi
}

report() {
    printf '\n%s────────────────────────────────────────────────────%s\n' "$bold" "$off"
    case " $* " in *" fw "*)     report_fw ;; esac
    case " $* " in *" helper "*) report_helper ;; esac

    # The two output trees differ by one character. Say which is which here,
    # so nobody goes looking for the image in the helper's directory.
    if [ -d build ] && [ -d .build ]; then
        printf '\n  %sbuild/%s   cmake, firmware — the .uf2\n' "$bold" "$off"
        printf '  %s.build/%s  swiftpm, the macOS helper — the agent binary\n' "$bold" "$off"
    fi
    printf '%s────────────────────────────────────────────────────%s\n' "$bold" "$off"
}

# --------------------------------------------------------------------- main

case "$target" in
    all)    build_fw; build_helper; test_helper; test_core; report fw helper ;;
    fw)     build_fw; report fw ;;
    helper) build_helper; test_helper; report helper ;;
    tests)  test_helper; test_core ;;
    clean)  # .build/ holds the agent binary, and an installed plist may point
            # straight at it -- in which case this leaves launchd running a
            # deleted file until the next build and restart (#93). Captured
            # rather than piped into grep: under pipefail a grep that exits on
            # its first match SIGPIPEs plutil and turns the test false, which
            # would silently suppress the warning.
            say "removing build/ tests/build/ .build/"
            if [ -f "$helper_plist_installed" ]; then
                agent_target="$(plutil -extract ProgramArguments.0 raw -o - \
                                    "$helper_plist_installed" 2>/dev/null)" || agent_target=""
                case "$agent_target" in
                    "$repo"/.build/*)
                        warn "the LaunchAgent runs $agent_target, which this deletes."
                        warn "After the next build, restart it:"
                        warn "  launchctl kickstart -k gui/\$(id -u)/com.deskhopplus.helper" ;;
                esac
            fi
            rm -rf build tests/build .build ;;
    *)      die "unknown target '$target' — one of: all fw helper tests clean" ;;
esac
