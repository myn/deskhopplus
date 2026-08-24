# ADR-0008: Key pairs, per-frame authentication, and an end-to-end sealed clipboard

- **Status:** Accepted
- **Date:** 2026-08-18
- **Resolves:** [#95](https://github.com/myn/deskhopplus/issues/95) (which carries [#72](https://github.com/myn/deskhopplus/issues/72))
- **Amends:** the resolution of [#34](https://github.com/myn/deskhopplus/issues/34) — exclusive ownership is no longer half the posture, because on macOS it does not exist
- **Depends on:** [ADR-0007](0007-inter-board-link-is-trusted-for-firmware.md) — the inter-board link is trusted, and this decision leans on that
- **Evidence:** #95 (two measurements, 2026-08-13), `tools/macos-checks/probe_seize_exclusivity.swift`, and the code reading recorded under *Two findings this decision adds* below

## Decision

The channel's credential stops being a bearer token. Each side gets a **key pair**, every frame
carries an **authentication tag**, and the clipboard payload is **sealed between the two helpers**
so that no board can read it.

| Element | Decision |
| --- | --- |
| **Board identity** | Each board generates a P-256 key pair on first boot into **its own flash sector**, beside `ADDR_CONFIG` and `ADDR_FW_RUNNING` and part of neither. It survives a config wipe and a firmware update. |
| **Helper identity** | Each helper generates its own P-256 key pair. On macOS it is a **Secure Enclave** key and is never extractable. On Windows a **TPM-backed CNG** key, falling back to DPAPI where there is no TPM. |
| **Pairing** | Unchanged as a gesture: the physical config chord, a 60 s window, nothing typed and nothing displayed. Only **public** halves cross. The **first registration closes the window**. A board holds **exactly one** registered helper key. |
| **Key agreement** | One ECDH, **at pairing only**, yielding a long-term shared secret stored in the board's config. Never on the wire. |
| **Session keys** | Derived per session by HKDF over that shared secret plus a fresh nonce from each side. Symmetric, microseconds. |
| **Per frame** | A 16-byte tag and a monotonic counter on every frame of the helper↔board hop. The board relays or acts on nothing it cannot authenticate. |
| **Hello correlation** | A helper's hello carries a fresh random value; the board echoes it in the ack. A helper acts only on an ack carrying its own value. |
| **Clipboard payload** | Sealed **helper to helper**. The boards relay ciphertext and hold no key that opens it. |
| **Placement** | Authenticated, not sealed. Cursor coordinates cross in the clear. |
| **Listener detection** | A board that receives frames it cannot authenticate, above a rate threshold, tells the helper. The helper reports it. |
| **Development builds** | The **board** skips authentication exactly as [#44](https://github.com/myn/deskhopplus/issues/44) specifies today, and says so in its build type, product string and config UI. The helper↔helper seal is not a board's to disable. |

One wire on both platforms. Windows does not need this — `hidclass.sys` refuses the second open, measured —
but a second wire format would mean two state machines, two sets of golden vectors, and
[#84](https://github.com/myn/deskhopplus/issues/84) implementing the weaker one on the managed laptop.

## Context

[ADR-0001](0001-vendor-hid-transport.md) chose vendor HID over CDC on two properties, one of which
was that exclusive ownership — #34's security control — survived the transport change. On Windows it
does. On macOS it was never measured, and on 2026-08-13 it was: a second
`kIOHIDOptionsTypeSeizeDevice` open returns `kIOReturnSuccess`, reads live session traffic, and the
first process observes nothing at all. ADR-0001 carries the retraction.

#95 then established that the token a helper sends **is** the secret — `dh_session.c:48` copies it raw
into the hello, `dh_pair_authenticate` compares those bytes against the stored secret, and
`DH_MSG_PAIR_GRANT` (`dh_session.c:194`) carries a fresh secret as cleartext. So the credential is
obtainable by listening, with no filesystem access and no permission of any kind, at pairing and on
every reconnect thereafter. Worse, `dh_pair.h`'s rotation rationale is explicitly conditioned on an
attacker having *won an exclusivity race*: where there is no race, a listener still attached when the
user presses the chord receives the replacement secret. **The documented remedy for a stolen pairing
hands the thief the new one.**

### Two findings this decision adds

Both came out of reading the tree during the grilling session on 2026-08-18, and both make the
exposure larger than #95 recorded.

**1. Injection needs no secret at all.** Relay authorisation is per *session*, not per *frame*:
`src/channel.c:283` gates bulk on `dh_session_may_relay` (`src/core/dh_session.h:199`), which is one
flag for the whole board. The device cannot attribute a frame to a client, because every client
writes into the same endpoint. So while the legitimate helper holds an authenticated session, **any
other process on the Mac can write a bulk frame and the board will relay it to the other computer** —
the isolation breach #34 exists to prevent, reachable without ever learning the secret. #95 listed
this as "not yet established"; it is established in the code and is scheduled for hardware
confirmation under the implementation ticket.

**2. A listener can manufacture the chord trap.** It sends a hello with a wrong token. The board
answers `DH_HELLO_AUTH_FAILED`. That answer reaches **every** attached client, because it is an input
report. The legitimate helper decodes it, and `SessionEngine.swift:412` (the v1 Swift machine, since
replaced by `src/core/dh_helper.c`) moves it to `.notPaired` —
*"Not paired — press the config chord"*. The user presses the chord and provisions the listener. This
is precisely the losing sequence #34 wrote down on 2026-08-10, which was then thought to require
winning a startup race. It requires no race, and the helper correlates nothing: `dh_hello_ack` has no
field tying an ack to the hello that asked for it, and `pairGranted` (`SessionEngine.swift:447`,
now `src/core/dh_helper.c`)
accepts any grant that arrives.

Together these mean the exclusivity loss is not "belt and braces gone". It is the only thing that was
keeping the credential off an open wire, and per-session gating means even the credential is optional
for the write half.

## Consequences

### The board pays its crypto cost once, at pairing — not per session

This constraint shaped the design more than any other. `src/main.c:26` runs six jobs on core 0 in one
cooperative loop with no preemption: the USB device task, the keyboard queue and the mouse queue at
2000 Hz, and `channel_task`. A blocking computation there stalls the user's own keyboard and mouse for
its whole duration. The watchdog budget is 500 ms (`src/include/watchdog.h:15`), kicked from the same
loop at 30 Hz.

A P-256 ECDH on an RP2040 at 120 MHz (`CMakeLists.txt:16`, `src/setup.c:212`) is estimated at
**80–200 ms** — unmeasured, and the implementation ticket must measure it. Per session that would be a
visible freeze of the mouse and keyboard on every reconnect, and
[#107](https://github.com/myn/deskhopplus/issues/107) measured 586 session teardowns in 16 hours,
about one every 98 s. Unacceptable.

So the asymmetric work happens **once, during the pairing window**, when the board has just rebooted
for the chord and already stalls to write flash. Every session afterwards is a hash. The helper stores
no shared secret at all: it re-derives it inside the Secure Enclave, in about a millisecond.

**The cost of this is forward secrecy**, which the design does not have and did not have before: an
attacker who later obtains the board's flash *and* has recorded the traffic can read it. Recorded here
rather than discovered later.

### The firmware still never reads a payload

[ADR-0003](0003-content-fidelity-over-content-validation.md) and `CONTEXT.md`'s **opaque relay** stand
word for word, and this decision strengthens them: the bulk payload is sealed between the helpers, so
a board relays bytes it *could not* read even if it wanted to. That matters because ADR-0007 accepts
that a compromised board can flash its peer — a board is already a thing you cannot fully trust, and
this decision declines to also hand it the clipboard.

**The seal leans on ADR-0007 in the other direction**, and this is the decision's one genuinely
uncomfortable dependency. For helper B to trust a public key that arrived through both boards, it must
trust board A's refusal to relay anything its own helper did not authenticate, carried across the
inter-board link. That link is trusted, by ADR-0007, for something strictly larger — arbitrary
firmware. The alternative was a fingerprint compared by eye in both config UIs, rejected because it
puts a manual step on a pairing gesture that exists to be one keystroke.

### Curve choice is driven by key storage, not by preference

X25519/Ed25519 with Monocypher would be smaller and roughly five times faster on this chip. **The
Secure Enclave only does P-256**, and a private key that a same-user process can read out of a file
would undo the entire wire fix — the threat model here is same-user malware. So the board runs P-256
too, via `micro-ecc` (BSD-2, GPLv3-compatible). Key storage chose the curve.

### Sequencing

The wire changes, so `DH_PROTO_VERSION` bumps, `docs/protocol.md` and `test-vectors/frames.txt` are
rewritten, and existing pairings **do not migrate** — a migration path would have to accept the old
bearer token, which is the thing being removed. Recovery is one chord press, by design.

[#80](https://github.com/myn/deskhopplus/issues/80) rewrites the helper's side of this exact layer and
[#84](https://github.com/myn/deskhopplus/issues/84) implements pairing on Windows against it, so the
protocol spec lands **before #80 begins**. This is what [#97](https://github.com/myn/deskhopplus/issues/97)
Stage 2 exists to say.

### What this does not fix

Stated plainly, because each was considered and accepted rather than overlooked:

1. **A purely passive listener stays undetectable.** Nothing at the IOKit level reports a second
   client, and a process that only reads emits nothing to detect. After this decision it learns
   nothing worth having, which is the answer to it.
2. **A listener can still break the channel.** It can write junk into a shared endpoint at will. On a
   transport with no exclusivity, availability cannot be defended. What it may **not** do is make the
   helper display *"Not paired — press the config chord"* — that lie is what the echoed hello value
   removes.
3. **Malware on the Mac can use the Secure Enclave key while it runs.** Keychain ACLs bind to a
   stable code signature, and this helper is unsigned and frequently rebuilt. What it cannot do is
   copy the key off the machine, so the credential cannot outlive the infection and re-pairing
   genuinely revokes.
4. **Cursor coordinates cross in the clear.** #34's reasoning holds: placement is a lesser harm, it is
   idempotent and self-correcting, and a local process can already ask the OS where the cursor is.
5. **A listener can win the single-shot pairing race.** Then it is registered and the helper is not —
   and the helper says so, which is exactly the detection signal #34 asked for on 2026-08-10 and never
   got. The remedy is awkward, but the failure is now visible instead of silent.
6. **Development builds skip the board's checks**, per #44.

### `HelperState.channelHeld` is removed, not repaired

[#72](https://github.com/myn/deskhopplus/issues/72) asked for it to be made reachable or removed. Its
message — *"Another program holds the channel — find and stop it"* — asserts something that can never
be true on macOS, because the open is never refused. It is replaced by a state that **is** true and is
measured: another program is *on* the channel, detected by frames the board cannot authenticate. The
rule the state exists to protect is unchanged and now applies to the new one: it must never prompt the
config chord.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Accept and document** | The remedy for a stolen pairing re-issues it to the thief, so there is no recovery to document. #72's first candidate; it was available before the credential was known to be on an open wire, and is not now. |
| **Shared secret with challenge-response** (keep `dh_pair`'s shape, prove knowledge instead of sending the token) | Much less code — HMAC only, no asymmetric primitives, no new flash sector. Rejected on one point: provisioning must still **send** the secret once, inside the pairing window. A listener attached at that moment takes it silently and permanently, which is today's hole made smaller rather than closed. A key pair has nothing to intercept, even during the window. |
| **Encrypt the whole helper↔board hop** | Uniform, and hides placement too. Costs the board a decrypt of every relayed byte (~3% of core 0 per channel, ~8% with three striped under ADR-0002) and puts a clipboard-bearing key inside the firmware. Rejected for the second reason more than the first. |
| **A board generates the clipboard key** | Simpler than a relayed exchange, and no ADR-0007 dependency. Rejected: it gives every board the means to read every payload, which contradicts *opaque relay* and compounds #62. |
| **Several registered helpers per board** | One board serves one computer. Rejected because "which one is legitimate?" becomes unanswerable and eviction stops meaning anything. |
| **Truncated 8-byte tags** | Saves 8 bytes per frame, which matters most on small placement frames inside 64-byte reports. Rejected: 0.5% on a bulk frame, ADR-0002 says the UART is the wall, and a truncated MAC is the sort of cleverness that ages badly. |
| **A macOS API that grants genuine exclusivity** | #72's third candidate. Nothing at the IOKit level was found that refuses a second client for a vendor collection, and `kIOHIDOptionsTypeSeizeDevice` — the API that names the property — was measured not doing it. The answer had to be at the protocol layer. |
| **Fingerprint compared by eye between the two helpers** (Deskflow's TOFU) | Removes the ADR-0007 dependency for the clipboard seal. Rejected: it puts a manual comparison on a gesture designed to cost one keystroke, including after every config wipe. |

## Prior art

[Deskflow](https://github.com/deskflow/deskflow) (the Synergy 1 core) and Barrier face the same shape
of problem on a different transport: their channel is a TCP port, so anything that can connect can
try. Neither has, or could have, transport exclusivity. Both answer it with TLS, self-signed
certificates, and trust-on-first-use over SHA-256 fingerprints — a key that stays on the machine, not
a password on the wire.

Barrier's [CVE-2021-42072](https://cve.circl.lu/cve/CVE-2021-42072) is finding 1 above in different
clothes: the server "does not sufficiently verify the identity of connecting clients", so a malicious
client could connect without authentication, send application-level messages, and modify the server's
clipboard. It was fixed in 2.4.0 by adding client identity verification — the industry treated this as
a vulnerability, not as an accepted risk.

The analogy does not transfer whole, and the difference is the reason per-frame tags are necessary
here and not there: **they get one socket per client, so authenticating a connection authenticates a
program.** We have one endpoint shared by every client, so "the session is authenticated" says nothing
about who sent a given frame.
