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
connection does not keep a **listener** out.
_Avoid_: session, link, socket

**Listener**:
A second program attached to the same channels as the helper. It receives every frame the helper
receives, and the frames it writes are ones the device cannot tell from the helper's. Refused on
Windows, real on macOS.
_Avoid_: intruder, second client, second helper, eavesdropper, attacker

**Session**:
The negotiated state established by the hello exchange: the agreed protocol version, channel count
and chunk size, plus the authentication that makes the peer one anything may be relayed for. It
ends when either end times the other out, on a protocol error, or when the pairing chord rotates
the secret. A session can be gone while the connection and the link are both perfectly healthy —
that gap is the whole reason these are three words and not one.
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

**Eager / Lazy**:
When bytes move: eager content transfers on copy (text, small images); lazy content transfers on
paste (files, large payloads).
_Avoid_: push/pull, immediate/deferred

**Chunk**:
The unit of integrity, loss detection, and selective retransmission — exactly one frame's payload,
carrying id, length, and CRC32.
_Avoid_: block, segment, packet (a packet is the 12-byte inter-board wire unit)

### Layout

**Seam**:
The configured boundary between the two computers' screen areas, crossed by pushing the cursor
into it. Mapped as ranges keyed by chain index.
_Avoid_: border (except in "border direction"), edge

**Chain axis / Border direction**:
The two independent per-output facts replacing DeskHop's single `pos`: which way a computer's own
monitors extend, and where the other computer is.
_Avoid_: orientation, position
