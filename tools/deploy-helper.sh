#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

# Build the Mac helper and put the launchd job on it, with whichever launchctl
# verb the job needs right now:
#
#   ./tools/deploy-helper.sh
#
# kickstart -k when the job is loaded (the daily case); bootstrap when it is
# not, which is what Quit in the menu bar leaves behind. launchctl print is
# the authority on the job, not launchctl list (#192).

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

./tools/build.sh helper

job="gui/$(id -u)/com.deskhopplus.helper"
plist="$HOME/Library/LaunchAgents/com.deskhopplus.helper.plist"

if launchctl print "$job" >/dev/null 2>&1; then
    launchctl kickstart -k "$job"
    echo "kickstarted $job"
elif [ -f "$plist" ]; then
    launchctl bootstrap "gui/$(id -u)" "$plist"
    echo "bootstrapped $plist"
else
    echo "no job loaded and no $plist — Start at login in the menu bar writes one" >&2
    exit 1
fi
