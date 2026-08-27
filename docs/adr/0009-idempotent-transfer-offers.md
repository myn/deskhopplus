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
with the session.

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
