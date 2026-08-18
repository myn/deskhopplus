#!/usr/bin/env bash
#
# Build the config disk image that ships inside the firmware.
#
#   ./create.sh [page] [image]     defaults: ../webconfig/config.htm, disk.img
#
# mtools only — no loopback mount and no root, so this runs identically on
# macOS, Linux and CI. The previous version needed mkdosfs and a `sudo mount`,
# which meant it could not run on the development machine at all and the image
# had to be patched by hand or left stale (#16).
#
# The volume is deliberately larger than what ships. `mformat` lays out 2 MB of
# FAT12 and only the first 64 kB is kept, because that is all the firmware
# stores: `src/ramdisk.c` reports NUMBER_OF_BLOCKS (4096) to the host and
# serves zeros past ACTUAL_NUMBER_OF_BLOCKS (128). Keep FULL_SECTORS and
# SHIPPED_SECTORS below in step with those two.
#
# Output is byte-reproducible. SOURCE_DATE_EPOCH pins both the volume serial
# and the directory entry timestamp, which is what lets CI regenerate the image
# and compare it against the committed copy.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

page="${1:-../webconfig/config.htm}"
image="${2:-disk.img}"

# Geometry, read back out of the image this replaces (`minfo -i disk.img ::`)
# so the volume the host sees does not change shape.
FULL_SECTORS=4096       # what the boot sector claims, and ramdisk.c reports
SHIPPED_SECTORS=128     # what is actually kept, and what ramdisk.c serves
SECTOR=512
LABEL=DESKHOP

# 2020-01-01T00:00:00Z. Any fixed value works; this one is only memorable.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1577836800}"

die() { printf '%s\n' "$*" >&2; exit 1; }

command -v mformat >/dev/null || die "no mformat. brew install mtools, or apt-get install mtools"
command -v mcopy   >/dev/null || die "no mcopy. brew install mtools, or apt-get install mtools"
[ -f "$page" ] || die "no config page at $page — run 'make' in webconfig/ first"

work="$(mktemp -t deskhop-disk)"
trap 'rm -f "$work"' EXIT

dd if=/dev/zero of="$work" bs="$SECTOR" count="$FULL_SECTORS" status=none

mformat -t 128 -h 2 -s 16 -r 32 -c 4 -m 248 -v "$LABEL" -N 0 -i "$work" ::

# Before the copy, not after. The overflowing clusters would land past the
# shipped end, where the firmware answers with zeros — so writing first and
# checking later is checking after the bytes are already gone.
DH_SHIPPED_SECTORS="$SHIPPED_SECTORS" python3 capacity.py "$work" "$page"

mcopy -o -i "$work" "$page" "::/$(basename "$page")"

dd if="$work" of="$image" bs="$SECTOR" count="$SHIPPED_SECTORS" status=none

printf 'wrote %s (%s bytes)\n' "$image" "$(wc -c <"$image" | tr -d ' ')"
