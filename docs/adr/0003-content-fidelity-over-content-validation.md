# ADR-0003: Content fidelity over content validation on the clipboard channel

- **Status:** Accepted
- **Date:** 2026-08-10
- **Resolves:** [#65](https://github.com/myn/deskhopplus/issues/65), closing [#61](https://github.com/myn/deskhopplus/issues/61) as evaluated-not-adopted
- **Constrains:** [#52](https://github.com/myn/deskhopplus/issues/52), [#55](https://github.com/myn/deskhopplus/issues/55), [#56](https://github.com/myn/deskhopplus/issues/56) (the clipboard slices), and the clipboard behaviour section of spec [#42](https://github.com/myn/deskhopplus/issues/42)

## Decision

The clipboard channel is **fidelity-preserving**: the wire payload is byte-identical end to end,
verified by the existing per-chunk CRC32, and **no component anywhere on the path validates,
filters, normalizes, or rejects clipboard content** — text, images, and files alike.

The only transform permitted anywhere is the **platform's native pasteboard encoding conversion
at each edge** (UTF-16 ↔ UTF-8 at the Windows clipboard boundary), because for text it is
physically unavoidable: the two operating systems' native clipboard encodings differ. Exactly one
conversion per edge, nothing else.

Consequently the chris-010 clipboard validation library ([#61](https://github.com/myn/deskhopplus/issues/61))
is **not adopted**, and the question #65 was opened to answer — *where* the validator runs —
dissolves: there is no validator. The opaque-relay principle of spec #42 stands untouched.

## Context

The 2026-08-10 map audit found that #61 proposed adopting a firmware-side text validator while
spec #42's load-bearing principle is that the firmware parses frame headers only, never payloads.
#65 was opened to decide where the validator should run. The grilling session that resolved it
surfaced a requirement senior to the location question: **the user requires the clipboard to carry
content verbatim, agnostic to its source** — prose out of Word with curly quotes and em-dashes,
code, logs, CJK, emoji, arbitrary files.

The candidate library's design philosophy is the opposite: strict codepoint whitelisting
(printable ASCII, Latin-1 Supplement, Latin Extended-A) that **rejects what it does not
understand**. Under that profile, an ordinary paragraph from Office would be refused. The
philosophy is sound for the firmware-resident, ASCII-adjacent clipboard it was written for; it is
wrong for a general clipboard between a Mac and a Windows machine.

**mkroamer is the behavioural spec, and it already implements fidelity.** Its wire format defines
clipboard text as UTF-8 (`CLIP_OFFER kind 0`), its Windows helper converts `CF_UNICODETEXT` to and
from UTF-8 at the pasteboard boundary (`windows/src/clipboard/clipboard.cpp:286-293`) and performs
no other transform — no whitelist, no rejection, no line-ending normalization. Files and images
are opaque byte streams. This ADR records that behaviour as deliberate rather than incidental.

## The malformed-text edge

Invalid UTF-16 (an unpaired surrogate) on the Windows clipboard has **no** verbatim UTF-8
representation, so a policy is forced. Decided: **best-effort OS-default conversion, silently, as
mkroamer does** (`MultiByteToWideChar`/`WideCharToMultiByte` with default flags) — the same
behaviour nearly every Windows application exhibits. The alternative, strict conversion with a
visible per-item failure, would refuse a paste over a defect the user cannot see or fix. The same
policy covers file names in the file-list metadata.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Adopt the chris-010 whitelist validator** (firmware- or helper-side) | Rejects everyday content — curly quotes, em-dashes, €, CJK, emoji. Incompatible with the fidelity requirement regardless of where it runs. Rejected on requirements, not on code quality; its evaluation (#59) already paid for itself by prompting ADR-0001's transport finding, #62, and #63's U2 risk. |
| **Structural validation** (well-formed UTF-8, strip C0/C1 controls, reject bidi overrides) | Still modifies or refuses content, so still breaks verbatim. The threats it addresses are carried by every clipboard on earth. |
| **Detect-and-warn layer** (flag suspicious content, never block) | Compatible with fidelity, but buys complexity before the feature exists. **Deliberately rejected for v1** — recorded here so a security review finds a decision, not an omission. Revisit only with evidence of the threat mattering on this desk. |

## What protects the receiving machine instead

Integrity and trust are **channel-level, not content-level**: the device-held pairing secret and
exclusive channel ownership (#34, ADR-0001) decide *who* may speak; per-chunk CRC32 end-to-end
guarantees that what was copied is what arrives; the paste-side confirmation (#56) covers consent
for large transfers; and received content is written host-only on macOS. A reviewer asking "why is
there no content sanitization on a cross-machine bridge?" should read this ADR: filtering was
considered and rejected because a clipboard that silently alters or refuses content is a broken
clipboard, and the containment argument that motivated firmware-side validation is void while
[#62](https://github.com/myn/deskhopplus/issues/62) stands (a compromised board can flash its
peer, so an egress-board validator is not a boundary).

## Consequences

- **#61 closes** as evaluated-not-adopted; its `CB_ALLOW_LF`, `CB_MAX_PAYLOAD` and RAM-budget work
  items are moot. The LF question dissolves with it — there is no filter to configure.
- **#52 loses its #65 blocker** and gains this constraint: received text is written to the
  destination pasteboard exactly as read from the source, modulo the single edge conversion.
- **Spec #42's opaque-relay principle is reaffirmed verbatim** — the firmware parses frame
  headers only, never payloads, with no carve-outs.
- **#62 is unchanged but sharpened**: it now stands purely as a firmware-security question, no
  longer entangled with a clipboard containment argument.
- The glossary (`CONTEXT.md`) defines **Fidelity** as the canonical term for this property.

## Amendment, 2026-09-01 — PNG is the cross-platform image edge encoding

Issue #55 makes the platform-edge image conversion explicit. The wire representation for kind 1
is PNG. A screenshot pasteboard that exposes only macOS TIFF or Windows bitmap data is encoded to
PNG once at the copy-side platform boundary; the exact PNG bytes then cross the channel and are
CRC32-verified without modification. The paste side publishes those bytes as PNG on macOS and as
the native Windows bitmap representation. This is the image equivalent of the text edge encoding:
it is required because the platforms have no shared native clipboard representation, and it does
not permit content validation, filtering, normalization, or firmware inspection.

“Files and images are opaque byte streams” therefore governs the channel after this edge encoding,
not the platform-native object before it. A PNG already present on macOS is carried as-is.
