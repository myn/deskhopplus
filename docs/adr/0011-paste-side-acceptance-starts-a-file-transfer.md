# ADR-0011: The paste side's user starts a file transfer, not a paste

- **Status:** Accepted
- **Date:** 2026-09-02
- **Resolves:** [#56](https://github.com/myn/deskhopplus/issues/56)
- **Amends:** [ADR-0010](0010-prefetch-large-images-before-platform-publication.md)

A file transfer begins when the paste side's user accepts an offer in the menu bar or tray, not
when an application asks the platform for the file's bytes. The copy side stays lazy: file contents
are read only on `CLIP_REQUEST`, which now arrives only after that acceptance.

#56 asks for a transfer that "begins on paste, not on copy", and both platform APIs that would make
a paste itself the trigger fail the same way ADR-0010 already recorded for images. `CF_HDROP`
delayed rendering answers `WM_RENDERFORMAT` synchronously, so the pasting application blocks for
the whole transfer; the virtual-file alternative, `CFSTR_FILEDESCRIPTOR` with
`CFSTR_FILECONTENTS`, needs a full OLE `IDataObject` and is enumerated speculatively by Explorer
previews, which starts transfers nobody asked for. On macOS an `NSPasteboardItemDataProvider`
blocks the pasting application outright, and `NSFilePromiseProvider` is honoured inconsistently
outside Finder. At the measured ~49 KB/s of #39 a 10 MB paste is roughly three and a half minutes,
so any of these is a frozen application rather than a slow one.

Acceptance keeps what the user story wanted and drops what it assumed. Copying costs nothing: the
offer carries the file list alone, and a folder copied and never pasted is never opened. The
transfer then runs with progress and a cancel, and the files are published as ordinary references
once complete, so the paste itself is instant and no application is ever blocked.

**Sets at or below 1 MB are accepted without asking**, and that is a second deviation from the
same criterion rather than a detail of the first: their bytes cross on the copy, with no paste and
no decision. It buys back what acceptance costs for the common case — under a second at the
measured rate — and the alternative is worse than the deviation, because a dialog for a
quarter-second transfer is how the dialog that matters gets dismissed unread. Above the threshold
nothing crosses the link without an explicit decision on the receiving computer.

An offer put to the user expires after two minutes and is declined for them. Not because the
question is urgent: an offer accepted-as-lazy and never requested leaves the copy side re-offering
every two seconds for the life of the session (#78), and it pins the receive buffer the size cap
sizes, so the cap cannot change while the question stands.

The cost is that the gesture is a menu-bar or tray answer rather than Cmd-V or Ctrl-V, so a user
who ignores the prompt has files that never arrive. Both helpers therefore show the question in
their permanent presence and not only in a notification, which is also why #56 carries the first
slice of [#54](https://github.com/myn/deskhopplus/issues/54): before this the macOS helper had no
user interface at all.
