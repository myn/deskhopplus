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
swift build                 # the library and the agent
swift run channel-tests     # the host tests
swift run deskhop-helper    # run it in the foreground, logging to stderr
```

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
3. **Says hello**, carrying protocol version, platform, build type, and the channel count and
   chunk size it asks for. The device answers with the *effective* values, and the session runs on
   those.
4. **Beats** at the interval the shared core defines, which is the same number the firmware
   measures its "absent after a couple of missed intervals" against.
5. **Reconnects by itself**, with an exponential backoff capped at a few seconds.

## The states, and the one that matters

| State | Shown as | Prompts the config chord |
| --- | --- | --- |
| connected | Connected and paired | no |
| not paired | Not paired — press the config chord on the device | **yes** |
| channel held | Another program holds the channel — find and stop it | **no** |
| config mode | Device in config mode | no |
| absent | Device not connected | no |
| version mismatch | Helper version does not match the device — file transfers are refused | no |

**A refused open must never prompt the chord.** The program holding the channel is exactly what a
chord press would provision during the pairing window
([#34](https://github.com/myn/deskhopplus/issues/34)), so "another program holds the channel" and
"not paired" are different states with different remedies, and they are reliably distinguishable:
the open was refused, versus the open succeeded and authentication failed.

Nothing is shown during a brief disappearance. Entering config mode reboots the device under a
different USB identity for up to five minutes and then reboots back — that is normal operation,
not an error, and it is reported distinctly from the device being absent.

## Installing the agent

Packaging, signing and distribution are out of scope for this ticket. To run it as a background
agent from a build:

```sh
swift build -c release
sudo cp .build/release/deskhop-helper /usr/local/bin/
cp helpers/macos/LaunchAgent/com.deskhopplus.helper.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.deskhopplus.helper.plist
```

Log output goes to `/tmp/deskhop-helper.log`. To stop it:

```sh
launchctl bootout gui/$(id -u)/com.deskhopplus.helper
```

## Confirming the transport's premise on real hardware

[ADR-0001](../../docs/adr/0001-vendor-hid-transport.md) leaves one macOS confirmation open: that
the channel's vendor collection landed in its own `IOHIDDevice` with no `RequiresTCCAuthorization`
property. That is what `tools/macos-checks/confirm_hid_tcc.py` measures, and it should be run once
against real hardware before the helper's permission-free property is relied on.
