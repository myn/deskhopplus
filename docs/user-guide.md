# deskhopplus user guide

How to pair, use, fix and remove deskhopplus. Nothing here needs the source code.

You need a DeskHop board pair running `deskhopplus.uf2`, and a helper on each computer. The
README's [Get started](../README.md#get-started) covers flashing and the first run of each helper.
This guide starts where that ends.

Two words used throughout:

- **Board A** and **board B** are the two halves of the DeskHop. Each plugs into one computer.
  The keyboard and mouse plug into one of them, usually board A.
- **The chord** is `Left Ctrl + Right Shift + C + O`. It always works, whatever you change on the
  config page.

A Linux computer gets the keyboard and mouse. There is no helper for it, so no clipboard and no
cursor placement on that side.

## Pair a helper

A helper must be paired with its board before the clipboard or cursor placement work. Pairing is
one chord press with the helper already running.

1. Start the helper on the computer you are pairing. Wait until its menu says
   **Not paired — press the config chord on the device**.
2. On the keyboard plugged into the board, press the chord once. The board reboots into config
   mode and its on-board LED blinks. A drive named `DESKHOP` may appear; ignore it.
3. Press the chord once more. The board reboots back to normal and opens a **60-second pairing
   window**. The helper pairs by itself. Its menu changes to **Connected and paired**.

Three things that cost people an hour:

- **The chord is a toggle.** One press enters config mode, the next leaves it. Two quick presses
  enter and leave at once, which looks exactly like the chord not working. Press
  once, wait for the blink, then press again.
- **The chord reaches only the board the keyboard is plugged into.** To pair the helper on the
  other board's computer, move the keyboard to that board's USB-A port, press the chord twice
  there, and move it back. [#196](https://github.com/myn/deskhopplus/issues/196) tracks doing
  this without moving the keyboard.
- **The helper must be running before the window opens.** The window is 60 seconds from the
  reboot back. A helper started after that has missed it. Press the chord again.

Each board holds exactly one paired helper. Pairing a second helper to the same board replaces the
first. The wipe chord (`Right Shift + F12 + D`) clears the pairing on both boards, along with every
other setting.

Config mode ends by itself after five minutes if you do not press the chord again. The pairing
window still opens on the way back.

### Which helper is a board paired with?

Open the config page: press the chord, open the `DESKHOP` drive, open `config.htm` in Chrome, click
**Connect**. Under **Device Status**, **Paired helper** shows an id of sixteen hex characters, or
*none — press the config chord to pair a helper*.

Each helper writes its own id, `helper key id: …`, as the first line every time it starts. Look
for the last one in the log. Where the log is:
[Where the logs are](#where-the-logs-are). Same id: this helper is the one. Different id: the
board is paired with something else, and a chord press moves it.

## What the menu bar or tray says

The macOS helper is a menu bar item. The Windows helper is an icon in the notification area. Both
show the same states; two rows are worded a little differently on Windows. Both are always there
while the helper runs.

The icon is two screens. It has three looks, and the look tells you the state at a glance; the
words are one hover or one click away, and they are what to act on:

| The icon | Meaning |
| --- | --- |
| Two solid screens joined by a bar | Connected and paired. |
| Two outlined screens | Looking for the board, board not connected, or config mode. Nothing to do unless it stays that way. |
| A badge with **!** on the screens | Something to read: open the menu. Every state with a remedy, a reconnect storm, or files waiting for your answer. |

On macOS the menu bar item is the icon alone, with a few characters beside it when something is
happening: **⬇ files?** when files are offered, **⬇ 42%** while they arrive, **⬆** while this
computer is sending, and **⚠** when the menu holds a message for you. Hover for the words.

On Windows the icon itself turns into the percent while a file arrives; hover for the rest. The
helper asks Windows to keep the icon on the taskbar rather than behind the **^** overflow. If it
still lands in the overflow, turn it on once by hand: **Settings › Personalization › Taskbar ›
Other system tray icons › deskhopplus helper**. The helper asks again on every start, so the
setting follows the exe if you move it.

The first row of the menu is greyed and shows the release, e.g. **deskhopplus helper 1.0**. The
second row is the state.

| Menu says | What it means | What to do |
| --- | --- | --- |
| **Waiting for the device** (macOS) or **Looking for the device** (Windows) | The helper is looking for the board, or the board went away for a moment. | Wait a few seconds. If it stays, check the USB cable. |
| **Connected and paired** | Everything works. | Nothing. |
| **Not paired — press the config chord on the device** | The board has no pairing for this helper. | [Pair a helper](#pair-a-helper). |
| **Device in config mode** | You pressed the chord once. | Press it again, or wait five minutes. |
| **Device not connected** | The helper cannot find the board. | Check the USB cable. If the board was just replugged, wait a few seconds. |
| **Reconnecting repeatedly — check the link, and that the helper is up to date** | The connection keeps dropping and coming back. | Check the cable. Make sure the helper and the firmware are from the same release. |
| **Helper version does not match the device — update the helper; file transfers are refused** | The board and the helper speak different versions. | Install the helper from the same release as the firmware. The cursor still works; the clipboard does not. |
| **Another program is writing to the device channel — find and stop it, and do not press the config chord while it is running** | Something else on this computer is talking to the board. | Find and quit it. Do not press the chord until it is gone: the chord would pair whatever is attached. |
| **Device identity changed — if you re-flashed it, remove the pinned board key** | The board's own key is not the one this helper remembers. A firmware upgrade never changes it; a whole-flash erase or a swapped board does. | Only if you did that yourself: [Identity changed](#identity-changed-after-a-re-flash). Otherwise do not press the chord; pressing it would accept the new board. |

Below the state the menu may also show:

- a cursor placement problem, until the next placement succeeds;
- a file offer with **Accept and start the transfer** and **Decline**;
- a transfer in progress, **Receiving 2.1 MB of 8.0 MB — 26%**, with **Cancel this transfer**;
- **Cancel what is being sent** while this computer is sending;
- **Start at login** (macOS) or **Start at logon** (Windows);
- **Quit deskhopplus helper**.

**Start at login / Start at logon** makes the helper start when you log in. On macOS the change
takes effect at the next login. On Windows a managed laptop may refuse every method the helper
tries; the helper log says so, and the helper still works when you start it yourself.

**Quit** stops the helper until you start it again or log in again. On macOS, double-click the
helper file to start it again now. On Windows, run the exe again.

## Clipboard

Copy on one computer, cross to the other, paste. What crosses depends on what was copied and how
big it is.

| Copied | What happens |
| --- | --- |
| Text | Crosses at once. |
| Text with a picture of it (Office applications do this) | Both cross together while text and picture add up to 256 KB or under. Above that the text crosses alone. |
| Image up to 256 KB | Crosses at once. |
| Image over 256 KB | The other computer fetches it as soon as it hears about it, then puts it on its clipboard. Large images take a moment to arrive. |
| Files up to 1 MB in total | Cross at once. |
| Files over 1 MB in total | Nothing crosses until you accept on the paste side. See below. |

A new copy replaces whatever the last copy was still doing.

### Files: the accept step

Copying files sends only their names and sizes. When you cross to the other computer, its helper
shows the offer in the menu: how many files, how big, and about how long it will take. Choose
**Accept and start the transfer** or **Decline**. On Windows, clicking the notification balloon
also accepts.

The rate is about 33 KB per second. A 10 MB set takes about five minutes. A paste does not start
the transfer, because both operating systems would freeze the pasting application for the whole
transfer.

An offer you do not answer in two minutes is declined for you. Copy again to get it back.

When the transfer completes, the files are put on the clipboard as if you had copied them locally.
Paste them anywhere. They live in a temporary folder:

- macOS: `deskhopplus` inside your temporary directory. In Terminal: `open "$TMPDIR/deskhopplus"`.
- Windows: `%TEMP%\deskhopplus`.

The helper empties that folder every time it starts. Only the newest set survives a restart. Move
the files somewhere real if you want to keep them.

A file that grows between the copy and the accept, such as a log, arrives at the size it had when
you copied it. A file that shrinks fails the transfer.

### The size cap

The board holds the largest clipboard it will carry: **10 MB** by default, up to **64 MB**. It is
on the config page under **Common Config → Clipboard size cap (MB)**. The same cap applies to both
directions and both helpers.

When something too big is copied, nothing happens on the copy side. When you cross to the paste
side, its helper tells you: *… is 23 MB, larger than the 10 MB clipboard limit, so it was not
brought over. Raise the limit on the board's config page.*

### Turning a direction off

On the config page under **Common Config → Clipboard** are **Block clipboard A to B** and
**Block clipboard B to A**. Tick one to stop copies crossing that way. The board is the only place
this is set; the helpers follow it.

Nothing crosses without a paired helper at each end, so a computer with no helper shares nothing.

## Layout, remapping and hotkeys

All of this is on the config page. To open it: press the chord, open the `DESKHOP` drive, open
`config.htm` in Chrome or another Chromium browser (Firefox has no WebHID), click **Connect**.
Change what you want, click **Save**, then click **Exit** to leave config mode.

The page has **Output A** and **Output B**, one per computer, then **Common Config**. Output A is
the computer on board A, Output B the computer on board B. Not sure which board is which? Unplug
and replug both boards: the keyboard and mouse always start on board A's computer.

### Where the computers sit

Each output has:

- **Screen Count** — how many monitors that computer drives (1 to 3).
- **Operating System** — set it. The lock-both-computers chord and the Cmd/Ctrl swap default
  depend on it.
- **Border Direction** — where the *other* computer is: Left, Right, Top or Bottom.
- **Chain Direction** — which way this computer's own monitors run from its main monitor: Left,
  Right, Top or Bottom. Screen 1 is the main monitor; screen 2 is the next one along that
  direction.

Side by side, with the Windows machine on the right of the Mac: Mac's Border Direction is Right,
Windows's is Left. One above the other, Mac on top: Mac's Border Direction is Bottom, Windows's is
Top. Both computers with their monitors running rightward: Chain Direction Right on both.

When Border Direction and Chain Direction are at right angles (monitors in a row, the other
computer above or below), every monitor touches the seam and the cursor can cross from any of
them. When they are on the same axis, only the monitor nearest the other computer crosses, as on a
stock DeskHop.

### Seam ranges: which monitor meets which

With more than one monitor on each side, the board needs to know which monitor on this side meets
which monitor on the other side, and where. That is the **Seam ranges** table on each output: four
segments, each with a **Screen**, a **Start** and an **End**.

- **Screen** is the monitor number, 1 to 3. 0 means the segment is unused.
- **Start** and **End** are positions along the seam, from 0 to 65535, on that monitor. The whole
  edge of a monitor is 0 to 65535.
- **Segment 1 on A meets segment 1 on B**, segment 2 meets segment 2, and so on. The cursor
  leaves at some fraction along A's segment and arrives at the same fraction along B's.

Two monitors above two monitors, left above left and right above right, with the left monitor the
main one on both computers:

| | Segment 1 | Segment 2 |
| --- | --- | --- |
| Output A (top) | Screen 1, 0 to 65535 | Screen 2, 0 to 65535 |
| Output B (bottom) | Screen 1, 0 to 65535 | Screen 2, 0 to 65535 |

If one computer's main monitor is on the right instead, swap the screen numbers on that side, so
each segment still pairs the two monitors that physically touch.

Narrower segments handle monitors of different widths, or a monitor that only half overlaps the
one across the seam; the position is scaled. A monitor with no segment does not cross. Until both
sides of a segment are filled in, the board falls back to the stock DeskHop rule, so the cursor is
never trapped while you set this up.

If your monitors are the same size and aligned, `Right Shift + F12 + Y` still works as on a stock
DeskHop: park the mouse where the smaller monitor's edge is and press it.

### Cmd and Ctrl swap

Each output has **Swap Ctrl and Cmd**. When ticked, Ctrl and Cmd (the Windows key) change places
on keys going to that computer, left and right both. `Ctrl+C` on the keyboard becomes `Cmd+C` on
the Mac. It is on by default for an output whose Operating System is MacOS and off for the others.

Hotkeys are matched on the keys you physically press, before the swap, so a chord is the same
fingering on both computers.

### Key overrides and passthrough

Each output has two text boxes.

**Key overrides** — one `from=to` per line. Keys going to that computer are changed on the way.
Modifiers and ordinary keys mix freely:

```
capslock=lctrl
rgui=ralt
```

Up to 32 lines per output.

**Passthrough keys** — a comma-separated list of keys that are never changed for that computer, by
overrides or by the swap:

```
capslock, f1
```

Up to 16 per output. Order of precedence: passthrough first, then overrides, then the swap.

### Hotkeys

**Common Config → Hotkeys** has one box per action. Write the keys joined by `+`, up to six keys:

```
lctrl+capslock
```

The defaults:

| Action | Default | What it does |
| --- | --- | --- |
| output_toggle | `lctrl+capslock` | Switch to the other computer |
| mouse_zoom | `ralt+m` | Slow mouse on or off |
| switch_lock | `ralt+k` | Stop the mouse crossing on or off |
| screen_lock | `ralt+l` | Lock both computers (needs Operating System set on both outputs) |
| gaming_mode | `lctrl+rshift+g` | Gaming mode on or off: locked to this computer, relative mouse |
| screensaver_pong | `lctrl+rshift+s` | Screensaver on, pong |
| screensaver_jitter | `lctrl+rshift+j` | Screensaver on, jitter |
| screensaver_disable | `lctrl+rshift+x` | Screensaver off |
| wipe_config | `rshift+f12+d` | Erase every setting on both boards, including pairings |
| screen_seam | `rshift+f12+y` | Save the cursor height for the seam |
| config_mode | `lctrl+rshift+c+o` | Config mode, and the pairing window on the way out |
| firmware_upgrade_a | `lshift+rshift+a` | Put board A into its USB bootloader (`RPI-RP2` drive) |
| firmware_upgrade_b | `lshift+rshift+b` | Put board B into its USB bootloader |

`lctrl+rshift+c+o` always enters config mode, even if you change the config_mode box to something
else. That is the way back if a hotkey change locks you out.

### Key names

Lower case. Letters `a` to `z`, digits `0` to `9`, `f1` to `f12`.

Modifiers: `lctrl`, `lshift`, `lalt`, `lgui`, `rctrl`, `rshift`, `ralt`, `rgui`. `gui` is Cmd on
a Mac and the Windows key on a PC.

Others: `enter`, `esc`, `backspace`, `tab`, `space`, `capslock`, `insert`, `delete`, `home`,
`end`, `pageup`, `pagedown`, `up`, `down`, `left`, `right`, `printscreen`, `scrolllock`,
`numlock`, `minus`, `equals`, `lbracket`, `rbracket`, `backslash`, `semicolon`, `quote`,
`grave`, `comma`, `period`, `slash`.

Keypad: `kp0` to `kp9`, `kp_divide`, `kp_multiply`, `kp_minus`, `kp_plus`, `kp_enter`,
`kp_period`.

## Troubleshooting

### macOS will not open the helper

macOS refuses an unsigned download the first time. If it says the app **cannot be verified**: open
**System Settings → Privacy & Security**, scroll down, click **Open Anyway**, then double-click the
helper again. If it says the app **is damaged**, there is no Open Anyway: in Terminal, run

```sh
xattr -dr com.apple.quarantine ~/Applications/deskhopplus-helper.app
```

with the path you put it at. Then double-click it. That command works in both cases: it removes
the mark Safari puts on a download, which is what macOS checks.

The helper needs macOS 13 or later on Apple Silicon, or an Intel Mac with the T2 chip. It keeps its
key in the Secure Enclave, and an Intel Mac without T2 has none.

### Windows says "Windows protected your PC"

SmartScreen, because the exe is not signed. Click **More info**, then **Run anyway**. Once.

### Identity changed after a re-flash

The helper remembers the board's key and refuses a board with a different one. A firmware
upgrade, by either route in Get started, keeps the key. Only erasing the whole flash (a "nuke"
image) or swapping in another board changes it.

If you did that yourself, tell the helper: quit it, delete the remembered key, start it again.

macOS: click **Quit deskhopplus helper** in the menu. In Terminal:

```sh
rm ~/Library/Application\ Support/deskhopplus/board_key
```

Then double-click the app, or log out and in.

Windows: click **Quit deskhopplus helper** in the tray menu, delete
`%LOCALAPPDATA%\deskhopplus\board_key`, run the exe again.

Then [pair](#pair-a-helper) again.

If you did **not** re-flash or swap anything, leave the chord alone and find out what changed.

### Where the logs are

- macOS: `/tmp/deskhop-helper.log` when started at login. When started by double-clicking, no
  file gets the log. To watch one, quit the helper and start it from Terminal instead:
  `~/Applications/deskhopplus-helper.app/Contents/MacOS/deskhopplus-helper`, with the path you put
  it at. The log is that window.
- Windows: `%LOCALAPPDATA%\deskhopplus\helper.log`.

On macOS each line starts with the wall clock and the time since the helper started. On Windows
each line starts with the time since Windows booted, in milliseconds.

### The DESKHOP drive does not appear on macOS

Pressed the chord, LED blinks, no drive. First: did you press twice? See
[Pair a helper](#pair-a-helper).

If you pressed once and still nothing, and Disk Utility or `diskutil list` hangs, macOS's disk
mounter is stuck. Nothing else will mount either until the Mac reboots. Reboot the Mac. This is
[#178](https://github.com/myn/deskhopplus/issues/178).

### The cursor jumps to the middle of the screen, or the clipboard fills with screenshots

Another keyboard-and-mouse sharing program is running. Deskflow did this on a Mac after a reboot,
where it starts itself: it warps the cursor to the centre of the main screen and syncs clipboards
on its own. Quit it. In Terminal, `pgrep -il deskflow` says whether it is running.

### On Windows the cursor lands in the bottom-right corner

An administrator window (a UAC prompt, an elevated app) had focus when you crossed. The helper may
not move the cursor while such a window is in front, so it stays where the board parked it. Click
a normal window; the next crossing is placed correctly.

### A managed Windows PC prompts, or refuses, when the DESKHOP drive appears

Corporate endpoint software such as Trellix treats the config-mode drive as removable storage. It
may ask you to justify it each time, and it may refuse writes, which also blocks the drag-and-drop
firmware upgrade on that PC. Nothing in deskhopplus can change that policy. Use the Mac, or any
unmanaged computer, for config mode and firmware upgrades. Pairing is unaffected: the pairing
window opens on the reboot back, whether or not the drive mounted. [#58](https://github.com/myn/deskhopplus/issues/58).

### Caps Lock does not blink

The keyboard LEDs do not show config mode or acknowledge a chord, although the original DeskHop
manual says they do. Watch the LED on the board instead; it blinks in config mode. Do not read a
still Caps Lock as "the chord did not work". [#101](https://github.com/myn/deskhopplus/issues/101).

### The keyboard is dead after the board reboots

Unplug the keyboard from the board and plug it back in. The board flashes its LED twice when it
sees a new keyboard.

### Nothing pastes, no message

Check the menu on the paste side. A file offer waits there. If the menu says **Connected and
paired** on both computers and the copy was under the size cap, check the direction is not blocked
on the config page.

## Uninstall

### macOS

1. Click the menu bar item. If **Start at login** is ticked, click it to untick it. (If the
   helper is not running, double-click it first.)
2. Click **Quit deskhopplus helper**.
3. In Terminal:

```sh
rm -f ~/Library/LaunchAgents/com.deskhopplus.helper.plist ~/Library/LaunchAgents/com.deskhopplus.helper.plist.disabled
rm -rf ~/Library/Application\ Support/deskhopplus "$TMPDIR/deskhopplus" /tmp/deskhop-helper.log
```

4. Delete the helper file from wherever you put it.

That removes the helper's key handle, the remembered board key and any received files. There is
nothing else.

### Windows

1. Right-click the tray icon. If **Start at logon** is ticked, click it to untick it. (If the
   helper is not running, run the exe first.) This removes the logon entry it made.
2. Click **Quit deskhopplus helper**.
3. Delete `%LOCALAPPDATA%\deskhopplus\`.
4. Delete the exe.

### The boards

The boards keep their pairings and settings until wiped. To unpair, press `Right Shift + F12 + D`
on the keyboard plugged into the board. That erases every setting on both boards. To go back to a
stock DeskHop, flash DeskHop's own `.uf2` the same way you flashed this one.
