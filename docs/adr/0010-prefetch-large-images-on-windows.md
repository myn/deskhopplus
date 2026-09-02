# ADR-0010: Prefetch large images on Windows

- **Status:** Accepted
- **Date:** 2026-09-02
- **Resolves:** [#55](https://github.com/myn/deskhopplus/issues/55)

Windows prefetches a large remote image without claiming the system clipboard, then publishes real
PNG and bitmap handles only after the transfer completes and only if the clipboard sequence has not
changed. This deliberately differs from macOS paste-triggered lazy images: Windows background
components request advertised delayed `CF_BITMAP` data for previews within milliseconds, turning a
nominally lazy image into a synchronous minute-long clipboard lock that prevents local file and text
copies. The working `myn/mkroamer` helper establishes the same receive-then-publish boundary.

The copy side still retains the payload until `CLIP_REQUEST`, so offer retry and cancellation remain
unchanged. On Windows the request is automatic rather than paste-triggered. A local Windows copy
during prefetch cancels the receive when observed; the final write also rechecks the sequence after
successfully opening the clipboard, immediately before `EmptyClipboard`, so a queued or racing local
copy is never overwritten.
