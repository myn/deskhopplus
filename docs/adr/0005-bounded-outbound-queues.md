# ADR-0005: Bounded outbound queues on the device's two outbound seams

- **Status:** Accepted
- **Date:** 2026-08-12
- **Arising from:** [#69](https://github.com/myn/deskhopplus/issues/69)

## Decision

Both of the device's single-frame outbound seams — the slot feeding this board's helper
(`channel.out`) and the bulk slot feeding the peer board (`dh_relay_tx`'s bulk slot) — grow a small
bounded queue: the existing single slot stays as the head, sized `DH_FRAME_MAX_SIZE` so any legal
bulk frame is still accepted, and **two** slots of `DH_OUTQ_STAGE_MAX` (1044B) queue behind it —
sized to the largest bulk frame a transfer can actually *complete* with, which is a full-metadata
`CLIP_OFFER` (1043B) rather than a `CLIP_CHUNK` (1040B). `channel.out` additionally splits into a
priority slot (session replies, single-buffered as today) and this bulk queue, mirroring the
priority/bulk split `dh_relay_tx` already has — so a burst of relayed bulk can never delay a
session reply.

The queueing logic is a shared primitive in `src/core/` (host-tested), not written twice and not
written inside `channel.c`.

## Context

A frame arriving for either outbound slot while it is occupied was refused outright, with no
retransmit beneath that layer — silent data loss under completely ordinary load, since the
inter-board link (3.6 Mbaud) outruns the 64 ms USB drain of a 4 KiB frame. #69 lists three options:
enforce the CLIP_CREDIT window on the device, a small outbound queue, or refuse to consume at the
reader until the slot frees.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Enforce CLIP_CREDIT on the device** | `credits`/`id` are payload fields. The firmware parses frame headers only, never payloads — the opaque-relay principle (CONTEXT.md, reaffirmed in ADR-0003) and the credit window's own design (ADR-0002: end-to-end between helpers, protecting *their* memory) both rule this out. Not just expensive — out of bounds. |
| **Refuse at the reader** | The reader differs by path, and both are worse than they look. Path (b)'s reader is the vendor HID OUT endpoint, which at the shipped `N=1` channel count (ADR-0002) also carries session/control traffic — refusing to re-arm it would stall session frames behind bulk, the exact priority-inversion ADR-0004's idle-gated heartbeat exists to avoid. Path (a)'s reader is the inter-board UART, shared with keyboard/mouse packets for the *other* computer — refusing to drain it stalls those too. |

## Consequences

- Slot sizing is heterogeneous by design: the head slot stays full-size so that no legal frame is
  ever un-carryable, while the queued slots behind it are bounded by the largest frame a transfer
  can complete with, to keep the RAM cost down against the ~68 KB SRAM headroom the current build
  leaves free. The measured cost is +4,088 bytes of SRAM (`data`+`bss`), leaving ~62 KB free.
  ([#135](https://github.com/myn/deskhopplus/issues/135) later raised the queued-slot bound from
  1068 to 1095 bytes, because the figure had been stated for the *clear* chunk and offer while
  the clipboard had begun sealing them — so a full-size chunk could never queue. That adds
  `DH_OUTQ_DEPTH × 27` = 54 bytes to the number above; it has not been re-measured.)
- **Not every refused frame is equally recoverable, and that sets the queued-slot size.** A refused
  `CLIP_CHUNK` is re-requested by the receiving helper's chunk accounting. A refused `CLIP_OFFER`
  has no retransmit behind it at all — protocol.md leaves the loss of a message with no `CLIP_DONE`
  behind it to the helper's transfer timeout — so it costs the whole transfer. Sizing the queued
  slots to a chunk alone would therefore have left the *expensive* refusal the only one the queue
  could not absorb; they are sized to hold a full-metadata offer instead, which costs 16 bytes in
  total. Beyond that bound an offer is one the receiver cancels on arrival (`DH_XFER_META_MAX`), so
  there is no viable frame left that cannot queue.
- A frame that still cannot be carried once the queue is full (a genuinely sustained overrun beyond
  three frames in flight) is not given a new firmware-level signal. For the bulk traffic that
  dominates, the existing end-to-end `CLIP_RETRANSMIT` machinery between helpers already treats a
  missing chunk as loss and re-requests it — that machinery is the "something that can act on it,"
  not a device invention.
- Because the primitive is shared, `dh_relay_tx`'s bulk slot and a new path-(a) module both drain
  through the same host-tested code, rather than two divergent implementations of the same shape.

## Amendment, 2026-08-25 — the band split reorders, and the counter had to learn it

Measured on hardware during [#52](https://github.com/myn/deskhopplus/issues/52)'s first run:
every clipboard transfer stalled, in both directions, while the session itself stayed healthy.
The Windows helper log named the cause without ambiguity — *dropping a clip_offer with a counter
already seen*.

The band split is what produced it, and it was working as designed. A priority frame overtakes
bulk that is merely queued; the board tags every frame when it is **built**, not when it leaves.
So a relayed clipboard frame tagged at N reaches the wire behind a heartbeat tagged at N+1, and
[ADR-0008](0008-channel-identity-and-sealed-clipboard.md)'s receiver refused anything not
strictly greater.

Two decisions, each right on its own, that could not both hold. Neither could fire before: until
a payload existed, nothing was ever *queued* behind anything, so the reorder never happened.
[#96](https://github.com/myn/deskhopplus/issues/96) said this code had never met sustained load
and named itself the least-exercised change in the 0.89 firmware. It was right.

**Resolved in ADR-0008's half, not this one.** The receiver now keeps a 64-counter replay window
rather than a high-water mark — the shape IPsec and DTLS use for exactly this. Replay protection
is unchanged: a counter accepted once is refused for ever, and one older than the window is
refused outright. Only the *ordering* requirement is relaxed, and it is relaxed to match what
this queue actually does.

The alternative was to tag at drain time instead, which keeps the strict rule by construction.
Rejected as the larger change on the riskier side: `dh_outq` would have to hold untagged frames
and call back for a tag on promotion, mixing the queue with the key material it is deliberately
ignorant of. A receiver that tolerates the reordering its own transport performs is the smaller
and more honest statement.

Neither band's behaviour changes here. This queue still lets a session reply overtake bulk, and
that is still the point.
