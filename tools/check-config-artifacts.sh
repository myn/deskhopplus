#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Derek Reynolds

#
# Are the committed config-UI artifacts what their sources produce?
#
#   ./tools/check-config-artifacts.sh
#
# Three questions, and deliberately not a fourth:
#
#   1. Does webconfig/config.htm match what the templates render?
#   2. Does disk/disk.img carry that page?
#   3. Does the page's helper glyph still match the geometry it copies?
#
# What it does NOT ask is whether disk.img is byte-identical to a fresh build.
# That was tried and cannot work: mformat writes version-dependent bytes into
# the FAT directory entries, so the same page built by mtools 4.0.43 (the CI
# runner) and 4.0.49 (this machine) differs in 16 bytes of creation and
# last-access timestamps — measured, see disk/imgdiff.py. Byte equality was
# only ever a proxy for "the image carries the current page", and a proxy that
# also asserts both machines run the same mtools.
#
# Run by CI and by hand, so the check a developer can run is the check that
# gates the build rather than an approximation of it.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

red=$'\033[31m'; green=$'\033[32m'; off=$'\033[0m'
fail() { printf '%s%s%s\n' "$red" "$*" "$off" >&2; exit 1; }
ok()   { printf '%s%s%s\n' "$green" "$*" "$off"; }

# 1. The page against its sources. Nothing is rendered here — that is the
#    caller's job (`make` in webconfig/, or the firmware build) — because a
#    check that regenerates what it is checking cannot fail.
if ! git diff --exit-code --stat -- webconfig/config.htm webconfig/config-unpacked.htm; then
    fail "The committed config page is not what the templates render.
Run 'make' in webconfig/ and commit the result."
fi
ok "config page matches its templates"

# 2. The committed image against the committed page. Read out of HEAD rather
#    than the working tree so this answers for what is actually committed,
#    which is what a firmware build embeds.
command -v mcopy >/dev/null || fail "no mcopy. brew install mtools, or apt-get install mtools"

committed="$(mktemp "${TMPDIR:-/tmp}/deskhop-committed.XXXXXX")"
extracted="$(mktemp "${TMPDIR:-/tmp}/deskhop-page.XXXXXX")"
trap 'rm -f "$committed" "$extracted"' EXIT

git show HEAD:disk/disk.img > "$committed" || fail "no disk/disk.img committed at HEAD"

# mcopy insists on writing to a path it can create; -n overwrites without asking.
if ! mcopy -n -i "$committed" ::/config.htm "$extracted" 2>/dev/null; then
    fail "could not read config.htm out of the committed disk/disk.img"
fi

if ! cmp -s "$extracted" webconfig/config.htm; then
    fail "disk/disk.img does not contain the committed config page.
Run './create.sh' in disk/ and commit the image."
fi
ok "committed image carries the current page ($(wc -c <"$extracted" | tr -d ' ') bytes)"

# 3. The page's helper glyph against the one geometry (#208, #233). The macOS
#    menu bar draws that glyph in code, and helpers/icon/render.sh packs every
#    shipped icon from the same drawing. The config page cannot run Swift, so
#    its mark is a hand copy — and a hand copy held in place by a comment alone
#    drifts the first time the glyph changes. This makes that drift a failure.
#
#    The heredoc is deliberately not inside $(...): bash 3.2 mis-parses a quote
#    in a heredoc in a command substitution, and this script runs on the Mac.
glyph="$(mktemp "${TMPDIR:-/tmp}/deskhop-glyph.XXXXXX")"
trap 'rm -f "$committed" "$extracted" "$glyph"' EXIT

if python3 - > "$glyph" 2>&1 <<'PY'
import re, sys

swift = open("helpers/macos/Sources/deskhop-helper/MenuBar.swift").read()
page = open("webconfig/templates/main.html").read()

def rects(text, pattern):
    return [" ".join(m) for m in re.findall(pattern, text)]

# The .paired look only: the two screens and the bar that joins them. The
# badge and its knocked-out mark belong to .attention, which the page never
# draws, so they are not read here.
screens = re.search(r"let screens = \[(.*?)\]\n", swift, re.S)
bar = re.search(r"NSBezierPath\(rect: (NSRect\(x:.*?\))\)\.fill\(\)", swift)
if not screens or not bar:
    sys.exit("could not read the paired glyph out of MenuBar.swift")
swift_rect = r"x: ([\d.]+), y: ([\d.]+), width: ([\d.]+), height: ([\d.]+)"
source = rects(screens.group(1), swift_rect) + rects(bar.group(1), swift_rect)

macro = re.search(r"\{% macro glyph_rects\(\) %\}(.*?)\{% endmacro %\}", page, re.S)
if not macro:
    sys.exit("could not find glyph_rects() in webconfig/templates/main.html")
page_rect = r'x="([\d.]+)" y="([\d.]+)" width="([\d.]+)" height="([\d.]+)"'
copy = rects(macro.group(1), page_rect)

if source != copy:
    sys.exit("MenuBar.swift draws " + str(source) + "\nthe page draws       " + str(copy))
print(" | ".join(source))
PY
then
    ok "page glyph matches the menu bar drawing ($(cat "$glyph"))"
else
    fail "The page's helper glyph no longer matches the menu bar drawing.
$(cat "$glyph")
Copy the numbers from MenuBar.image(for:) into glyph_rects() in
webconfig/templates/main.html, then run 'make' in webconfig/ and './create.sh' in disk/."
fi
