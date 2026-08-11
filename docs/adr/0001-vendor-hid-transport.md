# ADR-0001: Vendor HID, not USB CDC, as the helper↔firmware transport

- **Status:** Accepted
- **Date:** 2026-08-09
- **Supersedes:** standing decisions 1 and 2 of [map #31](https://github.com/myn/deskhopplus/issues/31)
- **Arising from:** [#59](https://github.com/myn/deskhopplus/issues/59)
- **Evidence:** [#40](https://github.com/myn/deskhopplus/issues/40), [#57](https://github.com/myn/deskhopplus/issues/57), [#58](https://github.com/myn/deskhopplus/issues/58), and `docs/research/hid-transport-windows-exclusivity.md`, `docs/research/hid-transport-macos-tcc.md`, `docs/research/trellix-dlp-endpoint-constraints.md`

## Decision

The helper↔firmware channel is a **vendor-defined HID interface** — usage page `0xFF00`+ — on both
boards, present in the **normal-mode descriptor only**, and declared as its **own USB HID interface**
with a report descriptor containing only vendor-page collections.

USB CDC is retained as a documented fallback, not as the primary.

## Context

The original choice was made as a two-way comparison, recorded in map #31 decision 2: **CDC vs
WinUSB**. WinUSB needs a driver association and therefore administrator rights, so it lost. CDC won on
being driverless at zero permission cost.

**Vendor HID satisfies the same criterion and was never in that comparison.** The gap surfaced while
evaluating a third-party fork's clipboard work (#59), not because anything was wrong with the original
answer to the question that was actually asked.

Two unknowns had to be settled before the decision could be reopened, because each attacked a locked
decision:

1. Does vendor HID lose the exclusive-ownership security control that #34 depends on?
2. Does vendor HID acquire a macOS TCC requirement that CDC does not have, costing the zero-permission
   property established in #35?

Both were researched against primary sources and then **measured**. Both came back in HID's favour —
the second immediately, the first only after the documentation-based answer was overturned by
measurement.

## Evidence

### Windows: exclusivity survives — measured

Documentation analysis said it would not. `CreateFile`'s parameter table states that `dwShareMode = 0`
blocks only subsequent opens requesting delete, read or write access, and `dwDesiredAccess = 0` is
explicitly supported — so a second process should get a handle regardless, and every HID class IOCTL
is declared `FILE_ANY_ACCESS`, so that handle should still exchange feature reports.

Measured on a managed Windows 11 Enterprise laptop, unelevated, against **every** vendor-defined,
non-system-held collection present — **10 collections across at least 4 independent vendors**:

| Open | Result |
| --- | --- |
| owner: `GENERIC_READ\|GENERIC_WRITE`, share 0 | HANDLE, 10/10 |
| second: `GENERIC_READ`, share 0 | FAILED `ERROR_SHARING_VIOLATION` |
| second: **`dwDesiredAccess = 0`**, share 0 | **FAILED `ERROR_SHARING_VIOLATION`, 10/10, zero leaks** |

Control: with the owner handle closed, the same zero-access open succeeds. Because the refusal is
uniform across unrelated vendors' minidrivers, the enforcement is in `hidclass.sys` rather than in any
one driver.

~~**[INFERENCE]** It will therefore behave the same for our device.~~ **Confirmed on hardware
2026-08-10** against our own collection — see *Confirmations, retired*.

### macOS: no TCC requirement — sourced and measured

The Input Monitoring gate is implemented in code Apple publishes. `IOHIDDevice::handleStart` sets
`RequiresTCCAuthorization` for exactly three usage pairs — GenericDesktop/Keyboard,
GenericDesktop/Mouse, Digitizer/TouchPad — and the userspace open path short-circuits to *granted* when
the flag is absent. Byte-identical across seven `IOHIDFamily` releases, Catalina through macOS 26.

Measured from an unsigned, unbundled launchd job holding no grants: vendor-page opens returned
`kIOReturnSuccess` across 7 devices, no TCC record was written, and `kIOHIDOptionsTypeSeizeDevice`
(exclusive open) also succeeded.

### Two costs CDC carries that vendor HID does not

Both measured on the same managed laptop:

- **Trellix `hdlpdbk`** — the device filter driver that enforces plug-and-play device rules — is a
  registered class filter on **Ports, USBDevice and USB**, and **not** on HIDClass.
- **`usbser.sys` binding under device-installation policy** (#40 check 4) is unresolved and cannot be
  closed until hardware exists. HID needs no driver installation, so the question does not arise.

## Consequences

### Binding constraint: the vendor collection must be its own USB HID interface

macOS `IOHIDDevice::conformsTo` iterates a device's **complete** usage-pair list, not its primary
usage. One keyboard or mouse collection anywhere in an `IOHIDDevice` flags the whole node — vendor
collections included.

Both of DeskHop's existing normal-mode HID interfaces are TCC-flagged today. **Appending the vendor
collection to the keyboard interface's report descriptor — the cheaper-looking change — would acquire
the exact Input Monitoring requirement this decision avoids.**

### Throughput: the USB hop becomes the system bottleneck

This is the real cost of the decision, and it was not visible when the choice was framed.

A full-speed interrupt endpoint moves **one report per 1 ms frame**. At a 64-byte report that is
**64 KB/s per direction, per interface** — and `CFG_TUD_HID_EP_BUFSIZE` is currently 32, i.e. 32 KB/s
until raised. CDC bulk on the same full-speed device would have carried roughly an order of magnitude
more.

The inter-board UART carries ~200 KB/s (8 payload bytes per 12 wire bytes). So where the UART was
previously the system bottleneck, **a single HID channel is about 3× worse than the link it feeds**:

| Payload | One channel at 64 KB/s |
| --- | --- |
| Text, small images (the everyday path) | imperceptible |
| 10 MB default cap | ~2.7 minutes |
| 64 MB maximum cap | ~17 minutes |

The mitigation — striping across parallel vendor HID interfaces — is a separate decision, recorded in
**[ADR-0002](0002-parallel-hid-channels.md)**. It restores roughly the UART's ceiling and no more,
because beyond that the UART is the wall.

This does not reverse ADR-0001. Interactive clipboard content is far below any of these numbers, files
were always the lazy pull-on-paste case behind a confirmation prompt, and the transport's permission
and policy properties are what the project could not otherwise buy. But the ceiling is a genuine loss
against CDC and is recorded here so that a future reader weighing this decision sees its cost, not
only its benefits.

### The helper must open with `dwShareMode = 0` as early after login as it can

Exclusivity is **first-come** on HID exactly as it is on CDC. `docs/research/windows-helper-constraints.md`
§4.1 records that the helper may start tens of seconds after login. This is a startup requirement, not
a transport discriminator, and it is the real residual risk on either transport.

### Unchanged by this decision

- The descriptor change lands in `usb_descriptors.c` and touches enumeration for every user, with the
  BIOS/UEFI tolerance question that carries. **CDC would have required the same change** — neither
  transport is present in the normal-mode descriptor today.
- Helpers still locate the device by identifier + serial, never by port name or device path.
- Config mode still reboots under a different USB identity, so the interface vanishes and returns
  across a config-mode round trip.
- The framing, priority discipline and chunking design in #37 are transport-agnostic and carry over.

### Changed elsewhere

- **#34** — "exclusive port ownership" becomes "exclusive HID collection ownership". The mechanism
  changes from `CreateFile` share mode 0 on a COM port to `CreateFile` share mode 0 on a HID
  collection path; the guarantee is the same and is measured. The device-held shared secret and
  pairing design are unaffected.
- **#40 check 4** (`usbser.sys` binding) drops off the critical path. It stays open only for the CDC
  fallback.
- **#58** — the mass-storage web-config path is unaffected by this ADR, but HID now looks like a
  plausible route for web config too, for the same reason: HIDClass carries no Trellix filter.

## Confirmations, retired

Both were outstanding on hardware that did not exist when this decision was taken. Both were
measured on 2026-08-10 once [#25](https://github.com/myn/deskhopplus/issues/25) put the interface
on real boards, and both came back as the decision assumed. **No inference remains behind this
ADR.** Evidence is on #25 and in `docs/verification/vendor-hid-interface.md`.

1. **`tools/windows-checks/Confirm-HidExclusivity.ps1 -Check A,E` against our own vendor
   collection** — `zero-access UP=0xFF00 U=0x0020 'DeskHop Channel' -> FAILED (err=32)`. Twice, on
   two independent runs, with 11 of 11 vendor collections excluded. `hidclass.sys` refuses the
   zero-access open for our device exactly as it did for the ten measured before it, which is what
   the *Windows: exclusivity survives* inference above rested on.
2. **`ioreg -c IOHIDDevice -r -l` on macOS** — the vendor collection landed in its own
   `IOHIDDevice` with no `RequiresTCCAuthorization` property. The separate-interface constraint was
   honoured, and the permission-free property this transport was chosen for holds on our hardware.

Also measured there, and worth having beside them: the device is locatable by identifier and
serial (board A `E6654854577F452F`, board B `E665485457895030`, stable across every flash), and
the 64-byte carrier reaches the Windows host intact.

**What is *not* retired** is pre-boot behaviour, which was never a claim of this ADR but is easily
mistaken for one. The keyboard does not work at either computer's pre-boot prompt — measured on
both, and on macOS control-tested against stock upstream, so it predates this fork rather than
following from this decision. The Windows control is still missing, and until it is run a
regression caused by the added interface cannot be fully excluded there. Tracked on
[#66](https://github.com/myn/deskhopplus/issues/66); the possible remedy is
[#67](https://github.com/myn/deskhopplus/issues/67).

## Alternatives considered

| Option | Why not |
| --- | --- |
| **USB CDC** | Retained as fallback. Costs the unresolved `usbser.sys` device-installation question and sits on a class carrying a live Trellix filter. Otherwise sound: driverless, and permission-free on macOS. |
| **WinUSB** | Still rejected, but ~~requires a driver association and administrator rights~~ — that rationale was stale: with MS OS 2.0 descriptors WinUSB binds in-box with no admin, and #7 had already found Windows parity, rejecting it as "strictly more work on both sides". The reasons that hold today: extra descriptor work in firmware, IOKit/libusb work on macOS where HID is free, and — decisive on this ADR's own evidence — WinUSB devices land in the **USBDevice** class, where `hdlpdbk` is a registered filter, while HIDClass carries none. Its bulk endpoints (~1 MB/s at full speed) would have removed the throughput ceiling ADR-0002 exists to mitigate; that forgone bandwidth is a real cost of choosing HID and is recorded here. |
| **Reusing the existing config-mode vendor HID interface** | It exists only in the config-mode descriptor, alongside the MSC ramdisk — so the clipboard would work only in config mode, which also mounts mass storage and raises a Trellix justification prompt (#58). |
| **Appending the vendor collection to the keyboard interface** | Cheaper descriptor change, but acquires the macOS Input Monitoring requirement via `conformsTo`. Rejected on the constraint above. |

## A note on how this decision was reached

Three conclusions in the supporting research were overturned by measurement rather than by argument:
that Trellix device blocking was not deployed, that the local policy paths were undocumented because
they did not exist, and that HID share mode 0 gave no exclusivity. Each was well-reasoned from
primary sources; each was wrong about what the implementation does.

The pattern is consistent enough to be worth stating as practice: **documentation establishes what is
specified, measurement establishes what is true, and a decision of this weight needs both.** The
research documents retain their superseded sections as reasoning trails for that reason.
