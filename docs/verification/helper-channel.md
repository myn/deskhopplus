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
board on the old firmware, silently. Confirm the number `./tools/build.sh` prints is ahead of
what the boards are running before flashing. **Both boards ran 0.89 as of 2026-08-13.**

**Peer propagation works — measured 2026-08-12.** It had never been observed until then, for the
mundane reason that the version had never moved between flashes. Flash board A by chord and
`picotool`, leave B alone, and B pulls the new firmware over the inter-board link. Both boards
ended that sitting on 0.83 from a single `picotool load`.

### A board mid-upgrade takes minutes, and used to go silent

Budget **minutes**, not seconds: the pull is byte-at-a-time over the UART with a round trip each,
across a 256 KB image.

**What this looked like on 2026-08-12, before #90 was fixed.** Throughout the pull the receiving
board sent no heartbeat at all, so its peer reported *not detected* — checking the peer version
straight after flashing showed nothing even when everything was working. That was misread for over
an hour as a dead inter-board link and then as an unpowered board; the link was carrying keyboard
and mouse traffic the whole time, which is the evidence that should have settled it immediately.
Worse, a stalled upgrade never ended: nothing cleared `upgrade_in_progress`, so the board stayed
silent, could not retry, and could not time out of config mode — measured at 14 minutes, well past
the 300 s that should have rebooted it. Recovery was a power cycle. The deceptive tell was the
**LED reverting to the normal-mode indicator** — lit while that board was the active output, dark
when the cursor was on the other computer — because `blink_led` sat behind the same early return,
so the board looked precisely as though it had left config mode.

**What to expect now.** The early return is gone, so during a pull the board keeps heartbeating
(its peer reports a version throughout), keeps blinking if it is in config mode, and still honours
the config-mode timeout. A transfer that makes no progress for 30 s is abandoned: the pull restarts
from address 0, which rewrites every page and repairs a half-written image. If that restart also
stalls, the board erases its stage 2 bootloader and drops to ROM — a `RPI-RP2` volume and a manual
UF2 drop, deliberately loud rather than a board quietly running a part-written image.

**Confirmed on hardware 2026-08-12.** With 0.85 on A and 0.84 on B, A's config page read *Other
board FW version: v0.84* continuously through B's pull. Pre-fix that field read `not detected` for
the whole transfer.

### Why a pull used to need a power cycle to take effect

Worth knowing, because every account of this before 0.88 describes the symptom rather than the
cause. The transfer **could not terminate**: `firmware_upgrade_task` completed on
`address > STAGING_IMAGE_SIZE`, the address only advanced on a response, and
`handle_request_byte_msg` refused to answer anything at or beyond that bound. So the last request
went unanswered every time, and the board waited on it forever.

Every page is written before that point, so the flash was complete and correct when the wait began
— which is why a power cycle "finished" the upgrade and why propagation looked like it worked. It
had not; it had stopped one unanswered request from the end. The 14-minute config-mode hang
recorded here was this, not a dropped packet.

Fixed in 0.88. A pull now reboots the receiving board by itself. **If you find yourself
power-cycling a board to make an upgrade take, that is a regression, not the procedure.**

**Confirmed 2026-08-12**, both boards on 0.88, A flashed to 0.89 by `picotool`: B pulled, rebooted
itself, and both boards read v0.89. No power cycle, no intervention. That is the first pull in this
tree's history known to have terminated on its own.

### Getting a fix onto the receiving board

The trap that cost most of that evening. Everything in the pull path that matters — the stall
window, the retry, the end condition — runs on the **receiving** board, which is by definition the
one still running the *old* firmware. So the first propagation after any change to this path
exercises the old code and proves nothing about the new.

Two ways through it, and it is worth picking one deliberately rather than discovering this again:

- **Flash both boards directly.** Board B is reachable by moving its USB cable to the Mac and
  holding BOOTSEL while plugging in — the role probe reads the inter-board harness, not the host,
  so B stays B. Needed when the old firmware cannot deliver the new one at all.
- **Let the old path deliver the fix, then verify it removed the need.** Flash A, let B pull, and
  work around the old firmware's failure once (before 0.88, a power cycle during a quiet phase).
  Then bump again and watch the same transfer complete unaided. This is how 0.88 reached B.

**One exception, and it will catch you on every first flash.** The heartbeat during a pull comes
from the *receiving* board, which is by definition the one still running the **old** firmware. So
the first propagation after any change to this path still shows `not detected`, and only the second
one — once both boards carry the fix — measures anything. Losing an evening to this is how it was
found.

Watch out for one more thing while testing: **entering config mode reboots the board**, so chording
A mid-pull leaves B's outstanding request unanswered. B re-asks every 100 ms and rides through it,
but before that retry existed each such gap cost a full 30 s stall and a restart from address 0 —
and two of those in one transfer is ROM recovery. That is exactly how a board ended up in BOOTSEL
on 2026-08-12, for what was really a dropped packet.

**The LED is still not a mode indicator during an upgrade.** In config mode the once-a-second
config blink now runs at the same time as the per-block `toggle_led` of a UF2 drop, and the two
overlap for the few seconds a drop takes. **Use the USB identity** — `0x2e8a/0x107c` is config
mode, `0x1209/0xc000` is normal:

```sh
system_profiler SPUSBDataType | grep -A4 "DeskHop Switch" | grep -E "Product ID|Vendor ID"
```

### Reading a running version without the DESKHOP volume

The volume may never mount: `diskarbitrationd` wedges, `/dev/disk2` appears, and `diskutil` hangs
indefinitely and returns nothing. `sudo killall diskarbitrationd` clears it — launchd respawns it.

You do not need the volume. The config page drives the board over WebHID, so serving the repo's own
copy works identically and sidesteps the mount, the wedge, and the hazard of macOS writing
`.fseventsd` onto the device:

```sh
cd webconfig && python3 -m http.server 8777 --bind 127.0.0.1   # then open 127.0.0.1:8777/config.htm
```

Localhost is a secure context, which WebHID requires; `file://` is not reliably one. A config-mode
entry reboots the board and drops the connection, so reconnect after each — but the page itself is
static and needs no re-serving.

Since [#89](https://github.com/myn/deskhopplus/issues/89) that page reports **both** versions: *This
board FW version* and *Other board FW version*, the latter reading `not detected` when no heartbeat
has arrived. That is how propagation was confirmed rather than inferred.

**Regenerating the page** needs `/usr/local/bin/python3.13` and a jinja2 venv — the system python is
3.9 and fails on `int | None`. On macOS the image is patched with mtools rather than
`disk/create.sh`, which wants Linux mount and sudo:

```sh
cd disk && mcopy -o -i disk.img ../webconfig/config.htm ::/config.htm
```

### Which flashes reset the configuration, and which do not

Two different mechanisms, routinely confused for each other:

- **`CURRENT_CONFIG_VERSION` went 8 → 9** for #46's pairing secret. That was spent on the first
  boot of 0.80. The constant is **still 9**, so a version bump is not in play today.
- **`5cf72db` moved the checksum** from offset 152 to 156 (`_reserved` became `uint32_t[2]`), so
  a config written by any earlier build cannot validate. That fired once, on the first boot of
  **0.81**, and is also spent. `7541a25` is often blamed for this and did not do it: it named
  interior padding and moved `config_t` to `config_layout.h`, moving no bytes.

Since 0.81 the layout has been stable, and `FLASH_CONFIG` is `NOLOAD`, so **flashing does not
reset anything**. If a board comes back unpaired after a flash, look elsewhere.

One trap in how this recovers: `load_config` (`src/utils.c:116`) falls back to defaults **in RAM
only** — it never writes them back. So an invalid stored config defaults on *every* boot, not
once, until something calls `save_config`. "The first boot defaults and the second is the test"
only holds because re-pairing performs that write
(`channel_open_pairing_window` → `save_config`, `src/channel.c:146`).

### Flash by ROM bootloader

Not from macOS onto the config-mode volume: macOS writes `.fseventsd` onto any volume it mounts
and the device can reject the image *silently*, rebooting exactly as it would on success but
running the old firmware. The managed Windows laptop denies writes to that volume outright
(#58). So for both boards: hold the on-board button while connecting, and copy the `.uf2` to
the `RPI-RP2` drive.

**Prefer `picotool` — it needs no mounted volume at all** (`brew install picotool`):

```sh
sudo picotool load -x /path/to/deskhop.uf2      # -x reboots into the new firmware
```

Board in BOOTSEL first (hold the on-board button while connecting). This sidesteps both hazards
above, and it is the only route that worked on 2026-08-11: the `RPI-RP2` volume would not
automount and `diskutil` hung, because `diskarbitrationd` was wedged — the visible tell being a
stale root-owned `/Volumes/DESKHOP` mount point left behind by an earlier config-mode exit
(`sudo rmdir` it). `sudo` is required, and it needs a real terminal: the `!` prefix inside a
Claude Code session has no TTY for the password prompt.

Note `picotool info -a` **segfaults** (v2.3.0, macOS) whenever a BOOTSEL board is present, with
or without `sudo`. `load` is unaffected. Do not read that crash as a flashing problem.

The UF2 spans `0x10000000`–`0x10040000`, and `FLASH_CONFIG` lives at the end of the 2 MB flash
and is `NOLOAD` — so **a firmware write does not erase the configuration**. If a board comes
back unpaired after flashing, the firmware write is not the reason.

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

Board A `E6654854577F452F`, board B `E665485457895030` — #46 pins pairing to these serials.
**Which computer each board serves has changed at least once** (as of 2026-08-11: A on the Mac,
B on Windows), so check the serial the machine actually enumerates rather than assuming.

### Two things that will waste an hour if you do not know them

**The config chord is a toggle.** `Left Ctrl + Right Shift + C + O` enters config mode; pressing
it again leaves. Two presses in quick succession therefore enter and immediately exit —
measured at about **3 seconds** in config mode, far too brief for macOS to mount the `DESKHOP`
volume. The symptom is "the chord does not work, I never see the drive", and the cause is
pressing it twice. Tap once, then leave the keyboard alone. `config_enable_hotkey_handler` is
the whole story: it sets the config magic only when config mode is *not* already active.

**The pairing window is per-board.** `scratch[3] = MAGIC_WORD_PAIR` is set only on the board
that processes the chord, and `channel_open_pairing_window` writes the secret into *that
board's* flash. Nothing crosses the UART. So the chord must be pressed on the board attached to
the computer whose helper you want paired, and a secret from one board is refused by the other
(measured 2026-08-11). Pressing the chord on the Windows side does nothing for the Mac's helper.

**The helper must already be running when the window opens.** The window is 60 s from the
normal-mode boot, and it can only provision a helper that is connected while it is open. Start
the helper — ideally as the `LaunchAgent`, so it survives the device's reboot — *before*
pressing the chord.

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
- [x] A config-mode round trip reconnects by itself. Press the chord, watch the helper report
      **device in config mode** distinctly from **device not connected**, leave config mode,
      confirm it reconnects with no interaction. **Passed 2026-08-13 (#73).** The helper was
      *restarted* into a device already in config mode — the harder case, and the one #73 was
      filed for — reported `Device in config mode`, and held it for 30 s, six times the 5 s
      silence window, without decaying. It recovered unaided when the device returned
- [ ] The `LaunchAgent` restarts the helper after a crash. Install per `helpers/macos/README.md`,
      then `kill -9` it and confirm it comes back. Note: run the installed copy from launchd
      only — running it once from a terminal that itself holds permissions is the false-pass
      trap recorded in the macOS research
- [ ] Hello and heartbeat run end to end against real firmware
- [x] The device marks the helper absent a couple of intervals after the helper is stopped, and
      **says so**. Stop the helper (`SIGSTOP`, not `SIGTERM` — a clean exit closes the channel
      and is a different path); about three seconds later the device emits `SESSION_END` with
      reason `1`. Resume it and confirm it reconnects by itself. **Passed 2026-08-13** — and
      read the next paragraph before recording it, because the frame arrives *too late to act
      on* and that is by design, not a fault
- [x] **The helper notices a session it no longer has.** The device's beat is traced at its
      edges — `device heartbeat: first beat of the session`, then `quiet for Xs` / `resumed
      after Xs`. Per-beat logging is deliberately absent: at a beat a second it buries
      everything else, which is how #94 stayed invisible for two days. **Passed 2026-08-13**:
      the first beat appeared on 0.89 and no `quiet` line followed, so beats kept arriving
      inside the 3 s deadline for as long as the session was watched
> **A reason-1 `SESSION_END` cannot be "acted on".** Measured 2026-08-13: the device does send it,
> and it arrives and decodes — but the helper logs `ignoring a session end outside a session`,
> because on resume its own 3 s deadline has already fired. That is structural, not a race worth
> re-running: both ends take the deadline from the same constant, so a helper silent long enough
> to earn the frame has necessarily already concluded the same thing. `channel.c:207` states the
> intent — the frame is *"an optimisation over that timeout, never a substitute for it"*. Only a
> reason-2 (protocol error) end can reach a healthy helper, and producing one means displacing the
> helper whose reaction is the measurement.

- [ ] **The beats stop while a transfer runs and resume when it finishes** (ADR-0004's
      idle gating). ***Not run — not runnable on this build.*** Nothing can make a direction
      busy: the helper has no payload source (`main.swift`: *"No payloads yet — clipboard
      (#52, #55, #56) and cursor placement (#51)"*), and the device only *routes* bulk
      (`channel.c:253`), never originates it. Blocked on a payload existing at either end

### Boxes that cannot be measured on this build

**Partial acquisition is degenerate at one channel.** `DH_SESSION_CHANNEL_COUNT` is `1`
(ADR-0002 ships one channel), so there is no *partial* to acquire: holding "one channel" from
another process is holding all of them, which is the second-process refusal already checked
above. The all-or-nothing rollback the helper implements is real and covered by host tests, but
hardware cannot distinguish it from a plain refusal until the channel count rises.
**This box becomes measurable at [#63](https://github.com/myn/deskhopplus/issues/63)** and
should be recorded as not run, blocked on it.

**~~The absent transition is invisible from outside.~~** *Resolved by
[#68](https://github.com/myn/deskhopplus/issues/68) / ADR-0004, 2026-08-11.* It used to be that
`channel_task` discarded `dh_session_tick`'s return and nothing device-side signalled the
transition — no LED, no frame, no counter — so the box was recorded as not run, blocked on #68.
The tick now puts the transition on the wire as `SESSION_END`, and the box above is measurable.
Both boxes are worth running together: the eviction and the helper's own detection of it are the
two halves of the same mechanism, and only hardware exercises them against real timing.

## 2. Pairing (#46)

The secret is never displayed or typed, so distinctness is observed through the helper's copy:

```sh
shasum ~/Library/Application\ Support/deskhopplus/secret
```

- [x] `pico_rand` produces a different secret on each window — two chord presses, two distinct
      digests from the command above. **Passed 2026-08-11** across five windows, and re-confirmed
      2026-08-13 on 0.89: `343e3568…` → `9ce4e1a7…` across one window
- [x] The secret survives a power cycle, and the helper reconnects paired with no interaction.
      **Failed on 0.80 and was the symptom that found [#74](https://github.com/myn/deskhopplus/issues/74)**
      — `config_t`'s CRC covered the checksum field, so every boot loaded defaults and the
      secret went with it. Fixed in `5cf72db`. (Earlier revisions of this sheet cited `5fb082d`,
      which is a pre-rebase copy carrying the same subject and is **not reachable from `main`**.)
      **Passed 2026-08-13 on 0.89**: board A unplugged and replugged, helper returned to
      `Connected and paired` with no chord press and no restart of the helper process. The
      config under test was written 2026-08-12 21:07, so it survived the whole 0.83 → 0.89
      sequence as well as the deliberate cycle
- [x] `scratch[3]` survives the config-mode reboot: press the chord to **enter** config mode,
      then leave by the inactivity timeout or the web UI **rather than the chord**, and confirm
      the window opens on the way back. This is the exact path the review found broken when the
      flag lived in `scratch[4]`, which the SDK's own `watchdog_enable()` overwrites.
      **Passed 2026-08-13**: entered by chord, left by the **inactivity timeout** at ~5 min
      untouched, and the window opened on the normal-mode boot — `paired by the device` in the
      log without anyone pressing anything. The timeout firing unaided also re-confirms #90's
      fix, since a stalled board used to hold config mode open indefinitely (measured at 14 min)
- [x] Rotation evicts: pair, press the chord again, confirm the old secret no longer
      authenticates. Keep a copy of the first secret file and restore it over the new one to
      test this — the helper must be refused, and must report **not paired**. **Wait out the
      60 s window first**: inside an open window the device simply re-grants and the test looks
      like a failure to evict, when in fact the old secret *was* rejected before the new grant.
      **Passed 2026-08-13, and observed without the restore trick.** On the normal-mode boot the
      helper still held the *pre-rotation* secret, so the rejection and the re-grant both appear
      in order, which is exactly the sequence this box exists to separate:

      ```
      state: Not paired — press the config chord on the device   <- old secret refused
      paired by the device                                       <- window grants a new one
      state: Connected and paired
      ```
- [ ] A configuration wipe leaves the helper unpaired, and one chord press restores it. The
      8 → 9 config version change gave this for free on the *first* boot of 0.80 and that is
      spent — `CURRENT_CONFIG_VERSION` is still 9, so a later flash wipes nothing. Trigger it
      deliberately with the wipe chord, **`Right Shift + F12 + D`**, which **wipes both boards**:
      `wipe_config_hotkey_handler` erases locally and sends `WIPE_CONFIG_MSG` to the peer.
      **The wipe does not take effect until the device is power-cycled** — see #75; the live
      session keeps authenticating against the RAM-cached secret. So: wipe, power-cycle,
      *then* expect **not paired — press the config chord on the device**

## 3. The #66 controls that could reopen #25

Both need the **other board unplugged** so peer firmware propagation cannot interfere, and
downgrades never propagate, so re-flashing 0.80 afterwards is a manual ROM-bootloader step on
each board.

Switch the device to the machine under test *before* rebooting it. Rebooting a computer the
device is not pointed at looks exactly like a keyboard failure and is the easiest way to record
a false negative.

**Both are superseded — do not run them.** Recorded 2026-08-11, agreed at the desk:

- ~~**Windows**: BOOTSEL board B, flash **stock upstream**, retest UEFI setup~~ — #66's own
  reference-device control settles it: the MKC75 receiver plugged *directly* into the HP
  navigates UEFI, the same keyboard through the device does not, and interface 0 is
  **byte-identical** between stock and this fork. #66 records this control as "not worth
  running"
- ~~**macOS**: BOOTSEL board A, flash stock upstream, retest the FileVault prompt~~ — already
  run against stock upstream on 2026-08-10 and recorded on #66; it failed identically

Running them would cost four ROM-bootloader flashes plus a manual downgrade on each board, for
answers #66 already holds. This section stayed on the checklist only because it was written
before #66's third comment landed; **#66 is the current record, not this sheet**.

#66 already establishes against stock that the keyboard is not a boot keyboard
(subclass/protocol 0) and does not work at the FileVault prompt — so the macOS control is
expected to *fail in the same way*, which is the point: a matching failure attributes it to
upstream rather than to this fork. A macOS control that *passes* would be the surprise.

## 4. Restore

- [ ] Both boards back on the current release build by ROM bootloader (0.89 as of 2026-08-13).
      Only a board actually reflashed needs restoring — a dev build stamped with the *same*
      version as its peer cannot propagate, so the other board is never disturbed
- [ ] Configuration re-entered through the web UI
- [ ] Both computers: keyboard, mouse, switching, and config mode all still work

## Not in this sitting

**ADR-0005's bounded outbound queues cannot be exercised.** *A sustained transfer neither drops
frames nor delays a session reply behind bulk* needs a sustained transfer, and no payload source
exists at either end — the same wall as the relay below, reached from the other side. The queues
are covered by `outq_test` on the host and are otherwise unmeasured on hardware. This is worth
knowing before treating this sitting as a complete baseline for #81: **the least-exercised change
in the set stays least exercised.**

**The relay (#47) moved out.** *A frame crosses a real UART and arrives intact* and *the burst
cap holds under load* both need an endpoint on the far side to receive the frame, and only the
macOS helper exists — [#49](https://github.com/myn/deskhopplus/issues/49) is open. Inbound
relay is gated on `dh_session_may_relay`, so an unpaired Windows-side endpoint receives nothing
even if one were written. Tracked separately and blocked on #49;
[#39](https://github.com/myn/deskhopplus/issues/39) (throughput) is behind the same wall and
wants the same sitting.
