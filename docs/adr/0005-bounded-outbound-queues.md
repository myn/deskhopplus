# ADR-0005: Bounded outbound queues on the device's two outbound seams

- **Status:** Accepted
- **Date:** 2026-08-12
- **Arising from:** [#69](https://github.com/myn/deskhopplus/issues/69)

## Decision

Both of the device's single-frame outbound seams — the slot feeding this board's helper
(`channel.out`) and the bulk slot feeding the peer board (`dh_relay_tx`'s bulk slot) — grow a small
bounded queue: the existing single slot stays as the head, sized `DH_FRAME_MAX_SIZE` so any legal
bulk frame is still accepted, and **two** slots of `DH_OUTQ_STAGE_MAX` (1044B) queue behind it
(three since #141 — see the amendment below) —
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

## Amendment, 2026-08-25 — the queued slots go from two to three

`DH_OUTQ_DEPTH` was 2, so the queue held three bulk frames: one in flight and two behind it.
[#137](https://github.com/myn/deskhopplus/issues/137) then sized `DH_XFER_CREDIT_WINDOW` against
that figure and cut it from 16 to 3. The two numbers matched — and the match was one frame short
of the truth.

`dh_xfer_pump` emits the batch's chunks and the `CLIP_DONE` behind them **in the same batch**, and
the DONE is not credit-gated. So the worst-case loss-free burst is the window *plus one*, and the
frame that overflowed a queue sized to the window alone was always the DONE: the one bulk frame
with no retransmit behind it, whose loss costs the whole transfer out to the helper's thirty-second
timeout. #137 turned a guaranteed thirteen-frame overrun into an occasional one-frame one, and the
one frame was the expensive kind.

**Depth is now 3, so the queue holds four bulk frames** — one pump batch, whole.

Fixed here rather than in the credit window, because every helper-side remedy breaks recovery.
A window of 2, a ceiling on accumulated credit, and a pump batch capped at 2 were each tried, and
each stalls `xfer_test`'s double-loss case. The reason is the same in all three: a covering grant
rides every `CLIP_RETRANSMIT`, and those grants are exactly what a cap discards. `docs/protocol.md`
already states why that inflation is deliberate — bounded and harmless, where draining is fatal.

This does not reopen the "short queue" decision above. The argument there is against absorbing a
**sustained** overrun, which is still the credit window's job and still something the firmware may
not read (ADR-0003). Holding one batch is what a bounded queue is for. The cost is one more
`DH_OUTQ_STAGE_MAX` slot per queue — 1,095 bytes each, against the ~62 KB of SRAM this build
leaves free.

**Measured cost**: SRAM `data`+`bss` grows by 2,200 bytes (184,524 → 186,724) — one more `DH_OUTQ_STAGE_MAX` slot in
each of the two queues.

**Verification is on hardware, not here.** `outbound refused` should stop climbing entirely rather
than merely climb slower; it is readable live in the helper logs since
[#133](https://github.com/myn/deskhopplus/issues/133). `outq_test` now asserts the honest bound —
`DH_XFER_CREDIT_WINDOW + 1 <= 1 + DH_OUTQ_DEPTH` — so the two constants cannot drift apart again.

## Amendment, 2026-08-25 — the same fault, at the inbound seam this ADR never covered

This ADR names *two* seams, and both are outbound. There is a third, and it had the identical
defect: the slot where core 1 parks a frame reassembled from the peer board for core 0 to re-tag.
It held one frame and refused the next outright, on this reasoning — *"a frame takes about 4 ms to
arrive over a 3.6 Mbaud link and core 0 drains at 1000 Hz, so a second slot would hold something
that is never there."*

True of a full-size `CLIP_CHUNK` and of nothing else. A relayed `CLIP_CREDIT` is ten bytes — three
inter-board packets, roughly 100 µs — and since the credit window was sized against the outbound
queue the receiver grants one credit per chunk rather than one per eight, so the reverse path
carries short frames in batches. Several land inside one of core 0's 1 ms passes and only the first
survives. Measured on both boards under nothing more than a few clipboard copies, and by
[#139](https://github.com/myn/deskhopplus/issues/139)'s readings it was the dominant loss on the
board by an order of magnitude. No core-0 stall is needed to explain it, and none was found.

Given the same shape, it gets the same answer: a bounded ring, `src/core/dh_inq.{h,c}`, host-tested
in `tests/inq_test.c`, four slots of `DH_OUTQ_STAGE_MAX`. Four is one pump batch — what actually
arrives back to back in either direction. Core 0 now drains the ring to exhaustion on each pass
rather than taking one frame, since taking one per pass would only move where the batch is lost;
the drain stops on a refused enqueue and leaves the rest parked, which is back-pressure this seam
previously had none of.

It is a ring rather than a fourth `dh_outq`. `dh_outq` tracks a frame's progress through a drain
that takes bytes in small fixed units; here a frame is taken whole, and the only thing that has to
be right is that one core writes while the other reads. The memory barriers stay in `channel.c`,
because a barrier is a platform instruction and `src/core` has no platform.

**One narrowing.** A slot is `DH_OUTQ_STAGE_MAX` where the old one was `DH_FRAME_MAX_SIZE`, so a
relayed frame longer than the largest a transfer can complete with is now refused and counted
rather than carried. Nothing `dh_xfer` emits is that long and the negotiated chunk size is clamped
below it, and a frame past that bound could never queue behind anything on the far board's outbound
queue either — it was only ever carried when that queue happened to be idle.

**Measured cost**: SRAM `data`+`bss` 186,724 → 187,012 bytes — 288 for the ring, on top of the
2,200 the amendment above cost. It replaces a 4 KiB slot, so four slots of 1,095 bytes are nearly
free. ~73 KB of SRAM is left.

## Amendment, 2026-08-26 — a new session empties the queue

This queue had no rule for what happens when the session it was filled for ends.

Nothing here is wrong on its own terms: a bounded queue holds frames, and a frame it holds is a
frame the transport still owes. What was missing is that the *reader* on the far end of that
transport does not survive a teardown. Every helper teardown closes its handles and reopens them,
so its `dh_frame_reader` restarts from nothing — and what this queue still held was then two
kinds of wrong at once. The unsent tail of a half-drained frame, which a fresh reader takes for a
header and follows into garbage. And whole frames tagged under keys that no longer exist, which
fail their tag. Both drop the session as fast as it was made, and the board is mid-frame again on
the next attempt, so one teardown became a flap.

**`channel.c` now calls `dh_outq_reset` when `dh_session.sessions` moves**, ahead of queueing the
`HELLO_ACK` — so the ack's first byte is the new stream's first byte. `dh_outq_reset` already
existed and already said this was its purpose ("what is queued was built for a session that no
longer exists"); it had no caller on this path. The four refusal totals survive the reset, as they
do everywhere else (#142).

Only a hello that authenticated moves the count, so a second writer on a shared endpoint cannot
make the board discard a live session's queue (#34). `dh_pair.registrations` is the same idiom,
sampled before `dh_session_on_frame` and compared after, in the same function.

The frames dropped here are a real loss with no retransmit, and that is the right trade: they were
addressed to a session that has gone, and delivering them costs the session that replaced it.

**Measured cost**: SRAM `data`+`bss` 188,100 → 188,108 bytes — eight, for a four-byte counter
that lands on an eight-byte boundary.

Found by [#143](https://github.com/myn/deskhopplus/issues/143), where it is the tail of the log
rather than the head — the first loss was a report the Windows HID class driver dropped, and this
is what turned that one desync into three teardowns.
