# Verifying the always-on vendor HID interface

The procedure behind [#25](https://github.com/myn/deskhopplus/issues/25). It exists as a
document rather than a one-off checklist because the descriptor is touched again by
[#20](https://github.com/myn/deskhopplus/issues/20) (identifiers) and possibly
[#58](https://github.com/myn/deskhopplus/issues/58) (web config off mass storage), and every
descriptor change has to answer the same questions on the same hardware.

**Why hardware.** Enumeration and descriptors are outside the host test seams by decision
(#15, *Explicitly not tested at a seam*). The specific unknown is #59's finding U2: DeskHop's
keyboard interface is declared `HID_ITF_PROTOCOL_NONE` yet works at BIOS and disk-encryption
prompts, so the risk of adding an interface is not boot-protocol negotiation but **whether
firmware that tolerates the current descriptor tolerates a longer one with a higher interface
count**. That is not answerable from source.

## What changed, so a failure can be attributed

Verified by decoding both descriptors out of the built ELF and diffing against a build of the
previous commit:

| Descriptor | Before | After |
|---|---|---|
| Config mode | 107 bytes, 4 interfaces | **byte-for-byte identical** |
| Normal mode | 59 bytes, 2 interfaces | 91 bytes, 3 interfaces |

In normal mode exactly three things differ: `wTotalLength` (59 → 91), `bNumInterfaces`
(2 → 3), and 32 appended bytes describing the new interface. **Interfaces 0 and 1 are
byte-identical**, so a keyboard or mouse regression cannot be caused by a changed keyboard or
mouse descriptor — only by the presence of a third interface. That is the single variable
under test.

The new interface: number 2, class HID, its own report descriptor of vendor-page collections
only, interrupt OUT `0x03` and interrupt IN `0x83`, both 64 bytes at 1 ms.

## Recording results

Copy the checklist into the issue and fill it in. A check that cannot be run is recorded as
**not run**, never as a pass.

### 1. macOS — driverless, and free of TCC

```
python3 tools/macos-checks/confirm_hid_tcc.py
```

Expected: three nodes listed. The keyboard and mouse nodes carry
`RequiresTCCAuthorization: YES` — that is pre-existing and harmless, nothing opens them. The
third node must show `0xff00/32 vendor` alone with `RequiresTCCAuthorization: no`, and the
script must print `PASS`.

Baseline on stock firmware for comparison: two nodes, both flagged, and the script fails with
*no node publishes the channel usage*.

- [ ] Interface binds with no kext, no entitlement, no permission prompt
- [ ] Script prints PASS — the ADR-0001 confirmation
- [ ] Serial number is reported and matches the board

### 2. Windows 11 — driverless, no administrator rights

```
powershell -ExecutionPolicy Bypass -File tools\windows-checks\Confirm-HidExclusivity.ps1 `
    -Check A,E -VendorId 0x1209 -ProductId 0xC000 -Usage 0x20
```

(The parameters are named in full because a PowerShell parameter binds a variable of the same
name, and `$Pid` is a read-only automatic variable — a `-Pid` parameter fails at bind time.)

Run **as a normal user**, not elevated: "without administrator rights" is part of what is
being measured.

**Pass `-Vid/-Pid/-Usage`.** Without them the script picks its target by a heuristic that
prefers a vendor collection supporting feature reports — and the channel deliberately has
none, so the heuristic would skip it, measure some unrelated vendor device on the laptop, and
report a pass that says nothing about this device. With them, a run that cannot find the
channel fails visibly instead. Confirm the `TARGET:` line names VID `0x1209` / PID `0xC000`
before recording anything below.

- [ ] The device appears in Device Manager with no driver installation and no unknown device
- [ ] Check A inventories the new vendor collection, and `TARGET:` names it
- [ ] Check E shows it refusing a zero-access open while a `dwShareMode = 0` handle is held —
      the second ADR-0001 confirmation
- [ ] Existing keyboard and mouse function normally on this machine

### 3. The one that cannot be answered from source

**Normal mode only.** #25 originally asked for both descriptor variants, but the config-mode
descriptor was shown to be byte-for-byte identical to its predecessor, so its pre-boot
behaviour cannot have changed and re-testing it measures nothing. Only the normal-mode
descriptor grew an interface. Should a future change touch the config-mode descriptor, this
section grows back.

**Switch to the machine under test first.** The device routes the keyboard to one output at a
time (`Left Ctrl + Caps Lock`). Rebooting a computer the device is not pointed at looks
exactly like a keyboard failure and is the easiest way to record a false negative here.

- [ ] Windows: keyboard works at the UEFI/BIOS setup prompt
- [ ] Windows: keyboard works at the BitLocker pre-boot prompt, or *not applicable* if the
      machine unlocks from the TPM without prompting
- [ ] macOS: keyboard works in Startup Manager (hold Option at power-on)
- [ ] macOS: keyboard works at the FileVault unlock screen, or *not applicable* if disabled

If any of these fail, the interface count is the cause and the finding belongs in the issue
before anything else is built on the channel.

### 4. Nothing else regressed

- [ ] Keyboard, mouse, consumer and system control all work on both computers
- [ ] Switching between computers works
- [ ] Config mode still enumerates its mass-storage interface
- [ ] The web config page loads and reads values
- [ ] Firmware upgrade through config mode still works

## Bump the version, or only one board changes

`handle_heartbeat_msg` pulls firmware from the peer only when the peer reports a **strictly
newer** version:

```c
if (other_running_version <= state->_running_fw.version)
    return;
```

So flashing a build that carries the same version as the board already running leaves the
second board on the old firmware, silently and with no error anywhere — the pair ends up
split, presenting different USB descriptors to the two computers. Measured on 2026-08-10: the
macOS side had the channel interface and the Windows side did not, purely because the version
had not moved.

**Any descriptor or protocol change must bump `VERSION_MINOR` in `CMakeLists.txt` before
flashing.** Verify propagation by confirming both computers see the new interface, not just
the one whose board was flashed directly.

## Recovery

The ROM bootloader is always reachable: hold the on-board button while connecting the Pico and
copy any `.uf2` to the `RPI-RP2` drive that appears. This works regardless of device state, so
a descriptor that breaks enumeration is recoverable without special tooling.
