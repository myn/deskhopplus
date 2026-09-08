# Two-channel hardware validation (#63)

Run on both boards with the new firmware and helpers. Record the commit, OS and
machine, descriptor mode, result, and helper/device drop counters in issue #63.
These checks require hardware; build and host-test results do not establish them.

## Expected topology

| Mode | Interfaces | Channel collections | Channel endpoints |
| --- | --- | --- | --- |
| Normal | keyboard 0, relative mouse 1, channel 0 at 2, channel 1 at 3 | `FF00:20`, `FF00:21` | `03/83`, `04/84`; 64 bytes, 1 ms |
| Config | keyboard 0, relative mouse 1, config HID 2, MSC 3 | none | none |
| Debug | either topology above plus CDC at 4/5 | as above | CDC `85`, `06/86` |

## Checklist — pending hardware

- [ ] Cold boot and reconnect at BIOS/UEFI: keyboard and mouse work, in normal
      and config descriptor modes. Record machine/firmware versions.
- [ ] Repeat at the disk-encryption password prompt in both descriptor modes.
- [ ] macOS: both vendor collections enumerate without Input Monitoring permission;
      inspect `ioreg -r -c IOHIDDevice -l` for usages 32 and 33 on page 65280,
      matching board serial and no `RequiresTCCAuthorization` on either collection.
- [ ] Windows, unelevated: run each command below with the helper stopped.
      Check A must identify the selected collection; check E must refuse a second
      zero-access open while the exclusive handle is held.

```powershell
powershell -ExecutionPolicy Bypass -File tools\windows-checks\Confirm-HidExclusivity.ps1 -Check A,E -VendorId 0x1209 -ProductId 0xC000 -Usage 0x20
powershell -ExecutionPolicy Bypass -File tools\windows-checks\Confirm-HidExclusivity.ps1 -Check A,E -VendorId 0x1209 -ProductId 0xC000 -Usage 0x21
```

- [ ] Hold either Windows collection in another process, start the helper, and
      confirm it fails the whole connection and releases any other handle.
      Release the competing handle; confirm recovery without re-pairing.
- [ ] Helper startup after USB attachment and attachment after helper startup both
      acquire two collections and negotiate two channels. Reconnect and config-mode
      round trips restore the session without repeated protocol/tag failures.
- [ ] Transfer files/images both ways, including a 10 MB payload. Verify identical
      bytes, record sustained throughput, and watch refusal/stream-gap counters.
      Move the cursor across the seam and type during transfer; check placement
      and input latency. Repeat with the macOS menu open.
- [ ] Verify a one-channel hello negotiates one and traffic remains on channel 0
      (an earlier helper build is sufficient if available).

The global transfer credit window is unchanged. Host tests cover its exhaustion,
recovery, and bounded pump batches; channels do not create separate transfer state.
Keep #63 open with `needs-hardware-validation` and `ready-for-human` until these
checks pass. No throughput improvement is claimed without this measurement.
