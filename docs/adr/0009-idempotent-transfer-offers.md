# ADR-0009: Transfer offers retry idempotently until requested

- **Status:** Accepted
- **Date:** 2026-08-27
- **Resolves:** [#78](https://github.com/myn/deskhopplus/issues/78)

A `CLIP_OFFER` is the only transfer message whose loss leaves no state at the paste side and
therefore nothing there that can ask again. The copy side retries its immutable **offer** on the
existing two-second, caller-owned recovery cadence until a request arrives or the existing
30-second no-progress deadline abandons the transfer. A retry is not progress and cannot extend
that deadline.

An offer id is monotonically ordered within its copy side's namespace. The paste side treats the
same id and identical kind, total length and metadata as an idempotent retry; it never resets an
active or completed receive. The same id with different immutable fields is a session-ending
protocol error. A newer id supersedes an incomplete receive, and an older or already-completed id
is stale and ignored. This ordering uses wrap-safe 32-bit serial arithmetic, skips zero, and resets
with the session — and, since the amendment below, with a fresh incoming seal.

When no chunk has arrived for an identical active offer, the paste side repeats `CLIP_REQUEST` and
one covering credit window. Once chunks have arrived, or once the transfer completed, it emits
nothing. This distinction prevents a delayed retry from erasing partial data, delivering twice, or
inflating a streaming sender's credit.

## Considered options

- **Keep the timeout and improve its wording.** Rejected as the primary mechanism: the copy side
  already retains everything needed to recover, and ordinary message loss should not require a
  user to copy again. The timeout remains the terminal fallback.
- **Resend without making offers idempotent.** Rejected because a lost request is indistinguishable
  from a lost offer at the copy side. The paste side may already hold the offer, and treating the
  retry as newer would erase its receive state every two seconds.
- **Acknowledge offers with a new message.** Rejected as an extra round trip and wire type when the
  existing request is already the acknowledgement that matters: it is what authorises streaming.

## Consequences

`dh_xfer` remains clock-free: a sender-side sweep exposes the retry decision, while each helper
owns the cadence and terminal deadline. Lazy transfers stop retrying as soon as a request makes
them data-pending, even though streaming has not started. Retry actions and duplicates observed are
counted per transfer; successful recovery produces one copy-side note when the first request arrives,
without logging every retry attempt.

This does not resolve [#136](https://github.com/myn/deskhopplus/issues/136). Offer ids are ordered in
the remote copy side's namespace, but `CLIP_CANCEL` still carries only a number and can match both
directions when their independent namespaces choose the same value.

## Amendment, 2026-08-26 — the paste side's own session is not the only boundary

The rule above says the ordering "resets with the session", and named only the paste side's own.
That namespace, though, belongs to the **copy side helper's process**: `dh_xfer_init` starts its
ids at one, so a helper that restarts starts them again at one.

The two are not the same event. The far helper can restart while this side's session stays
perfectly healthy — nothing message-driven notices, exactly as [#52](https://github.com/myn/deskhopplus/issues/52)'s
third interruption already described for a stalled transfer. It then negotiates a fresh seal and
offers id 1, the paste side measures that against the dead process's offer-id frontier, and drops a valid
offer as stale. The clipboard stays unavailable in that direction until the paste side's own
session happens to end too.

**A fresh incoming seal is now the second boundary of that namespace.** It is the right one and
the only one available: a helper offers a seal exactly when it holds no key to send under, so a
fresh seal from the far end is that end saying its sending state started over. `dh_xfer_rx_seal_replaced`
forgets the incoming direction whole, and each helper's clipboard service calls it when it accepts
a `SEAL_OFFER`.

An incomplete receive goes with it, reported as abandoned rather than delivered in part: it belongs
to the seal it arrived under, and the helper that sent it has forgotten it exists. A straggling
offer sealed under the replaced key cannot bring it back either — the paste side holds one incoming
seal, so the straggler answers `SEAL_STALE` instead of opening.

The outgoing direction is deliberately untouched. What this end is sending recovers by the ordinary
`SEAL_STALE` exchange, which re-offers it under a key the far end can open.

Found by the post-implementation adversarial review of [#147](https://github.com/myn/deskhopplus/issues/147),
and resolved as [#151](https://github.com/myn/deskhopplus/issues/151).
