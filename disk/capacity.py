#!/usr/bin/env python3
"""How much of the shipped config disk the page uses, and whether it fits.

The volume is deliberately a lie. `mformat` lays out a 2 MB FAT12 filesystem
and only its first 64 kB is kept, because that is all the firmware stores:
`src/ramdisk.c` reports NUMBER_OF_BLOCKS (4096) to the host and serves zeros
past ACTUAL_NUMBER_OF_BLOCKS (128). A page that overflows those 128 sectors
therefore does not fail — the host writes it, the FAT says it is there, and
the bytes are simply gone. That is the truncation this check exists to turn
into a build failure.

Capacity is read out of the image's own boot sector rather than hardcoded, so
changing the geometry cannot leave a stale number here. The shipped size is
the image file's own length, which is the same thing said a third way.

    usage: capacity.py <image> [page]

Exits 1 if the page does not fit, 0 otherwise. With no page argument it only
reports.

DH_SHIPPED_SECTORS overrides the length taken from the file. create.sh needs
that: it has to know whether the page fits *before* copying it in, and at that
point the image is still the full 2 MB that mformat laid out. Asking after the
truncation would be asking after the bytes were already lost.
"""

import os
import struct
import sys

SECTOR = 512


def geometry(image):
    """Data-region start, cluster size and shipped length, from the boot sector."""
    with open(image, "rb") as fh:
        boot = fh.read(SECTOR)

    bytes_per_sector, sectors_per_cluster = struct.unpack("<HB", boot[11:14])
    reserved, num_fats, root_entries = struct.unpack("<HBH", boot[14:19])
    sectors_per_fat = struct.unpack("<H", boot[22:24])[0]

    if bytes_per_sector == 0 or sectors_per_cluster == 0:
        sys.exit(f"{image}: not a FAT boot sector")

    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    data_start = reserved + num_fats * sectors_per_fat + root_sectors

    override = os.environ.get("DH_SHIPPED_SECTORS")
    shipped = int(override) if override else os.path.getsize(image) // bytes_per_sector

    return {
        "bytes_per_sector": bytes_per_sector,
        "cluster_bytes": sectors_per_cluster * bytes_per_sector,
        "data_start": data_start,
        "shipped_sectors": shipped,
    }


def capacity(geo):
    """Bytes a file can occupy without any cluster falling past the shipped end."""
    data_sectors = geo["shipped_sectors"] - geo["data_start"]
    if data_sectors <= 0:
        return 0

    sectors_per_cluster = geo["cluster_bytes"] // geo["bytes_per_sector"]
    return (data_sectors // sectors_per_cluster) * geo["cluster_bytes"]


def main(argv):
    if not 2 <= len(argv) <= 3:
        sys.exit(__doc__)

    image = argv[1]
    geo = geometry(image)
    limit = capacity(geo)

    if len(argv) == 2:
        print(f"config disk: {limit} bytes usable in {geo['shipped_sectors']} shipped sectors")
        return 0

    page = argv[2]
    used = os.path.getsize(page)
    pct = 100.0 * used / limit if limit else 0.0

    # A file occupies whole clusters, so the last one is charged in full.
    clusters = (used + geo["cluster_bytes"] - 1) // geo["cluster_bytes"]
    allocated = clusters * geo["cluster_bytes"]

    print(
        f"config page: {used} of {limit} bytes ({pct:.1f}%), "
        f"{allocated} allocated in {clusters} clusters of {geo['cluster_bytes']}"
    )

    if allocated > limit:
        sys.exit(
            f"\n{page} does not fit the config disk.\n"
            f"  page      {used} bytes, {allocated} once rounded up to whole clusters\n"
            f"  capacity  {limit} bytes\n"
            f"  over by   {allocated - limit} bytes\n\n"
            "The image would be written and the overflowing clusters silently dropped,\n"
            "because only the first 64 kB of the volume is shipped. Shrink the page."
        )

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
