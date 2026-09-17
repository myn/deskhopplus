# deskhopplus

A hard fork of DeskHop: a two-board USB keyboard/mouse switch extended with a 2-D screen layout,
per-direction key remapping, configurable hotkeys, and clipboard sharing via companion helpers.
This glossary is the ubiquitous language; decisions live in `docs/adr/` and the wayfinder maps
(issues #1 and #31).

## Language

### The channel

**Channel**:
The helper↔firmware link — a vendor-defined HID interface (up to three, negotiated) carrying
clipboard payloads and cursor placement.
_Avoid_: serial port, CDC, pipe

**Helper**:
The companion application on each computer (Swift on macOS, C++ on Windows) that reads and writes
the pasteboard and places the cursor. An enhancement, never a dependency.
_Avoid_: agent, daemon, client, companion app

**Presence**:
The helper's permanent place in the menu bar or the tray: it carries the state in words, the file
question and the progress, and is there whenever the helper runs. Its look changes with the state,
and a look never replaces the words (#38).
_Avoid_: icon, status item, menu-bar item, tray icon (each of those is one platform's half of it)

**Link**:
The physical USB attachment between a computer and its board. Losing it takes the connection and
the session with it; neither of those falling over touches the link. The board-to-board one is
always written **inter-board link** in full — bare "link" is the helper↔firmware one.
_Avoid_: connection, session, cable

**Peer board**:
The other board of the pair, reached across the inter-board link. Each board knows only what its
peer tells it. Written **peer board** in full wherever the boards are meant, because bare "peer"
belongs to the session, where it is the authenticated other end of the channel — the two are
different things at different layers, and the same word for both hides which one a rule governs.
_Avoid_: other device, remote board, secondary, slave

**Connection**:
The channels a helper holds open, all-or-nothing. It can be dropped and reopened without the device
going anywhere, which is a helper's ordinary recovery from a session it has lost. Holding a
connection is not relied on to keep a **listener** out.
_Avoid_: session, link, socket

**Listener**:
A second program attached to the same channels as the helper. It receives every frame the helper
receives, and the frames it writes are ones the device cannot tell from the helper's. Refused on
Windows. On macOS it depends on the version: a second seize succeeded on 2026-08-13 (#95) and is
refused on macOS 15.7.9 (#191, 2026-09-14). The protocol is written as if it is real.
_Avoid_: intruder, second client, second helper, eavesdropper, attacker

**Session**:
The negotiated state established by the hello exchange: the agreed protocol version, channel count
and chunk size, plus the **session keys** every frame after it is authenticated under. It ends when
either end times the other out, on a protocol error, or when a config wipe clears the registration
it was negotiated against. A session can be gone while the connection and the link are both
perfectly healthy — that gap is the whole reason these are three words and not one.

Holding one is not what authorises a frame. Authorisation is **per frame**, on the tag: a board with
a live session still drops a frame that does not carry one.
_Avoid_: connection, link, pairing (pairing is what authorises a session, not the session itself)

**Registration**:
The board's record of exactly one helper public key, written during a pairing window and
cleared by a config wipe. Pairing is the gesture, a session is what the registration lets a
helper negotiate, and the registration is the durable fact in between — a board can hold one
while no helper is attached at all.
_Avoid_: pairing (the gesture), enrolment, trust store

**Correlation value**:
The fresh random value a helper puts in a hello or a pairing request, which the board echoes in
its answer. A helper acts only on an answer carrying its own. It is what stops one client's
refusal being read by another as its own, which is how a listener manufactured the chord trap
(#108).
_Avoid_: nonce (a nonce feeds key derivation here and is a different field), token, request id

**Frame**:
The logical unit of the wire protocol: a type, an authentication prefix (every type but the
handful that precede a session), and a body. One frame carries exactly one message — a hello, a
credit grant, a chunk. A frame never shares a **report** with another frame: it always begins at
a report boundary and pads out to fill however many reports it takes to carry it.
_Avoid_: message, packet (a packet is the 12-byte inter-board wire unit), report (a frame's
carrier, not the frame itself)

**Report**:
The physical carrier: one fixed 64-byte HID exchange, with no ID, length or sequence of its own.
Byte 0 is a **frame-start flag** — `1` when a frame begins in this report, `0` when it continues
the one in progress (ADR-0012) — and the framing layer owns the other 63. A small frame fits in
one; a chunk spans many, back to back. The flag is the one fact a reader cannot recover on its
own once the report carrying a header is gone; with it, a lost report costs the frame riding it
and nothing after, and the reader counts each **resync** — a report or partial frame discarded
to get back to a frame start.
_Avoid_: packet, message, frame (a report carries a frame; it is not one)

**Opaque relay**:
The firmware's role on the channel: it parses frame headers only — never payloads — and forwards
fragments between the USB and inter-board links. Clipboard format changes never touch firmware.
_Avoid_: proxy, gateway

**Tag**:
The 16-byte authenticator on every frame of one hop, with a monotonic counter beside it. It is
hop-local: a board verifies the tag its helper wrote and writes a fresh one toward the far
helper. A tag says who wrote this frame **on this hop**, which is the question a shared endpoint
made unanswerable.
_Avoid_: signature, MAC, checksum, CRC (the CRC32 is fidelity, not authentication)

**Seal**:
The encryption of a bulk payload between the two **helpers**, end to end. Neither board holds a
key that opens it, so the opaque relay becomes a property rather than a discipline. Placement is
authenticated but never sealed.
_Avoid_: encryption (unqualified), envelope, wrapping

**Fidelity**:
The clipboard channel's binding content guarantee: the wire payload is byte-identical end to end
(CRC32-verified), and no component validates, filters, normalizes, or rejects content. The only
transform anywhere is the platform's native pasteboard encoding conversion at each edge.
_Avoid_: sanitization, validation, verbatim (informal — this is the canonical term)

### Clipboard behaviour

**Copy side / Paste side**:
The machine where content was copied, and the machine where it is pasted. Confirmation prompts
and lazy transfers belong to the paste side.
_Avoid_: source/destination machine, sender/receiver

**Eager / Lazy / Prefetched**:
When bytes move: eager content transfers on copy, lazy content transfers only once they are asked
for, and prefetched content transfers when the paste side accepts an offer but before an
application pastes it. Large images are prefetched on both paste-side platforms. Files are lazy on
the copy side — never read until requested — and their request waits on the **acceptance** below,
so a copy the user never pastes costs one small frame and opens nothing.
_Avoid_: push/pull, immediate/deferred

**Acceptance**:
The paste side's user agreeing to a file transfer, in the menu bar or the tray, which is what
starts it (ADR-0011). Not a paste: both platforms' paste-triggered APIs block the pasting
application for the whole transfer, which at the measured rate is minutes. A set at or below the
prompt threshold is accepted without asking.
_Avoid_: confirmation (the dialog is one way of asking; the acceptance is the decision), approval,
consent

**Size cap**:
The largest payload a helper will assemble, stated by the board in `CLIP_POLICY` — 10 MB by
default and up to 64. The device is the single source of truth for it, as for every setting, so no
helper holds a cap of its own.
_Avoid_: limit, maximum size, quota

**Direction toggle**:
One of the two settings that turn clipboard sharing off for A→B or B→A, both defaulting on.
They live on the device, which is the single source of truth for settings; a helper holds no
toggle of its own and is told instead what its own two verbs are — whether it **may send** what
was copied here, and whether it **may write** what arrives. The two namings are deliberate: a
helper cannot act on a direction, because it has no way to know which end of "A to B" it is.
_Avoid_: clipboard enable, sharing flag, direction flag

**Chunk**:
The unit of integrity, loss detection, and selective retransmission — exactly one frame's payload,
carrying id, length, and CRC32.
_Avoid_: block, segment, packet (a packet is the 12-byte inter-board wire unit)

**Bundle**:
One copy carried as more than one representation of the same content — its plain text and the
picture an application drew of it — so that the pasting application picks, as it would from a
native clipboard. One offer, one transfer, one kind. The copy side builds one only when both are
present and they fit the eager threshold together; past that, the text travels alone and the
picture does not travel (ADR-0013).
_Avoid_: multi-format, rich copy, both formats

**Offer**:
The immutable announcement of one clipboard transfer: its transfer id, kind, total length, and
metadata. Repeating the same offer is a retry of that transfer; reusing its id with any different
field is a protocol error, never a newer transfer. For a lazy file the total is the length that
will be sent, measured at the copy — not a snapshot of the file's contents at that moment.
_Avoid_: request (the paste side requests an offered transfer), proposal, header, snapshot

### Layout

**Seam**:
The configured boundary between the two computers' screen areas, crossed by pushing the cursor
into it. Mapped as ranges keyed by chain index.
_Avoid_: border (except in "border direction"), edge

**Chain axis / Border direction**:
The two independent per-output facts replacing DeskHop's single `pos`: which way a computer's own
monitors extend, and where the other computer is.
_Avoid_: orientation, position

**Monitor**:
One display a computer drives. Monitors are numbered from the main monitor outward along the
chain axis, so monitor 2 is the next one along and monitor 3 the one after. A computer's monitors
always form one straight line.
_Avoid_: screen (the firmware's word for the same thing; the page and the guide say monitor),
display, output (an output is the computer)

**Main monitor**:
The monitor the computer's own operating system calls main. It is monitor 1, and the only one
that can cross when the chain axis points at the other computer. The user does not choose it
here; the layout only says where it sits.
_Avoid_: primary, first screen, screen 1

**Segment**:
One pairing of a stretch of a monitor's edge on A with a stretch of a monitor's edge on B.
Segment n on A meets segment n on B, and the cursor leaves at some fraction along one and
arrives at the same fraction along the other. A monitor with no segment does not cross.
_Avoid_: range (one side of a segment), pair, link, mapping

**Layout**:
The picture of both computers' monitors on the config page. Every fact the board needs — chain
axis, border direction, monitor count and segments — is derived from where the boxes sit; the
picture holds nothing of its own. **Advanced** is the same settings shown one field at a time.
_Avoid_: arrangement, screen setup, map, grid (the grid is what the layout snaps to)
