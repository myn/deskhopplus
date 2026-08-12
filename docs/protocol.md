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

### The report carrier

The channel's HID reports are a fixed 64 bytes with no report ID and no length field of their
own, so the framing layer owns every byte of a report. Frames are packed into that byte stream
back to back, and the tail of the last report of a batch is filled with **`0x00`**.

`0x00` is not a message type — the registry starts at `0x01` — so it cannot begin a frame. A
decoder skips it **between** frames and nowhere else: inside a frame it is ordinary payload,
accounted for by the length the header already gave. An all-padding report is idle traffic and
means nothing.

This is a property of a fixed-size carrier, not of the framing: it costs no golden vector, and a
carrier that already delimits its own records (the inter-board link's packets, a CDC stream)
never emits it.

## Bands

Ids are banded so the firmware's two decisions are each **a single range test** on the
type byte, without reading a payload:

- **Priority:** `type < 0x30` → priority queue, drains strictly before bulk.
- **Routing:** `type >= 0x30` → bulk, relayed opaquely to the peer helper; everything
  below is addressed to (or emitted by) the firmware and is never forwarded.

| band | range | contents |
|------|-------|----------|
| session | 0x01–0x1F | hello, heartbeats, session end, pairing |
| placement | 0x20–0x2F | placement, position query/response |
| bulk | 0x30–0x3F | clipboard transfer and its reliability machinery |

## Message types

| type | name              | dir | payload |
|------|-------------------|-----|---------|
| 0x01 | HELLO             | h→d | `proto_version:u16` `os:u8` (1=mac, 2=windows) `build_type:u8` (0=release, 1=development) `channel_count:u8` (requested) `max_chunk:u16` (requested, bytes) `token:bytes` (authentication; format owned by #46 — example vectors use 16 bytes) |
| 0x02 | HELLO_ACK         | d→h | `proto_version:u16` `status:u8` (0=ok, 1=auth_failed, 2=version_incompatible — distinguishable because the remedies differ) `build_type:u8` `channel_count:u8` (effective) `max_chunk:u16` (effective). On a non-ok status the effective fields are zero. |
| 0x05 | HEARTBEAT         | h→d | empty (id kept from mkroamer; vector ports verbatim) |
| 0x06 | DEVICE_HEARTBEAT  | d→h | empty. The device's own beat, sent only while a session exists, so its absence is meaningful. Idle-gated — see Liveness. |
| 0x07 | SESSION_END       | d→h | `reason:u8` (0=unspecified, 1=liveness_timeout, 2=protocol_error). An unknown reason reads as unspecified rather than as an error, so a later device may end a session for a reason this helper predates. |
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
| 0x36 | CLIP_CREDIT       | h↔h | `id:u32` `credits:u16` — the id makes a grant for a superseded transfer recognisably stale (#48) |

`dir`: h→d helper to device, d→h device to helper, h↔h helper to helper (relayed opaquely
by the firmware, which parses frame headers only, never payloads).

**Dropped from mkroamer:** PING/PONG, MOUSE_MOVE, MOUSE_BTN, WHEEL, KEY (input rides HID),
HANDOFF/HANDOFF_ACK (inverted into PLACE), RELEASE_CONTROL, RESET_MODIFIERS,
CONFIG_SYNC/CONFIG_ACK (configuration lives on the device).

## Liveness

Per [ADR-0004](adr/0004-independent-bidirectional-liveness.md). Liveness is **symmetric and
independently timed in each direction**, and it is carried by **traffic**, not by an
acknowledgement. There is no request/response pair here: each end runs its own timer over what
arrives.

- **Any frame proves the sender is alive, in both directions.** An end treats its peer as present
  while anything at all has arrived from it within three heartbeat intervals — hello, placement,
  clipboard bulk, a refused hello, a heartbeat. Nothing is excluded, and an implementation that
  credits only heartbeats is wrong: the *other* end suppresses its beat whenever it has real
  traffic to send, so counting only beats evicts a peer in the middle of its own transfer. The
  channel is held exclusively, so every frame on it comes from the one process that owns the
  session; a process that is writing is alive, which is the only thing this deadline measures.
- **Being alive and holding a session are different claims.** In the device→helper direction
  arrival proves both, since the device relays and answers nothing for a peer it has no session
  with. In the helper→device direction it proves only the first — which is all that is needed,
  because the device is the end that owns the session state and does not need to be told.
- **A heartbeat fills an idle direction only.** HEARTBEAT and DEVICE_HEARTBEAT are sent only when
  that direction has carried nothing for a full interval. A busy link emits neither. They exist so
  that silence is unambiguous, not to be the measurement.
- **Why idle-gated rather than unconditional.** The device holds one outbound frame slot, shared
  with relayed bulk, and a refused queue is a silent loss. An unconditional beat would be starved
  by a sustained transfer, and a few starved beats in a row would look exactly like a dead session
  — the mechanism would manufacture the failure it exists to detect. Gating on idleness removes
  that: a busy direction emits no beat, and a beat refused by a busy slot is self-correcting,
  because whatever occupied the slot refreshes the peer anyway.
- **SESSION_END is an optimisation, never the mechanism.** The device announces an eviction it
  knows about so the helper need not wait out a timeout. A device that reboots, wedges, or loses
  power announces nothing, so the timeout above is what must be correct.
- **On detection, drop and reconnect.** The peer is no longer trustworthy and the byte stream may
  be mid-frame, so recovery is closing and reopening the channels, not re-introducing over them.

## Transfer semantics

The chunked transfer state machine (#48) lives in the shared core and runs **end-to-end
between the helpers**; the firmware relays its messages opaquely.

- **Chunking.** A payload divides into chunks of `DH_XFER_CHUNK_SIZE` (1024 bytes — a build
  constant until [#39](https://github.com/myn/deskhopplus/issues/39) measures; the hello
  negotiates the effective value). Every chunk except the last is exactly that size, so a
  chunk's offset is `seq × chunk_size` and reassembly needs no bookkeeping beyond a
  received-set. A chunk is exactly one CLIP_CHUNK frame's payload.
- **Streaming starts on request, never before.** An offered transfer emits nothing until
  CLIP_REQUEST arrives — a lazy payload (files) is not even read until then.
- **Integrity and loss.** The paste side verifies each chunk's CRC32 and tracks received
  seqs. A corrupt chunk, a skipped seq, or a gap found when CLIP_DONE arrives produces
  CLIP_RETRANSMIT for exactly the missing chunks. After retransmitting, the sender repeats
  CLIP_DONE. A loss is reported once per round: a DONE sweep leaves a freshly requested
  chunk alone once — its retransmission is behind that DONE in the FIFO — **but a chunk
  still missing a full round later is requested again**, so a retransmitted chunk that is
  itself lost converges on the next DONE round. Only the loss of a message with no DONE
  behind it (the final DONE, a lone request) is left to the helper's transfer timeout.
- **The sender retains its payload after CLIP_DONE** — retransmit requests may still
  arrive. It is released when the transfer is superseded by a newer offer, cancelled, or
  the link drops. There is no completion acknowledgement in v1: CLIP_DONE always travels
  sender→receiver, which keeps it unambiguous when both sides transfer at once.
- **Flow control.** The sender spends one credit per chunk sent (retransmits included) and
  stops at zero; CLIP_DONE is not gated. The paste side grants `DH_XFER_CREDIT_WINDOW`
  (16 chunks) with its CLIP_REQUEST, replenishes in half-window batches as chunks arrive,
  and **every CLIP_RETRANSMIT is accompanied by a covering credit grant** — a lost or
  corrupt chunk consumed the sender's credit without ever being counted on the paste side,
  and without the covering grant sustained loss would drain the window permanently. (When
  the lost message was the *request* rather than the chunk, the covering grant mildly
  inflates the window — bounded and harmless, where draining is fatal.) **The window is
  per transfer**; a direction carries one transfer at a time, so this is per-direction
  accounting in practice, and grants carry the transfer id so a superseded transfer's
  grants are ignored rather than credited to its successor. What is protected globally —
  the shared inter-board queue behind all channels — is the egress board's burst cap
  (#47) plus this window.
- **Failure is abandonment.** A link drop mid-transfer abandons both directions: the
  paste side discards its partial payload — never delivering it as complete — and its
  helper deletes any partial file and reports the failure. No resumption.
- **Supersede.** A newer offer replaces an incomplete transfer in either direction; stale
  messages for the old id are ignored.

## Golden vectors

`test-vectors/frames.txt` is the cross-implementation gate: the shared C core (and every
binding of it) must decode each vector and re-encode it byte-identically. mkroamer's
vectors are ported verbatim where the message survives unchanged (HEARTBEAT, CLIP_OFFER,
CLIP_REQUEST, CLIP_DONE, CLIP_CANCEL). Any protocol change updates this document, the
vectors, and the core in the same change.
