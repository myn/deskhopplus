# deskhopplus

A hard fork of DeskHop: a two-board USB keyboard/mouse switch extended with a 2-D screen layout,
per-direction key remapping, configurable hotkeys, and clipboard sharing via companion helpers.
This glossary is the ubiquitous language; decisions live in `docs/adr/` and the wayfinder maps
(issues #1 and #31).

## Language

### The channel

**Channel**:
The helper↔firmware link — a vendor-defined HID interface (up to three, negotiated) carrying
clipboard payloads and cursor placement.
_Avoid_: serial port, CDC, pipe

**Helper**:
The companion application on each computer (Swift on macOS, C++ on Windows) that reads and writes
the pasteboard and places the cursor. An enhancement, never a dependency.
_Avoid_: agent, daemon, client, companion app

**Opaque relay**:
The firmware's role on the channel: it parses frame headers only — never payloads — and forwards
fragments between the USB and inter-board links. Clipboard format changes never touch firmware.
_Avoid_: proxy, gateway

**Fidelity**:
The clipboard channel's binding content guarantee: the wire payload is byte-identical end to end
(CRC32-verified), and no component validates, filters, normalizes, or rejects content. The only
transform anywhere is the platform's native pasteboard encoding conversion at each edge.
_Avoid_: sanitization, validation, verbatim (informal — this is the canonical term)

### Clipboard behaviour

**Copy side / Paste side**:
The machine where content was copied, and the machine where it is pasted. Confirmation prompts
and lazy transfers belong to the paste side.
_Avoid_: source/destination machine, sender/receiver

**Eager / Lazy**:
When bytes move: eager content transfers on copy (text, small images); lazy content transfers on
paste (files, large payloads).
_Avoid_: push/pull, immediate/deferred

**Chunk**:
The unit of integrity, loss detection, and selective retransmission — exactly one frame's payload,
carrying id, length, and CRC32.
_Avoid_: block, segment, packet (a packet is the 12-byte inter-board wire unit)

### Layout

**Seam**:
The configured boundary between the two computers' screen areas, crossed by pushing the cursor
into it. Mapped as ranges keyed by chain index.
_Avoid_: border (except in "border direction"), edge

**Chain axis / Border direction**:
The two independent per-output facts replacing DeskHop's single `pos`: which way a computer's own
monitors extend, and where the other computer is.
_Avoid_: orientation, position
