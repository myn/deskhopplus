# deskhopplus channel protocol — v1

The single source of truth for bytes on the helper↔firmware channel. Any change here must
update `test-vectors/frames.txt` and the shared C core (`src/core/`) in the same change.

Derived from mkroamer's frozen v1.1 protocol, re-cast for the asymmetric device-mediated
link per [#37](https://github.com/myn/deskhopplus/issues/37) and the channel spec
[#42](https://github.com/myn/deskhopplus/issues/42). The framing is unchanged; the message
set is re-cast: the `CLIP_*` family survives, placement inverts (the firmware is the single
authority, so the epoch goes), input and config-sync messages are dropped, and session,
pairing, position, and reliability messages are added.

## Assumptions

The transport (vendor HID channels per [ADR-0001](adr/0001-vendor-hid-transport.md) /
[ADR-0002](adr/0002-parallel-hid-channels.md), striped over the inter-board link) delivers
an **ordered byte stream per direction**. Loss and integrity are handled *above* framing:
the chunk CRC32 and selective retransmission live in the `CLIP_*` messages, end-to-end
between the helpers. The framing layer is transport-neutral and must stay that way — the
golden vectors survived the CDC→HID move untouched, and a vector that depends on the
transport is a sign the framing layer has leaked.

## Framing

All integers little-endian.

```
offset  size  field
0       1     type    (message type, below)
1       1     flags   (reserved, 0 in v1; preserved verbatim by codecs)
2       2     len     (payload length in bytes, u16; max 4096)
4       len   payload
```

A frame with `len > 4096` or an unknown `type` is a protocol error: log, drop the
connection, reconnect.

## Bands

Ids are banded so the firmware's two decisions are each **a single range test** on the
type byte, without reading a payload:

- **Priority:** `type < 0x30` → priority queue, drains strictly before bulk.
- **Routing:** `type >= 0x30` → bulk, relayed opaquely to the peer helper; everything
  below is addressed to (or emitted by) the firmware and is never forwarded.

| band | range | contents |
|------|-------|----------|
| session | 0x01–0x1F | hello, heartbeat, pairing |
| placement | 0x20–0x2F | placement, position query/response |
| bulk | 0x30–0x3F | clipboard transfer and its reliability machinery |

## Message types

| type | name              | dir | payload |
|------|-------------------|-----|---------|
| 0x01 | HELLO             | h→d | `proto_version:u16` `os:u8` (1=mac, 2=windows) `build_type:u8` (0=release, 1=development) `channel_count:u8` (requested) `max_chunk:u16` (requested, bytes) `token:bytes` (authentication; format owned by #46 — example vectors use 16 bytes) |
| 0x02 | HELLO_ACK         | d→h | `proto_version:u16` `status:u8` (0=ok, 1=auth_failed, 2=version_incompatible — distinguishable because the remedies differ) `build_type:u8` `channel_count:u8` (effective) `max_chunk:u16` (effective). On a non-ok status the effective fields are zero. |
| 0x05 | HEARTBEAT         | h→d | empty (id kept from mkroamer; vector ports verbatim) |
| 0x08 | PAIR_REQUEST      | h→d | empty (semantics owned by #46) |
| 0x09 | PAIR_GRANT        | d→h | `secret:bytes` (length and rotation owned by #46; example vectors use 16 bytes) |
| 0x20 | PLACE             | d→h | `chain_index:u8` `border_direction:u8` (which side of the output the seam was crossed from) `entry_pos:u16` (0–65535 normalized along the seam segment). Fire-and-forget; no reply path. mkroamer's HANDOFF minus epoch and modifiers — one arbiter needs no epoch, and input rides HID. |
| 0x21 | POS_QUERY         | d→h | empty |
| 0x22 | POS_RESPONSE      | h→d | `chain_index:u8` `x:u16` `y:u16` (0–65535 normalized within that output; layout may be amended by #51/#53 with vectors in the same change) |
| 0x30 | CLIP_OFFER        | h↔h | `id:u32` `kind:u8` (0=utf8-text, 1=png, 2=file-list) `total_size:u64` `meta_len:u16` `meta:bytes` (kind 2: UTF-8 JSON array of `{name,size}`) — unchanged from mkroamer |
| 0x31 | CLIP_REQUEST      | h↔h | `id:u32` — unchanged from mkroamer |
| 0x32 | CLIP_CHUNK        | h↔h | `id:u32` `seq:u32` `crc32:u32` (CRC32 of `data`, the end-to-end integrity check) `data:bytes`. A chunk is exactly one frame's payload; the chunk length is the frame `len` minus this 12-byte header. mkroamer's layout plus the CRC32 the reliability model added. |
| 0x33 | CLIP_DONE         | h↔h | `id:u32` — unchanged from mkroamer |
| 0x34 | CLIP_CANCEL       | h↔h | `id:u32` — unchanged from mkroamer |
| 0x35 | CLIP_RETRANSMIT   | h↔h | `id:u32` `seq:u32` — request selective retransmission of one chunk |
| 0x36 | CLIP_CREDIT       | h↔h | `credits:u16` (global window across all channels; semantics owned by #48) |

`dir`: h→d helper to device, d→h device to helper, h↔h helper to helper (relayed opaquely
by the firmware, which parses frame headers only, never payloads).

**Dropped from mkroamer:** PING/PONG, MOUSE_MOVE, MOUSE_BTN, WHEEL, KEY (input rides HID),
HANDOFF/HANDOFF_ACK (inverted into PLACE), RELEASE_CONTROL, RESET_MODIFIERS,
CONFIG_SYNC/CONFIG_ACK (configuration lives on the device).

## Golden vectors

`test-vectors/frames.txt` is the cross-implementation gate: the shared C core (and every
binding of it) must decode each vector and re-encode it byte-identically. mkroamer's
vectors are ported verbatim where the message survives unchanged (HEARTBEAT, CLIP_OFFER,
CLIP_REQUEST, CLIP_DONE, CLIP_CANCEL). Any protocol change updates this document, the
vectors, and the core in the same change.
