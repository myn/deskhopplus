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
