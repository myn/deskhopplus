#!/usr/bin/env python3
"""
deskhopplus #25 / ADR-0001: is the channel's HID node free of TCC?

The transport was chosen because a vendor-defined HID interface needs no
permission grant on macOS. That property is not a property of the usage page
-- it is a property of the *node*. IOHIDDevice::conformsTo iterates a node's
complete usage-pair list, so one keyboard or mouse collection anywhere in the
same USB interface flags the whole node as requiring Input Monitoring,
vendor collections included (docs/research/hid-transport-macos-tcc.md §4).

This script measures that rather than assuming it. For every IOHIDDevice the
device publishes it reports the usage pairs and whether the node carries
RequiresTCCAuthorization, then judges the channel node specifically:

  PASS  a node exists whose usage pairs are vendor-page only, and it does not
        carry RequiresTCCAuthorization
  FAIL  the channel's usage pair shares a node with a keyboard or mouse
        collection, or its node is TCC-flagged

The keyboard and mouse nodes being flagged is expected and harmless -- nothing
opens them. Only the channel node's answer decides the acceptance criterion.

Usage:
    python3 tools/macos-checks/confirm_hid_tcc.py [--vid 0x1209] [--pid 0xc000]

Exit status is 0 only when the channel node is present and unflagged.
"""

import argparse
import plistlib
import subprocess
import sys

# Normal mode identifiers. Config mode reboots under a different identity and
# has no channel interface, so it is not what this script looks for.
DEFAULT_VID = 0x1209
DEFAULT_PID = 0xC000

VENDOR_PAGE_MIN = 0xFF00
CHANNEL_USAGE = 0x20  # TUD_HID_REPORT_DESC_CHANNEL

# The pairs macOS gates behind Input Monitoring. One of these anywhere in a
# node flags the whole node, which is the trap this script exists to detect.
GATED_PAIRS = {
    (0x01, 0x06): "keyboard",
    (0x01, 0x02): "mouse",
    (0x0D, 0x05): "touchpad",
}


def ioreg_nodes():
    """Every IOHIDDevice in the registry, as plist dictionaries."""
    out = subprocess.run(
        ["ioreg", "-a", "-r", "-l", "-c", "IOHIDDevice"],
        capture_output=True,
    )
    if out.returncode != 0 or not out.stdout.strip():
        return []
    return plistlib.loads(out.stdout)


def usage_pairs(node):
    return [
        (p.get("DeviceUsagePage"), p.get("DeviceUsage"))
        for p in node.get("DeviceUsagePairs", [])
    ]


def describe(page, usage):
    if page is None:
        return "?"
    if page >= VENDOR_PAGE_MIN:
        return f"0x{page:04x}/{usage} vendor"
    gated = GATED_PAIRS.get((page, usage))
    return f"{page}/{usage} {gated}" if gated else f"{page}/{usage}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    args = ap.parse_args()

    nodes = [
        n
        for n in ioreg_nodes()
        if n.get("VendorID") == args.vid and n.get("ProductID") == args.pid
    ]

    if not nodes:
        print(f"NOT FOUND  no IOHIDDevice with VID 0x{args.vid:04x} "
              f"PID 0x{args.pid:04x}")
        print("           the device is absent, in config mode, or running "
              "firmware without the channel interface")
        return 2

    serial = next((n.get("SerialNumber") for n in nodes if n.get("SerialNumber")), None)
    product = next((n.get("Product") for n in nodes if n.get("Product")), None)
    print(f"device     {product or '(no product string)'}")
    print(f"serial     {serial or '(none exposed)'}")
    print()

    channel_node = None
    for node in nodes:
        pairs = usage_pairs(node)
        flagged = bool(node.get("RequiresTCCAuthorization", False))
        pretty = ", ".join(describe(p, u) for p, u in pairs) or "(none)"
        print(f"  node     {pretty}")
        print(f"    RequiresTCCAuthorization: {'YES' if flagged else 'no'}")
        if any(p is not None and p >= VENDOR_PAGE_MIN and u == CHANNEL_USAGE
               for p, u in pairs):
            channel_node = (node, pairs, flagged)
    print()

    if channel_node is None:
        print("FAIL       no node publishes the channel usage "
              f"(0xFF00+/{CHANNEL_USAGE})")
        return 1

    _, pairs, flagged = channel_node
    shares_with_gated = [(p, u) for p, u in pairs if (p, u) in GATED_PAIRS]

    if shares_with_gated:
        print("FAIL       the channel shares an interface with "
              f"{describe(*shares_with_gated[0])}")
        print("           macOS gates the whole node; the channel must be its "
              "own USB interface (ADR-0001)")
        return 1

    if flagged:
        print("FAIL       the channel node carries RequiresTCCAuthorization")
        print("           opening it will prompt for Input Monitoring")
        return 1

    print("PASS       the channel is a vendor-only node with no "
          "RequiresTCCAuthorization")
    print("           it can be opened with no permission grant (ADR-0001 "
          "confirmed on this machine)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
