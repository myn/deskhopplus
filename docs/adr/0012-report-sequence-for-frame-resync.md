# ADR-0012: A report sequence, so a lost USB report resynchronizes the reader instead of ending the session

- **Status:** Proposed
- **Date:** 2026-09-13
- **Follows from:** [ADR-0001](0001-vendor-hid-transport.md) (the report carrier), [ADR-0002](0002-parallel-hid-channels.md) (per-channel report streams)
- **Amends the security posture recorded on:** [ADR-0008](0008-channel-identity-and-sealed-clipboard.md)'s 2026-09-13 amendment, which this generalizes
- **Arising from:** [#63](https://github.com/myn/deskhopplus/issues/63), hardware-observed on 2026-09-13

## Decision

Every 64-byte HID **report** (see `CONTEXT.md`) carries a 2-byte monotonic sequence number, per
channel, per direction. A receiver that sees a gap treats the missing reports as present-but-empty —
zero-filled — for exactly as many report-slots as were skipped, so the byte count for whatever
**frame** was riding across the gap comes out exactly as long as its own header always said it would.
The reader therefore always finds the correct start of the *next* frame, however many reports were
actually lost inside the one before it.

The frame that ate the gap fails its tag deterministically (some of its ciphertext is now the
zero-fill, not what the sender wrote). That is not new machinery: it lands in the tag-failure
handling ADR-0008's 2026-09-13 amendment already built — tolerated if the frame is a `CLIP_CHUNK`,
session-ending otherwise, unchanged. This ADR's whole job is getting the reader to *reach* that
judgement at all, rather than derailing one layer earlier.

## Context

Hardware testing on 2026-09-13 (an HP G5 Thunderbolt dock, board B, concurrent channel and input
traffic) reproduced a **second, distinct** dock-corruption shape from the one #63's tag-failure fix
already covers. There, bytes inside a report arrive wrong. Here, a **whole report never arrives at
all** — nothing about the report carrier says so, because a report has no ID, length or sequence of
its own (`docs/protocol.md`, "The report carrier, and what it makes checkable").

The frame reader currently only learns about a hole retroactively, and only sometimes: after a frame
decodes, it checks whether what follows inside the same report is all padding (`only_padding`,
`dh_helper.c`). If not, that frame's tail ate the head of the next one — logged (`DH_NOTE_STREAM_
MISALIGNED`), but the reader's position is already wrong. The very next `dh_frame_reader_push` call
then fails outright — not a tag failure, a parse failure — and `drop_connection` ends the session
unconditionally. Observed directly: a 2 MB transfer survived two genuinely corrupted `CLIP_CHUNK`
tags (ADR-0008's fix working exactly as designed), then died 32 seconds later to this instead.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **A "new frame starts here" flag alongside the sequence** | Unnecessary once you look at the existing rule: a frame *always* begins at a report boundary and pads its own tail (`docs/protocol.md`). Once the reader knows a frame's declared byte length and can count exactly how many report-slots (present or gap-filled) that length spans, it already knows where the next frame starts — a second bit would duplicate what the length field already says. |
| **Recover the lost bytes** | Not attempted, and not needed. The frame riding the gap is going to fail its tag regardless — real content or zero-fill, both are "not what the sender wrote" to an AEAD tag. The only thing worth recovering is the reader's *position*, not the bytes. |
| **1-byte sequence (mod 256)** | Too small a window. At roughly one report/ms, 256 values is a ~256 ms horizon — inside the range of ordinary jitter, and uncomfortably close to timers already in the system (`DH_HELPER_HELLO_TIMEOUT_MS` at 2000 ms). A 2-byte counter (mod 65536, ~65 s) stays unambiguous well past every existing liveness timeout, at one more byte of cost. |
| **Do nothing; a lost report already ends in a reconnect, which is fast** | True — reconnect itself measured under half a second. But the *transfer* does not survive it: today's design ties transfer state to the connection, so the whole file restarts from chunk 0. That is the user-visible cost, not the reconnect latency, and it is what actually needs fixing. (A cheaper, narrower fix along just this line — make a transfer resume across a reconnect instead of restarting — was considered and set aside in favor of fixing the reader itself, since the reader problem is the more fundamental one and a resumable transfer would still restart on every other frame-parse failure this doesn't reach.) |

### This is not #32 or ADR-0002's rejected "per-report round-robin striping" again

Both of those are about a *different* question — reliability lives at the chunk level, end to end
between the two helpers (#32), and reports are assigned to channels by chunk affinity, not
round-robin, because per-report sequencing for *interleave ordering* was rejected as unnecessary cost
on top of what chunk-level retransmission already buys (ADR-0002, "per-report round-robin striping").
Neither considered a report failing to arrive **at all** on its own channel — both assumed the report
carrier itself was reliable and only reasoned about ordering *across* channels or *within* the
inter-board UART's much smaller 8-byte packets (where the same style of per-unit sequence number
costs a proportionally much larger fraction of the payload — #32's rejection of it there does not
transfer to a 64-byte USB report). This ADR adds nothing to how chunks are retried, ordered, or
assigned to channels; it only lets the reader keep parsing when one report of the many carrying a
chunk simply never shows up.

## Consequences

### Cost: roughly 3% off the report's usable payload

64 bytes minus 2 for the sequence leaves 62. ADR-0002 already accepted a 5.0% overhead for the sealed
chunk framing on the same grounds this one stands on: the inter-board UART, not the USB hop, is the
measured wall (#166). This does not change that arithmetic materially.

### Every emitter and every reader needs it, symmetrically

Firmware's `channel_pump_out` (device→helper) and each helper's outbound HID write (helper→device)
both start writing the sequence; `dh_frame_reader_push` (`src/core/dh_frame.c`, shared by firmware
and both helpers) is where the gap-fill and resync logic lives, once, for every direction and every
platform — the same reasoning `dh_helper.c`'s own header gives for writing session logic once rather
than per platform.

### A protocol version bump; no mixed-version rollout

An old reader fed the new report shape misreads the sequence bytes as frame content, and a new reader
fed the old (unsequenced) shape has nothing to check — this is a wire-incompatible change on both
directions and both boards. Firmware and both helpers move together, the same discipline ADR-0002's
own channel-count negotiation and #177's flash-across-a-version-boundary note already established.
Reflashing both boards is required; no already-shipped board interoperates with a new helper, or vice
versa, mid-rollout.

### `DH_NOTE_STREAM_MISALIGNED` changes from a diagnostic to a resync count

Today it only explains, after the fact, why the next parse is about to fail. Once the reader can
actually bridge the gap, the same note becomes the count of *times it did* — still counted, no longer
followed by a certain session drop.

## Outstanding

- The exact reset point for a fresh reader's sequence expectation (no prior report to compare
  against) needs a sentinel, matching how `dh_auth_counter_init` already resets its own counter on
  every session start — implementation detail, not a design question, but worth pinning down before
  code review.
- Hardware validation must show the resync actually holds under the same dock and load conditions
  #63 was diagnosed against, not just in host tests.
