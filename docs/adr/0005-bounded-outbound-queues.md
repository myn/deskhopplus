# ADR-0005: Bounded outbound queues on the device's two outbound seams

- **Status:** Accepted
- **Date:** 2026-08-12
- **Arising from:** [#69](https://github.com/myn/deskhopplus/issues/69)

## Decision

Both of the device's single-frame outbound seams — the slot feeding this board's helper
(`channel.out`) and the bulk slot feeding the peer board (`dh_relay_tx`'s bulk slot) — grow a small
bounded queue: the existing single slot stays as the head, sized `DH_FRAME_MAX_SIZE` so any legal
bulk frame (including a large `CLIP_OFFER`) is still accepted, and **two** chunk-sized
(`DH_XFER_CHUNK_SIZE`-ish, ~1040B) slots queue behind it. `channel.out` additionally splits into a
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

- Slot sizing is heterogeneous by design: the head slot must stay full-size to keep accepting an
  oversized one-off frame like `CLIP_OFFER`'s file-list meta; only the queued slots behind it are
  chunk-sized, to keep the RAM cost down against the ~68 KB SRAM headroom the current build leaves
  free.
- A frame that still cannot be carried once the queue is full (a genuinely sustained overrun beyond
  three frames in flight) is not given a new firmware-level signal. The existing end-to-end
  `CLIP_RETRANSMIT` machinery between helpers already treats a missing chunk as loss and re-requests
  it — that machinery is the "something that can act on it," not a device invention.
- Because the primitive is shared, `dh_relay_tx`'s bulk slot and a new path-(a) module both drain
  through the same host-tested code, rather than two divergent implementations of the same shape.
