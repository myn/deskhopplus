# The macOS helper

A background agent that finds the device, seizes every vendor HID channel, introduces itself, and
keeps the session alive ([#45](https://github.com/myn/deskhopplus/issues/45)). It carries the
clipboard — text, images and files — and places the cursor on the session this establishes.

It needs a window server, because it has a menu-bar item
([#54](https://github.com/myn/deskhopplus/issues/54)'s first slice, built for
[#56](https://github.com/myn/deskhopplus/issues/56)'s acceptance and progress). The LaunchAgent
runs it in the user's GUI session, so it has one; running it over ssh with nobody logged in does
not, and is not a supported arrangement — reading this machine's pasteboard needs the same
session.

## Layout

| Path | What it is |
| --- | --- |
| `Sources/DeskhopChannel` | The binding to the shared C core, and what each of its outputs *does* (`OutputDispatch`). No IOKit — all of it runs in the tests. |
| `Sources/deskhop-helper` | The agent: IOKit transport, run loop, menu bar, received-file store, and the state the user is shown. |
| `Tests/channel-tests` | The host tests. |
| `LaunchAgent/` | The launchd job that starts it at login and restarts it after a crash. |

Files arriving from the other computer are **offered, not pushed**
([ADR-0011](../../docs/adr/0011-paste-side-acceptance-starts-a-file-transfer.md)): a set over
256 KB waits in the menu bar until it is accepted here, and only then does anything cross the
link. They are written under the per-user temporary directory, which is emptied when the helper
starts.

The Swift package manifest is `Package.swift` at the **repository root**, because the `DHCore`
target compiles `src/core` in place — the same sources the firmware compiles. A copy of the codec
inside the helper would be a second implementation of the wire format wearing a binding's name,
which is exactly what [#64](https://github.com/myn/deskhopplus/issues/64) consolidated away.

## The session machine is not here

Every decision this helper makes is `src/core/dh_helper.c`
([#79](https://github.com/myn/deskhopplus/issues/79),
[#80](https://github.com/myn/deskhopplus/issues/80),
[#81](https://github.com/myn/deskhopplus/issues/81)) — the hello exchange, negotiation,
ADR-0004's liveness, pairing, all-or-nothing acquisition, the capped backoff, and the states
below. `HelperSession.swift` carries events down to it and outputs back up, and that is all it
does.

It used to be a second implementation of that machine, in Swift, that only macOS could run. Two
of them would have given ADR-0004's traffic-gated liveness two chances to be got right, failing
differently on two operating systems under load, in a way that looks like a hardware fault —
and [#49](https://github.com/myn/deskhopplus/issues/49)'s Windows helper drives the same one.

**Three things stay on this side, and only these three:**

| Kept in Swift | Why |
| --- | --- |
| The wording — `HelperState.message`, `HelperNotes` | A Windows tray tooltip and a macOS menu bar item are not one string table living in C. Outputs cross carrying a code and its numbers. |
| Secret storage — `SecretStore` | The core decides a board key is worth keeping; whether it lands in a 0600 file or in DPAPI is the platform's business. |
| The Secure Enclave — `EnclaveIdentity` | The private half cannot be handed to C at all. What the enclave *can* do is one ECDH, which is the whole of `dh_helper_identity`. The HKDF over the result stays in the core, so both ends run one derivation rather than two that happen to agree. |

The two policy predicates — *does this state prompt the config chord*, and *may bulk go out* —
are read off the core rather than restated, because the first carries a security property
(a chord press provisions whatever is attached to the channel during its window,
[#34](https://github.com/myn/deskhopplus/issues/34)) and a second helper must not get to answer
it differently.

Tests follow the same line. `Tests/channel-tests` describes what the *helper* does and proves the
binding loses nothing on the way through; the machine's own arithmetic — the beat trace, the
backoff, the correlation guards — is `tests/helper_test.c`, where the other end of every round
trip is the real `dh_session` rather than a mock. `OutputDispatchTests` covers the last step of
that path — that each output reaches the effect it names
([#152](https://github.com/myn/deskhopplus/issues/152)) — with the transport, the pasteboard and
the Keychain behind an interface, so no IOKit or AppKit call is reached from a test.

## Build and test

```sh
./tools/build.sh helper     # release build, tests, and what the agent is running
swift build                 # the library and the agent, debug
swift run channel-tests     # the host tests
swift run deskhop-helper    # run it in the foreground, logging to stderr
```

Prefer `./tools/build.sh helper` when the agent is installed. A bare `swift build` produces
`.build/debug/`, which is **not** what the install below deploys, so it cannot refresh a running
agent — and it reports success either way ([#93](https://github.com/myn/deskhopplus/issues/93)).

The tests are an executable rather than a `.testTarget`: XCTest and swift-testing both ship with
Xcode, not with the Command Line Tools, and a test suite that needs a 10 GB install to run is a
test suite that stops being run. The style matches the C harness next door — an assertion helper,
a main, a non-zero exit, no framework.

The C core's own tests are a separate build, and the session machine's are among them:

```sh
cmake -S tests -B tests/build && cmake --build tests/build && ctest --test-dir tests/build
```

## What the helper does

1. **Finds the device** by USB identifier, serial, usage page and usage — never by a device path.
   Matching is narrow on purpose: a helper that matches broadly opens a keyboard and triggers an
   Input Monitoring prompt, which is the mistake behind most public claims that HID access needs
   one ([ADR-0001](../../docs/adr/0001-vendor-hid-transport.md)).
2. **Seizes every channel or none.** `kIOHIDOptionsTypeSeizeDevice` on each, rolled back and
   reported as a refusal if any one is refused. Partial acquisition is worse than outright
   failure: a second process holding one channel would silently receive part of every bulk
   transfer while both sides looked healthy ([ADR-0002](../../docs/adr/0002-parallel-hid-channels.md)).
3. **Says hello**, carrying protocol version, platform, build type, the channel count and chunk
   size it asks for, the id of its own key, a fresh nonce, and a fresh correlation value. The
   frame is authenticated under a key derived from ECDH against the pinned board key. The device
   answers with the *effective* values, and the session runs on those.
4. **Beats** at the interval the shared core defines, which is the same number the firmware
   measures its "absent after a couple of missed intervals" against.
5. **Reconnects by itself**, with an exponential backoff capped at a few seconds — and says so
   when it finds itself doing that over and over, because a single reconnection is invisible by
   design and a great many of them must not be
   ([#94](https://github.com/myn/deskhopplus/issues/94)). "Over and over" is two readings, not
   one: several drops inside half a minute, and — over a far longer window — a handful of
   sessions that came up and then died with the board never going anywhere. The second is there
   because the first cannot see a slow loop, and a slow loop ran for sixteen hours under
   *Connected and paired* ([#107](https://github.com/myn/deskhopplus/issues/107)).

## The states, and the one that matters

| State | Shown as | Prompts the config chord |
| --- | --- | --- |
| connected | Connected and paired | no |
| reconnecting repeatedly | Reconnecting repeatedly — check the link, and that the helper is up to date | no |
| not paired | Not paired — press the config chord on the device | **yes** |
| config mode | Device in config mode | no |
| absent | Device not connected | no |
| version mismatch | Helper version does not match the device — file transfers are refused | no |
| listener detected | Another program is writing to the device channel — find and stop it, and do not press the config chord while it is running | **no** |
| board identity changed | Device identity changed — if you re-flashed it, remove the pinned board key | **no** |

**Only "not paired" prompts the chord.** A chord press provisions whatever is attached to the
channel during the pairing window ([#34](https://github.com/myn/deskhopplus/issues/34)), so the two
states where something else may be attached — a listener writing, or a board that granted under an
identity this helper never pinned — are exactly the two where the chord is the wrong move. Both say
so in their own words, and a test fails if either starts prompting it.

**"Channel held" is gone** ([#72](https://github.com/myn/deskhopplus/issues/72),
[#114](https://github.com/myn/deskhopplus/issues/114), ADR-0008). It said *"Another program holds
the channel — find and stop it"*, which on macOS asserts something that never happens: a second
`kIOHIDOptionsTypeSeizeDevice` open succeeds, measured on 2026-08-13, so the open is not refused
for the reason that message named. What replaces it is *listener detected*, which is measured
rather than assumed — the board counts frames it could not authenticate and reports the rate.

An open that does fail anyway — a device unplugged mid-open, or the channel nodes still arriving
one at a time ([#63](https://github.com/myn/deskhopplus/issues/63)) — is a device this helper
cannot use. It retries, says nothing at first because a partial acquisition is ordinary, and
reports *Device not connected* if the failure lasts. The log line carries the real reason.

Nothing is shown during a brief disappearance. Entering config mode reboots the device under a
different USB identity for up to five minutes and then reboots back — that is normal operation,
not an error, and it is reported distinctly from the device being absent.

**A rate is a state too.** Every one of those silences is correctly judged too brief to report, so
a connection torn down and rebuilt inside the window is invisible however often it happens — which
is how a helper failing every frame it received read as *Connected and paired* for two days
([#94](https://github.com/myn/deskhopplus/issues/94)). Four rebuilds inside thirty seconds is
therefore its own state, and it does not flap back to connected on each successful hello.

It is reported whether or not the handshake ever completes: a device that takes the hello and says
nothing loops on the timeout, and each re-acquisition clears the deferred *device not connected* a
second before it comes due, so that helper would otherwise say nothing at all — for ever. What the
rate never overrides is a state with its own remedy; an unpaired helper, a detected listener or a
version mismatch each keeps its place. The session it reports on is otherwise ordinary, and carries
what any other session carries.

## Identity and pairing

The helper holds a **P-256 key pair in the Secure Enclave**
([ADR-0008](../../docs/adr/0008-channel-identity-and-sealed-clipboard.md),
[#112](https://github.com/myn/deskhopplus/issues/112)). The private half is generated inside the
enclave and never leaves it; the helper asks the enclave to perform key agreement and gets a
shared secret back. There are **no secret bytes at rest**. What is on disk is an enclave key
*handle*, which is useless on any other machine, and the board's 64-byte public key, which is
public by definition.

Pairing is a chord press. The helper sends a `PAIR_REQUEST` carrying its public key and a fresh
random correlation value; the board, only inside a pairing window the user opened, answers with a
`PAIR_GRANT` carrying its own public key and **echoing that correlation value**. The helper pins
what it is sent and immediately says hello under it.

Every frame outside the pairing band carries a 16-byte HMAC-SHA256 tag and a monotonic counter,
under per-direction keys derived from the shared secret and both nonces. A frame whose tag does
not verify is dropped in silence, and — this is the fix for
[#95](https://github.com/myn/deskhopplus/issues/95) — **does not count as the device being
alive**. A bystander writing at the endpoint can no longer keep a dead session looking healthy.

The correlation value is what closes [#108](https://github.com/myn/deskhopplus/issues/108): a
manufactured `PAIR_GRANT` or `HELLO_ACK` cannot guess the random value in the request it claims
to answer, so a helper never acts on a reply to a question it did not ask.

Old pairings do not migrate. A v1 helper cannot pair with a v2 board, and the first run of this
version deletes the v1 `secret` file. Recovery is one chord press.

**A board whose key has changed is not silently accepted.** If a chord press produces a grant
carrying a different identity key from the one this helper had pinned, the helper refuses it and
says so. That is a board wiped past its identity sector, re-flashed, or swapped for another one.
The chord is deliberately *not* offered as the remedy, because pressing it is the act that would
accept the new board.

Nothing on the channel can make the helper drop that pin — not even the board saying it has
forgotten this helper, which leaves the pin exactly where it was. A control a restart clears is
not a control. If you re-flashed the board yourself, say so where a bystander on the channel
cannot reach:

```sh
rm ~/Library/Application\ Support/deskhopplus/board_key
launchctl kickstart -k gui/$(id -u)/com.deskhopplus.helper
```

### Which helper is the board paired with?

The board's config page answers it, under **Paired helper**
([#114](https://github.com/myn/deskhopplus/issues/114)): the key id of the one helper it has
registered — SHA-256 of that helper's public key, first eight bytes — or *none* when the board
holds no registration. It is read-only there, and the shared secret stored beside it never leaves
the board at all.

The helper prints its own key id, in the same byte order and spelling, as the first line of every
run:

```
helper key id: 4f2a91c7e30b56d8
```

Equal means this helper is the registered one. Different means the board is paired with something
else, and a chord press is what moves it — with the usual caution: press it only when nothing you
did not start is attached to the channel.

### The clipboard payload is sealed

Bulk payloads are encrypted **helper to helper**
([ADR-0008](../../docs/adr/0008-channel-identity-and-sealed-clipboard.md),
[#113](https://github.com/myn/deskhopplus/issues/113)). The two boards relay ciphertext and hold
no key that opens it, so *opaque relay* stops being a discipline the firmware keeps and becomes a
property it could not break: a board relays bytes it could not read even if it wanted to. That
matters because a board is already something you cannot fully trust —
[ADR-0007](../../docs/adr/0007-inter-board-link-is-trusted-for-firmware.md) accepts that a
compromised board can flash its peer.

The key is agreed per seal, over ephemeral P-256 keys that cross through both boards, and the
cipher is CryptoKit's AES-256-GCM. The enclave identity above is **not** part of it: a seal needs
no long-term identity. What vouches for the far helper is that board A relays only what its own
registered helper authenticated, carried across the inter-board link — which ADR-0008 records as
the decision's one uncomfortable lean.

Cursor placement is authenticated but **not** sealed. Coordinates cross in the clear, by decision:
they are idempotent, self-correcting, and already available to any local process through the OS.

No build turns this off. [#44](https://github.com/myn/deskhopplus/issues/44)'s development-build
exemption is the *board's*, and the board is not a party to this key.

### What this does not protect against

Two limits, stated plainly, because a security note that only lists wins is not a security note.

1. **Malware running as you can use the key while it runs.** The enclave protects the key from
   being *copied* — off the disk, out of a backup, onto another machine. It does not protect it
   from being *used* by code running as your user on this machine, because that code can ask the
   enclave for key agreement exactly as the helper does. The property this buys is that a stolen
   disk or a stolen backup yields nothing; it is not a defence against a compromised account.
   Requiring a biometric prompt per operation would change that and is not something a background
   agent can do on every frame.

2. **A passive listener is undetectable.** The board counts frames it *refused* and raises
   `LISTENER_ALERT` when too many arrive in a window — so it detects something probing the
   channel, which is an active listener. A process that only opens a channel and reads is silent
   by construction: it writes nothing to refuse, and nothing in the protocol can distinguish it
   from a channel nobody has opened. Nothing keeps it off the channel either — a second
   `kIOHIDOptionsTypeSeizeDevice` open succeeds on macOS, measured on 2026-08-13, which is what
   ADR-0001 retracted and ADR-0008 was written to answer. The answer is not detection but the
   seal above: after it, a reader learns that a transfer happened and roughly how big it was, and
   nothing about what was in it.

## Installing the agent

Packaging, signing and distribution are out of scope for this ticket. To run it as a background
agent from a build:

```sh
swift build -c release
sudo cp .build/release/deskhop-helper /usr/local/bin/
cp helpers/macos/LaunchAgent/com.deskhopplus.helper.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.deskhopplus.helper.plist
```

To skip the `sudo`, edit `ProgramArguments` in your installed copy of the plist to point straight at
`.build/release/deskhop-helper` in the repo. `tools/build.sh` reads the installed plist, so it
understands either arrangement — but then anything that clears `.build/` (`./tools/build.sh clean`,
`swift package clean`, `rm -rf .build`) deletes the binary launchd is running, leaving the agent
holding a deleted file. `./tools/build.sh clean` warns when your plist points there; the others do
not, and the next build reports it either way.

Log output goes to `/tmp/deskhop-helper.log`, one line per event, prefixed with the wall clock and
the time since the helper started (#103):

```
2026-08-18 13:59:31.525  +0.0s         deskhop-helper: deskhop helper started; waiting for the channel
2026-08-18 13:59:32.531  +1.0s         deskhop-helper: device heartbeat: first beat of the session
```

Both, because neither is enough alone: the wall clock is what you cross-reference against anything
outside the process, and the `+` column is awake time, which no clock correction can move. They
diverge across a sleep, and that divergence is how you spot one. The format is pinned by
`LogStampTests.swift`.

To stop it:

```sh
launchctl bootout gui/$(id -u)/com.deskhopplus.helper
```

### Refreshing it after a change

**Building is not deploying.** launchd holds the binary it started with, so a rebuild changes
nothing until the agent is restarted:

```sh
./tools/build.sh helper                                        # builds .build/release/
sudo cp .build/release/deskhop-helper /usr/local/bin/          # unless the plist points at .build/
launchctl kickstart -k gui/$(id -u)/com.deskhopplus.helper     # the step that is easy to forget
```

Skipping the last step is [#93](https://github.com/myn/deskhopplus/issues/93): an agent that started
before a wire-format change ran against newer firmware for two days, failing every frame it received
at roughly 1.4 teardowns a second, while every rebuild reported success. `./tools/build.sh helper`
now ends by printing where the agent points, whether anything is there, and whether the running
process predates the binary you just built — so that state is visible instead of silent.

## Confirming the transport's premise on real hardware

[ADR-0001](../../docs/adr/0001-vendor-hid-transport.md) leaves one macOS confirmation open: that
the channel's vendor collection landed in its own `IOHIDDevice` with no `RequiresTCCAuthorization`
property. That is what `tools/macos-checks/confirm_hid_tcc.py` measures, and it should be run once
against real hardware before the helper's permission-free property is relied on.
