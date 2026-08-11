# Verifying the helper channel: session and pairing

The procedure behind [#70](https://github.com/myn/deskhopplus/issues/70). It exists as a
document rather than a one-off checklist for the same reason as
[the vendor HID one](vendor-hid-interface.md): #45's session, #46's pairing and #49's Windows
helper all touch this surface again, and every change has to answer the same questions on the
same hardware.

**Why hardware.** #45, #46 and #47 are covered by host tests at the session seam, and all of
them pass — 29 helper tests, plus the C core's own. What no host test reaches is IOKit seizure
against a real `IOHIDDevice`, `pico_rand` on a real RP2040, `scratch[3]` surviving a real
watchdog reboot, and launchd restarting a real crashed process. Those are the boxes below.

**One sitting, not five.** A config-mode round trip is needed for pairing and interferes with
the pre-boot tests, and #66's controls need the *other* board unplugged so firmware propagation
cannot interfere. Doing this ad hoc is how #25's Windows pre-boot result was measured without
its control, believed for a day, and then retracted.

## Before you touch a board

### The version must move, or only one board changes

`handle_heartbeat_msg` pulls firmware from the peer only when the peer reports a **strictly
newer** version, so flashing a build carrying the version already running leaves the second
board on the old firmware, silently. The four channel commits (#45, #46, #47 and the relay)
touched `CMakeLists.txt` without bumping it; **`VERSION_MINOR` was raised 79 → 80 for this
sitting**. Confirm it is still ahead of what the boards are running before flashing.

### This flash resets the configuration

`CURRENT_CONFIG_VERSION` went **8 → 9** — the pairing secret joined `config_t`. `load_config`
falls back to `default_config` on any version mismatch, so screen layout, hotkeys and every
other setting return to defaults on first boot of this build. Expected, not a fault. Record
anything you want to keep before starting.

### Flash by ROM bootloader

Not from macOS onto the config-mode volume: macOS writes `.fseventsd` onto any volume it mounts
and the device can reject the image *silently*, rebooting exactly as it would on success but
running the old firmware. The managed Windows laptop denies writes to that volume outright
(#58). So for both boards: hold the on-board button while connecting, and copy the `.uf2` to
the `RPI-RP2` drive.

### Build

```sh
./tools/build.sh                 # firmware, helper, and both test suites
```

It ends by printing the artifact to flash, its timestamp, and **the version stamped into it**,
read back out of `build/deskhop.crc` rather than out of `CMakeLists.txt` — so it reports what
is in the file rather than what someone intended to be. Check that number is above what the
boards are running before you flash anything. A red bar in either test suite means stop.

Auth must be **on** — `DH_DEV_NO_AUTH` defaults to `OFF` and the pairing checks are meaningless
without it. Do not pass the flag for this sitting.

### The boards

Board A `E6654854577F452F` (Mac), board B `E665485457895030` (Windows) — #46 pins pairing to
these. The config chord is **Left Ctrl + Right Shift + C + O**; it both enters and leaves
config mode, and either direction opens a pairing window.

## Recording results

Copy the checklist into #70 and fill it in with the **measurement**, never a tick. A check that
cannot be run is recorded as **not run** with the reason — never as a pass.

The helper's log is the surface until the menu-bar item exists (#54): `/tmp/deskhop-helper.log`
under launchd, or stderr when run in the foreground. Every state change prints as
`deskhop-helper: state: <message>` — the same sentence the menu bar will carry.

---

## 1. The session (#45)

```sh
swift run deskhop-helper         # foreground, logging to stderr
```

- [ ] Every channel opens with `kIOHIDOptionsTypeSeizeDevice` against the real device, and a
      second process is refused. Start a second `deskhop-helper` while the first holds the
      channel; the second must report **another program holds the channel** and must **not**
      prompt the config chord — those are different states with different remedies
- [ ] **Partial acquisition fails the session.** *Expected `not run`* — see the note below
- [ ] A config-mode round trip reconnects by itself. Press the chord, watch the helper report
      **device in config mode** distinctly from **device not connected**, leave config mode,
      confirm it reconnects with no interaction
- [ ] The `LaunchAgent` restarts the helper after a crash. Install per `helpers/macos/README.md`,
      then `kill -9` it and confirm it comes back. Note: run the installed copy from launchd
      only — running it once from a terminal that itself holds permissions is the false-pass
      trap recorded in the macOS research
- [ ] Hello and heartbeat run end to end against real firmware
- [ ] The device marks the helper absent a couple of intervals after the helper is stopped.
      *Expected `not run`* — see the note below

### Two boxes that cannot be measured on this build

**Partial acquisition is degenerate at one channel.** `DH_SESSION_CHANNEL_COUNT` is `1`
(ADR-0002 ships one channel), so there is no *partial* to acquire: holding "one channel" from
another process is holding all of them, which is the second-process refusal already checked
above. The all-or-nothing rollback the helper implements is real and covered by host tests, but
hardware cannot distinguish it from a plain refusal until the channel count rises.
**This box becomes measurable at [#63](https://github.com/myn/deskhopplus/issues/63)** and
should be recorded as not run, blocked on it.

**The absent transition is invisible from outside.** `channel_task` discards
`dh_session_tick`'s return — `(void)dh_session_tick(...)` — and nothing device-side signals the
transition: no LED, no frame to the helper, no counter. The one observable consequence is
`dh_session_may_relay` going false, and the relay checks moved out of this sitting. This is
exactly the gap [#68](https://github.com/myn/deskhopplus/issues/68) exists to close, and it
cannot be closed without a wire-format change. Record as not run, blocked on #68 — and note
that #68 is therefore load-bearing for verification, not only for #52.

## 2. Pairing (#46)

The secret is never displayed or typed, so distinctness is observed through the helper's copy:

```sh
shasum ~/Library/Application\ Support/deskhopplus/secret
```

- [ ] `pico_rand` produces a different secret on each window — two chord presses, two distinct
      digests from the command above
- [ ] The secret survives a power cycle, and the helper reconnects paired with no interaction
- [ ] `scratch[3]` survives the config-mode reboot: press the chord to **enter** config mode,
      then leave by the inactivity timeout or the web UI **rather than the chord**, and confirm
      the window opens on the way back. This is the exact path the review found broken when the
      flag lived in `scratch[4]`, which the SDK's own `watchdog_enable()` overwrites
- [ ] Rotation evicts: pair, press the chord again, confirm the old secret no longer
      authenticates. Keep a copy of the first secret file and restore it over the new one to
      test this — the helper must be refused, and must report **not paired**
- [ ] A configuration wipe leaves the helper unpaired, and one chord press restores it. The
      8 → 9 config version change gives you this for free on first boot: the helper should come
      up **not paired — press the config chord on the device**

## 3. The #66 controls that could reopen #25

Both need the **other board unplugged** so peer firmware propagation cannot interfere, and
downgrades never propagate, so re-flashing 0.80 afterwards is a manual ROM-bootloader step on
each board.

Switch the device to the machine under test *before* rebooting it. Rebooting a computer the
device is not pointed at looks exactly like a keyboard failure and is the easiest way to record
a false negative.

- [ ] **Windows**: BOOTSEL board B, flash **stock upstream**, retest UEFI setup.
      **If stock works where 0.80 does not, the added interface caused a regression and #25
      should be reopened**
- [ ] **macOS**: BOOTSEL board A, flash stock upstream, retest the FileVault prompt as an
      isolated control

#66 already establishes against stock that the keyboard is not a boot keyboard
(subclass/protocol 0) and does not work at the FileVault prompt — so the macOS control is
expected to *fail in the same way*, which is the point: a matching failure attributes it to
upstream rather than to this fork. A macOS control that *passes* would be the surprise.

## 4. Restore

- [ ] Both boards back on 0.80 by ROM bootloader
- [ ] Configuration re-entered through the web UI
- [ ] Both computers: keyboard, mouse, switching, and config mode all still work

## Not in this sitting

**The relay (#47) moved out.** *A frame crosses a real UART and arrives intact* and *the burst
cap holds under load* both need an endpoint on the far side to receive the frame, and only the
macOS helper exists — [#49](https://github.com/myn/deskhopplus/issues/49) is open. Inbound
relay is gated on `dh_session_may_relay`, so an unpaired Windows-side endpoint receives nothing
even if one were written. Tracked separately and blocked on #49;
[#39](https://github.com/myn/deskhopplus/issues/39) (throughput) is behind the same wall and
wants the same sitting.
