#!/usr/bin/python3

from dataclasses import dataclass, field
from pathlib import Path
import re

@dataclass
class FormField:
    offset: int
    name: str
    default: int | None = None
    values: dict[int, str] = field(default_factory=dict)
    data_type: str = "int32"
    elem: str | None = None
    action: int | None = None

SHORTCUTS = {
    0x73: "None",
    0x2A: "Backspace",
    0x39: "Caps Lock",
    0x2B: "Tab",
    0x46: "Print Screen",
    0x47: "Scroll Lock",
    0x53: "Num Lock",
    }

STATUS_ = [
    # "This board" and "other board" rather than A and B: which one you are
    # looking at depends on which computer the browser is running on (#89).
    FormField(78, "This board FW version", None, {}, "uint16", elem="fw_version"),
    FormField(84, "Other board FW version", None, {}, "uint16", elem="peer_fw_version"),
    FormField(79, "Running FW checksum", None, {}, "uint32", elem="hex_info"),
    # 85 against 79 is the propagation check that survives #91: at equal
    # versions the checksums are the only thing that differs, so 84 alone
    # cannot tell a synced pair from an unsynced one.
    FormField(85, "Other board FW checksum", None, {}, "uint32", elem="peer_fw_checksum"),
    FormField(83, "Build", None, {}, "uint8", elem="dev_build"),
    # The helper this board is paired with, as the key id its hellos carry
    # (#114). The flag is the field the user sees; the two halves of the key id
    # are hidden and the page joins them, because one field carries at most
    # seven bytes and a key id is eight.
    FormField(86, "", None, {}, "uint32", elem="helper_key_lo"),
    FormField(87, "", None, {}, "uint32", elem="helper_key_hi"),
    FormField(88, "Paired helper", None, {}, "uint8", elem="helper_paired"),

    # What this board has dropped on the helper channel since boot (#52). Every
    # one was already counted; none was readable, which made "counted rather
    # than silently dropped" untrue in practice. Each names a different seam,
    # and each seam has a different remedy — so they are four fields and not a
    # total.
    #
    # The heading says "since this boot" because this page cannot say anything
    # else: it is reachable only in config mode, config mode is entered by
    # rebooting the board, and the counters live in RAM. So a row of zeros here
    # is what a board that just started always shows, and it is not evidence
    # that the seams are clean — it was read as evidence three times on #132
    # before #133 named it. The live reading goes to the helper over the
    # channel and lands in its log.
    FormField(1005, "Channel drops (since this boot)", elem="label"),
    FormField(91, "Reports not taken", None, {}, "uint32", elem="hex_info"),
    FormField(92, "From peer board", None, {}, "uint32", elem="hex_info"),
    FormField(93, "Outbound refused", None, {}, "uint32", elem="hex_info"),
    FormField(94, "Inter-board refused", None, {}, "uint32", elem="hex_info"),
    FormField(95, "Peer orphan packets", None, {}, "uint32", elem="hex_info"),
    FormField(96, "Peer frames truncated", None, {}, "uint32", elem="hex_info"),
    FormField(97, "Relay queue refused", None, {}, "uint32", elem="hex_info"),
]

CONFIG_ = [
    FormField(1001, "Mouse", elem="label"),
    FormField(71, "Force Mouse Boot Mode", None, {}, "uint8", "checkbox"),
    FormField(75, "Enable Acceleration", None, {}, "uint8", "checkbox"),
    FormField(77, "Jump Threshold", 0, {"min": 0, "max": 3000}, "uint16", "range"),

    FormField(1002, "Keyboard", elem="label"),
    FormField(72, "Force KBD Boot Protocol", None, {}, "uint8", "checkbox"),
    FormField(73, "KBD LED as Indicator", None, {}, "uint8", "checkbox"),

    FormField(76, "Enforce Ports", None, {}, "uint8", "checkbox"),

    # Clipboard sharing, one toggle per direction (#52). Named for the *block*
    # rather than the permission because that is how the board stores them —
    # zero means allowed, which is what let the two bytes land in existing
    # padding instead of costing every user their settings and their pairing
    # (config_layout.h). Showing them the other way round would put an
    # inversion between what the page says and what the board holds, which is
    # one more place for the two to drift.
    FormField(1004, "Clipboard", elem="label"),
    FormField(89, "Block clipboard A to B", None, {}, "uint8", "checkbox"),
    FormField(90, "Block clipboard B to A", None, {}, "uint8", "checkbox"),

    FormField(1006, "Hotkeys", elem="label"),
]

_CATALOG = (Path(__file__).parent.parent / "src/core/dh_hotkey_actions.h").read_text()
HOTKEY_FIELD_BASE = int(re.search(r"DH_HOTKEY_CONFIG_FIELD_BASE\s+(\d+)", _CATALOG).group(1))
HOTKEY_NAMES = re.findall(r"X\([^,]+,\s*([^\)]+)\)", _CATALOG)
CONFIG_.extend(FormField(HOTKEY_FIELD_BASE + 2 * action, name.strip(), elem="hotkey", action=action)
               for action, name in enumerate(HOTKEY_NAMES))

OUTPUT_ = [
    FormField(1, "Screen Count", 1, {1: "1", 2: "2", 3: "3"}, "uint32"),
    FormField(2, "Speed X", 16, {"min": 1, "max": 100}, "int32", "range"),
    FormField(3, "Speed Y", 16, {"min": 1, "max": 100}, "int32", "range"),
    FormField(4, "Border Top", None, {}, "int32"),
    FormField(5, "Border Bottom", None, {}, "int32"),
    FormField(6, "Operating System", 1, {1: "Linux", 2: "MacOS", 3: "Windows", 4: "Android", 255: "Other"}, "uint8"),
    FormField(7, "Screen Position", 1, {1: "Left", 2: "Right"}, "uint8"),
    FormField(8, "Cursor Park Position", 0, {0: "Top", 1: "Bottom", 3: "Previous"}, "uint8"),
    FormField(1003, "Screensaver", elem="label"),
    FormField(9, "Mode", 0, {0: "Disabled", 1: "Pong", 2: "Jitter"}, "uint8"),
    FormField(10, "Only If Inactive", None, {}, "uint8", "checkbox"),
    FormField(11, "Idle Time (μs)", None, {}, "uint64"),
    FormField(12, "Max Time (μs)", None, {}, "uint64"),
]

def generate_output(base, data):
    output = [
        {
            "name": field.name,
            "key": base + field.offset,
            "default": field.default,
            "values": field.values,
            "type": field.data_type,
            "elem": field.elem,
            "action": field.action,
        }
        for field in data
    ]
    return output

def output_A(base=10):
    return generate_output(base, data=OUTPUT_)

def output_B(base=40):
    return generate_output(base, data=OUTPUT_)

def output_status():
    return generate_output(0, data=STATUS_)

def output_config():
    return generate_output(0, data=CONFIG_)
