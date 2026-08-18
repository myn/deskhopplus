#!/usr/bin/env python3
"""Where two disk images differ, as byte ranges.

Diagnostic only. The config disk is not byte-reproducible across mtools
versions — mformat writes version-dependent content into the boot sector, and
overwriting the OEM field was not enough on its own — so CI checks that the
image *carries the current page* rather than that its bytes match. This says
what did differ, so if reproducible bytes are ever wanted again the fields to
normalise are named rather than guessed at.

    usage: imgdiff.py <a> <b>

Always exits 0: a difference is information here, not a failure.
"""

import sys


def runs_of_difference(a, b):
    runs, start = [], None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i - 1))
            start = None
    if start is not None:
        runs.append((start, min(len(a), len(b)) - 1))
    return runs


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)

    a = open(argv[1], "rb").read()
    b = open(argv[2], "rb").read()

    if len(a) != len(b):
        print(f"sizes differ: {len(a)} vs {len(b)}")

    runs = runs_of_difference(a, b)
    if not runs:
        print("byte-identical")
        return 0

    total = sum(hi - lo + 1 for lo, hi in runs)
    print(f"{total} bytes differ in {len(runs)} run(s)")
    for lo, hi in runs[:12]:
        print(f"  {lo:#06x}-{hi:#06x}  a={a[lo:hi + 1][:16].hex()}  b={b[lo:hi + 1][:16].hex()}")
    if len(runs) > 12:
        print(f"  ... and {len(runs) - 12} more")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
