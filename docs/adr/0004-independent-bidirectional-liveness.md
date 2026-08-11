# ADR-0004: Independent bidirectional liveness, carried by traffic rather than by acknowledgement

- **Status:** Accepted
- **Date:** 2026-08-11
- **Resolves:** [#68](https://github.com/myn/deskhopplus/issues/68)
- **Constrains:** [#52](https://github.com/myn/deskhopplus/issues/52) and the clipboard slices, [#49](https://github.com/myn/deskhopplus/issues/49) (the Windows helper), and the session section of spec [#42](https://github.com/myn/deskhopplus/issues/42)
- **Related:** [#69](https://github.com/myn/deskhopplus/issues/69) — the single outbound frame slot this decision is shaped around

## Decision

Liveness on the channel is **symmetric and independently timed in each direction**, and it is
carried by **any traffic**, not by an acknowledgement.

- Each end considers its peer alive while **any frame** has arrived from it inside
  `DH_SESSION_ABSENT_MS` (three heartbeat intervals). Traffic in a direction is itself the proof.
- A heartbeat is emitted **only when that direction has been idle** for a full interval. It is a
  filler for a quiet link, not the liveness signal. A busy link emits none at all.
- `DEVICE_HEARTBEAT` (`0x06`, d→h, empty) is added so the device→helper direction has a filler of
  its own. It is sent only while a session exists, so its absence is meaningful.
- `SESSION_END` (`0x07`, d→h, `reason:u8`) is added so the device can announce an eviction it knows
  about, rather than leaving the helper to wait out a timeout. It is an **optimisation over** the
  timeout and never a substitute: a device that reboots or wedges announces nothing.

Both ids sit in the session band, so the firmware never forwards them and the routing decision
stays a single range test.

## Context

The v1 heartbeat was one-way. A helper beat every second and the device refreshed a timestamp;
nothing travelled back. The device drops a session on **four** paths, none of which sent anything —
its own liveness timeout, a framing error on its reader, a version mismatch, and (a path #68 did
not list) the config chord opening a pairing window, which rotates the secret and evicts the helper
on that board. On the first, second and fourth the helper went on beating into a device that
ignores heartbeats, with no ack to miss and no re-hello timer, and its menu bar kept reading
*connected and paired*.

The chord path is the sharpest, because it is the one moment the user is watching: #34 promises
that the state changing to connected is the confirmation the press worked, and the state never left
connected, so the confirmation could not fire.

The cost of the gap rises sharply at the first payload. Cursor placement degrading quietly is
survivable; a helper that believes it holds a session it does not will offer clipboard content that
never arrives, and the paste side will wait on a transfer nobody is sending. Hence settled before
#52 rather than after.

## Why acknowledgement lost

The obvious mechanism — the device answers each beat — was rejected for a reason worth recording,
because it is the option a future reader will assume was correct.

**An acknowledgement only proves anything while the helper is still asking.** The first failure
path is a helper whose own beats *stalled rather than stopped*; that helper has stopped asking, so
it never notices the missing answers. The mechanism is blind to one of the exact cases motivating
it. Two free-running timers have no such hole: each end is watching for something the other end
sends unprompted.

## Why traffic rather than a dedicated beat

This is the part that will look wrong at a glance, since #68 itself rejected "treat existing
traffic as liveness" on its face. That rejection was correct **in the helper→device direction**,
where placement is fire-and-forget and may not happen for hours, so absence proves nothing. The
inverse proposition is what is used here: the *presence* of a device→helper frame proves the device
is alive and holds an authenticated session, because every such frame has already passed
`dh_session_may_relay` or come from the session layer itself.

The forcing constraint is `channel_queue_frame`: **one frame slot**, shared by relayed bulk, and a
refusal is silent loss with no retransmit beneath it (#69). A once-per-second beat competing with a
sustained clipboard transfer would be refused repeatedly, three refusals is a false eviction, the
transfer is abandoned, and the reconnect starts a transfer that dies the same way. A beat that
fires only on an idle link **fabricates none of this**: a busy link emits no beat at all, and a
refused beat is self-correcting, because the slot being busy means something else is going out that
refreshes the peer anyway.

So the mechanism designed to detect a broken session would, in its obvious form, have manufactured
broken sessions — and only once #52 landed, which is the deadline it was written to beat.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Acknowledged heartbeat** — the device answers each beat | Blind to a helper whose beats stall rather than stop, which is the first of the four failure paths. Also a strict 1:1 obligation inside the frame handler, and one guaranteed frame per second per direction contending for the single out slot. |
| **Dedicated device beat, sent unconditionally** | Contends with relayed bulk for the one outbound slot; refusals are silent, and three of them is a false eviction mid-transfer. Idle-gating it costs one timestamp and removes the failure mode entirely. |
| **Reuse existing traffic, helper→device** | Rejected, and stays rejected. Placement is fire-and-forget and may be hours apart, so silence there proves nothing about the helper. |
| **`SESSION_END` as the mechanism, with no timeout** | A device that reboots, wedges, or loses power announces nothing. Announcement is an optimisation on top of a timeout, never in place of one. |
| **Hoist the helper session engine into the shared core** so #49 inherits the detector | After idle-gating, the detector is one comparison against a constant the core already exports. A C module wrapping it would be worse code than the duplication it prevents. The larger question — whether all of the helper's session logic belongs in C — is real but far bigger than #68, and is tracked separately. |

## Consequences

- **The heartbeat becomes conditional in both directions.** Existing helper tests that assert a
  beat at exactly `heartbeatInterval` become "a beat only if nothing else was sent."
- **`dh_session_tick` stops returning `bool`.** The eviction transition *is* the emitted
  `SESSION_END`, so the signal and the frame are one thing that cannot disagree. `dh_session_drop`
  splits: a silent form for a lost link, where there is nobody to tell, and `dh_session_end`, which
  drops and encodes.
- **The detector is scoped to a session, never to a phase.** A helper refused authentication stays
  connected, beating and asking, because #46's silent provisioning only works on a helper that is
  connected when the chord lands. Keying the detector on the phase would tear that down every three
  seconds and break pairing outright.
- **`DH_PROTO_VERSION` does not change.** v1 is unreleased, the registry change is additive, and
  the version check is exact equality, so a bump would only force a mismatch nothing benefits from.
- **#52 inherits a seam**, not a condition to invent: bulk may be sent only while a session is
  established, exposed once and tested here.
- **#49 inherits the constants and the wire format**, which is all there is to inherit; #68's
  acceptance criterion was amended to say so.
- The glossary (`CONTEXT.md`) now separates **Link**, **Connection** and **Session**. Having one
  word for all three is why this defect was invisible: a dead session and a live connection look
  identical when you cannot name the difference.
