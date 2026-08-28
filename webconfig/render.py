#!/usr/bin/python3

# Takes a HTML file, outputs a minified and compressed version that self-decompresses when loaded.
# This way, the device config page can be fitted in a small 64 kB "flash" partition and distributed
# with the main binary.

from jinja2 import Environment, FileSystemLoader
from form import *
import base64
import zlib
import json
import re
from pathlib import Path

# Input and output
TEMPLATE_PATH = "templates/"
INPUT_FILENAME = "main.html"
PACKER_FILENAME = "packer.j2"
OUTPUT_FILENAME = "config.htm"
OUTPUT_UNPACKED = "config-unpacked.htm"

def render(filename, *args, **kwargs):
    env = Environment(loader=FileSystemLoader(TEMPLATE_PATH))
    template = env.get_template(filename)
    return template.render(*args, **kwargs)


def write_file(payload, filename=OUTPUT_FILENAME):
    with open(filename, 'w', encoding='utf-8') as file:
        file.write(payload)


def encode_file(payload):
    # Compress using raw DEFLATE. The level is pinned rather than left to
    # Z_DEFAULT_COMPRESSION so the output does not move if that default ever
    # changes: CI compares the rendered page against the committed one, and a
    # shift here would read as a stale page nobody touched. 6 is the current
    # default, so pinning it changes no bytes today.
    compressed_data = zlib.compress(payload.encode('utf-8'), 6)[2:-4]

    # Encode to base64
    base64_compressed_data = base64.b64encode(compressed_data).decode('utf-8')

    return base64_compressed_data


if __name__ == "__main__":
    config_text_source = (Path(__file__).parent.parent / "src/core/dh_config_text.c").read_text()
    named_keys = {name: int(usage, 16) for name, usage in
                  re.findall(r'\{"([^"]+)",\s*(0x[0-9a-f]+)\}', config_text_source)}
    config_text_header = (Path(__file__).parent.parent / "src/core/dh_config_text.h").read_text()
    chord_capacity = int(re.search(r"DH_CONFIG_TEXT_CHORD_CAPACITY\s+(\d+)u", config_text_header).group(1))
    override_capacity = int(re.search(r"DH_CONFIG_TEXT_OVERRIDE_CAPACITY\s+(\d+)u", config_text_header).group(1))
    passthrough_capacity = int(re.search(r"DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY\s+(\d+)u", config_text_header).group(1))
    keymap_header = (Path(__file__).parent.parent / "src/core/dh_keymap.h").read_text()
    keymap_value = lambda name: int(re.search(rf"{name}\s+(\d+)u", keymap_header).group(1))
    # Read main template contents
    webpage = render(
        INPUT_FILENAME,
        screen_A=output_A(),
        screen_B=output_B(),
        status=output_status(),
        config=output_config(),
        hotkey_field_base=HOTKEY_FIELD_BASE,
        hotkey_count=len(HOTKEY_NAMES),
        hotkey_last_field=HOTKEY_FIELD_BASE + 2 * len(HOTKEY_NAMES),
        named_keys_json=json.dumps(named_keys, separators=(",", ":")),
        chord_capacity=chord_capacity,
        override_capacity=override_capacity,
        passthrough_capacity=passthrough_capacity,
        keymap_profile_size=keymap_value("DH_KEYMAP_PROFILE_SIZE"),
        keymap_chunk_size=keymap_value("DH_KEYMAP_CONFIG_CHUNK_SIZE"),
        keymap_chunk_count=keymap_value("DH_KEYMAP_CONFIG_CHUNK_COUNT"),
        keymap_field_bases=[keymap_value("DH_KEYMAP_CONFIG_FIELD_A_BASE"), keymap_value("DH_KEYMAP_CONFIG_FIELD_B_BASE")],
        keymap_override_count_offset=keymap_value("DH_KEYMAP_OVERRIDE_COUNT_OFFSET"),
        keymap_passthrough_offset=keymap_value("DH_KEYMAP_PASSTHROUGH_OFFSET"),
        keymap_passthrough_count_offset=keymap_value("DH_KEYMAP_PASSTHROUGH_COUNT_OFFSET"),
    )
    # Jinja preserves indentation on control-only lines. Keep the generated
    # artifact compliant with the repository's no-trailing-whitespace rule.
    webpage = "\n".join(line.rstrip() for line in webpage.split("\n"))

    # Compress file and encode to base64
    encoded_data = {'payload': encode_file(webpage)}

    # Tiny Inflate JS decoder (https://github.com/foliojs/tiny-inflate)
    # Decompress the data and replace existing HTML with the decoded version
    self_extracting_webpage = render(PACKER_FILENAME, encoded_data)

    # Write data to output filename
    write_file(self_extracting_webpage)

    # Write unpacked webpage
    write_file(webpage, OUTPUT_UNPACKED)
