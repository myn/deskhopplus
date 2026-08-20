# The macOS helper

A background agent that finds the device, seizes every vendor HID channel, introduces itself, and
keeps the session alive ([#45](https://github.com/myn/deskhopplus/issues/45)). No payloads yet:
clipboard and cursor placement arrive on the session this establishes.

## Layout

| Path | What it is |
| --- | --- |
| `Sources/DeskhopChannel` | The binding to the shared C core, and the session logic. No IOKit — all of it runs in the tests. |
| `Sources/deskhop-helper` | The agent: IOKit transport, run loop, and the state the user is shown. |
| `Tests/channel-tests` | The host tests. |
| `LaunchAgent/` | The launchd job that starts it at login and restarts it after a crash. |

The Swift package manifest is `Package.swift` at the **repository root**, because the `DHCore`
target compiles `src/core` in place — the same sources the firmware compiles. A copy of the codec
inside the helper would be a second implementation of the wire format wearing a binding's name,
which is exactly what [#64](https://github.com/myn/deskhopplus/issues/64) consolidated away.

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

The C core's own tests are separate and unchanged:

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
   ([#94](https://github.com/myn/deskhopplus/issues/94)).

## The states, and the one that matters

| State | Shown as | Prompts the config chord |
| --- | --- | --- |
| connected | Connected and paired | no |
| reconnecting repeatedly | Reconnecting repeatedly — check the link, and that the helper is up to date | no |
| not paired | Not paired — press the config chord on the device | **yes** |
| channel held | Another program holds the channel — find and stop it | **no** |
| config mode | Device in config mode | no |
| absent | Device not connected | no |
| version mismatch | Helper version does not match the device — file transfers are refused | no |
| listener detected | Listener detected — another process is probing the channel | no |
| board identity changed | Device identity changed — if you re-flashed it, remove the pinned board key | **no** |

**A refused open must never prompt the chord.** The program holding the channel is exactly what a
chord press would provision during the pairing window
([#34](https://github.com/myn/deskhopplus/issues/34)), so "another program holds the channel" and
"not paired" are different states with different remedies, and they are reliably distinguishable:
the open was refused, versus the open succeeded and the board refused the hello.

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
rate never overrides is a state with its own remedy; a held channel, an unpaired helper or a
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
   from a channel nobody has opened. The exclusive-seize rule is what actually keeps a reader off
   the channel, and it is enforced by the OS, not by this document.

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
