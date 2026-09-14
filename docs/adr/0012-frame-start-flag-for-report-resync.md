# ADR-0012: A frame-start flag on every report, so a lost USB report costs a frame instead of the session

- **Status:** Proposed
- **Date:** 2026-09-13 (rewritten the same day; the first draft chose a 2-byte sequence — see
  "Alternatives considered")
- **Follows from:** [ADR-0001](0001-vendor-hid-transport.md) (the report carrier), [ADR-0002](0002-parallel-hid-channels.md) (per-channel report streams)
- **Amends the security posture recorded on:** [ADR-0008](0008-channel-identity-and-sealed-clipboard.md)'s 2026-09-13 amendment, which this generalizes
- **Arising from:** [#63](https://github.com/myn/deskhopplus/issues/63), hardware-observed on 2026-09-13

## Decision

Byte 0 of every 64-byte HID **report** (see `CONTEXT.md`) is a **frame-start flag**: `1` when a
frame begins in this report, `0` when the report continues the frame in progress. The other 63
bytes are the byte stream the framing layer already owns.

The shared frame reader (`dh_frame.c`) takes one report per call and applies two rules:

- **Flag `1` while a frame is in progress:** the frame in progress lost at least one report. Throw
  the partial frame away, count one resync, and parse from this report.
- **Flag `0` while no frame is in progress:** this is the tail of a frame whose head was lost. Throw
  the report away and count one resync.

And one check the flag cannot make for it, the padding rule `only_padding` already applied one
layer up: **a frame that completes mid-report with a non-padding tail** borrowed the bytes of a
lost report's neighbour (a gap spanning a frame boundary). Throw it away and count one resync.

Everything else is unchanged. A frame still begins at a report boundary and pads its own tail, so
a frame never shares a report with another frame, and at most one frame completes per report.

A frame that loses a report is therefore **discarded before it is judged**, not delivered broken.
The receiver sees exactly what it sees when the sender's bounded outbound queue refused the frame
([ADR-0005](0005-bounded-outbound-queues.md)): a gap in the tag counter, which
`docs/protocol.md` ("Counters") already accepts, and — for a credit, a request, a retransmit or a
DONE — a stall that `dh_xfer_sweep_rx` (#145) already recovers from by asking again. A lost hello
times out and is resent; a lost heartbeat is one beat inside a window built for it. No new
recovery machinery is added, because a lost frame was already a failure this system handles.

## Context

Hardware testing on 2026-09-13 (an HP G5 Thunderbolt dock, board B, concurrent channel and input
traffic) reproduced a **second, distinct** dock-corruption shape from the one #63's tag-failure fix
already covers. There, bytes inside a report arrive wrong. Here, a **whole report never arrives at
all** — and nothing about the report carrier says so, because a report has no ID, length or
sequence of its own (`docs/protocol.md`, "The report carrier, and what it makes checkable").

The reader currently only learns about a hole retroactively, and only sometimes: after a frame
decodes, it checks whether what follows inside the same report is all padding (`only_padding`,
`dh_helper.c`). If not, that frame's tail ate the head of the next one — logged
(`DH_NOTE_STREAM_MISALIGNED`), but the reader's position is already wrong. The very next
`dh_frame_reader_push` call then fails outright — not a tag failure, a parse failure — and
`drop_connection` ends the session unconditionally. Observed directly: a 2 MB transfer survived two
genuinely corrupted `CLIP_CHUNK` tags (ADR-0008's fix working exactly as designed), then died 32
seconds later to this instead.

The one thing the reader lacks is one bit of information per report: *does a frame start here?*
That bit is what this ADR adds. It is the only thing the receiver cannot work out for itself — the
sender's frame boundaries are the only fact about the stream that no padding rule can recover once
the report carrying the header is gone.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **A 2-byte per-report sequence, zero-filling the gap** (this ADR's first draft) | It cannot resync across a gap that spans a frame boundary. Lose frame N's last report and frame N+1's first: the reader zero-fills two slots, N completes inside the fill, the rest of the fill is skipped as padding, and N+1's second report — ciphertext — is then read as a header. That is a protocol error and the session drops. Only a frame-start marker recovers that case, and once the marker is there the sequence has nothing left to do. It also cost twice the bytes, a per-channel-per-direction counter, a reset sentinel, a wraparound test, and an argument about counter range against liveness timers that a flag never needs. |
| **Zero-fill and deliver the broken frame, so the tag layer judges it** | The first draft wanted this so that a lost report inside a control frame would still end the session, "as today". But ending the session restarts the transfer from chunk 0 — the exact cost this ADR exists to remove — and on the copy side's inbound hop nearly every report is a credit, so nearly every lost report there would still kill the transfer. A lost frame is already an accepted failure here (ADR-0005, `dh_xfer_sweep_rx`); discarding lands on that recovery, zero-filling lands on the session-ending path. |
| **Recover the lost bytes** | Not attempted, and not needed. The frame riding the gap fails its tag regardless — real content or fill, both are "not what the sender wrote" to an AEAD tag. The only thing worth recovering is the reader's *position*, not the bytes. |
| **No wire change: hunt for the next plausible header** | Ciphertext is uniform, so a report's first bytes look like a known type with a legal length about 0.6% of the time; across the 66 reports of one chunk that is a coin flip on a false start. Checking the tag counter's plausibility as well would make it work, but it couples the codec to the auth layer and guesses where a bit could simply tell. Boring beats clever here. |
| **Do nothing; make a transfer resume across the reconnect** | Reconnect itself measured under half a second, but the transfer does not survive it: today's design ties transfer state to the connection, so the whole file restarts from chunk 0. Making transfer state outlive the session touches both helpers' transfer services, the seal handshake and ADR-0009's offer identity — far more moving parts than one byte and two rules — and a resumable transfer would still restart on every other frame-parse failure this does not reach. |

### This is not #32 or ADR-0002's rejected "per-report round-robin striping" again

Both of those rejected a per-report *sequence* used for *ordering*. A frame-start flag is framing,
not ordering: it says nothing about which report came first, only whether a frame begins here. This
ADR adds nothing to how chunks are retried, ordered, or assigned to channels; it only lets the
reader keep parsing when one report of the many carrying a chunk never shows up.

## Consequences

### Cost: one byte in sixty-four, about 1.6% off the report's usable payload

ADR-0002 already accepted a 5.0% overhead for the sealed chunk framing on the same grounds this one
stands on: the inter-board UART, not the USB hop, is the measured wall (#166). This does not change
that arithmetic materially.

### Every emitter and every reader needs it, symmetrically

Three emitters already write one frame per run of reports and already know where the frame
starts: firmware's `channel_pump_out` (`owed.remaining == owed.total`), the Windows helper's
`HidTransport::send` (`offset == 0`), and the macOS helper's `FrameCodec.reports(for:)` (the
same). Each sets one byte.

Two readers already feed the shared reader exactly one report per call — `dh_helper_received_channel`
states it as a precondition, `drain_reports` does it by construction — so the reader gains a
per-report entry point and the callers' inner loops go away: a frame never shares a report, so at
most one frame completes per call. `only_padding` and its note move into the reader as the resync
count.

### A protocol version bump; no mixed-version rollout

Firmware and both helpers move together, the same discipline ADR-0002's channel-count negotiation
and #177's flash-across-a-version-boundary note already established. Reflashing both boards is
required.

The bump is a record, not a gate. The hello itself rides the changed report shape, so a mismatched
peer never parses it far enough to reach the version check: an old board reads a new hello's length
as `0x3F00` and refuses it as oversize; a new board reads an old hello's flags byte as type `0x00`
and refuses it as unknown. The board ends the session either way. An old helper reads the new
board's flagged `SESSION_END` as an oversize frame and loops on a protocol error; a new helper
discards the old board's un-flagged one as the orphan of a lost head and loops on its 2 s hello
timeout instead, counting a resync each time. Both loops are the repeated reconnection the helper
already reports — loud, never silent, but not `HELLO_REFUSED`.

### `DH_NOTE_STREAM_MISALIGNED` becomes a resync count

Today it only explains, after the fact, why the next parse is about to fail. Now it counts the
times the reader bridged a gap and kept going.

### The firmware's own "stream gap" session end is no longer needed

`channel_lifecycle.c` ended the session when its inbound backlog overflowed and a report was dropped
(`stream_broken`, `DH_SESSION_END_STREAM_GAP`), because "no amount of waiting recovers" — the
reasoning the frame_test gap test proved (since rewritten as
`test_a_lost_report_mid_frame_costs_that_frame_only`). That reasoning stops being true once the
reader resyncs; an overflow becomes a lost frame like any other. Removed by #188 (2026-09-14); the
reason value stays reserved.

### What still ends the session

A frame that completes with borrowed bytes and a clean tail. That needs a gap of two or more reports
spanning a frame boundary *and* the first frame's last report exactly full, so that nothing in the
report betrays the borrow. The frame then reaches the tag layer and is judged as today: tolerated
for a `CLIP_CHUNK`, session-ending otherwise. Rare squared; noted in code, not engineered around.

A corrupted flag on a continuation. The flag is the one byte a corruption no longer costs just a
frame: a continuation's `0` flipped to exactly `1` makes the reader parse ciphertext as a header,
which is a protocol error and ends the session, where the same flip in any other byte is a failed
tag #63 tolerates. It needs byte 0 and bit 0 exactly, so it is a small fraction of the corruption
#63 measured; the signature on hardware is a protocol-error session end mid-transfer.

## Outstanding

- Hardware validation must show the resync actually holds under the same dock and load conditions
  #63 was diagnosed against, not just in host tests.
