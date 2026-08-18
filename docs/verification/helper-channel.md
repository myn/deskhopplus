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
what the boards are running before flashing. **Both boards ran 0.92 as of 2026-08-17 evening** —
board A by `picotool`, board B by peer propagation over the inter-board link, read back in the
config UI rather than assumed. That is the third time propagation has been observed working end to
end.

`26f4c25` is the example of what this section is for: it fixed #104 and left `VERSION_MINOR` at 91,
the number both boards already ran, so the fix could not have reached hardware at all. `b6c589c`
bumped it. Nothing warns you — [#91](https://github.com/myn/deskhopplus/issues/91) is the proposal
to make an equal-version CRC mismatch propagate anyway.

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
indefinitely and returns nothing.

**`sudo killall diskarbitrationd` does not clear it.** Measured 2026-08-17 on Darwin 24.6: the
daemon is SIP-protected, so the command returns without effect and `/usr/libexec/diskarbitrationd`
keeps both its pid and its original start time. This sheet asserted the opposite for months and it
cost a cycle to find out. Check `ps -p <pid> -o lstart` rather than trusting that it respawned.

Mounting the device by hand does work, because `mount_msdos` talks to the kernel and never
involves that daemon:

```sh
sudo mount -t msdos /dev/disk2 /Volumes/DESKHOP    # confirm the node first, it moves
```

**Unmount before the board leaves config mode.** The device disappears on exit but the mount does
not, and a stale mount pins the departed USB device alive: `ioreg` then reports *two* boards in
config mode at once, the next config entry lands on `/dev/disk3`, and mounting `/dev/disk2` again
fails with `Resource busy`. A driver watching USB identity sees the ghost and never observes the
board return to normal. `sudo umount -f /Volumes/DESKHOP` clears it, as does unplugging the board.

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

**Regenerating the page** needs jinja2 and python 3.10+ — the system python is 3.9 and fails on
`int | None`. `make venv` in `webconfig/` builds a suitable one, and from #16 the build drives the
whole chain itself:

```sh
cd webconfig && make venv    # once, if you are changing the config UI
cmake --build build          # renders the page, rebuilds the image, rebuilds the firmware
```

Both `webconfig/config.htm` and `disk/disk.img` stay committed, so building firmware needs no python
at all — only changing the UI does. `disk/create.sh` builds the image by hand and is mtools-only
now, with no mount and no sudo; the hand-patch this sheet used to recommend
(`mcopy -o -i disk.img ...`) is no longer needed and will now be caught by CI as a stale image.

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

**The firmware-upgrade chords, which need no replug and no on-board button** (#88 asked this sheet
to name them; confirmed working on board A, 2026-08-17):

| Chord | Effect |
|---|---|
| `Left Shift + Right Shift + A` | Board A into BOOTSEL |
| `Left Shift + Right Shift + B` | Board B into BOOTSEL — travels as `FIRMWARE_UPGRADE_MSG`, since the keyboard is on A |

Defined at `src/keyboard.c:97-111`. Board A's is a local `reset_usb_boot`; board B's crosses the
inter-board link, so B reboots itself on receipt. Prefer these over the button — the board stays
plugged in, and nothing has to be reseated.

Board in BOOTSEL first (chord above, or hold the on-board button while connecting). This sidesteps
both hazards above, and it is the only route that worked on 2026-08-11: the `RPI-RP2` volume would not
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

**The caps-lock half of every LED indication is dead — do not use it to confirm anything.**
Measured 2026-08-17: no flash on the wipe chord, and no blink at all through a config-mode
session, which the user manual explicitly promises. Not a fork defect and not new — upstream
`b65e8eb` added `led_sync_task`, which runs at 30 Hz right after `led_blinking_task`
(`src/main.c:56-57`), sees `keyboard_leds_actual != keyboard_leds_desired` and writes the desired
state straight back (`src/led.c:60-66`). The blink only toggles every 80 ms, so its keyboard-LED
half is reverted within ~33 ms and is invisible. The **on-board LED still blinks** — sync never
touches it — so read the board, never the keyboard. This is the same restore-versus-blink fight
the comment at `src/tasks.c:186-189` already describes one layer down.

**A power cycle can cost the board its USB keyboard.** Observed once, 2026-08-17: board A came
back from an unplug/replug with the channel healthy and every HID interface present to the Mac,
but no key reached it. Board A is the USB *host* for the physical keyboard and has to enumerate
it on boot; that did not take. Unplugging and replugging the keyboard from board A fixed it, with
the double LED flash that confirms a new peripheral (`README.md:393`). Not `enforce_ports` —
that defaults to `0` (`src/include/user_config.h:192`). Budget for it before a sitting that leans
on chords, and do not read a dead chord as a firmware fault until the keyboard has been replugged.

### The hotkey bindings, checked against the defaults

#88 asked for the config UI's hotkey list to be compared against the defaults. **The config UI has
no hotkey list** — checked 2026-08-17 — so that comparison cannot be made there and the box is
answered against the **user manual** instead, which does carry a shortcut table.

All ten shortcuts the manual documents match the firmware defaults exactly (`src/keyboard.c:18-111`,
with `HOTKEY_MODIFIER` / `HOTKEY_TOGGLE` at `src/include/user_config.h:50-51`). **Three real
bindings are missing from the manual:**

| Chord | Missing from the manual |
|---|---|
| `Left Ctrl + Right Shift + J` | Screensaver **jitter** — the manual documents only pong (`S`) and off (`X`) |
| `Left Shift + Right Shift + A` | Board A into BOOTSEL |
| `Left Shift + Right Shift + B` | Board B into BOOTSEL |

The manual instead tells you to hold the BOOTSEL button while plugging the cable in, which is the
slow route and the one that needs the board reseated. Nothing is wrong in the manual; it is
incomplete, and the two BOOTSEL chords are the ones worth knowing.

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

- [x] Every channel opens with `kIOHIDOptionsTypeSeizeDevice` against the real device, and a
      second process is refused. Start a second `deskhop-helper` while the first holds the
      channel; the second must report **another program holds the channel** and must **not**
      prompt the config chord — those are different states with different remedies.
      **FAILED 2026-08-13 — see [#95](https://github.com/myn/deskhopplus/issues/95).** macOS does
      not refuse the second seize. A second helper reported `holding 1 channel(s) exclusively` and
      `Connected and paired` while the first held a live session and saw no disruption at all;
      `tools/macos-checks/probe_seize_exclusivity.swift` independently got
      `IOHIDDeviceOpen(seize) -> 0x00000000` and then **read ten `DEVICE_HEARTBEAT` frames off the
      seized channel**. The state this box asks for is unreachable on macOS, so the check cannot
      pass as written. Windows refusal was measured separately and stands
- [ ] **Partial acquisition fails the session.** *Expected `not run`* — see the note below
- [x] A config-mode round trip reconnects by itself. Press the chord, watch the helper report
      **device in config mode** distinctly from **device not connected**, leave config mode,
      confirm it reconnects with no interaction. **Passed 2026-08-13 (#73).** The helper was
      *restarted* into a device already in config mode — the harder case, and the one #73 was
      filed for — reported `Device in config mode`, and held it for 30 s, six times the 5 s
      silence window, without decaying. It recovered unaided when the device returned
- [x] The `LaunchAgent` restarts the helper after a crash. Install per `helpers/macos/README.md`,
      then `kill -9` it and confirm it comes back. Note: run the installed copy from launchd
      only — running it once from a terminal that itself holds permissions is the false-pass
      trap recorded in the macOS research. **Passed 2026-08-13**: pid 94445 `kill -9`'d, launchd
      respawned it as 9454 with `ppid 1` and `launchctl list` reporting last exit `-9`, and the
      session re-established unaided. One gotcha while checking this: `pgrep -f release/deskhop-helper`
      matches its *own* shell, since the pattern is in that command line — it will show a phantom
      second pid. Confirm against `ps -o ppid=` or `launchctl list`, not `pgrep` alone
- [x] Hello and heartbeat run end to end against real firmware. **Passed 2026-08-13**, repeatedly
      and incidentally: every session established during this sitting ran the full hello /
      hello_ack / beat exchange against 0.89
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
      fix, since a stalled board used to hold config mode open indefinitely (measured at 14 min).
      **Re-run passed 2026-08-17 on 0.91**, which is what this box needed: the pass above is
      0.89 evidence for a path #100 changed — the unmount no longer re-reads the secret. The
      chord was pressed on **entry only**, the exit was unattended, and a window still opened on
      the normal-mode boot, so `scratch[3]` survived the config-mode reboot on the new code path.
      The helper reported `Device in config mode` throughout (#73 holds on 0.91) and the board
      left config mode unaided (#90 holds on 0.91). Sequence in the log:

      ```
      state: Device in config mode
      state: Not paired — press the config chord on the device   <- old secret refused
      paired by the device                                       <- window granted a fresh one
      state: Connected and paired
      ```

      **The duration could not be measured**, only bounded: the exit was unattended and the log
      is consistent with the 300 s timeout counting from the last config-page read, but the
      helper log carries **no timestamps**, so entry time is unrecoverable. Anything that needs
      a config-mode duration — #92's first criterion — must be timed at the desk against the USB
      identity, not reconstructed from this log afterwards
- [ ] A bus reset inside the window does **not** cancel it — **not run, and not runnable by the
      method this box used to give.** #100 asked for "unplug and replug the board's USB within
      the 60 s". **That cannot test it.** Each board is powered only by the computer it plugs
      into (`README.md:468`) and the two are separated by a digital isolator that carries no
      power (`README.md:218`), so unplugging board A's USB is a **power cycle, not a bus reset**.
      On power loss the RP2040's watchdog scratch registers clear and RAM goes with them, so the
      window dies from the power cut whether the code calls `channel_init` (≤0.89) or
      `channel_link_lost` (0.91). The method cannot distinguish the defect from the fix.

      **Attempted 2026-08-17 on 0.91 and recorded as inconclusive**, for that reason and for a
      second one worth keeping: the helper was **already provisioned before the unplug**, because
      an open window grants within about a second of the helper saying hello. So the bus reset
      had no open window left to cancel. Any valid run has to keep the window unprovisioned at
      the moment of the reset.

      **Step 0 — settle which link-drop routes keep the board alive, before running anything
      else.** Config mode is the discriminator, and it is free. `is_config_mode_active` clears the
      magic as it reads it (`src/setup.c:112-121`), so config mode is one-shot: a board that
      reboots for *any* reason comes back in **normal** mode, and a board that only loses its host
      link never re-runs `initial_setup` and comes back **still in config mode**. So chord board A
      into config mode, apply the candidate drop, and read the identity that returns:

      | Identity after the drop | What it proves |
      |---|---|
      | `0x2e8a/0x107c` — config mode | the board kept running and re-enumerated. **The route works**, and `tud_umount_cb` fired |
      | `0x1209/0xc000` — normal mode | the board rebooted. That route can never test #100 |
      | nothing returns | the port died with the upstream cable — that hub is no use |

      Non-destructive, no pairing touched, under a minute per candidate. It must finish inside
      config mode's 300 s, and the keyboard is dead throughout, so anything after the chord has to
      already be running — lift `state()` out of
      `tools/macos-checks/config_timeout_with_stall.sh` rather than writing a third copy of it.

      A method that would work needs the board to **keep power while losing the host link**, with
      the helper stopped so nothing is provisioned first:

      1. Stop the helper (`launchctl bootout gui/$(id -u)/com.deskhopplus.helper`).
      2. Chord round trip, so the window opens on the normal-mode boot with no helper attached.
      3. Inside the 60 s, drop the host link without cutting board power — board A behind a
         **self-powered USB hub**, unplugging the hub's upstream cable; or sleep the Mac for a
         few seconds, which suspends the bus while the port stays powered.
      4. Start the helper, still inside the 60 s.
      5. 0.91 grants (`paired by the device`); ≤0.89 refuses (`Not paired`).

      Neither route is confirmed to produce a `tud_umount_cb` without a reboot — establish that
      first, or the next run is inconclusive for a third reason. The window is opened by a
      deliberate physical gesture and is the only way to provision a helper, so losing one to a
      re-enumeration costs a second chord press with nothing anywhere saying why the first did
      not take
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
- [x] A configuration wipe leaves the helper unpaired, and a chord round trip restores it. The
      8 → 9 config version change gave this for free on the *first* boot of 0.80 and that is
      spent — `CURRENT_CONFIG_VERSION` is still 9, so a later flash wipes nothing. Trigger it
      deliberately with the wipe chord, **`Right Shift + F12 + D`**, which **wipes both boards**:
      `wipe_config_hotkey_handler` erases locally and sends `WIPE_CONFIG_MSG` to the peer.
      **From 0.90 the wipe takes effect immediately** (#75 — before it, the secret stayed live
      in RAM and nothing was revoked until the next power-up). So, with **no power cycle**:

      ```
      the device ended the session: unpaired            <- the wipe, reason 3
      state: Not paired — press the config chord on the device
      ```

      Then, **before pressing anything**, power-cycle the board and confirm it is *still*
      unpaired — that is what separates a wipe that revoked from one that only looked like it
      did, and it is the pair of observations #75 was found by, in the order that makes them
      mean something. Only after that does a chord round trip return it to `Connected and paired`.

      **Passed on board A 2026-08-17, on 0.91, in that order.** All three observations, measured
      rather than ticked:

      | Step | Helper |
      |---|---|
      | Paired and connected | `Connected and paired` |
      | Wipe chord, **no power cycle** | `the device ended the session: unpaired` → `Not paired — press the config chord on the device` |
      | Power-cycle, **press nothing** | `Not paired — press the config chord on the device` — *still* unpaired |
      | Chord round trip | `paired by the device` → `Connected and paired` |

      Compare the 0.81 run this box exists because of, where the wipe changed **nothing at all**
      and only a power cycle brought the board up unpaired. `SESSION_END` reason 3 decoded at the
      helper as `unpaired`, so the revoke is legible at the desk rather than inferred.

      **Board B stays unmeasured** — the peer takes the identical path over `WIPE_CONFIG_MSG`
      through the same `_wipe_local_config`, but there is no Windows helper to observe it (#49).

      **"One chord press" was wrong and is corrected above.** One press only *enters* config
      mode; `channel_pairing_window_owed` is consumed on the next **normal-mode** boot
      (`src/setup.c:235`), so the window opens on the way back out. Recovery is a round trip:
      chord, wait for config mode, chord again. One press plus a 5-minute timeout also works, and
      is what the earlier runs on this sheet actually did

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

- [x] Both boards back on the current release build by ROM bootloader (0.89 as of 2026-08-13).
      Only a board actually reflashed needs restoring — a dev build stamped with the *same*
      version as its peer cannot propagate, so the other board is never disturbed.
      **Nothing to restore for the 2026-08-13 sitting**: no board was reflashed, both stayed on
      0.89 throughout, and only the *helper* was rebuilt
- [x] Configuration re-entered through the web UI — **not needed 2026-08-13**, the configuration
      was never wiped. See the config-wipe box in §2, which remains outstanding
- [ ] Both computers: keyboard, mouse, switching, and config mode all still work.
      **Not run as a deliberate check on 2026-08-13.** Config mode was exercised on board A and
      the Mac stayed usable throughout, but no switching test was run and the Windows side was
      not observed at all — board B was never attached to this machine during the sitting. Do not
      read the Mac's continued operation as covering this box

## 5. The two #90 stall paths ([#92](https://github.com/myn/deskhopplus/issues/92))

Sitting of **2026-08-17**. **Neither of #92's two criteria was met.** Three inductions were
attempted and all three were defeated on the host side rather than by the firmware. Recorded
anyway, because the reasons are reusable and one of them makes #92's own instructions wrong.

Time is USB identity sampled with `ioreg -r -c IOUSBHostDevice -l -d 1`, 0.03 s per sample.
`system_profiler SPUSBDataType` takes seconds and is too coarse to time a 300 s deadline against a
330 s bound. Board A was used throughout, deliberately: it is the board on the Mac, so a ROM
landing costs one `picotool load` rather than the cable trip board B would cost (#58).

### #92's stated induction for criterion 1 cannot produce criterion 1

#92 says to unplug the peer board mid-transfer and then watch for the config-mode timeout. That
cannot work. `fw_upgrade_must_recover` is `image_dirty && !peer_present` (`src/fw_upgrade.c:32`),
and a pull is past its first page within ~16 ms — so a peer board that has gone sends the stall to
**ROM**, which is criterion 2. Run as written, #92's two criteria are the same test, and it is the
destructive one.

Criterion 1 needs `upgrade_in_progress` set at t+300 s **with the peer still heartbeating**. The
only path that gives that is a partial UF2 drop onto the config-mode disk (`src/ramdisk.c:79`),
with board B left plugged in throughout.

### The config-mode timeout with nothing in flight

- [x] **301.25 s, measured 2026-08-17.** Entry 13:38:18.2 by chord, exit 13:43:19.5, identity
      `0x2e8a/0x107c` → `0x1209/0xc000`. This is the *plain* timeout and is a baseline only. It is
      **not** #92's criterion, which asks for the timeout to fire *despite* a transfer in flight:
      the `!upgrade_in_progress` guard at `src/tasks.c:183` was never exercised

### Config mode ends within ~330 s with a stalled transfer in flight

- [x] **312.6 s, measured 2026-08-17 on 0.92, fifth attempt.** Config entry **17:11:27.0**, drop of
      16 blocks written and synced at **t+280.9 s**, exit at **t+312.6 s** by USB identity
      `0x2e8a/0x107c` → `0x1209/0xc000`. Driven by
      `tools/macos-checks/config_timeout_with_stall.sh`; log kept at
      `~/deskhop-stall-run-20260817-171107.log`

Set against the 301.25 s plain timeout measured the same day, config mode was held **11.4 s past
its own deadline** by the transfer in flight and then released — which is the criterion. The
`!upgrade_in_progress` guard at `src/tasks.c:183` deferred the reboot, `fw_upgrade_stalled` came
true 30 s after the last block at t+310.9 s, `abandon_firmware_upgrade` cleared the flag, and the
board rebooted 1.7 s later. The bound in `fw_upgrade.h` — config mode held for at most
`CONFIG_MODE_TIMEOUT + FW_UPGRADE_STALL_US` — is now measured rather than argued.

The four earlier attempts all failed host-side, none of them on the firmware: (1) copy to
`/Volumes/DESKHOP` while it was not mounted, so nothing was written; (2) raw write to
`/dev/rdisk2`, `ENXIO` partway; (3) copy to a hand-mounted volume, blocked by the stale-mount
ghost; (4) induced cleanly, but the board went to **ROM** — the phantom pull of #104, fixed in
`26f4c25`.

### The peer was never lost: `peer_present` was true at the stall

This run settles what #104's triage had to leave open, and it settles it the other way from the
original reading.

`abandon_firmware_upgrade` recovers to ROM when `image_dirty && !peer_present` (`src/utils.c:109`).
Sixteen page writes had certainly set `image_dirty`. **The board did not go to ROM, so
`peer_present` was true** — board B was heartbeating throughout a UF2 drop, at the exact instant
the old reading claimed it had gone.

So the hypothesis this sheet carried for a day — sixteen flash programs from core0 with interrupts
disabled, starving `heartbeat_output_task` on core1 past `PEER_FW_STALE_US` — is **disproved**, not
merely unproven. The UF2 write path does not lose the peer. Every ROM landing on 2026-08-17 is
accounted for by the phantom pull instead, which is what #104 fixed.

The general lesson is the one that cost the day: the ROM landing was read as evidence about the
peer check when it was evidence about the pull. Only closing the other roads made the same
observation mean anything.

### How to run it, with the two traps that spoiled the last attempt

Both boards on **0.92** or later first — `26f4c25` fixed the phantom pull but left the version at
91, so `b6c589c` had to bump it before the fix could reach a board at all.

```sh
sudo ./tools/macos-checks/config_timeout_with_stall.sh check   # touches nothing
sudo ./tools/macos-checks/config_timeout_with_stall.sh run     # then press the chord
```

The script waits for config mode, takes `T` from the USB identity, mounts the disk, drops 16 blocks
at `T`+280 s, unmounts at once, and watches until the board leaves. It exists because this run
cannot be driven by hand: the config page must never be opened, the keyboard is dead throughout so
nothing can be typed after the chord, and the unmount has to land immediately. Run it from a real
terminal — `sudo` has no TTY for its password prompt inside a Claude Code session.

By hand it is:

1. Board A into config mode by chord. Note the time as `T`
2. `sudo mount -t msdos /dev/diskN /Volumes/DESKHOP` at ~`T`+10 s — confirm the node, it moves
3. At `T`+280 s, `dd if=build/deskhop.uf2 of=/Volumes/DESKHOP/part.uf2 bs=512 count=16`
4. Immediately `sudo umount -f /Volumes/DESKHOP`
5. Watch `ioreg` for `0x2e8a/0x107c` → `0x1209/0xc000`

Those 16 blocks are blocks 0-15 of the running image, `0x10000000`-`0x10000f00` — exactly one
4096-byte sector. Block 0 starts the sector so `write_flash_page` erases it once, and the following
fifteen refill it with the bytes that were already there. Block 16 would start erasing sector 1,
which is why the count must be a multiple of 16 and why 16 is the smallest safe one.

The stall lands at `T`+310 s and clears `upgrade_in_progress`, which releases the timeout deferred
at `src/tasks.c:183`. Expect the exit at ~`T`+310 s; anything up to ~`T`+330 s passes.

**Do not open the config page, at any point in the run.** Every `GET_VAL_MSG` and `SET_VAL_MSG`
ends in `reset_config_timer` (`src/handlers.c:321`), which sets the deadline to `now +
CONFIG_MODE_TIMEOUT`. The page reads the peer version over WebHID as a `GET`, so reading it pushes
the deadline out by a further 300 s and the run measures the wrong clock. There is no way to read
the peer version without doing this — the WebHID page and the DESKHOP volume are the same
transport underneath.

**Unmount as soon as the drop is written**, which settles the ghost rather than working around it.
It is safe: a non-UF2 write returns at the magic check (`src/ramdisk.c:74`) *before*
`fw_upgrade_progress`, so FAT metadata flushed on unmount cannot reset the stall window. With the
mount gone, USB identity is trustworthy for the rest of the run.

**A ROM landing is now itself the peer measurement.** All three roads to `recover_to_rom` are
accounted for in this induction: the `ramdisk.c` end-of-drop checksum needs `blockNo == 1023` and
these are blocks 0-15; the pull's end-of-transfer checksum cannot run because `source` is `DROP`,
so `fw_upgrade_may_pull` is false and no response is accepted; which leaves
`abandon_firmware_upgrade` under `image_dirty && !peer_present` as the only one open. The inference
that failed below is sound again, because #104's fix closed the road that broke it.

### The fourth attempt, and the defect it found

The clean run, 2026-08-17. Config entry **15:21:37.4**, volume hand-mounted at t+10.8 s, and at
**t+280.1 s** an ordinary file write put 16 UF2 blocks — one whole sector of board A's *own*
running image, identical bytes — onto the mounted volume. Board B was powered and working
throughout. That is the induction this sheet recommends, performed exactly.

Board A ended in **ROM**. Reading the flash back afterwards (`~/deskhop-pm2.uf2`) showed **16 of
1024 pages differing, all of them sector 0, all erased** — `recover_to_rom`'s single-sector wipe —
and every other page byte-identical to what the board had been running. So the drop wrote identical
bytes and disturbed nothing, and `recover_to_rom` ran.

This sheet first read that as `abandon_firmware_upgrade` under `image_dirty && !peer_present`, and
concluded that `peer_present` had been false with a healthy peer board attached. **That conclusion
was wrong, and it is withdrawn.** A third road to `recover_to_rom` was open during the run.

The drop sets `upgrade_in_progress`, and `firmware_upgrade_task` — the *pull* — took that same flag
as its cue to start requesting words from the peer board. So the drop started a pull nobody asked
for. Both transports summed into one checksum field, and the pull's own end-of-transfer compare
(`src/tasks.c`) then failed and wiped the sector. Full working in
[#104](https://github.com/myn/deskhopplus/issues/104); fixed by recording which transport is
delivering the image, so a drop can no longer make this board pull.

**The flash readback cannot separate the two roads.** One binary serves both boards and the role is
auto-probed from a pin at boot, so if A and B were on the same build a completed pull rewrote all
1024 pages with *identical bytes*. "1008 pages byte-identical" is equally consistent with a pull
that ran and with one that never happened.

Three readings still bound the peer question, and they were taken with the phantom pull present:

- Board A reports *Other board FW version: v0.91* in **normal** mode
- Board A reports it in **config** mode too, so config mode alone does not lose the peer
- After a `save_config` flash write from the config UI's Save button — one erase plus one program,
  through `write_flash_page`, the same call the UF2 path uses — it **still** reports v0.91. A
  single-page flash write therefore does not lose the peer either

**Resolved by the fifth attempt above: the peer was never lost.** It was never measured directly
here; it was inferred from a ROM landing that had another explanation. The clean run on 0.92 did not
land in ROM with `image_dirty` set, so `peer_present` was true at the stall, and the old hypothesis —
sixteen programs from core0 with interrupts off, against `heartbeat_output_task` on core1 — is
disproved rather than merely unsupported.

Criterion 1 was unreachable for a sharper reason than this sheet first recorded: every accepted
response called `fw_upgrade_progress`, so while a phantom pull advanced, an interrupted drop never
went quiet and **the stall never fired at all**. With #104 fixed, a drop that stops now reaches the
stall path in a clean state, which is what the criterion needs.

### The instrument lied once: a live mount pins a ghost

Worth more than the result it spoiled. The driver timed the exit by USB identity, and reported
*still in config mode at t+420 s* when the board had already been in ROM for minutes. The
hand-mounted volume pinned the departed config-mode device in the IORegistry, so `ioreg` kept
reporting `0x2e8a/0x107c` for a device that was gone. The tell was board A's LED: dark, with
keyboard and mouse both dead, which is ROM, not config mode.

**A test that requires a mounted volume cannot also measure config mode by USB identity.** Unmount
before the board can exit, or measure something else. The same ghost is why an earlier run appeared
to hang past 420 s; neither observation was evidence of a firmware fault.

### An interrupted pull cannot leave a part-written image running

- [x] **Passed 2026-08-17.** Board A flashed with `~/deskhop-0.90.uf2` at 17:29:13 against board B
      on 0.92, board B unplugged mid-pull, board A in **ROM at 17:29:59** with its first sector
      erased. Log at `~/deskhop-pull-run-20260817-172856.log`, flash saved to
      `~/deskhop-rom-20260817-172856.uf2` before recovery

The saved image is the whole result, and it is unusually legible:

| Pages | State | What it is |
|---|---|---|
| 0-15 | erased | sector 0 — `recover_to_rom` |
| 640-648 | written | the pull's last sector, partly refilled |
| 649-655 | erased | the rest of that sector, never reached |
| 1008 | version **190** | the metadata page, still the old build's |

Everything else is byte-identical to what A started from. So the pull stopped **mid-sector at page
648** — 166,144 bytes of 262,144, or 63.4% — and `write_flash_page` had already erased sector 40 to
write into it. That ragged edge is exactly the *part-written image* the criterion exists to prevent
from running, and the board wiped its stage 2 sector rather than boot it.

**This is also what rules out the confound.** #104's triage warned that a flash readback cannot
separate a pull that ran from one that never happened, because one binary serves both boards and a
*completed* pull rewrites all 1024 pages with identical bytes. That holds only for a completed
pull. An interrupted one leaves a frontier, and here the frontier is visible twice over: the
half-erased sector 40, and the metadata page still reading 190 rather than 192, which is proof the
pull never reached page 1008.

### The unplug moment is not the moment you can press a key

The script timed the landing at **u+18.8 s** from the Return keypress, against a 30 s
`FW_UPGRADE_STALL_US`. The firmware is not early; the instrument is late. The stall counts from the
last accepted response, so the last one arrived 30 s before 17:29:59 — at **17:29:29**, some 11 s
before the keypress at 17:29:40. Board B is on the other machine, so the hand that pulls the cable
cannot also be on the keyboard.

The pull rate confirms it independently: 166,144 bytes between A coming up at 17:29:15 and the last
response at 17:29:29 is ~11.9 KB/s, against the ~16 KB/s that 4 kHz × 4 bytes predicts — the right
order, where the keypress reading would demand a pull that stopped 11 s before the cable moved.

**So treat the script's figure as a lower bound on the interval, not a measurement of it.** The one
number to trust is the ROM landing time.

```sh
sudo ./tools/macos-checks/interrupted_pull_to_rom.sh check   # touches nothing
sudo ./tools/macos-checks/interrupted_pull_to_rom.sh run     # ends with A in ROM
```

The script flashes A, waits for the pull, prompts for the unplug, times the landing, and then —
before anything else — takes the `picotool save` and reads the first sector back to say which of
the two roads to ROM was taken. That order is the point of it: the evidence has been destroyed by
a premature `picotool load` once already, and it is the only thing that separates `recover_to_rom`
from `reset_usb_boot`.

**Board B is only the sender here.** Its own image is never written, so the unplug costs nothing
but a replug — the cable trip warning in #58 is about flashing B, which this does not do. What
lands in ROM is board A, on the Mac, and one `picotool load -x build/deskhop.uf2` restores it.

      **0.90 and 0.92 share a crc, and that is correct.** The version lives only in
      `.section_metadata`, patched post-build by `misc/crc32.py`; `VERSION_MAJOR` and
      `VERSION_MINOR` appear nowhere in the C source. So the two boards run byte-identical code
      differing only in the stamp, and the criterion exercises the shipping firmware. The older
      `~/deskhop-0.89.uf2` (version 189, crc `0x2bdc0c83`) predates #104's fix and should not be
      used for this.

### A board in ROM does not by itself mean anything was destroyed

Board A reached ROM once, at ~14:15, from no deliberate induction. Two paths lead there and they
mean opposite things:

| Path | Erases? | Reached by |
|---|---|---|
| `recover_to_rom` (`src/utils.c:89`) | **yes**, the first sector | dirty image + peer gone, or a bad checksum |
| `reset_usb_boot` | no, image intact | the `LShift+RShift+A` chord, or `FIRMWARE_UPGRADE_MSG` from the peer |

`sudo picotool save -r 0x10000000 0x10040000 <file>` **before** reflashing settles which. Here the
first sector read back all `0xff`, so `recover_to_rom` had run. Take that image before recovering:
a `picotool load` destroys the only evidence there is.

The same read showed sector 1 erased with only pages 16–20 rewritten, which is how the failed raw
write was shown to have *half* landed — 21 UF2 blocks reached flash before `ENXIO`, leaving the
board mid-transfer with `image_dirty` set and the host believing the write had failed outright.
**Writing raw to `/dev/rdiskN` is not a code path any real host takes** and should not be used to
induce this again.

### A partial UF2 drop must be a multiple of 16 blocks

`write_flash_page` erases a whole 4096-byte sector whenever a write starts one
(`target_addr & 0xf00`, `src/utils.c:64`). A drop that stops mid-sector leaves the rest of that
sector erased — a real hole in the running image. Dropping the board's **own** image in a multiple
of 16 blocks rewrites identical bytes into whole sectors and leaves the image unchanged, which is
what makes the criterion-1 induction non-destructive. 200 blocks is wrong; 192 is right.

### The keyboard is dead while config mode is active

Observed repeatedly on 2026-08-17 and not recorded anywhere before: with board A in config mode the
keyboard does not reach the Mac, **so the config chord cannot be used to leave config mode**. Every
entry therefore costs the full 300 s. Budget for it, and do not read a dead keyboard as a hung
board — this sheet already warns that the LED is not a mode indicator either.

After the sitting the keyboard did not return until board A was unplugged and replugged, even
though the board was enumerating normally as `0x1209/0xc000` with its keyboard, mouse and vendor
HID interfaces all present in `ioreg`. Cause not established.

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
