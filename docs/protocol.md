# deskhopplus channel protocol — v2

The single source of truth for bytes on the helper↔firmware channel. Any change here must
update `test-vectors/frames.txt` and the shared C core (`src/core/`) in the same change.

v2 replaces v1's bearer token with a key pair per side, an authentication tag on every frame,
and a clipboard payload sealed between the two helpers. It is
[ADR-0008](adr/0008-channel-identity-and-sealed-clipboard.md)'s wire half, decided on
[#95](https://github.com/myn/deskhopplus/issues/95) and specified by
[#109](https://github.com/myn/deskhopplus/issues/109). The framing is unchanged, so the codec
and the relay are unchanged; what changed is what sits inside a frame's payload and what a
board will act on.

**Old pairings do not migrate.** A migration path would have to accept the old bearer token,
which is the thing being removed. Recovery is one chord press, by design.

> **Sequencing.** The **board speaks this document** as of
> [#111](https://github.com/myn/deskhopplus/issues/111): `DH_PROTO_VERSION` in
> `src/core/dh_session.h` is `2`, and it moved with `dh_session.c` rather than with this file,
> because a board that announced version 2 while speaking v1 would be worse than one that
> announces the version it actually speaks.
> [#110](https://github.com/myn/deskhopplus/issues/110) landed the primitives underneath —
> P-256, HKDF, the frame tag and the counter, in `src/core/dh_p256.c`, `src/core/dh_sha256.c`
> and `src/core/dh_auth.c` — without touching the codec, which is why the constant did not move
> with it either.
>
> **The macOS helper speaks this document** as of
> [#112](https://github.com/myn/deskhopplus/issues/112): it holds a Secure Enclave identity, pins
> the board's public key, and authenticates every session-band frame. `src/core/dh_session_v1.[ch]`
> is deleted — there is one wire format again.
>
> **The Windows helper speaks this document** as of
> [#84](https://github.com/myn/deskhopplus/issues/84): a DPAPI-held key pair, the same pairing
> gesture, and the same per-frame tags, over the shared core rather than a second implementation
> of it. A v1 helper cannot pair with a v2 board, which is this document's own rule rather than a
> defect — old pairings do not migrate. `#109` shipped this spec first because #111, #112 and #84
> are all written from it — that is what
> [#97](https://github.com/myn/deskhopplus/issues/97) Stage 2 exists to say.
>
> **The seal exists** as of [#113](https://github.com/myn/deskhopplus/issues/113):
> `src/core/dh_seal.c` holds the exchange, the key agreement, the counter and the layouts, and
> `src/core/dh_clip.c` splits each bulk message into the clear head a board routes on and the
> plaintext only the far helper sees. The cipher is each platform's — CryptoKit in
> `helpers/macos/Sources/DeskhopChannel/Seal.swift`, CNG in
> `helpers/windows/src/seal_aead.cpp` — so no board and no core links an AEAD, and each cipher is
> gated against `clip_offer_text2` and `clip_chunk_hi` in its own suite. What is still to come is
> the clipboard itself (#52, #55, #56): this is the layer those carry their bytes in.

## Assumptions

The transport (vendor HID channels per [ADR-0001](adr/0001-vendor-hid-transport.md) /
[ADR-0002](adr/0002-parallel-hid-channels.md), striped over the inter-board link) delivers
an **ordered byte stream per direction**. Loss and integrity are handled *above* framing:
the chunk CRC32 and selective retransmission live in the `CLIP_*` messages, end-to-end
between the helpers. The framing layer is transport-neutral and must stay that way — the
golden vectors survived the CDC→HID move untouched, and a vector that depends on the
transport is a sign the framing layer has leaked.

**The channel is shared, not exclusive.** On macOS a second
`kIOHIDOptionsTypeSeizeDevice` open succeeds (#95, measured 2026-08-13), so the **listener**
of `CONTEXT.md` is a real thing on that platform and every rule below is written for it. The
leverage a listener has is asymmetric, and v2 is shaped by the asymmetry:

- **Device→helper frames reach every attached client.** A listener reads them all. This is why
  the clipboard payload is sealed and why refusal messages must be safe to overhear.
- **Helper→device frames are unattributable.** Every client writes into the same endpoint and
  the board cannot tell them apart. This is why authorisation is **per frame** and not per
  session: v1 gated relay on `dh_session_may_relay`, one flag for the whole board, so any
  process could push bulk into a session it never authenticated.
- **A listener cannot make a frame arrive at the helper.** Only the device emits
  device→helper reports. A listener's leverage on a helper is entirely indirect: it makes the
  *board* send something the helper misreads. Closing that is what the correlation value and
  the board's silence below are for.

Windows is unaffected — `hidclass.sys` refuses the second open, measured — but there is one
wire on both platforms. A second, weaker format would mean two state machines, two sets of
golden vectors, and #84 implementing the weaker one on the managed laptop.

## Framing

All integers little-endian.

```
offset  size  field
0       1     type    (message type, below)
1       1     flags   (reserved, 0 in v2; preserved verbatim by codecs)
2       2     len     (payload length in bytes, u16; max 4096)
4       len   payload
```

A frame with `len > 4096` or an unknown `type` is a protocol error: log, drop the
connection, reconnect.

### The authentication prefix

Every frame **except types `0x08`–`0x0F`** begins its payload with a 24-byte prefix:

```
offset  size  field
0       8     counter  (u64, monotonic, per key)
8       16    tag      (HMAC-SHA256, truncated to 16 bytes)
24      ...   body     (the message layout in the registry below)
```

The prefix is **inside `len`**. That is the whole reason it is placed here rather than in the
header: a frame is still a 4-byte header and a payload, so `dh_frame.c`, the incremental
reader, the relay, and the report carrier are all untouched by v2, and the firmware's
header-only view of a frame (**opaque relay**) is unchanged.

The counter comes first so that a board can reject a replay after reading 12 bytes, without
buffering a payload it is going to throw away.

**What the tag covers**, exactly:

```
tag = HMAC-SHA256(key, type ‖ flags ‖ len ‖ counter ‖ body)[0..16]
```

— that is, the 4 header bytes as they appear on the wire, then the 8 counter bytes, then the
body. The tag does not cover itself. Binding `type` and `len` is what stops a frame being
re-typed or truncated into a different message with the same tag.

Sixteen bytes, not eight: ADR-0008 rejected truncation to save 8 bytes on small frames, on the
grounds that [ADR-0002](adr/0002-parallel-hid-channels.md) makes the UART the wall and a
truncated MAC ages badly. The honest cost is that a `PLACE` frame goes from 8 bytes to 32 —
four times the size, still one report, on a message that is not sent at input rates because
input rides HID and never this channel.

### Counters

One counter space **per key**, so per direction and per session. It starts at 0 for the first
frame sent under a key and increases by at least 1 per frame.

A receiver keeps the highest counter it has accepted for that key, plus a bitmask of the
**64 counters ending at it**, and refuses a counter it has already accepted or one older than
that window. Gaps ahead of the highest are ordinary and always accepted: the device's outbound
path is a short bounded queue ([ADR-0005](adr/0005-bounded-outbound-queues.md)) and a frame it
cannot take is a silent loss, so a gap is not evidence of an attack.

**Why a window and not "strictly greater".** Counters rise, but frames do not arrive in the
order they were tagged. The board tags a frame when it is *built* and the same outbound queue
then reorders: a priority frame overtakes bulk that is merely queued, which is the entire point
of the band split. So a clipboard frame tagged at N reaches the wire behind a heartbeat tagged
at N+1, and a strictly-greater receiver drops it.

Both rules were deliberate and they contradicted each other. Nothing could notice until
something put a bulk frame in that queue, and the first payload to do so was
[#52](https://github.com/myn/deskhopplus/issues/52)'s clipboard — where every transfer stalled
in both directions while the session itself looked healthy. The helper log said it outright:
*dropping a clip_offer with a counter already seen*. This is the shape IPsec and DTLS use, for
the same reason.

**What is not relaxed is replay.** A counter accepted once is refused for ever after — that is
what the bitmask is for, and it is why tolerating a gap does not tolerate a replay into it. A
counter older than the window is refused outright rather than searched for: past that distance
the record no longer exists, and accepting on an absence of evidence is exactly what a replay
window exists to prevent. The window is sized far above the real reorder distance, which is
bounded by that queue — two staged frames behind one in flight.

A counter that would wrap is not a case to handle. It is 64 bits wide and the keys do not
outlive a session; #107 measured a session lasting about 98 seconds on average.

### What an unauthenticated frame causes

Three things, in this order, and nothing else:

1. **It is not acted on and not relayed.** This is the rule per frame, not per session. A
   board holding a live authenticated session still drops a bulk frame that does not carry a
   good tag, which is the isolation breach #34 exists to prevent and v1 left open.
2. **It is counted**, in a sliding window, separately from frames that fail for other reasons.
   A hello that fails its tag is always counted as the stronger signal, because it is always a
   hello naming the registered helper's key id: one naming any other key id was refused at step 2
   of *The hello exchange* and never reached the tag.
3. **Above a rate threshold it is reported** to the helper as `DH_MSG_LISTENER_ALERT`, carrying
   the window and the count. A rate says what no single event can — the shape
   [#94](https://github.com/myn/deskhopplus/issues/94) established for `reconnectingRepeatedly`.
   The threshold is a firmware constant, owned by #111, not a wire-format fact.

**The board never answers a frame whose tag failed to verify.** Answering is acting, and the
answer would reach every attached client. See *The hello exchange* for why this specific silence
is what closes [#108](https://github.com/myn/deskhopplus/issues/108).

That is narrower than "never answers an unauthenticated frame", and the difference matters. Two
refusals *are* sent to frames that were never authenticated — `HELLO_REFUSED(unpaired)` and
`PAIR_REFUSED` — and both are deliberate. Neither can be provoked into saying anything false: a
board that did not register **the key id this hello names** really is unpaired as far as that
asker is concerned, and a board with no open window really will not pair. A **failed** tag is the
case that must stay silent, because there the board holds *this asker's* key, the sender cannot
prove it, and any answer is a statement about a helper that did not ask for it.

The `unpaired` refusal is **per key id, not per board** ([#117](https://github.com/myn/deskhopplus/issues/117)),
and that widening was weighed against this same "safe to overhear" argument before it was made:

- **It cannot say anything false.** "I hold no registration for this key id" is a statement about
  the board's own registration, exactly as "I hold no registration" was. It says nothing about any
  helper, and nothing about whether some *other* key is registered beyond the bare fact that this
  one is not.
- **It creates one oracle, and that oracle is not reachable.** Refusal-versus-silence does tell an
  asker whether a given key id is the registered one — a distinction that did not exist before,
  when both cases drew silence. To use it you must already hold the key id, and a key id is
  `SHA-256(public key)[0..8]`: 64 bits, so probing for it is a 2^64 search over USB HID. It cannot
  be read off the wire either. Nothing in *The channel is shared, not exclusive* lets a listener
  read a **helper→device** frame — that direction is unattributable, meaning a listener can *write*
  it, not that it can see one — and the helper's key id appears in no device→helper frame at all.
  A process that can obtain the helper's public key some other way, by reading the helper's own
  files as the same user, already knows the answer without asking the board.
- **#108's trap stays closed**, and it rests on the same asymmetry. A listener cannot read the real
  helper's hello, so it cannot copy that helper's correlation value; the refusal it provokes
  carries its *own*, and the real helper discards it. This is exactly the argument that already
  licenses `HELLO_REFUSED(version_incompatible)` — which any client can provoke on any board, with
  no tag checked — so `unpaired` joining it adds no new reach. Were that asymmetry ever to fail,
  `version_incompatible` would fall first and the correlation defence would need rebuilding
  wholesale; this refusal is not what would break it.
- **It is not counted** towards `DH_MSG_LISTENER_ALERT`, for the reason the board-wide refusal
  never was: this is the honest recovery path, and a genuinely unpaired helper retrying it must not
  manufacture an alert about itself.

  That does narrow what the alert sees, and the narrowing is accepted rather than overlooked. A
  hello naming an unregistered key id used to fail its tag and be counted; it is now refused and is
  not. What is lost is the detection of a listener sending hellos it *knows* will be refused —
  which gains that listener nothing, because only a hello naming the registered key id can lead to
  a session, and those still reach the tag and are still counted, as the stronger signal. The
  detection that matters is untouched.

What it buys is the case the board-wide test could not see: a board registered to *someone else*.
That helper now hears `unpaired`, reaches `notPaired`, and sends a `PAIR_REQUEST` — so a chord
press has something to provision, which is what ADR-0008 promises. Reached with no attacker at all:
a helper identity regenerated, a home directory restored onto a second Mac, or the board plugged
into a computer the first one had already registered.

`DH_MSG_LISTENER_ALERT` rides a session, so a board with no session has no one to tell. It keeps
counting; the alert is sent on the next session that authenticates, carrying the window it was
measured over.

**The helper's side of the same rule is simpler.** It ignores and counts a frame whose tag does
not verify, and answers nothing. It is not expected to see one: only the device emits
device→helper reports, so a bad tag there means the board is not the board this helper paired
with, or the byte stream is corrupt. Either way the answer is to drop the connection and
reconnect, not to reply.

The helper state this alert drives must never prompt the config chord. That rule is inherited
verbatim from the `channelHeld` state it replaces, which is now removed
([#72](https://github.com/myn/deskhopplus/issues/72),
[#114](https://github.com/myn/deskhopplus/issues/114), ADR-0008). The macOS helper asserts it in
`HelperState.swift`, under test.

### The report carrier

The channel's HID reports are a fixed 64 bytes with no report ID and no length field of their
own, so the framing layer owns every byte of a report. Frames are packed into that byte stream
back to back, and the tail of the last report of a batch is filled with **`0x00`**.

`0x00` is not a message type — the registry starts at `0x01` — so it cannot begin a frame. A
decoder skips it **between** frames and nowhere else: inside a frame it is ordinary payload,
accounted for by the length the header already gave. An all-padding report is idle traffic and
means nothing.

This is a property of a fixed-size carrier, not of the framing: it costs no golden vector, and a
carrier that already delimits its own records (the inter-board link's packets, a CDC stream)
never emits it. A frame is free to span reports and several now do — `HELLO` is 67 bytes.

## Bands

Ids are banded so the firmware's decisions are each **a single range test** on the
type byte, without reading a payload:

- **Priority:** `type < 0x30` → priority queue, drains strictly before bulk.
- **Routing:** `type >= 0x30` → bulk, relayed opaquely to the peer helper; everything
  below is addressed to (or emitted by) the firmware and is never forwarded.
- **Authentication:** `0x08 <= type <= 0x0F` → no authentication prefix; every other type has
  one. (`dh_msg_is_authenticated` in `dh_frame.h`.)

| band | range | contents |
|------|-------|----------|
| session | 0x01–0x07, 0x10–0x1F | hello, ack, heartbeats, session end, listener alert |
| unauthenticated | 0x08–0x0F | pairing, and the refusals a board has no key to tag |
| placement | 0x20–0x2F | placement, position query/response |
| bulk | 0x30–0x3F | clipboard transfer, its reliability machinery, and the seal exchange |

**Why the unauthenticated set is a band and not a status.** Whether a frame carries the prefix
has to be decidable from the type byte alone, before anything reads a payload — otherwise the
decoder needs the payload to know where the payload starts. That is the reason a refused hello
is its own message type (`HELLO_REFUSED`) rather than a status inside `HELLO_ACK`, which v1 did.

## Identity, pairing, and keys

### Identities

| Holder | Key | Storage |
|---|---|---|
| Board | P-256 key pair, generated on first boot | its **own flash sector**, beside `ADDR_CONFIG` and `ADDR_FW_RUNNING` and part of neither. Survives a config wipe and a firmware update. |
| Helper (macOS) | P-256 key pair | **Secure Enclave**, never extractable. |
| Helper (Windows) | P-256 key pair | **TPM-backed CNG**, falling back to DPAPI where there is no TPM. |

P-256 is not a preference. ADR-0008 records that the Secure Enclave only does P-256 and that a
private key a same-user process could read out of a file would undo the entire wire fix, so key
storage chose the curve; the board runs the same curve via `micro-ecc`.

A **key id** is `SHA-256(public key)[0..8]` — the first 8 bytes. It names a key on the wire
without carrying one.

Public keys cross the wire as **64 raw bytes, X ‖ Y, big-endian**: micro-ecc's native form and
CryptoKit's `rawRepresentation`, so no end has to add or strip an X9.63 `0x04` prefix.

### Pairing

The gesture is unchanged: the physical config chord, a 60-second window, nothing typed and
nothing displayed. What changed is what crosses.

1. The user presses the chord. The board opens the window.
2. A helper sends `DH_MSG_PAIR_REQUEST`, carrying a fresh random correlation value and **its
   public key**.
3. The board registers that key, computes one ECDH against its own private key, stores the
   resulting 32-byte shared secret in its config, and answers `DH_MSG_PAIR_GRANT` — carrying
   **the echoed correlation value and its own public key**, and nothing secret.
4. The helper computes the same ECDH and pins the board's public key beside its own identity.

**Only public halves cross, at any point, ever.** There is nothing in the pairing exchange for a
listener to take. This is the whole difference from v1, where `PAIR_GRANT` carried the secret
itself as cleartext.

- **The window is single-shot.** The first registration closes it. A listener can no longer be
  provisioned silently *alongside* the helper — if it wins the race it is registered and the
  helper is not, and the helper is told so with `DH_MSG_PAIR_REFUSED(already_registered)`. That
  failure is awkward but **visible**, which is the detection signal #34 asked for on 2026-08-10
  and never got.
- **A board holds exactly one registered helper key.** One board serves one computer. Several
  registrations would make "which one is legitimate?" unanswerable and eviction meaningless.
- **A grant a helper did not ask for is ignored**, on the correlation value. A listener's
  request produces a grant carrying the listener's correlation value, which the real helper
  discards.
- **A request outside a window is refused**, `DH_MSG_PAIR_REFUSED(no_window)`, so a helper never
  waits on an answer that is not coming.
- **A config wipe unpairs** — it clears the registration, not the board's own identity. If a
  wipe took the identity too, every wipe would make every helper report "this board changed",
  a false alarm on a routine action.

The board's asymmetric work happens **here and nowhere else**. `src/main.c:26` runs six jobs on
core 0 in one cooperative loop with no preemption, including the keyboard and mouse queues at
2000 Hz, and the watchdog budget is 500 ms (`src/include/watchdog.h:15`). A P-256 ECDH on this
chip is estimated at 80–200 ms and is unmeasured — #110 must measure it. During the pairing
window the board has just rebooted for the chord and already stalls to write flash, so the cost
is paid where a stall is already expected. Every session afterwards is a hash.

### Session keys

Derived per session by HKDF-SHA256 over the pairing-time shared secret and a fresh 16-byte
nonce from each side. **No asymmetric work per session** — this is the constraint that shaped
the design more than any other.

```
ss      = ECDH(helper private, board public)      # 32 bytes, the X coordinate, at pairing only
k_hello = HKDF(ikm = ss, salt = helper_nonce,               info = "deskhopplus/2 hello")
k_h2b   = HKDF(ikm = ss, salt = helper_nonce ‖ board_nonce, info = "deskhopplus/2 h2b")
k_b2h   = HKDF(ikm = ss, salt = helper_nonce ‖ board_nonce, info = "deskhopplus/2 b2h")
```

All three are 32 bytes. `info` strings are ASCII, no terminator.

`k_hello` exists because the hello has to be authenticated before the board's nonce is known —
the helper cannot use a key that mixes in a value it has not received yet. It authenticates
exactly one frame, the hello, and is discarded. Everything after the hello uses the direction
key for its direction, so a frame can never be replayed back at its sender.

**Forward secrecy is not provided**, and was not provided before. An attacker who later obtains
the board's flash *and* has recorded the traffic can read it. ADR-0008 records this deliberately
rather than leaving it to be discovered.

## The hello exchange

```
helper → board   HELLO          correlation, key id, helper nonce, tagged under k_hello
board  → helper  HELLO_ACK      correlation echoed, board nonce, tagged under k_b2h   (success)
board  → helper  HELLO_REFUSED  correlation echoed, status                            (untagged)
board  → (none)  silence                                                    (tag did not verify)
```

The board decides in this order, and stops at the first that applies. The order is part of the
specification, not an implementation choice — two of these conditions can hold at once, and
which answer is given tells the user which remedy to reach for.

1. **`proto_version` is one the board does not implement** → `HELLO_REFUSED(version_incompatible)`,
   untagged. First, because a board cannot verify a tag under rules it does not know.
2. **The board holds no registration for the `helper_key_id` this hello names** →
   `HELLO_REFUSED(unpaired)`, untagged. There is no secret to prove and the honest remedy really
   is the config chord. Per key id, not per board: a board registered to *someone else* is
   unpaired as far as this asker is concerned, and telling it so is what keeps the chord a remedy
   ([#117](https://github.com/myn/deskhopplus/issues/117)). See *What an unauthenticated frame
   causes* for why this stays safe to overhear.
3. **The tag does not verify** → **nothing at all**. Counted, per *What an unauthenticated frame
   causes*. This is the case that used to be `DH_HELLO_AUTH_FAILED`.
4. **Otherwise** → `HELLO_ACK`, tagged under `k_b2h`, counter 0.

The helper acts on an answer only if it **echoes the correlation value from that helper's own
hello**. Anything else is discarded without changing any state.

### What each end reads before it has verified anything

A tag cannot be checked without reading some of the frame it protects, so both ends read a
little of an unverified frame. Exactly what, and for what, is part of the specification.

**The board reads three fields of a hello, and nothing else:**

- **`proto_version`**, at body offset 0, so a version the board does not implement is refused
  rather than failed on a tag it could not have computed the same way.
- **`helper_key_id`**, at body offset 15, so the board knows whether the sender claims to be its
  registered helper. That decides two things: whether the hello is refused as `unpaired` at step 2
  above, and — for the ones that get past it — what the listener count in `DH_MSG_LISTENER_ALERT`
  means, rather than counting every stray frame alike.
- **`helper_nonce`**, at body offset 23, because `k_hello` is derived from it. **This read is
  what makes the tag checkable at all** — without it the board has no key to check against.

**The helper reads one field of a `HELLO_ACK` before verifying it:** `board_nonce`, at body
offset 14, because `k_b2h` mixes in both nonces. This is the mirror of the board's `helper_nonce`
read and it is safe for the same reason: an altered nonce yields a different key, so the tag
then fails. It is not an exception to the rule at *Session keys* that a key cannot mix in a value
its user has not received yet — the helper *has* received this one, in the very frame it is about
to verify.

In every case the field is read to **derive or select a key, or to choose a refusal**, and for
nothing else. No value read this way is recorded, acted on, or believed until the tag verifies.
A `PLACE` that does not authenticate does not move the cursor even though its coordinates were
parsed to check it.

### A retry is a new session, not a resumed one

A helper that is refused, or that hears nothing, retries with backoff — and a retried hello
carries a **fresh** correlation value and a **fresh** nonce. Every key above is therefore new,
so `HELLO` at counter 0 under `k_hello` and `HELLO_ACK` at counter 0 under `k_b2h` are never a
counter reuse. There is no resumption and nothing carries over from the attempt before.

### Why this closes the manufactured chord trap

[#108](https://github.com/myn/deskhopplus/issues/108) measured the v1 sequence on hardware on
2026-08-18: a listener sends a hello carrying a made-up token, the board correctly answers
`DH_HELLO_AUTH_FAILED`, that answer is an input report so it reaches the real helper, and the
helper — with no field tying an ack to the hello that asked for it — displays *"Not paired —
press the config chord"*. One frame was enough, and the state was sticky: the helper never
recovered on its own. The chord then provisions whatever is attached.

Two changes close it, and they are deliberately belt **and** braces, because the failure they
prevent is a user being told to hand over their pairing:

1. **The board is silent** when a hello does not authenticate. There is no answer to be
   overheard, so the sequence has no first step. `auth_failed` is not a status in v2 at all;
   status `1` is left reserved so that no v2 status can be misread as v1's.
2. **The correlation value must match.** A listener could still provoke a genuine
   `HELLO_REFUSED(version_incompatible)` by sending a hello with a nonsense version — that
   answer reaches every client — but it echoes the *listener's* correlation value, and the real
   helper drops it. Even if it did not, `version_incompatible` says "this board speaks a
   different protocol", which must not prompt the chord.

What this costs: the honest "this board no longer knows me" case — the board was wiped, or
re-paired to something else — is now reached by the helper's own timeout rather than by a
message. Slower, and worth it, because that message could not be told apart from a manufactured
one.

**A helper must retry.** #108 found that v1's helper set `phase = .live` on a refusal and never
sent the hello again, so a single manufactured rejection held the machine in the wrong state
indefinitely. A refusal, or silence, is retried with backoff.

## Message types

`dir`: h→d helper to device, d→h device to helper, h↔h helper to helper (relayed opaquely
by the firmware, which parses frame headers only, never payloads).

Every layout below is the **body** — what follows the 24-byte authentication prefix, except for
types `0x08`–`0x0F`, which have no prefix and whose body starts at offset 4 of the frame.

| type | name | dir | key | body |
|------|------|-----|-----|------|
| 0x01 | HELLO | h→d | `k_hello` | `proto_version:u16` `os:u8` (1=mac, 2=windows) `build_type:u8` (0=release, 1=development) `channel_count:u8` (requested) `max_chunk:u16` (requested, bytes) `correlation:u64` (fresh random, per hello) `helper_key_id:8` `helper_nonce:16` (fresh random, per hello). 39 bytes; counter is always 0. |
| 0x02 | HELLO_ACK | d→h | `k_b2h` | `correlation:u64` (echoed) `proto_version:u16` `build_type:u8` `channel_count:u8` (effective) `max_chunk:u16` (effective) `board_nonce:16`. 30 bytes; counter is always 0. Sent only on success — there is no failure form. |
| 0x03 | LISTENER_ALERT | d→h | `k_b2h` | `window_ms:u32` `refused:u32` — frames that failed authentication in that window. A rate, not an event. |
| 0x04 | CLIP_POLICY | d→h | `k_b2h` | `flags:u8` — bit 0 **may send** (clipboard copied on this computer may leave it), bit 1 **may receive** (content arriving may be written here). The board's answer to its own two direction toggles, for the helper on *its* side; `dh_clip_policy_for` is the single place the two are turned into each other. Sent once per session and again whenever the setting changes. A helper that has been told nothing allows both, which is what the toggles default to. |
| 0x05 | HEARTBEAT | h→d | `k_h2b` | empty (id kept from mkroamer) |
| 0x06 | DEVICE_HEARTBEAT | d→h | `k_b2h` | empty. The device's own beat, sent only while a session exists, so its absence is meaningful. Idle-gated — see Liveness. |
| 0x07 | SESSION_END | d→h | `k_b2h` | `reason:u8` (0=unspecified, 1=liveness_timeout, 2=protocol_error, 3=unpaired — the configuration holding the registration was wiped). An unknown reason reads as unspecified rather than as an error, so a later device may end a session for a reason this helper predates. |
| 0x08 | PAIR_REQUEST | h→d | — | `correlation:u64` `helper_pubkey:64`. Acted on only inside an open pairing window. |
| 0x09 | PAIR_GRANT | d→h | — | `correlation:u64` (echoed) `board_pubkey:64`. Nothing secret; the shared secret is computed independently at each end. |
| 0x0A | PAIR_REFUSED | d→h | — | `correlation:u64` (echoed) `reason:u8` (0=no_window, 1=already_registered — the single-shot window closed on someone else). |
| 0x0B | HELLO_REFUSED | d→h | — | `correlation:u64` (echoed) `proto_version:u16` (the board's own) `status:u8` (1=**reserved**, never sent — it was v1's `auth_failed`; 2=version_incompatible; 3=unpaired). |
| 0x10 | DEVICE_DROPS | d→h | `k_b2h` | Nine `u32` totals, since the board booted, in this order: `reports` (reports the USB callback could not hand to the channel task) `inbound` (frames from the peer board the channel task had not drained) `outq` (frames the outbound queue to this helper refused, **all bands**) `unsent` (frames this board could not hand on, to its helper or to the peer board — the sum of `outq` and `relay_q`, not a third seam) `orphans` (peer data packets with no start to attach to) `truncated` (peer frames abandoned because packets went missing) `relay_q` (frames the relay's own outbound queue refused) `outq_priority` (of `outq`, those the single-frame priority band turned away) `outq_bad_header` (of `outq`, those whose header did not parse — version skew, not congestion). 36 bytes. The bulk figure is `outq − outq_priority − outq_bad_header`, derived rather than sent so the frame stays inside the board's reply buffer. Sent once per session and again whenever a total moves — see Reporting what the device has dropped. |
| 0x20 | PLACE | d→h | `k_b2h` | `chain_index:u8` `border_direction:u8` (which side of the output the seam was crossed from) `entry_pos:u16` (0–65535 normalized along the seam segment). Fire-and-forget; no reply path. Authenticated, **not** sealed — coordinates cross in the clear. |
| 0x21 | POS_QUERY | d→h | `k_b2h` | empty |
| 0x22 | POS_RESPONSE | h→d | `k_h2b` | `chain_index:u8` `x:u16` `y:u16` (0–65535 normalized within that output; layout may be amended by #51/#53 with vectors in the same change) |
| 0x30 | CLIP_OFFER | h↔h | per hop | `id:u32` `seal_id:u32` `seal_counter:u64` `ciphertext:bytes` `gcm_tag:16`. Sealed plaintext: `kind:u8` (0=utf8-text, 1=png, 2=file-list) `total_size:u64` `meta_len:u16` `meta:bytes` (kind 2: UTF-8 JSON array of `{name,size}`). AAD is the 16 clear bytes. |
| 0x31 | CLIP_REQUEST | h↔h | per hop | `id:u32` |
| 0x32 | CLIP_CHUNK | h↔h | per hop | `id:u32` `seq:u32` `seal_id:u32` `seal_counter:u64` `ciphertext:bytes` `gcm_tag:16`. Sealed plaintext: `crc32:u32` (of `data`, the end-to-end integrity check) `data:bytes`. AAD is the 20 clear bytes. |
| 0x33 | CLIP_DONE | h↔h | per hop | `id:u32` |
| 0x34 | CLIP_CANCEL | h↔h | per hop | `id:u32` |
| 0x35 | CLIP_RETRANSMIT | h↔h | per hop | `id:u32` `seq:u32` — request selective retransmission of one chunk |
| 0x36 | CLIP_CREDIT | h↔h | per hop | `id:u32` `credits:u16` — the id makes a grant for a superseded transfer recognisably stale (#48) |
| 0x37 | SEAL_OFFER | h↔h | per hop | `seal_id:u32` (fresh random) `nonce:16` `eph_pubkey:64` |
| 0x38 | SEAL_ACCEPT | h↔h | per hop | `seal_id:u32` (echoed) `nonce:16` `eph_pubkey:64` |
| 0x39 | SEAL_STALE | h↔h | per hop | `seal_id:u32` — "I hold no key for this id." |

**Dropped from mkroamer:** PING/PONG, MOUSE_MOVE, MOUSE_BTN, WHEEL, KEY (input rides HID),
HANDOFF/HANDOFF_ACK (inverted into PLACE), RELEASE_CONTROL, RESET_MODIFIERS,
CONFIG_SYNC/CONFIG_ACK (configuration lives on the device).

**Dropped from v1:** the hello's `token` field, and `DH_HELLO_AUTH_FAILED` as a status.

## The sealed clipboard payload

Bulk payloads are sealed **helper to helper**. The boards relay ciphertext and hold no key that
opens it. This strengthens **opaque relay** from a discipline into a property: a board relays
bytes it *could not* read even if it wanted to. That matters because
[ADR-0007](adr/0007-inter-board-link-is-trusted-for-firmware.md) accepts that a compromised
board can flash its peer — a board is already a thing you cannot fully trust, and v2 declines to
also hand it the clipboard.

### Per hop versus end to end

These are two different layers and confusing them is the easiest mistake to make here.

- The **tag and counter are per hop.** Helper A's frame is authenticated to board A under
  `k_h2b` for that session. Board A verifies it, forwards it across the inter-board link, and
  board B emits it to helper B tagged under *board B's* `k_b2h` with *board B's* counter. The
  tag is rewritten at each hop; the counter restarts in each hop's own space.
- The **seal is end to end.** It is established between the two helpers and no board holds it.

### What each board vouches for

Helper B's trust in a public key that arrived through both boards is exactly this chain, and it
is worth writing down because ADR-0008 calls it the design's one genuinely uncomfortable
dependency:

> Board B says: *this frame came to me over the inter-board link from board A.* Board A relays
> only frames its own registered helper authenticated. The inter-board link is trusted, by
> ADR-0007, for something strictly larger — arbitrary firmware.

So the relay itself is the vouching. There is no extra field, and no fingerprint compared by
eye: that alternative was considered and rejected because it puts a manual step on a pairing
gesture that exists to cost one keystroke.

### Establishing a seal

**One seal per direction, established by the sender.** A helper that has bulk to send and holds
no live seal offers one; the receiver accepts. Two independent one-directional seals cost one
extra ECDH on each helper — a millisecond on a laptop, and no board work at all — and they
remove the entire question of who wins when both sides offer at once.

```
sender   → receiver   SEAL_OFFER   seal_id, nonce_offer,  ephemeral public key
receiver → sender     SEAL_ACCEPT  seal_id, nonce_accept, ephemeral public key

k_seal = HKDF(ikm  = ECDH(ephemeral private, peer ephemeral public),
              salt = nonce_offer ‖ nonce_accept,
              info = "deskhopplus/2 seal")
```

The ephemeral keys are per seal and are not either party's identity key.

- **`seal_id`** is a fresh random `u32` chosen by the offerer. It names the key in every sealed
  frame, so a receiver knows which key to try without guessing.
- **A sealed frame naming an unknown `seal_id`** is dropped and answered with `SEAL_STALE`. On
  receiving `SEAL_STALE` for its current seal, a sender discards that seal and offers a new one.
  This is the ordinary recovery when one side's session ended and the other's did not.
- **A helper drops all seal state when its own session ends.** #107 measured 586 session
  teardowns in 16 hours; re-offering is cheap and the alternative is a key whose peer may no
  longer exist.
- **A `CLIP_OFFER` is not sent until the seal is accepted.** The offer is itself sealed, so
  there is nothing to send before there is a key to send it under.
- **An offer that is never accepted times out** on the sender's transfer timeout and the
  transfer is abandoned, exactly as any other stalled transfer is. The far computer having no
  helper running is the ordinary reason, and it is not an error worth its own message.

### The seal itself

**AES-256-GCM.** Chosen because both helpers have it natively — CryptoKit on macOS, CNG on
Windows — with no new dependency; ChaCha20-Poly1305 would need Windows 11 through CNG, and #84
targets a managed laptop. The board needs **no AEAD at all**: the seal never runs on the
firmware, which keeps the primitive set there down to SHA-256, HMAC, HKDF and P-256.

```
nonce = seal_counter (u64, little-endian) ‖ 00 00 00 00        # 12 bytes
```

`seal_counter` starts at 0 per seal and increases by exactly 1 per sealed frame. It is carried
in the clear in each sealed message and is covered by the AAD. Because each seal key is
one-directional and each counter value is used once, no nonce is ever reused under a key.

**What is sealed, and what is not.** Only the two messages that carry the user's bytes:
`CLIP_OFFER`'s metadata and `CLIP_CHUNK`'s data. `CLIP_REQUEST`, `CLIP_DONE`, `CLIP_CANCEL`,
`CLIP_RETRANSMIT` and `CLIP_CREDIT` carry transfer ids and sequence numbers only, and sealing
them would add 16 bytes each to hide nothing.

So a listener still learns that a transfer happened, roughly how big it is, and how long it
took. It does not learn the content, the file names, or the kind. That is the trade, stated so
it is not mistaken for an oversight.

**The CRC32 stays** and stays inside the seal. GCM already authenticates, so the CRC32 is no
longer the integrity check against a hostile change — it is the **fidelity** check of
`CONTEXT.md` and [ADR-0003](adr/0003-content-fidelity-over-content-validation.md), covering the
plaintext end to end and catching a bug in the seal layer itself, which an authenticator that
sits *outside* the plaintext cannot.

### The cost, measured in bytes

A sealed 1024-byte chunk is a 1092-byte frame: 4 header, 24 authentication prefix, 20 clear
chunk header, 4 CRC32 (sealed), 1024 data, 16 GCM tag. v1's was 1040. That is **+5.0%**, against
ADR-0002's finding that the inter-board UART is the wall.

This puts a ceiling on the negotiated chunk size. The frame payload maximum is 4096, and a
sealed chunk spends 64 bytes of it on overhead, so:

> **`max_chunk` must not exceed 4032.** The device offers `DH_XFER_CHUNK_SIZE` (1024) and
> negotiates down, so this ceiling is never reached in practice — but a helper that asks for
> 4096 must be clamped, not honoured.

### The board's reply buffer has to grow

The frames the board *emits* grew too, and one of them no longer fits the buffer it is built in.
`src/channel.c:323` builds every reply in `uint8_t reply[CHANNEL_REPORT_SIZE]` — 64 bytes — under
a comment reading *"The largest reply this path can produce is a 20-byte pair grant."* In v2:

| reply | v1 | v2 |
|---|---|---|
| `PAIR_GRANT` | 20 bytes | **76 bytes** — over the buffer |
| `HELLO_ACK` | 11 bytes | 58 bytes — inside it, with nothing to spare |
| `HELLO_REFUSED` | — | 15 bytes |
| `SESSION_END` | 5 bytes | 29 bytes |

`dh_frame_encode` returns `DH_FRAME_ERR_BUFFER` rather than truncating, and `src/channel.c:325`
queues only on `DH_FRAME_OK` — so a v2 board built on today's buffer would simply never answer a
pairing request, with no error anywhere.

**Done in #111.** The buffer is `DH_SESSION_REPLY_MAX`, stated in `src/core/dh_session.h` as the
largest frame the session layer can emit, so the size follows the protocol rather than being
restated in the firmware. The reasoning behind the old size survived and the claim about the
largest reply did not: it is small because this path runs on core 0's stack — 2 KB, inside a 4 KB
`SCRATCH_Y` next to core 1's — and what that argues against is a **frame-sized** buffer, since
`DH_FRAME_MAX_SIZE` is 4100 and would overrun both. 76 bytes does not. The one place a whole frame
genuinely has to be assembled is the relayed frame the board re-tags for its own helper, and that
buffer is static for exactly this reason.

## Liveness

Per [ADR-0004](adr/0004-independent-bidirectional-liveness.md). Liveness is **symmetric and
independently timed in each direction**, and it is carried by **traffic**, not by an
acknowledgement. There is no request/response pair here: each end runs its own timer over what
arrives.

- **Any authenticated frame proves the sender is alive, in both directions.** An end treats its
  peer as present while anything at all has arrived from it within three heartbeat intervals —
  hello, placement, clipboard bulk, a heartbeat. Nothing is excluded, and an implementation that
  credits only heartbeats is wrong: the *other* end suppresses its beat whenever it has real
  traffic to send, so counting only beats evicts a peer in the middle of its own transfer.
  **v2 narrows this to frames that authenticate**, which is the fix for what v1 could not
  express. Under v1 the deadline measured "*something* is writing", and on macOS a second
  writer's traffic would hold the device's view of the helper alive after the real helper had
  stopped (#95). A frame that carries a good tag under the session key came from the holder of
  the registered key, so the deadline now measures what it always claimed to.
- **Being alive and holding a session are different claims.** In the device→helper direction
  arrival proves both, since the device relays and answers nothing for a peer it has no session
  with. In the helper→device direction it proves only the first — which is all that is needed,
  because the device is the end that owns the session state and does not need to be told.
- **A heartbeat fills an idle direction only.** HEARTBEAT and DEVICE_HEARTBEAT are sent only when
  that direction has carried nothing for a full interval. A busy link emits neither. They exist so
  that silence is unambiguous, not to be the measurement.
- **Why idle-gated rather than unconditional.** The device's outbound path is a short bounded queue
  ([ADR-0005](adr/0005-bounded-outbound-queues.md)), and a frame it cannot take is a silent loss.
  An unconditional beat would be starved by a sustained transfer, and a few starved beats in a row
  would look exactly like a dead session — the mechanism would manufacture the failure it exists to
  detect. Gating on idleness removes that: a busy direction emits no beat, and a beat that is
  refused anyway is self-correcting, because whatever displaced it refreshes the peer just as well.
  The beat rides the queue's priority band, so it is never stuck behind clipboard bulk that is
  merely waiting — only behind bulk already on the stream, which is bounded by one frame.
- **A drop report is not a beat, and does not displace one.** DEVICE_DROPS is sent on change,
  ahead of the beat and behind the listener alert, and it is the one frame the device sends that
  does **not** charge the idle timer. Its rate limit is exactly the beat's interval, so charging
  the timer would mean a device whose counters move once a second never sends a beat at all — and
  a helper's quiet detector keys on beats. It would report the beat gone for the whole duration of
  the fault the frame exists to describe, which is a diagnostic manufacturing a false reading. So
  the two travel together while something is dropping. A drop report is still liveness on arrival,
  like any authenticated frame; it is not liveness in its own right, and a helper must never read
  its absence as anything but "nothing has moved".
- **SESSION_END is an optimisation, never the mechanism.** The device announces an eviction it
  knows about so the helper need not wait out a timeout. A device that reboots, wedges, or loses
  power announces nothing, so the timeout above is what must be correct.
- **On detection, drop and reconnect.** The peer is no longer trustworthy and the byte stream may
  be mid-frame, so recovery is closing and reopening the channels, not re-introducing over them.

## The two direction toggles

Clipboard sharing has one toggle per direction — **A→B** and **B→A** — both defaulting on. The
device is the single source of truth for settings, so a helper stores neither: it is told, in
`CLIP_POLICY`, and it acts on what it was last told.

The translation is the whole of the design, and it is one function
(`dh_clip_policy_for`, `dh_session.h`). A helper cannot act on a *direction* — it has no way to
know which end of "A to B" it is standing at — but it can act on two verbs: **may I send what
was copied here**, and **may I write what arrives**. Each board holds both toggles and turns
them into the pair of verbs for the helper on its own side, using its own `board_role`.

Consequences worth stating, because each is a place the obvious implementation is wrong:

- **The toggles are one setting, held identically on both boards.** The config page writes a
  changed field to both, which is what keeps "A to B" meaning the same thing on either side.
- **A direction turned off takes what is already crossing it with it.** Letting the bytes
  already on the wire finish arriving would make the control a control over *new* copies only.
- **A refusal to receive happens before the seal is opened.** The clear head of a `CLIP_OFFER`
  carries the transfer id, which is everything a `CLIP_CANCEL` needs — so a helper told not to
  receive never decrypts a payload it has already decided to refuse.
- **Silence means allowed.** A helper that has heard no `CLIP_POLICY` allows both directions,
  matching the stored default. Refusing until told would refuse every copy made in the moment
  before the frame lands, which at the desk is indistinguishable from the feature not working.
- **Stored as blocks, not permissions.** On the board the two bytes mean *blocked*, so that
  zero means allowed — they came out of `config_t`'s existing padding, and a field meaning
  "enabled" would have read as off on every configuration already written.

## Reporting what the device has dropped

The device counts every frame it loses at every seam ([#43](https://github.com/myn/deskhopplus/issues/43)),
and for a long time counted them where nobody could read the count. The first attempt put the
seven totals on the config page. That could not work, and the way it could not work is worth
writing down: the page is reachable only in config mode, config mode is entered by **rebooting**,
and the counters live in plain RAM. Every reading was therefore taken on a device that had just
zeroed them. Three sittings on [#132](https://github.com/myn/deskhopplus/issues/132) read a row of
zeros as evidence the seams were clean; it was an artifact of the measurement.

`DEVICE_DROPS` is the reading that means something, because the helper is attached *while the
fault is happening*.

- **Sent on change, and once as a baseline.** A fresh session is owed one whatever the totals are.
  All-zero and "the device has said nothing" are the two answers a stall has to tell apart, and
  without a baseline they look identical — which is the mistake above, in a new place.
- **One per heartbeat interval at most.** The device's outbound path is a short bounded queue
  ([ADR-0005](adr/0005-bounded-outbound-queues.md)), and a seam losing a frame per tick would
  otherwise put a frame per tick into it. A diagnostic that crowds out the traffic it is measuring
  is worse than none. Skipping a reading costs nothing: these are totals, so the next one carries
  everything the skipped one would have. A reading the queue refuses waits out that interval like
  any other; a total can afford that where a rate cannot.
- **Nine totals, not one.** Each names a different seam and each seam has a different remedy. A
  sum would say only that something is wrong — which is exactly what `outq` was on its own until
  [#142](https://github.com/myn/deskhopplus/issues/142). That one field covers a queue with two
  bands and a header check, and [#141](https://github.com/myn/deskhopplus/issues/141) deepened one
  band; a total still climbing was equally consistent with that change having worked and with it
  not having. `outq_priority` and `outq_bad_header` are what make the third figure derivable, so a
  reader can say which seam to act on.
- **`relay_q` is not split, deliberately.** It has the same three causes and the board tracks all
  three, but sending them would cost the last of this frame's headroom — the body may not push the
  frame past `DH_SESSION_REPLY_MAX`, which the pairing grant also has to fit — and that seam has
  read zero at every sitting. The counters exist, so appending them later is a small change.
- **Fields are appended, never inserted.** A helper built before a field existed sees a body length
  it does not know and loses the reading — `on_device_drops` notes it undecodable, which is not a
  protocol error and does not end the session. Inserting a field instead would silently renumber
  every seam after it, and the helper would go on printing confident names for the wrong counters.
- **Each device reports its own.** There is no proxied read: a device in config mode can forward an
  API GET over the inter-board link, but the answering device drops the response because *it* is
  not in config mode. So both ends of a pair need the firmware before both ends can be read.
- **The helper quotes them where they are wanted.** A transfer abandoned for lack of progress logs
  the totals next to how far each direction got — the one moment the numbers are being asked for.

## Transfer semantics

The chunked transfer state machine (#48) lives in the shared core and runs **end-to-end
between the helpers**; the firmware relays its messages opaquely.

- **Chunking.** A payload divides into chunks of `DH_XFER_CHUNK_SIZE` (1024 bytes — a build
  constant until [#39](https://github.com/myn/deskhopplus/issues/39) measures; the hello
  negotiates the effective value, capped at 4032 as above). Every chunk except the last is
  exactly that size, so a chunk's offset is `seq × chunk_size` and reassembly needs no
  bookkeeping beyond a received-set. A chunk is exactly one CLIP_CHUNK frame's sealed payload.
- **Streaming starts on request, never before.** An offered transfer emits nothing until
  CLIP_REQUEST arrives — a lazy payload (files) is not even read until then.
- **Integrity and loss.** The paste side unseals each chunk, verifies its CRC32 and tracks
  received seqs. A chunk that fails to unseal is treated as a corrupt chunk, not as a protocol
  error: it is a bulk payload and the transfer's own machinery already handles losing one. A
  corrupt chunk, a skipped seq, or a gap found when CLIP_DONE arrives produces
  CLIP_RETRANSMIT for exactly the missing chunks. After retransmitting, the sender repeats
  CLIP_DONE. A loss is reported once per round: a DONE sweep leaves a freshly requested
  chunk alone once — its retransmission is behind that DONE in the FIFO — **but a chunk
  still missing a full round later is requested again**, so a retransmitted chunk that is
  itself lost converges on the next DONE round. Only the loss of a message with no DONE
  behind it (a CLIP_OFFER, a lone request) is left to the helper's transfer timeout.
- **A receive completes on its last chunk, not on CLIP_DONE.** The paste side verifies every
  chunk's length and CRC32 as it arrives, so a receiver holding the whole received-set already
  knows it is finished; CLIP_DONE carries nothing but the id. Completing on the chunk is what
  makes a lost CLIP_DONE free: the device's outbound queue refuses frames under a burst deeper
  than itself and nothing retransmits beneath that seam ([ADR-0005](adr/0005-bounded-outbound-queues.md)),
  and CLIP_DONE is always part of such a burst because it is emitted in the same pump batch as
  the last chunk. Before
  [#132](https://github.com/myn/deskhopplus/issues/132) that one refusal stranded a complete,
  verified payload until the 30-second stall timeout — in both directions, intermittently, with
  every board counter reading zero. CLIP_DONE keeps its other job, the retransmit sweep for an
  *incomplete* receive, and is still sent on every transfer.

  Two cases this does **not** cover, both still resolved by the helper's transfer timeout. If the
  same burst eats the *last chunk* as well as the DONE behind it, nothing completes the set and no
  DONE arrives to drive a sweep. And a zero-length payload has no chunks at all, so it can only
  ever complete at CLIP_DONE — reachable in `dh_xfer` but not from either helper, which refuse an
  empty copy before offering it.
- **The transfer timeout is a *progress* deadline, not a duration.** Each helper's clipboard
  service gives up on a direction that has produced nothing for 30 seconds and reports it. It
  cannot live in `dh_xfer`, which has no clock and must not gain one — and a deadline on the
  whole transfer would abandon healthy ones, because a large payload legitimately takes minutes
  on this link. This is what covers the third interruption in
  [#52](https://github.com/myn/deskhopplus/issues/52): an unplug and a config-mode reboot both
  end a helper's own session, but the far helper *crashing* leaves this end's session perfectly
  healthy and simply unanswered, where no message arrives for anything to react to.
- **A retransmitted chunk is resealed**, under a fresh `seal_counter`. A seal counter is never
  reused, so a retransmission is not a byte-identical copy of the frame that was lost.
- **The sender retains its payload after CLIP_DONE** — retransmit requests may still
  arrive. It is released when the transfer is superseded by a newer offer, cancelled, or
  the link drops. There is no completion acknowledgement in v2: CLIP_DONE always travels
  sender→receiver, which keeps it unambiguous when both sides transfer at once.
- **Flow control.** The sender spends one credit per chunk sent (retransmits included) and
  stops at zero; CLIP_DONE is not gated. The paste side grants `DH_XFER_CREDIT_WINDOW`
  (3 chunks) with its CLIP_REQUEST, replenishes as chunks arrive — the rule is half-window
  batches, which at a window of 3 means one grant per chunk,
  and **every CLIP_RETRANSMIT is accompanied by a covering credit grant** — a lost or
  corrupt chunk consumed the sender's credit without ever being counted on the paste side,
  and without the covering grant sustained loss would drain the window permanently. (When
  the lost message was the *request* rather than the chunk, the covering grant mildly
  inflates the window — bounded and harmless, where draining is fatal.) **The window is
  per transfer**; a direction carries one transfer at a time, so this is per-direction
  accounting in practice, and grants carry the transfer id so a superseded transfer's
  grants are ignored rather than credited to its successor. What is protected globally —
  the shared inter-board queue behind all channels — is the egress board's burst cap
  (#47) plus this window. The device's own outbound queues absorb the *burst* either
  direction can produce inside one drain ([ADR-0005](adr/0005-bounded-outbound-queues.md)),
  but they are bounded and deliberately short: sustained overrun is this window's job, and
  the firmware cannot help with it, because a credit lives in a payload the firmware may
  not read ([ADR-0003](adr/0003-content-fidelity-over-content-validation.md)).

  **So the window is sized against that queue, not against the link.** It was 16 while the
  queue holds one frame in flight and `DH_OUTQ_DEPTH` behind it, which made "sustained
  overrun is this window's job" untrue in practice — the receiver granted all 16 up front,
  the sender emitted all 16 in one pump, and everything past the third was refused. A
  refused chunk is re-requested; a refused CLIP_OFFER or CLIP_DONE is not, and costs the
  whole transfer. `outq_test` pins the two constants against each other so they cannot
  drift apart again, and raising the window means deepening that queue first.

  **The burst is the window plus one, and the queue is sized for that**
  ([#141](https://github.com/myn/deskhopplus/issues/141)). CLIP_DONE is not credit-gated
  and rides the same pump batch as the last chunks, so the worst-case loss-free burst is
  the window *plus one* — and the frame that overflowed a queue sized to the window alone
  was always the DONE, the one with no retransmit behind it. `DH_OUTQ_DEPTH` is 3, so the
  queue holds four bulk frames: the window's chunks and the DONE behind them.

  Fixed by the deeper queue rather than a smaller window, because every helper-side remedy
  breaks double-loss recovery: a window of 2, capping the accumulated credit at the window,
  and capping the pump batch at 2 were each tried and each stalls it. Credit still
  accumulates without a ceiling, deliberately — the covering grant riding every
  CLIP_RETRANSMIT is what pays for the retransmission, and that inflation is bounded and
  harmless where draining is fatal. So a lossy round can leave the sender holding more
  credit than the window names, and the queue absorbs the batch rather than the window
  policing it.
- **Failure is abandonment.** A link drop mid-transfer abandons both directions: the
  paste side discards its partial payload — never delivering it as complete — and its
  helper deletes any partial file and reports the failure. No resumption.
- **Supersede.** A newer offer replaces an incomplete transfer in either direction; stale
  messages for the old id are ignored.

## Development builds

The **board** skips its authentication checks exactly as
[#44](https://github.com/myn/deskhopplus/issues/44) specifies today, and says so in its build
type, its product string and the config UI. The `build_type` field in the hello and the ack is
what carries that on the wire.

The helper↔helper seal is **not a board's to disable**. A development board relays sealed bulk
it cannot read, the same as a release board, because the key that opens it was never the
board's.

## Golden vectors

`test-vectors/frames.txt` is the cross-implementation gate: the shared C core (and every
binding of it) must decode each vector and re-encode it byte-identically. Any protocol change
updates this document, the vectors, and the core in the same change.

`test-vectors/primitives.txt` is the same gate one layer down (#110): SHA-256, HMAC-SHA256,
HKDF-SHA256 and P-256 ECDH, each on its own, so a core that computes a hash wrongly fails on a
line naming that hash rather than on every frame at once. The vectors named for an RFC or a FIPS
publication are those documents' published answers — FIPS 180-4, RFC 4231, RFC 5869 and
RFC 6979 — so the arithmetic is gated by numbers this project did not compute for itself. It
also carries the session key material, which is what lets `auth_test` verify and rebuild every
tag in `frames.txt` rather than only re-encoding its bytes.

The v2 vectors are **generated**, not written:

```sh
python3 tools/gen-frame-vectors.py              > test-vectors/frames.txt
python3 tools/gen-frame-vectors.py --primitives > test-vectors/primitives.txt
```

Every tag and every seal in that file is real, computed from the fixed test key material the
generator publishes in full. That matters more than it did in v1: a hand-written tag would
make the gate agree with nothing, and the first implementation to disagree with it would
"fix" the file rather than the code. The generator reimplements P-256, AES-256-GCM, HKDF and
HMAC rather than linking `src/core`, because a generator that shares its encoder with the thing
it gates cannot catch the encoder being wrong — and it self-tests against published
known-answer vectors before it emits anything.

mkroamer's five byte-identical vectors do not survive v2. Their names are kept (`heartbeat`,
`clip_offer_text2`, `clip_request_1`, `clip_done_2`, `clip_cancel_2`) and their bytes are not,
because every one of them now carries a tag mkroamer's protocol had no field for.

## What v2 does not fix

Each of these was considered and accepted rather than overlooked. ADR-0008 carries the full
reasoning; they are listed here because this is the document implementations are written from.

1. **A purely passive listener stays undetectable.** Nothing at the IOKit level reports a second
   client, and a process that only reads emits nothing to detect. After v2 it learns nothing
   worth having, which is the answer to it.
2. **A listener can still break the channel.** It can write junk into a shared endpoint at will,
   and on a transport with no exclusivity availability cannot be defended. What it may **not**
   do is make the helper display *"Not paired — press the config chord"*.
3. **Malware on the Mac can use the Secure Enclave key while it runs.** Keychain ACLs bind to a
   stable code signature and this helper is unsigned and frequently rebuilt. It cannot copy the
   key off the machine, so the credential cannot outlive the infection and re-pairing genuinely
   revokes.
4. **Cursor coordinates cross in the clear.** Placement is a lesser harm: idempotent,
   self-correcting, and a local process can already ask the OS where the cursor is.
5. **A listener can win the single-shot pairing race.** Then it is registered and the helper is
   not — and the helper says so, which is the detection signal #34 asked for and never got.
6. **No forward secrecy.** Flash plus recorded traffic reads the traffic.
7. **Development builds skip the board's checks**, per #44.
