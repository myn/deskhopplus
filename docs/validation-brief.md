# Validation brief: the helper↔firmware transport decision

**For a reviewer coming to this cold.** Written 2026-08-09 by the session that produced the work, which makes it the wrong session to review it — hence this.

This is deliberately **not a summary of the conclusions.** Restating them persuasively is precisely how a reviewer gets talked into agreeing with them. It is a map of where things are, what is soft, and where errors have actually been found before.

---

## What was decided

Two ADRs, both in `docs/adr/`:

- **ADR-0001** — the helper↔firmware transport moved from USB CDC to a vendor-defined HID interface.
- **ADR-0002** — three parallel HID channels, striped per chunk, count negotiated at handshake, shipped as one.

Read the ADRs for the reasoning. Read the issues for how the reasoning changed under measurement.

## Where everything is

| Artifact | Location |
| --- | --- |
| Decisions | `docs/adr/0001-*.md`, `docs/adr/0002-*.md` |
| Research | `docs/research/` — 5 documents |
| Measurement tooling | `tools/windows-checks/*.ps1` — 3 scripts |
| **Raw evidence** | `tools/windows-checks/deskhopplus-*.log` and `*.json` — **gitignored, local disk only** |
| Channel spec | issue #42 |
| Map holding locked decisions | issue #31 |
| Commits | `11bbd79`, `59fae16` |

**The raw logs are the primary evidence and are not in git.** They are gitignored because they carry corporate hostnames, usernames, an AD domain and clipboard previews of internal documents — so every number in the research and the issues is a *transcription* made by the same session that drew the conclusions. If the point of the review is to check the readings rather than trust them, read the logs directly. They may be deleted; if they are gone, the transcriptions are all that remain.

## The prior worth having before you start

**Three well-sourced conclusions in this work were overturned by measurement.** Each was correct about what documentation specified and wrong about what implementations do:

1. **Trellix device blocking was "not deployed."** Inferred from `hdlpdbk`'s absence in a driver list — a list filtered to `State = Running`. The driver is installed and is a registered class filter on three of the four device classes that matter.
2. **The local DLP policy paths were "undocumented, therefore nonexistent."** They exist, are world-readable, and no vendor document mentions them.
3. **HID share-mode 0 "gives no exclusivity."** Reasoned correctly from `CreateFile`'s documented parameter table. Measurement refused a zero-access open on 10 of 10 vendor collections.

Two of the three were failures of *negative evidence* — "absent from X" where the filter on X was never examined. The third was documentation that did not describe the implementation.

**A reviewer spending effort where documentation was trusted without measurement will likely find more than one auditing the reasoning chains.** The chains were checked repeatedly; the documentation-to-implementation gaps were not, until they bit.

## Claims that are soft, ranked by how much rests on them

### 1. Three channels give roughly 3× throughput — ADR-0002

Untested arithmetic on top of untested arithmetic. The base figure (~64 KB/s per channel) comes from a 64-byte report at a 1 ms interrupt interval, never observed. The 3× assumes host scheduling, the RP2040 USB controller and TinyUSB all deliver parallel interrupt endpoints independently — plausible, unverified, and the whole justification for the added descriptor complexity and enumeration risk.

If this is wrong, ADR-0002 is wrong and #63 should not be built. Issue #39 is meant to measure it and is blocked on hardware.

### 2. `hidclass.sys` will behave the same for our device — ADR-0001

Marked `[INFERENCE]`. Measured across 10 vendor collections from 4 independent vendors, never ours, because ours does not exist. The argument is that uniformity across unrelated minidrivers implies the enforcement is in the class driver rather than per-driver. Reasonable; not proven.

The security control in issue #34 depends on this being true.

### 3. One `IOHIDDevice` per USB HID interface — ADR-0001

Marked `[INFERENCE]`. Inferred from observed `ioreg` structure, not documented by Apple. The macOS TCC constraint — that the vendor collection must be its own interface — rests on it. If the mapping is different, the vendor collection could land in a TCC-flagged node and acquire the Input Monitoring requirement the transport was chosen to avoid.

### 4. The Trellix findings where the vendor knowledge base was unreachable

`kcm.trellix.com` refused connections throughout. **KB85654, "Explanation of the clipboard blocking feature,"** could not be read and by its title may be the single most relevant document in existence for the clipboard question. Anything it says could contradict the research.

### 5. The fork's clipboard core library

Issue #61 proposes adopting it. It was **read, never built or run**. Its size figure (2,266 bytes), its test suite and its fuzz properties are its author's claims, verified only by reading the source.

### 6. mkroamer protocol reuse

The spec inherits framing, the transfer state machine and test vectors from a separate project. That code was reasoned about, not executed here.

## Things that look like problems and are not

- **`README.md:117` documents a `DH_DEBUG_CDC_FLASH` command over CDC serial.** Real, unrelated debug-build feature. Not a leftover of the transport change.
- **CDC references remain in issues #1, #7, #8, #15, #31, #32, #34, #36.** Deliberate. Maps and closed decision tickets carry supersession notes rather than rewrites, so the reasoning trail survives.
- **Research documents contain struck-through sections.** Also deliberate — superseded content is preserved with corrections above it.
- **Issue #40 check 4 is unrun.** It concerns `usbser.sys` binding, which the transport change made moot.

## Questions worth asking that nobody asked

These are gaps I am aware of, not ones I found and fixed:

- Does the 4 KiB frame maximum still make sense when a chunk equals a frame and the inter-board link carries 8 payload bytes per packet? A 4 KiB chunk is 512 inter-board packets, and one lost packet costs the whole chunk.
- Is the ~256 KB eager/lazy threshold right at 64 KB/s? It was chosen against CDC bulk. Noted on #55, not resolved.
- Does the all-or-nothing exclusivity rule interact badly with the helper starting late? Both are accepted risks; their combination was not analysed.
- The spec assumes helpers can be locked out by whoever opens first. Nothing analyses what a hostile process that *wins* that race can actually do.
- Nothing has ever been built. Every conclusion is about a device whose vendor interface does not yet exist.

## How to reach the evidence

```bash
gh issue view 42          # the spec
gh issue view 31          # the map, with locked decisions
gh issue view 40 --comments   # the Windows measurements, with corrections
gh issue view 59 --comments   # the transport decision, and two reversals

ls tools/windows-checks/deskhopplus-*.log   # raw evidence, if not yet deleted
```

The scripts are runnable and unelevated. `Confirm-HidExclusivity.ps1 -Check A,E` reproduces the exclusivity result on any machine with a vendor-page HID device; `Confirm-Check1.ps1 -Part A` reproduces the cursor-placement result and needs one UAC approval.

---

## Validation outcome (2026-08-09, appended by the reviewing session)

The review ran. Everything above this line is the original brief, unmodified except two inventory
counts corrected in its table (5 research documents, not 4; 3 scripts, not 4). The raw logs were
deliberately not opened, per instruction — so the transcriptions were checked for internal
consistency across the ADRs, issues, research documents and source tree, not against the raw
evidence. Within that scope, the record held: every number, section reference, script invocation and
supersession note cited across the corpus checked out, and the arithmetic is right (one benign
note: the "~200 KB/s" UART figure rounds down from ~246 KB/s of pure framing arithmetic —
conservative in the safe direction, and flagged everywhere as unmeasured anyway).

### Defects found and fixed (commit `b8531f9`)

- **"Decision 6" meant two different things.** `windows-helper-constraints.md` uses "standing
  decision 6" correctly for map #31's cursor-placement decision, but both HID transport research
  documents used "decision 6" for the exclusive-ownership control, which actually lives in #34.
  Both now point at #34.
- **#63 declared its blockers (#25, #39) in prose only.** The repo convention makes native GitHub
  dependencies the canonical gate, so a frontier query would have offered #63 prematurely. Both
  `blocked_by` edges now exist.

### Four amendments adopted after review (commit `e9cf15d`; comments on #34, #42, #63; #42's body rewritten to match)

1. **The pairing window and the exclusivity race compose into an attack path** — this answers the
   open question above ("what can a hostile process that *wins* that race actually do"): it waits
   for the user to press the config chord, because "not paired — press the config chord" was the
   documented remedy for exactly the state the attacker causes. The chord protects against a
   *remote* process opening the window, not against a *local* one being the thing provisioned.
   Fixed in posture, not mechanism: a refused open now surfaces as "another program holds the
   channel" and never prompts the chord; pairing success is confirmed visibly by the helper; #46
   decides secret rotation per window. Full analysis in the #34 comment.
2. **ADR-0001's WinUSB rejection rationale was stale.** "Requires administrator rights" contradicted
   #7's own finding (MS OS 2.0 descriptors bind WinUSB in-box, no admin; rejected then as "strictly
   more work on both sides"). The row now records the reasons that hold — extra work both sides,
   and `hdlpdbk` registered on the USBDevice class where WinUSB devices land — plus the genuine
   forgone cost: WinUSB's ~1 MB/s bulk endpoints would have made ADR-0002 unnecessary.
3. **Seams 1 and 2 consolidated to one shared C core** (frame codec and transfer state machine),
   compiled into the firmware and linked into both helpers, instead of three independent
   implementations. The golden-vector file keeps its exact role; the surface it must keep honest
   drops to one implementation plus thin bindings. mkroamer's per-platform codec ports are not
   reused — the re-cast message set meant they needed substantive rework anyway.
4. **ADR-0002 amended: `N = 2` is the default candidate for the channel raise, not an off-ramp.**
   The latency benefit arrives fully at two channels; the third buys ~64 KB/s against the UART wall
   while carrying the longest descriptor and a third exclusive acquisition, so it now needs #39 to
   make the case. The chunk-size choice also gained the loss-amplification constraint this brief
   raised: a 4 KiB chunk is a 512-packet retransmission unit; ~512 bytes costs ~2% more overhead
   for 8× less amplification.

### Status of this brief's open questions

- 4 KiB frame maximum vs 8-byte inter-board packets — **recorded as a binding constraint on the
  chunk-size choice** in ADR-0002 (amendment 4). Not resolved; #39 still sets the number.
- ~256 KB eager/lazy threshold — **still open on #55**, unchanged.
- Exclusivity × late helper start — **partially addressed** by amendment 1 (the remedy-separation
  closes the provisioning path; the DoS-by-holding remains accepted and stated).
- Hostile race-winner — **answered**; see amendment 1.
- Nothing has been built — **still true.** The soft-claims ranking above stands, with one stake
  lowered: if the 3× striping arithmetic is wrong, the amended default (`N = 2`) loses less than
  the original plan did.
