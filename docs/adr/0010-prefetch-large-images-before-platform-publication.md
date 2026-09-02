# ADR-0010: Prefetch large images before platform publication

- **Status:** Accepted
- **Date:** 2026-09-02
- **Resolves:** [#55](https://github.com/myn/deskhopplus/issues/55)

Each paste side prefetches a large remote image without claiming its platform clipboard, then
publishes real image data only after the transfer completes and only if no newer local copy exists.
Windows background components request advertised delayed `CF_BITMAP` data for previews within
milliseconds, turning a nominally lazy image into a synchronous minute-long clipboard lock. A macOS
`NSPasteboardItemDataProvider` instead blocks the pasting application synchronously while the same
slow transfer runs, producing the observed beachball. The working `myn/mkroamer` helpers establish
the same receive-then-publish boundary on both platforms.

The copy side still retains the payload until `CLIP_REQUEST`, so offer retry and cancellation remain
unchanged. The request is automatic rather than paste-triggered. A local copy during prefetch
cancels the receive when observed; the final write also rechecks the Windows sequence after opening
the clipboard, or the macOS `changeCount` immediately before and after preparing new contents.
Windows provides atomic exclusion once its clipboard is open. macOS offers no atomic
compare-and-replace: the helper detects an overlapping `changeCount` jump and stops stale
publication, but an overlap inside that API call may already have cleared the newer content.
Windows publishes PNG plus bitmap compatibility data; macOS publishes PNG with
`NSPasteboardContentsCurrentHostOnly`.
