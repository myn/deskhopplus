---
name: deskhopplus-adversarial-review
description: Review a completed non-trivial DeskHopPlus change after normal code review to find credible boundary, failure, or emergent defects; not for implementation or routine code review.
---

# DeskHopPlus Adversarial Review

Use this post-implementation review to answer: *now that the change works, what else did it expose, break, or make reachable?* DeskHopPlus is embedded firmware and a distributed protocol system; happy-path correctness is insufficient.

Use only after normal code review is complete. Skip genuinely trivial documentation, comment, or formatting-only changes. This skill discovers and records defects; a finding becomes a separate implementation task only when the user asks to fix it.

## 1. Establish the review surface

Confirm the reviewed change and its baseline: the linked issue, reviewed diff/commit range, and test and code-review evidence. Do not treat unrelated worktree changes as part of the review. If normal code review is incomplete, stop and request that it be completed first.

Before exploring the implementation, read `CONTEXT.md`, then the ADRs relevant to the changed area, following `docs/agents/domain.md`. Use the repository's terms in findings and explicitly surface ADR conflicts.

Identify each changed subsystem, its state ownership, inputs/outputs, and affected peers. For each affected path, trace:

```text
input -> state change -> queue/buffer -> protocol/wire -> remote state -> ack/retry/timeout -> observable result
```

## 2. Select the investigation routes

Read only the references that apply:

- For queues, backpressure, retry, timeout, heartbeat, resource limits, or timing, read [reliability and liveness](references/reliability-and-liveness.md).
- For state machines, reconnect/reset, IDs, messages, protocol compatibility, or USB/HID, read [state, protocol, and USB](references/state-protocol-usb.md).
- For communication or traffic changes, read [boundaries and load](references/boundaries-and-load.md).

For every relevant boundary, inspect both directions. Do not infer symmetry from the changed direction.

## 3. Investigate candidates

Use three lenses: **same-path failure** (another failure in the modified path), **adjacent-seam failure** (a neighbor mishandles an otherwise correct result), and **emergent failure** (reasonable components interact into failure). Prefer boundary, lifecycle, and sustained-load candidates over duplicates of normal code-review findings.

For each candidate, attempt to disprove it before retaining it. Classify retained findings as exactly one of:

- **CONFIRMED** — a test, reproduction, trace, or other executable evidence demonstrates it.
- **PROVEN BY CODE** — it follows deterministically from code and its invariants.
- **HARDWARE / ENVIRONMENT DEPENDENT** — the code establishes the failure mode, but physical hardware, OS behavior, or timing is needed to reproduce it.
- **SPECULATION** — insufficient evidence; discard it and never create an issue for it.

Investigate at least two credible additional candidates, spanning two lenses when applicable. Do not manufacture findings: if fewer than two survive, document the routes examined and why no other credible defect was established.

## 4. Record outcomes

For each retained finding, capture the affected subsystem, violated behavior or invariant, exact code path, proof/reproduction, expected and actual behavior, impact, classification, related change/issue, and hardware-verification need.

Search existing `myn/deskhopplus` issues for the same underlying defect before drafting a new one. Follow `docs/agents/issue-tracker.md` and `docs/agents/triage-labels.md`. Unless the user has authorized GitHub writes, provide issue-ready findings for approval rather than creating issues.

Finish without implementation changes:

```text
Adversarial Review
------------------
Changed area:
Review baseline and evidence:
Boundaries inspected:
Findings:
  1. [classification] ...
  2. [classification] ...
Issues created or ready for approval:
Additional investigation:
Hardware verification required:
```
