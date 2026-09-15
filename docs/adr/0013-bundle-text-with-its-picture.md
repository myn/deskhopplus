# ADR-0013: Bundle text with its picture in one transfer

- **Status:** Accepted
- **Date:** 2026-09-14
- **Resolves:** [#195](https://github.com/myn/deskhopplus/issues/195)

Every Office application puts a rendered picture of a text selection on the clipboard beside the
text, and a copy side that sends one kind per copy must choose. mkroamer chose differently on each
platform (text first on Windows, image first on macOS) and deskhopplus unified both on the macOS
order, so text copied in PowerPoint on Windows arrived on the Mac as a picture of the words and
pasted into a text field as nothing (#195). The spec had carried format precedence as recorded fog
since #42.

When a copy holds both text and a picture, the copy side now sends both in **one transfer** as a
new offer kind, 3 = **bundle**: a list of `part_kind:u8 len:u32 bytes` parts reusing the offer
kinds, so the pasting application picks the representation it wants, as it would from a native
clipboard. The bundle is built only while text plus picture fit the eager image threshold
(256 KiB); above that the picture is dropped and the text goes alone, so text is never more than
about two seconds late at the measured ~128 KB/s of two channels, which is the promise story 49 of
#42 makes. Firmware is untouched: the kind is a byte it relays and never reads.

**One transfer, not two.** Sending the text eagerly and the picture as a second transfer would keep
text instant for any picture size, but a helper holds exactly one pending copy — a new copy replaces
it — and the paste side would need to add a picture to a pasteboard entry it wrote earlier only if
that entry is still its own. That is a queue plus a compare-and-republish on both platforms, and
macOS has no atomic form of the latter (ADR-0010). A bundle rides every existing path unchanged
and the cap bounds its cost. Revisit if a picture beside text that exceeds 256 KiB turns out to be
something people miss.

**A list, not a pair.** The bundle is typed parts rather than a fixed text-then-PNG layout so that
RTF and HTML, deliberately left out of the first cut, arrive as new part kinds and not as new offer
kinds or a wire change.
