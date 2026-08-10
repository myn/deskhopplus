# ADR-0002: Parallel HID channels, striped by chunk, negotiated at handshake

- **Status:** Accepted
- **Date:** 2026-08-09
- **Follows from:** [ADR-0001](0001-vendor-hid-transport.md), whose *Throughput* consequence this addresses
- **Affects:** [#42](https://github.com/myn/deskhopplus/issues/42) (spec), [#25](https://github.com/myn/deskhopplus/issues/25) (interface), [#39](https://github.com/myn/deskhopplus/issues/39) (measurement)

## Decision

The helper↔firmware channel is **three parallel vendor HID interfaces**, striped **per chunk**, with
the channel count **negotiated in the session handshake** and **shipped initially as one**.

Specifically:

1. **Three channels** when fully deployed. Each is a vendor HID interface with an interrupt IN and an
   interrupt OUT endpoint at 64 bytes / 1 ms, so ~64 KB/s per direction each, ~192 KB/s in total.
2. **Chunk affinity.** A whole chunk goes out on one channel and stays on it. Different chunks may
   take different channels.
3. **Channel 0 carries session, control and placement**; bulk stripes across all channels.
4. **The channel count is negotiated in the hello** and is not a wire-format property. Ship `N = 1`;
   raise it once [#39](https://github.com/myn/deskhopplus/issues/39) has measured throughput and the
   longer descriptor has been tested at a BIOS/UEFI prompt. *(Amended 2026-08-09: `N = 2` is the
   default candidate for the raise — see Outstanding.)*
5. **All-or-nothing exclusivity.** The helper acquires every channel exclusively or fails the session.
6. **The channel is invisible to the firmware and to the wire format.** No channel field on any frame.

## Context

ADR-0001 moved the transport to vendor HID and, in doing so, dropped the USB hop from CDC bulk to a
full-speed interrupt endpoint: **one 64-byte report per 1 ms frame, i.e. ~64 KB/s per direction**. The
inter-board UART carries ~200 KB/s, so a single HID channel is roughly **3× worse than the link it
feeds**, and the USB hop becomes the system bottleneck where the UART used to be.

Pasting speed is a user-visible property that matters, so the ceiling needed addressing rather than
accepting.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Accept ~64 KB/s** | 10 MB at the default cap is ~2.7 minutes, 64 MB is ~17 minutes. Acceptable for the lazy file case, poor for images and anything interactive. |
| **Hybrid: HID for control, CDC for bulk** | Reintroduces every cost ADR-0001 avoided — the `usbser.sys` binding question and the Trellix `hdlpdbk` filter on the Ports class — and does so for the payload most likely to be DLP-inspected. |
| **More than three channels** | The UART is the wall at ~200 KB/s. Three lands just under it; four or more is machinery feeding a queue. |
| **Per-report round-robin striping** | Costs a sequence byte per report and requires a resequencing buffer at both ends — precisely the per-packet sequencing overhead [#32](https://github.com/myn/deskhopplus/issues/32) rejected on the inter-board link, reappearing on a different link for the same bad reason. |

## Why chunk affinity, specifically

Multiple interrupt endpoints carry **no ordering guarantee between them** — the host schedules each
independently, so a byte stream sprayed across channels can arrive interleaved wrongly.

Chunks are already the unit of CRC, loss detection and selective retransmission, and the transfer
state machine already tolerates them arriving out of order. **Reordering across channels is therefore
already handled by machinery the design has**, at zero additional per-report cost.

This composes with the decision that a **chunk is exactly one frame's payload**. The firmware parses
frame headers and never payloads, so frame boundaries are the only boundaries it can see — and because
chunk equals frame, the egress board can keep a whole chunk on one channel without parsing anything.
Chunk affinity falls out of per-frame affinity for free.

## Consequences

### The firmware does not learn what a channel is

All channels funnel into one inter-board link, which serialises them regardless. The ingress board
forwards; the egress board assigns per-frame affinity — control and placement to channel 0, bulk
round-robin. So the fragmentation and relay seam is **unchanged** by this ADR, and no frame carries a
channel id.

### Flow control stays global

The credit window accounts bytes in flight **across all channels**, not per channel. What is being
protected is the receiver's memory and the inter-board queue, and the channels are parallel paths into
the same funnel — three individually-respectful channels could overflow a shared queue. A global window
also means raising `N` from 1 to 3 changes no flow-control behaviour, which is the point of negotiating
it.

### Exclusivity is all-or-nothing

[#34](https://github.com/myn/deskhopplus/issues/34) guarantees that one program holds the channel. With
three collections there are three acquisitions and therefore a partial-success state — and that state
is worse than outright failure: a second process holding one channel would silently receive **a third
of every bulk transfer**, while both sides appeared to have a working session. The helper acquires all
channels or reports "another program holds the channel" and stops.

### Enumeration risk is deferred, not avoided

Three interfaces is a materially longer descriptor than one, and [#59](https://github.com/myn/deskhopplus/issues/59)'s
finding U2 is that BIOS/UEFI tolerance of a longer descriptor with a higher interface count **cannot be
answered from source** — only on real hardware, and DeskHop is used at disk-encryption prompts.

Shipping `N = 1` first means [#25](https://github.com/myn/deskhopplus/issues/25) can land and be verified
against ADR-0001's two confirmations with the simplest possible descriptor, before enumeration risk is
tripled. Raising `N` is then a descriptor edit plus a negotiated value — never a protocol revision
across three implementations.

### Interactive latency improves incidentally

Each endpoint is polled independently every 1 ms, so a placement report on channel 0 cannot queue
behind bulk sitting on channels 1 and 2. The "input outranks bulk" priority discipline was written when
everything shared one pipe; on the USB hop it is now largely structural. It still matters on the
inter-board link, which remains a single serialised path.

## Outstanding

- **[#39](https://github.com/myn/deskhopplus/issues/39)** must measure real throughput on one channel
  before `N` is raised. The 64 KB/s figure is arithmetic from the frame interval and report size, not
  an observation — the same status the spec's earlier ~200 KB/s UART figure had, and it should be
  treated with the same suspicion.
- The working **chunk size** is a build constant to be set after that measurement. The design fixes only
  that a chunk is one frame's payload, within the unchanged 4 KiB frame maximum. **The
  loss-amplification math, not only measured throughput, should drive the choice** *(amended
  2026-08-09)*: the chunk is the retransmission unit, and a 4 KiB chunk is 512 inter-board packets —
  one lost packet retransmits all 512. At 512 bytes a chunk is 64 packets, for roughly 2% additional
  header overhead.
- A follow-up ticket ([#63](https://github.com/myn/deskhopplus/issues/63)) raises `N` from 1 and
  carries the BIOS/UEFI re-test at the longer descriptor.
- **Amended 2026-08-09: `N = 2` is the default candidate for that raise, not an off-ramp.** The
  latency benefit — placement on channel 0 never queuing behind bulk — arrives fully at two channels.
  The third channel moves bulk from ~128 to ~192 KB/s against a UART wall the same arithmetic puts at
  ~200 KB/s, while carrying the longest descriptor and a third exclusive acquisition. Raise to 3 only
  if #39 shows a single channel actually achieves its arithmetic and the inter-board link, not the
  USB hop, is measurably the wall. The negotiated count makes this a late, cheap choice — which is
  the point of negotiating it.
