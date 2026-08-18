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
# Output is byte-reproducible across machines, which is what lets CI regenerate
# the image and compare it against the committed copy. Three things have to be
# pinned for that: `-N 0` fixes the volume serial, SOURCE_DATE_EPOCH fixes the
# directory entry timestamp, and the OEM field is overwritten below because
# mformat stamps its own version into it.

set -euo pipefail

page="${1:-}"
image="${2:-}"

# Absolute before the cd below, so a relative path given on the command line
# still means what the caller meant rather than something inside disk/.
absolute() {
    local dir
    dir="$(cd "$(dirname "$1")" 2>/dev/null && pwd)" || dir=""

    # A path whose directory does not exist is left alone, so the "no config
    # page at ..." message below names what the caller actually typed.
    if [ -n "$dir" ]; then
        printf '%s/%s\n' "$dir" "$(basename "$1")"
    else
        printf '%s\n' "$1"
    fi
}

if [ -n "$page" ]; then
    page="$(absolute "$page")"
fi
if [ -n "$image" ]; then
    image="$(absolute "$image")"
fi

cd "$(dirname "${BASH_SOURCE[0]}")"

page="${page:-../webconfig/config.htm}"
image="${image:-disk.img}"

# Geometry, read back out of the image this replaces (`minfo -i disk.img ::`)
# so the volume the host sees does not change shape.
FULL_SECTORS=4096       # what the boot sector claims, and ramdisk.c reports
SHIPPED_SECTORS=128     # what is actually kept, and what ramdisk.c serves
SECTOR=512
LABEL=DESKHOP
HEADS=2
SECTORS_PER_TRACK=16
ROOT_SECTORS=32         # mtools -r is in SECTORS, not entries: 32 -> 512 slots,
                        # which is what mkdosfs's default produced and what the
                        # image being replaced had. A quarter of the shipped
                        # disk goes to a directory holding one file; reclaiming
                        # it would change the geometry, so it is left alone here.
CYLINDERS=$(( FULL_SECTORS / (HEADS * SECTORS_PER_TRACK) ))

# 2020-01-01T00:00:00Z. Any fixed value works; this one is only memorable.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1577836800}"

die() { printf '%s\n' "$*" >&2; exit 1; }

command -v mformat >/dev/null || die "no mformat. brew install mtools, or apt-get install mtools"
command -v mcopy   >/dev/null || die "no mcopy. brew install mtools, or apt-get install mtools"
[ -f "$page" ] || die "no config page at $page — run 'make' in webconfig/ first"

work="$(mktemp "${TMPDIR:-/tmp}/deskhop-disk.XXXXXX")"
trap 'rm -f "$work"' EXIT

dd if=/dev/zero of="$work" bs="$SECTOR" count="$FULL_SECTORS" status=none

mformat -t "$CYLINDERS" -h "$HEADS" -s "$SECTORS_PER_TRACK" -r "$ROOT_SECTORS" \
        -c 4 -m 248 -v "$LABEL" -N 0 -i "$work" ::

# mformat stamps its own version into the OEM field — the image built here read
# "MTOO4049" for mtools 4.0.49. That makes the bytes a property of whichever
# mtools built them, so CI (a different version) would regenerate a different
# image and report the committed one stale on every run. Overwrite it with a
# fixed string; nothing reads this field, it is informational.
#
# It was the only version-carrying part: the rest of the boot sector is the
# label, the FAT12 type string and a 43-byte "not bootable" stub with no
# version in it. If the CI freshness gate ever fails on a commit that did not
# touch the config UI, that stub and zlib (see render.py) are the two inputs
# still not pinned — check them before assuming the page is stale.
printf 'DESKHOP ' | dd of="$work" bs=1 seek=3 count=8 conv=notrunc status=none

# Before the copy, not after. The overflowing clusters would land past the
# shipped end, where the firmware answers with zeros — so writing first and
# checking later is checking after the bytes are already gone.
DH_SHIPPED_SECTORS="$SHIPPED_SECTORS" python3 capacity.py "$work" "$page"

mcopy -o -i "$work" "$page" "::/$(basename "$page")"

dd if="$work" of="$image" bs="$SECTOR" count="$SHIPPED_SECTORS" status=none

printf 'wrote %s (%s bytes)\n' "$image" "$(wc -c <"$image" | tr -d ' ')"
