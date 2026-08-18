#!/usr/bin/env bash
#
# Are the committed config-UI artifacts what their sources produce?
#
#   ./tools/check-config-artifacts.sh
#
# Two questions, and deliberately not a third:
#
#   1. Does webconfig/config.htm match what the templates render?
#   2. Does disk/disk.img carry that page?
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
