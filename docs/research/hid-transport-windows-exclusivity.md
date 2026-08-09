# Windows: can an unelevated process own a vendor HID collection exclusively?

Research for [issue #59](https://github.com/myn/deskhopplus/issues/59) (CDC vs vendor HID transport),
testing whether the control locked by [issue #34](https://github.com/myn/deskhopplus/issues/34)
survives, under [map #31](https://github.com/myn/deskhopplus/issues/31).

**Question.** Can an unelevated, unsigned, per-user Windows process obtain **exclusive** access to a
vendor-defined HID top-level collection on a USB composite device — the way `CreateFile` with
`dwShareMode = 0` gives exclusive access to a COM port?

Nothing broader. This is not a HID survey; it is one control, tested.

**Why it is the whole question.** #34 locked exclusive port ownership as a security control because
the channel bridges two otherwise-isolated machines and the firmware relays opaquely: any local
process that can open the endpoint can push files across it. On a COM port that control is free —
`CreateFile` on a communications resource *requires* `dwShareMode` to be zero, so the port is
exclusive by construction
([CreateFileA, "Communications Resources"](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea),
quoted in `docs/research/windows-helper-constraints.md` §1.1). If vendor HID cannot reproduce it,
decision 6 on #31 reopens.

**Sourcing.** Microsoft Learn (HID driver documentation, `hidsdi.h` / `hidclass.h` API references,
WDM kernel documentation) and the WDK's own `hidclass.h` header. Microsoft's `hclient` WDK sample
source is cited as primary-adjacent: it is Microsoft-authored code shipped as the reference HID
client. HIDAPI is cited only as secondary corroboration and is labelled as such. Conclusions
reasoned from primary sources rather than stated by them are marked **[INFERENCE]** with the
reasoning shown; where nothing primary could be found, **[UNVERIFIED]**.

**A warning this document tries to honour.** Both CORRECTION blocks in
`docs/research/trellix-dlp-endpoint-constraints.md` record the same failure: a conclusion drawn from
negative evidence that turned out to be an artefact of how the measurement was filtered. The
temptation here is the mirror image — Microsoft's HID docs never say "you cannot open a vendor
collection exclusively", and it would be easy to read that silence as permission. The findings below
rest on what the documents *do* say plus one header definition, and every step that silence had to
carry is marked.

---

## Summary of findings

| Question | Answer |
| --- | --- |
| Does `CreateFile` on a vendor HID collection accept `dwShareMode = 0`? | **Yes** — confirmed on 10 vendor collections across 4 vendors. |
| Does that give exclusive ownership? | ~~**No.**~~ **RETIRED BY MEASUREMENT — yes, it does. See the correction below.** A zero-access second open was **refused** (`ERROR_SHARING_VIOLATION`) on every vendor collection tested. |
| Is a vendor usage page (`0xFF00`–`0xFFFF`) subject to the system's exclusive open? | **No.** The system's exclusive list is enumerated and vendor pages are absent. Microsoft actively *recommends* vendor TLCs for proprietary data. |
| Is there any other mechanism giving an equivalent guarantee? | **One, and we cannot use it.** `IOCTL_HID_ENABLE_SECURE_READ` requires **SeTcbPrivilege**. It also only covers *input reports*, not feature/output reports. |
| Does elevation change the answer? | **No.** SeTcbPrivilege is "act as part of the operating system" — not held by administrators; effectively LocalSystem only. Our helper cannot elevate anyway. |
| If two processes both open the collection, what happens to input reports? | **[INFERENCE] Broadcast, not consumed.** The HID class driver maintains per-open-file state and per-collection ring buffering; the existence of the secure-read mechanism only makes sense if additional readers would otherwise see the same input. |
| **Does #34's control survive the move to vendor HID?** | ~~**No.**~~ **Measured: YES.** See the correction immediately below; the verdict in §8 is superseded. |

---

## CORRECTION (measured 2026-08-09, `tools/windows-checks/Confirm-HidExclusivity.ps1`)

**§3's central conclusion is wrong. `dwShareMode = 0` does exclude a zero-access opener on a HID
collection, and #34's exclusivity control survives a move to vendor HID.**

The reasoning in §3 was sound as documentation analysis — `CreateFile`'s parameter table really does
say share mode 0 blocks only opens requesting delete, read or write access, and `dwDesiredAccess = 0`
really is supported. It simply does not describe what `hidclass.sys` does.

Measured on a managed Windows 11 laptop, unelevated, against **every** vendor-defined
(`usage page ≥ 0xFF00`), non-system-held collection present — **10 collections across at least 4
independent vendors** (Synaptics HIDI2C, a Logitech-style USB receiver, Shokz Loop120, two USB Audio
devices, a generic "HID Interface"):

| Open | Result |
| --- | --- |
| B1 owner: `GENERIC_READ\|GENERIC_WRITE`, share 0 | **HANDLE** (`err=0`) — 10/10 |
| B2 second: `GENERIC_READ`, share 0 | **FAILED** `err=32` `ERROR_SHARING_VIOLATION` |
| B3 second: **`dwDesiredAccess = 0`**, share 0 | **FAILED** `err=32` — 10/10, **zero leaks** |

Control: with the owner handle closed, the same zero-access open **succeeds** (`err=0`), so the
refusal is attributable to the share mode and not to some other holder.

Because the refusal is uniform across unrelated vendors' minidrivers, the enforcement is in
**`hidclass.sys`**, not in any one driver — the property we need. It is therefore reasonable to
expect it for our own device. **[INFERENCE]**, and cheap to re-confirm once the vendor interface
exists.

### Reconciling this with Microsoft's RIM sentence

§2 quotes Microsoft saying that even against RIM's exclusive open, "the user can still open a HID
device interface without requesting read and write permissions." That is the sentence §3 leaned on,
and it appears to contradict the measurement.

**[INFERENCE]** The two are consistent if "Exclusive" in the Top-Level Collections table does not mean
`dwShareMode = 0`. RIM's claim is over the *input stream* — it consumes reports so that ordinary
applications cannot keylog the keyboard — while still permitting others to open the interface for
metadata. That is a different mechanism from file-level share access, which is what B1 exercises and
what `hidclass.sys` enforced 10 times out of 10. The document's error was reading a statement about
RIM's exclusivity as a statement about share-mode semantics generally.

### What this changes, and what it does not

- **`IOCTL_HID_ENABLE_SECURE_READ` / `SeTcbPrivilege` (§4) is no longer relevant.** It was the
  fallback for a guarantee we now get from share mode 0 directly.
- **`FILE_ANY_ACCESS` (§3.2) is moot for this threat.** Its premise was that an attacker would hold a
  zero-access handle. If the open is refused, no such handle exists. `FILE_ANY_ACCESS` still means
  that *if* an attacker gets a handle by any route, it can move data — so opening early still matters.
- **Check C never ran.** Two attempts returned `ERROR_INVALID_PARAMETER` then `ERROR_INVALID_FUNCTION`
  across a sweep of report IDs 0–8: the chosen collection advertises `FeatureReportByteLength = 4` but
  does not implement `HidD_GetFeature`. That is "the check did not run", not a permission answer. It
  is now a secondary question rather than the decisive one.
- **The property is first-come, exactly as on CDC.** Whoever opens with share mode 0 first wins. §8's
  claim that HID is worse *in degree* — "on CDC an attacker must win a race and hold the port visibly;
  on HID it joins silently at any time" — **does not hold**. On both transports the attacker must win
  the same race. `windows-helper-constraints.md` §4.1's warning that the helper may start tens of
  seconds after login applies equally to both, and is the real residual risk for either.

### Corrected verdict

**#34's exclusive-ownership control survives a move to vendor HID.** The Windows objection to HID is
withdrawn. Combined with `hid-transport-macos-tcc.md`, which found vendor-page HID permission-free on
macOS, **neither platform now discriminates against vendor HID on permissions or exclusivity** — and
the helper must open with `dwShareMode = 0`, as early after login as it can, on either transport.

A note on how this document failed, since the same shape has now appeared three times in this
project's research: it reasoned correctly from documentation that turned out not to describe the
implementation, and stated the result with more confidence than a documentation-only source can carry.
The `[INFERENCE]` marker on the driver-behaviour link was right; the summary table's flat "No" was not.

---

## Superseded content below

§§1–8 are preserved as the reasoning trail. Where they conflict with the correction above, **the
correction wins** — §3's conclusion, §4's relevance, and §8's verdict are all superseded.

---

## 1. What `CreateFile` on a HID top-level collection actually is

The user-mode path is documented and unremarkable: `HidD_GetHidGuid` → `SetupDiGetClassDevs`
(`DIGCF_PRESENT | DIGCF_DEVICEINTERFACE`) → `SetupDiEnumDeviceInterfaces` →
`SetupDiGetDeviceInterfaceDetail`, whose `DevicePath` is passed to `CreateFile`:

> - Calls **HidD_GetHidGuid** to obtain the system-defined GUID for HIDClass devices.
> - Calls **SetupDiGetClassDevs** to obtain a handle to an opaque device information set […]
> - Calls **SetupDiGetDeviceInterfaceDetail** to format interface information for each collection as
>   a SP_INTERFACE_DEVICE_DETAIL_DATA structure. The **DevicePath** member of this structure contains
>   the user-mode name that the application uses with the Win32 function **CreateFile** to obtain a
>   file handle to a HID collection.
> - Calls **CreateFile** to obtain a file handle to a HID collection.

— [Finding and Opening a HID Collection](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/finding-and-opening-a-hid-collection)

The unit being opened is the **top-level collection**, not the device. Windows splits a multi-TLC
report descriptor into one PDO per TLC:

> In Windows, the HID device setup class (HIDClass) generates a unique physical device object (PDO)
> for each top-level collection described by the report descriptor.

— [Top-Level Collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections)

So the deskhopplus vendor TLC would be its own PDO with its own device interface path, independent of
the keyboard and mouse TLCs on the same composite device. That part of the design is sound and is
exactly what Microsoft recommends:

> Vendors should create separate, vendor specific, TLCs to exchange proprietary data between their
> HID client and the device. Avoid using filter drivers unless critical.

— [Developing Keyboard and Mouse HID Client Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers)

Neither the "Finding and Opening" page nor
[Opening HID Collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/opening-hid-collections)
states *any* constraint on `dwShareMode`, unlike the COM-port case where the constraint is spelled
out. Microsoft's HID documentation is simply silent on sharing for user-mode clients. That silence is
the gap this document has to close from elsewhere.

---

## 2. What the system already holds open — and why a vendor page is not on the list

The rule is real, enumerated, and does not reach vendor pages. Two pages carry the same table; the
architecture page carries the explanation:

> ### HID clients supported in Windows
>
> Windows supports the following top-level collections:
>
> | Usage page | Usage | Notes | Access mode |
> | --- | --- | --- | --- |
> | 0x0001 | 0x0001 - 0x0002 | Mouse class driver and mapper driver | Exclusive |
> | 0x0001 | 0x0004 - 0x0005 | Game controllers | Shared |
> | 0x0001 | 0x0006 - 0x0007 | Keyboard / Keypad class driver and mapper driver | Exclusive |
> | 0x0001 | 0x000C | Flight mode switch | Shared |
> | 0x0001 | 0x0080 | System controls (Power) | Shared |
> | 0x000C | 0x0001 | Consumer controls | Shared |
> | 0x000D | 0x0001 | External pen device | Exclusive |
> | 0x000D | 0x0002 | Integrated pen device | Exclusive |
> | 0x000D | 0x0004 | Touchscreen | Exclusive |
> | 0x000D | 0x0005 | Precision touchpad (PTP) | Exclusive |
> | 0x0020 | *Multiple | Sensors | Shared |
> | 0x0084 | 0x0004 | HID UPS battery | Shared |
> | 0x008C | 0x0002 | Barcode scanner (hidscanner.dll) | Shared |
>
> In the preceding table, the access mode for input HID clients is *exclusive* to prevent other HID
> clients from intercepting or receiving global input state when they aren't the target recipient of
> that input. **For security reasons, Raw Input Manager (RIM) opens all such devices exclusively.**
>
> If RIM opens a device in *exclusive* mode, the user can still open a HID device interface **without
> requesting read and write permissions** and obtain HID device information via HIDClass support
> routines (HidD_GetXxx).
>
> Sharing mode allows multiple applications to access a device. For example, multiple applications
> can access a barcode scanner to inquire about device capabilities and retrieve statistics. However,
> retrieving decoded data from a barcode scanner is done in *exclusive* mode.

— [HID Architecture, "HID clients supported in Windows"](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture)

The same table appears as
[Top-Level Collections Opened by Windows for System Use](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections-opened-by-windows-for-system-use),
whose title states the scope directly: "Windows opens the following top-level collections for system
use". And the keyboard/mouse client page states the rule in one line:

> The system opens all keyboard and mouse collections for its exclusive use.

— [Developing Keyboard and Mouse HID Client Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers)

**Reading this correctly matters.** The table's highest usage page is `0x008C`. Vendor-defined pages
begin at `0xFF00`. This is an *enumeration of what the system opens*, not a list of what applications
may open — so "vendor pages are absent" is a positive finding about system behaviour, not the kind of
negative-evidence artefact the Trellix corrections warn about. Corroborated by Microsoft
recommending vendor TLCs for exactly this purpose (§1): the system does not open them, which is what
makes them usable.

**Three things follow, and they are the good news:**

1. Our vendor collection will be openable. Nothing in the system holds it.
2. Our keyboard and mouse TLCs on the same composite device **will** be held exclusively by RIM. That
   is fine and expected — we never open them ourselves; the firmware drives them.
3. The exclusivity the system enjoys here is *its own doing*, not a property of HID. RIM chooses to
   open exclusively. That choice is available to us too — §3 is about how far it gets us.

---

## 3. The decisive finding: share mode 0 does not exclude, because HID needs no access

### 3.1 What share mode 0 promises, precisely

Read the `CreateFile` contract with care. The promise is narrower than "nobody else can open this":

> If this parameter is zero and **CreateFile** succeeds, the file or device cannot be shared and
> cannot be opened again until the handle to the file or device is closed.

then, in the parameter table for the value `0`:

> | **0** / 0x00000000 | Prevents subsequent open operations on a file or device **if they request
> delete, read, or write access.** |

— [CreateFileW (fileapi.h), *dwShareMode*](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)

The prose sentence and the table disagree in scope, and the table is the operative one — it is the
per-value specification. An open requesting **no** access requests neither delete, read nor write, so
share mode 0 does not prevent it. Microsoft confirms this is not a documentation quirk but the actual
behaviour, in the RIM sentence quoted in §2: even against RIM's exclusive open, "the user can still
open a HID device interface without requesting read and write permissions."

`dwDesiredAccess = 0` is explicitly supported:

> If this parameter is zero, the application can query certain metadata such as file, directory, or
> device attributes without accessing that file or device, even if **GENERIC_READ** access would have
> been denied.

— [CreateFileW, *dwDesiredAccess*](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)

So: whatever we do with share mode, **a second process gets a handle.** The only question left is
what that handle can do.

### 3.2 What a zero-access HID handle can do — and this is the finding

Every HID class driver IOCTL is defined with `FILE_ANY_ACCESS`. From the WDK's `hidclass.h`:

```c
#define HID_CTL_CODE(id)            CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_NEITHER,    FILE_ANY_ACCESS)
#define HID_BUFFER_CTL_CODE(id)     CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define HID_IN_CTL_CODE(id)         CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_IN_DIRECT,  FILE_ANY_ACCESS)
#define HID_OUT_CTL_CODE(id)        CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

#define IOCTL_HID_SET_FEATURE                   HID_IN_CTL_CODE(100)
#define IOCTL_HID_SET_OUTPUT_REPORT             HID_IN_CTL_CODE(101)
#define IOCTL_HID_GET_FEATURE                   HID_OUT_CTL_CODE(100)
#define IOCTL_HID_GET_INPUT_REPORT              HID_OUT_CTL_CODE(104)
#define IOCTL_HID_GET_OUTPUT_REPORT             HID_OUT_CTL_CODE(105)
```

*Sourcing note.* `hidclass.h` ships in the WDK and Microsoft does not host its text on Learn. The
lines above were read from three independent mirrors of the header — the Windows 10 SDK/WDK tree, the
Wine `include/ddk/hidclass.h`, and the mingw-w64 `ddk/hidclass.h` — which agree exactly on the four
`CTL_CODE` macros and on the IOCTL numbering. Treat this as high-confidence primary content reached
through secondary hosting, not as a Learn citation. It is worth re-reading from a local WDK install
before anything is built on it.

Microsoft documents what `FILE_ANY_ACCESS` means, and it is unambiguous:

> **Access** (bits 14-15) — Indicates the type of access that a caller must request when opening the
> file object that represents the device […]. The I/O manager will create IRPs and call the driver
> with a particular IOCTL only if the caller requested the specified access rights.
>
> - **FILE_ANY_ACCESS** — The I/O manager sends the IRP for any caller that has a handle to the file
>   object that represents the target device object. Before specifying FILE_ANY_ACCESS for a new
>   IOCTL code, you must be absolutely certain that allowing unrestricted access to your device
>   doesn't create a possible path for malicious users to compromise the system.
>
> Some system-defined I/O control codes have an **Access** value of FILE_ANY_ACCESS, which allows the
> caller to send the particular IOCTL **regardless of the access granted to the device**.

— [Defining I/O Control Codes](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/defining-i-o-control-codes)

And the user-mode wrappers are exactly these IOCTLs. `HidD_SetFeature`'s only stated requirement on
its handle is "An open handle to a top-level collection" — no access requirement, and the Remarks
name the IOCTL directly:

> Only user-mode applications can call **HidD_SetFeature**. Kernel-mode drivers can use an
> **IOCTL_HID_SET_FEATURE** request.

— [HidD_SetFeature (hidsdi.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_setfeature)

The same "An open handle to a top-level collection", with no access requirement, appears on
[HidD_GetPreparsedData](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_getpreparseddata)
and
[HidD_SetNumInputBuffers](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_setnuminputbuffers).

**The chain, stated plainly.**

1. Our helper opens the vendor TLC with `dwShareMode = 0`.
2. A hostile local process opens the same TLC with `dwDesiredAccess = 0`. Share mode 0 does not
   prevent this — documented, per the value table above.
3. That process calls `HidD_SetFeature` / `HidD_GetFeature` on its handle. The I/O manager permits it
   because the IOCTLs are `FILE_ANY_ACCESS` — documented.
4. It is now exchanging arbitrary bytes with the firmware over our channel.

**[INFERENCE]** — one link needs marking. Step 3's I/O-manager check is documented, but `hidclass.sys`
could impose a *stricter* check of its own on the file object; Microsoft documents
`IoValidateDeviceIoControlAccess` as available precisely "to perform stricter access checking than
that provided by an IOCTL's Access bits"
([Defining I/O Control Codes](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/defining-i-o-control-codes)).
No Microsoft document says whether `hidclass.sys` does so for feature reports. Two pieces of evidence
say it does not: Microsoft's own text says a zero-access handle against a RIM-exclusive device still
works for `HidD_GetXxx` (§2), and the whole point of the separate secure-read mechanism (§4) is to
restrict *reads* — which would be redundant if a zero-access handle were already impotent. This is
**the single most important thing to measure**, and check C in §7 measures exactly it, on hardware
that exists today.

*(If the measurement comes back the other way — if `hidclass.sys` refuses feature IOCTLs on a
zero-access handle — then share mode 0 becomes a real control and this document's verdict flips.
That is why the check is written to be run before anything is decided.)*

### 3.3 Whether `dwShareMode = 0` is even honoured for a HID collection

Separate question, and it does not change the verdict — but it changes what a passing test means.

For device objects, share-access enforcement is the driver's job, not the I/O manager's:

> The **IoCheckShareAccess** routine is called by file system drivers (FSDs) or other highest-level
> drivers to check whether shared access to a file object is permitted.
>
> **Other highest-level drivers might call this routine** to check the access requested when a file
> object representing such a driver's device object is opened.

— [IoCheckShareAccess (wdm.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocheckshareaccess)

"Might" is the operative word: it is optional, and `IRP_MJ_CREATE` merely hands the driver the value —

> The **Parameters.Create.ShareAccess** member is a USHORT value that describes the type of share
> access.

— [IRP_MJ_CREATE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mj-create)

**[INFERENCE] `hidclass.sys` does enforce share access.** Reasoning: RIM's exclusive open demonstrably
blocks subsequent read/write opens — Microsoft states that against such a device only a *no-access*
open remains available (§2), which is precisely the residue share-access checking leaves behind. That
behaviour cannot arise without someone performing the check, and `hidclass.sys` owns the collection
PDO. No Microsoft page states this outright.

**[UNVERIFIED]** Whether `hidclass.sys` applies that enforcement uniformly, or only in the
system-opened cases. Nothing primary addresses it. Check B in §7 settles it directly.

### 3.4 What Microsoft's own reference client does

`hclient` is the WDK's reference HID client application. It has an explicit exclusivity switch — and
never uses it:

```c
BOOLEAN
OpenHidDevice (
    _In_     LPSTR          DevicePath,
    _In_     BOOL           HasReadAccess,
    _In_     BOOL           HasWriteAccess,
    _In_     BOOL           IsOverlapped,
    _In_     BOOL           IsExclusive,
    _Out_    PHID_DEVICE    HidDevice
)
{
    DWORD   accessFlags = 0;
    DWORD   sharingFlags = 0;
    ...
    if (!IsExclusive)
    {
        sharingFlags = FILE_SHARE_READ | FILE_SHARE_WRITE;
    }

    HidDevice->HidDevice = CreateFile (DevicePath,
                                   accessFlags,
                                   sharingFlags,
                                   NULL, OPEN_EXISTING, 0, NULL);
```

— [`hid/hclient/pnp.c`, microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples/blob/main/hid/hclient/pnp.c)

Every call site in the sample passes `IsExclusive = FALSE`. The device enumeration path opens with no
access at all — `FALSE, FALSE, FALSE, FALSE`, commented "Open device with just generic query abilities
to begin with" — which is precisely the zero-access open of §3.2, written by Microsoft, in the
reference client. And `hclient` opens the *same collection twice concurrently*, once synchronous and
once overlapped, from one process, because `hid.dll`'s API surface needs a non-overlapped handle.

That last detail is not a curiosity. It means **shared, concurrent opens of a single TLC are the
assumed normal case**, designed for.

Two things follow for the design, independent of the security question: the parameter exists at all,
so passing `dwShareMode = 0` is a supported shape (it will not fail on grounds of being nonsensical);
and if the helper ever needs both overlapped and non-overlapped handles, `dwShareMode = 0` would
block its *own* second open.

**Secondary corroboration.** HIDAPI's Windows backend opens with
`share_mode = FILE_SHARE_READ|FILE_SHARE_WRITE` unconditionally
([`windows/hid.c`, libusb/hidapi](https://github.com/libusb/hidapi/blob/master/windows/hid.c)) — so
any machine running any HIDAPI-based tool has a process able to open our collection alongside us.
Secondary; cited only to show the shared-open assumption is ecosystem-wide, not Microsoft-specific.

---

## 4. The one real exclusivity mechanism, and why it is out of reach

Windows *does* have a mechanism that gives the guarantee #34 wants. It is gated on a privilege we
cannot obtain by any means available to this project.

> If a secure read is enabled for a collection, only "trusted" clients (those with SeTcbPrivilege
> privileges) can obtain input from an open file of a collection. Kernel-mode drivers have
> SeTcbPrivilege privileges by default, but user-mode applications do not.
>
> This mechanism is provided primarily so that "trusted" user-mode system components can prevent
> user-mode applications without SeTcbPrivilege privileges from obtaining input from a collection
> during critical system operations. For example, a "trusted" user-mode system component can prevent
> a user-mode application without SeTcbPrivilege privileges from obtaining confidential information
> that a user supplies during a logon operation.
>
> If a client without SeTcbPrivilege privileges uses these requests, the request does not change the
> secure read state of a collection, and the HID class driver returns a status value of
> STATUS_PRIVILEGE_NOT_HELD.

— [Enforcing a Secure Read for a HID Collection](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/enforcing-a-secure-read-for-a-hid-collection)

and on the IOCTL itself:

> Only a "trusted" user-mode application (an application with SeTcbPrivilege privileges) can
> successfully use this request. […] The HID class driver sets the **Status** field […] to
> STATUS_SUCCESS if the requester has SeTcbPrivilege privileges and the file is valid. Otherwise, it
> sets the **Status** field to STATUS_PRIVILEGE_NOT_HELD.

— [IOCTL_HID_ENABLE_SECURE_READ (hidclass.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidclass/ni-hidclass-ioctl_hid_enable_secure_read)

**Two independent reasons this does not rescue us, and the second is the harder one.**

**Reason one — the privilege.** `SeTcbPrivilege` is the "act as part of the operating system" right:

> **SE_TCB_NAME** / TEXT("SeTcbPrivilege") — This privilege identifies its holder as part of the
> trusted computer base. Some trusted protected subsystems are granted this privilege.
> User Right: Act as part of the operating system.

— [Privilege Constants (Winnt.h)](https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants)

**[INFERENCE]** Administrators do not hold it by default and cannot obtain it by elevating. Reasoning:
the same page marks the privileges that *are* default-granted — `SeChangeNotifyPrivilege` "is enabled
by default for all users", `SeCreateGlobalPrivilege` "is enabled by default for administrators,
services, and the local system account", `SeSyncAgentPrivilege` "is assigned to the Administrator and
LocalSystem accounts on domain controllers" — and says no such thing for `SeTcbPrivilege`, describing
its holders instead as "some trusted protected subsystems". A privilege is only in a token if the
account's user-rights assignment grants it; a UAC elevation surfaces rights the account already has,
it does not add new ones. Microsoft nowhere states "administrators lack SeTcbPrivilege" in those
words, so this is inference — but it is inference from a page that is explicit everywhere it means
"default-granted".

That settles sub-question 4: **elevation does not change the answer.** It is moot regardless — the
user is not a local administrator and the helper cannot elevate — but it is worth recording that even
if that constraint were lifted, this door stays shut. The only door is a kernel-mode driver, which
needs a signed driver and admin install, and #31 already rejected WinUSB on exactly that ground.

**Reason two — the scope, which would defeat us even with the privilege.** Secure read covers *input
reports* only: "only 'trusted' clients […] can obtain input from an open file of a collection". It
says nothing about feature or output reports. A relay of the shape deskhopplus needs — bulk clipboard
payloads, likely over feature reports because they are the only HID transfer with a caller-chosen
size — would have its **write** direction, the file-exfiltration direction #34 actually cares about,
entirely outside the mechanism's coverage.

---

## 5. What happens with two readers: broadcast, not consumed

Sub-question 5. Microsoft never states it in one sentence; three documented facts converge.

**Per-open-file state is architectural.** The secure-read design is described entirely in terms of
multiple open files of one collection:

> The HID class driver maintains a **file-specific secure read count for each open file of a
> collection**. The HID class driver also maintains a secure read count for the collection, which is
> the sum of the file-specific secure read counts.

— [Enforcing a Secure Read for a HID Collection](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/enforcing-a-secure-read-for-a-hid-collection)

**Reads are served from a ring buffer, not a queue with consumption semantics:**

> When the application calls **ReadFile**, it doesn't have to specify overlapped I/O because the HID
> Client Drivers buffers reports in a ring buffer.

— [Obtaining HID Reports](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/obtaining-hid-reports)

and the buffer count is set **per handle**, defaulting to 32:

> **HidD_SetNumInputBuffers** […] sets the maximum number of input reports that the HID class driver
> ring buffer can hold for a specified top-level collection.
> `[in] HidDeviceObject` — Specifies an open handle to a top-level collection.
> […] The default number of input buffers is 32.

— [HidD_SetNumInputBuffers (hidsdi.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_setnuminputbuffers)

**[INFERENCE] Input reports are delivered to every open handle that has read access — broadcast.**
Reasoning: (a) buffer depth is a property of an *open handle*, so each handle has its own buffer;
(b) `hidclass.sys` tracks per-file state and sums it per collection, which presumes concurrent files;
(c) the secure-read mechanism exists to stop untrusted clients "obtaining input from an open file of
a collection" — a mechanism that is pure redundancy if a second reader would merely steal reports
from the first rather than receive copies; (d) Microsoft's own `hclient` opens one collection twice
and reads from both (§3.4). Microsoft nowhere writes "input reports are broadcast to all open
handles", so this stays inference. Check D in §7 measures it directly.

**Consequence if it holds.** A passive eavesdropper is *free* — no share-mode trick, no race, no
privilege. It opens the collection with read access and receives a copy of every input report the
firmware sends, including every clipboard byte travelling device→host, while the helper continues
working normally and observes nothing wrong. On a COM port that same eavesdropper is locked out
outright. **This, not the write direction, is the sharpest regression** — and it is exactly the
exposure #34 was written about, since the eager text sync pushes copied passwords across without the
user acting.

---

## 6. Where this leaves the CDC-vs-HID comparison

Restricted to the one axis this document covers.

| | CDC serial | Vendor HID |
| --- | --- | --- |
| Exclusivity mechanism | `dwShareMode` **must** be 0 — documented API contract, not a choice | share mode is at best advisory; no documented contract |
| Second process can obtain a handle | No (any access request is refused) | **Yes**, with `dwDesiredAccess = 0` |
| Second process can move data | No | **[INFERENCE, testable]** Yes, via `FILE_ANY_ACCESS` feature IOCTLs |
| Passive eavesdropping on host-bound data | No | **[INFERENCE, testable]** Yes, and undetectably |
| Privileged lockout available | n/a (not needed) | Yes — secure read — but needs SeTcbPrivilege, and covers input only |

One point in HID's favour that this document does **not** disturb: the Part F measurement recorded in
`docs/research/trellix-dlp-endpoint-constraints.md` — Trellix's `hdlpdbk` filter is registered on
Ports, USBDevice and USB(system), and **not** on HIDClass. That remains a genuine advantage for HID
and is unaffected by anything here. The trade is now legible: HID buys a device stack the DLP agent
does not filter, and pays for it with the loss of exclusive ownership.

---

## 7. Cheap empirical checks — unelevated, runnable today

**Any existing HID device answers the access-semantics question; our hardware need not exist yet.**
The right target is a collection Windows does *not* hold: from §2, anything outside usage pages
`0x0001` (usages 1–2, 6–7), `0x000D` and the other Exclusive rows. Best candidate is a **vendor-defined
top-level collection, usage page ≥ `0xFF00`** — nearly every managed laptop has several, contributed
by the touchpad/keyboard vendor's configuration collections, and they are the closest possible analogue
to ours. Check A finds and ranks them. Second-best is a **Consumer Controls** collection
(`0x000C`/`0x0001`), listed Shared. Do **not** target keyboard, mouse, touchpad or pen collections:
RIM holds those, the result would be about RIM, and it tells us nothing about our case.

Style follows `tools/windows-checks/Confirm-Check1.ps1`. Two things carried over from that script,
both learned the hard way:

- **`SetLastError(0)` immediately before every call.** An earlier run read a stale `203` throughout,
  because a succeeding API need not set the error. Every P/Invoke below is preceded by a clear.
- **Score on the API return value**, not on a downstream effect. A second `CreateFile` returning a
  valid handle is the measurement; what that handle can then do is a *separate*, separately-scored
  measurement.

Save as `tools/windows-checks/Confirm-HidExclusivity.ps1` and run **unelevated**. It is read-only with
respect to the machine: it opens handles and, in check C, reads one feature report from a device the
user picks. It writes nothing to any device. If check C's target is anything other than a
vendor-defined collection, read the caveat in the script before running it.

```powershell
<#
.SYNOPSIS
    deskhopplus #59 / #34: does dwShareMode = 0 give exclusive ownership of a HID
    top-level collection the way it does for a COM port?

.DESCRIPTION
    Four checks, each scored on an API return value.

    CHECK A - inventory. Enumerate every HID top-level collection, read its usage page
      and usage via HidD_GetPreparsedData + HidP_GetCaps, and classify each against the
      table in learn.microsoft.com/.../top-level-collections-opened-by-windows-for-system-use.
      Picks a target: a vendor-defined page (>= 0xFF00) if one exists, else a Shared-listed
      page. NEVER a keyboard/mouse/digitizer collection - RIM holds those and the result
      would be about RIM, not about us.

    CHECK B - does share mode 0 exclude anything?
      B1  open target with GENERIC_READ|GENERIC_WRITE, dwShareMode = 0        (the "owner")
      B2  while B1 is held: open again with GENERIC_READ, dwShareMode = 0     expect FAIL
      B3  while B1 is held: open again with dwDesiredAccess = 0, share 0      expect SUCCESS
      B2 failing proves hidclass.sys honours share access at all (research inference 3.3).
      B3 succeeding proves share mode 0 does not exclude (documented, 3.1) - and B3's handle
      is what check C then uses. If B2 SUCCEEDS, hidclass ignores share mode entirely and
      the situation is worse than the document describes, not better.

    CHECK C - THE decisive one. Can B3's zero-access handle move data?
      Calls HidD_GetFeature on it. Every HID IOCTL is FILE_ANY_ACCESS in hidclass.h, so the
      I/O manager should permit it - but hidclass.sys may check more strictly
      (IoValidateDeviceIoControlAccess). This measures which.
      GetFeature, not SetFeature: reading a feature report is passive. Writing one to a
      device we do not own could reconfigure someone's touchpad.
      NOTE: some devices do not implement feature reports at all. An ERROR_INVALID_FUNCTION /
      "not supported" answer means the CHECK did not run, not that access was denied - the
      script distinguishes the two by error code and says so.

    CHECK D - broadcast or consumed? Opens the target twice for read from this one process,
      issues an overlapped ReadFile on both, and reports whether both complete for a single
      device report. Needs a device that actually emits input reports; it will time out on a
      quiet collection, which is reported as INCONCLUSIVE, not as a result.

    Run UNELEVATED. Read-only with respect to device state.
#>

[CmdletBinding()]
param(
    [string]$OutDir = [Environment]::GetFolderPath('Desktop'),
    [string[]]$Check = @('A','B','C','D'),
    # Override check A's automatic pick, e.g. -TargetPath '\\?\hid#vid_...'
    [string]$TargetPath
)

$ErrorActionPreference = 'Continue'
$Check = @($Check | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ } | ForEach-Object { $_.ToUpper() })
$bad = @($Check | Where-Object { $_ -notin @('A','B','C','D') })
if ($bad.Count -gt 0) { Write-Host "Unknown -Check value(s): $($bad -join ', ')" -ForegroundColor Red; exit 1 }

$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogPath = Join-Path $OutDir "deskhopplus-hid-$stamp.log"

function Write-Log {
    param([string]$Message, [string]$Level = 'INFO')
    $line = '{0} [{1,-5}] {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    switch ($Level) {
        'PASS' { Write-Host $line -ForegroundColor Green }
        'FAIL' { Write-Host $line -ForegroundColor Red }
        'WARN' { Write-Host $line -ForegroundColor Yellow }
        'HEAD' { Write-Host ''; Write-Host $line -ForegroundColor Cyan }
        default { Write-Host $line }
    }
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
}

$sig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class H {
    public const uint GENERIC_READ  = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ  = 0x00000001;
    public const uint FILE_SHARE_WRITE = 0x00000002;
    public const uint OPEN_EXISTING = 3;
    public static readonly IntPtr INVALID = new IntPtr(-1);

    // #40 found GetLastError reading a stale 203 throughout, because a succeeding API need
    // not set it. Clear immediately before every call so a reported code belongs to it.
    [DllImport("kernel32.dll")] public static extern void SetLastError(uint code);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFileW(string path, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);

    [DllImport("hid.dll", SetLastError = true)] public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll", SetLastError = true)] public static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr ppd);
    [DllImport("hid.dll", SetLastError = true)] public static extern bool HidD_FreePreparsedData(IntPtr ppd);
    [DllImport("hid.dll", SetLastError = true)] public static extern bool HidD_GetFeature(IntPtr h, byte[] buf, int len);
    [DllImport("hid.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool HidD_GetProductString(IntPtr h, StringBuilder s, int len);

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS {
        public ushort UsagePage, Usage;
        public ushort InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
        public ushort NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
    }
    [DllImport("hid.dll", SetLastError = true)] public static extern int HidP_GetCaps(IntPtr ppd, ref HIDP_CAPS caps);

    // SetupAPI - enumerate the HID device interfaces.
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr SetupDiGetClassDevsW(ref Guid g, IntPtr enumerator, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid g, uint index, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data,
        IntPtr detail, int detailSize, ref int required, IntPtr devInfoData);
    [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    public const uint DIGCF_PRESENT = 0x02, DIGCF_DEVICEINTERFACE = 0x10;

    public static string[] EnumHidPaths() {
        Guid hid; HidD_GetHidGuid(out hid);
        IntPtr set = SetupDiGetClassDevsW(ref hid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID) return new string[0];
        var list = new System.Collections.Generic.List<string>();
        try {
            var did = new SP_DEVICE_INTERFACE_DATA();
            did.cbSize = Marshal.SizeOf(did);
            for (uint i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hid, i, ref did); i++) {
                int need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, ref did, IntPtr.Zero, 0, ref need, IntPtr.Zero);
                if (need <= 0) continue;
                IntPtr buf = Marshal.AllocHGlobal(need);
                try {
                    // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize is 6 on x86, 8 on x64 -
                    // the struct is {DWORD cbSize; WCHAR DevicePath[ANYSIZE];} with pointer alignment.
                    Marshal.WriteInt32(buf, (IntPtr.Size == 8) ? 8 : 6);
                    if (SetupDiGetDeviceInterfaceDetailW(set, ref did, buf, need, ref need, IntPtr.Zero)) {
                        string p = Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + 4));
                        if (!string.IsNullOrEmpty(p)) list.Add(p);
                    }
                } finally { Marshal.FreeHGlobal(buf); }
                did.cbSize = Marshal.SizeOf(did);
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        return list.ToArray();
    }
}
'@

try { Add-Type -TypeDefinition $sig -Language CSharp -ErrorAction Stop }
catch { Write-Log "FATAL: shim would not compile: $($_.Exception.Message)" 'FAIL'; exit 2 }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
Write-Log "=== deskhopplus #59/#34 - HID exclusivity ===" 'HEAD'
Write-Log ("Host {0}   User {1}   Elevated {2}" -f $env:COMPUTERNAME, $id.Name,
    ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
Write-Log ("Log file: {0}" -f $LogPath)

function Open-Hid {
    param([string]$Path, [uint32]$Access, [uint32]$Share, [string]$Label)
    [H]::SetLastError(0)
    $h = [H]::CreateFileW($Path, $Access, $Share, [IntPtr]::Zero, [H]::OPEN_EXISTING, 0, [IntPtr]::Zero)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok = ($h -ne [H]::INVALID)
    Write-Log ("  {0,-34} access=0x{1:X8} share=0x{2:X8} -> {3} (err={4})" -f `
        $Label, $Access, $Share, $(if ($ok) { 'HANDLE' } else { 'FAILED' }), $err)
    [pscustomobject]@{ Handle = $h; Ok = $ok; Error = $err; Label = $Label }
}

# ---------------------------------------------------------------------------------------
# CHECK A - inventory and target selection
# ---------------------------------------------------------------------------------------

$script:Target = $null

function Invoke-CheckA {
    Write-Log "--- CHECK A: HID top-level collections on this machine ---" 'HEAD'
    Write-Log "Classification follows the Exclusive/Shared table at"
    Write-Log "learn.microsoft.com/windows-hardware/drivers/hid/top-level-collections-opened-by-windows-for-system-use"

    $rows = @()
    foreach ($p in [H]::EnumHidPaths()) {
        # Zero-access open: this is exactly the open the research says nothing can prevent,
        # so it also works on RIM-held keyboards and mice. Enumeration must not need read access.
        $o = $null
        [H]::SetLastError(0)
        $h = [H]::CreateFileW($p, 0, [H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE, [IntPtr]::Zero, [H]::OPEN_EXISTING, 0, [IntPtr]::Zero)
        if ($h -eq [H]::INVALID) { continue }
        try {
            $ppd = [IntPtr]::Zero
            if (-not [H]::HidD_GetPreparsedData($h, [ref]$ppd)) { continue }
            try {
                $caps = New-Object H+HIDP_CAPS
                if ([H]::HidP_GetCaps($ppd, [ref]$caps) -ne 0x110000) { continue }  # HIDP_STATUS_SUCCESS
                $sb = New-Object Text.StringBuilder 256
                [void][H]::HidD_GetProductString($h, $sb, 512)

                $up = $caps.UsagePage; $u = $caps.Usage
                $sysHeld =
                    ($up -eq 0x0001 -and ($u -in 1,2,6,7)) -or
                    ($up -eq 0x000D -and ($u -in 1,2,4,5))
                $vendor = ($up -ge 0xFF00)
                $rows += [pscustomobject]@{
                    Path = $p; UsagePage = $up; Usage = $u
                    Product = $sb.ToString()
                    Feature = $caps.FeatureReportByteLength
                    Input   = $caps.InputReportByteLength
                    SystemHeld = $sysHeld; Vendor = $vendor
                }
            } finally { [void][H]::HidD_FreePreparsedData($ppd) }
        } finally { [void][H]::CloseHandle($h) }
    }

    foreach ($r in ($rows | Sort-Object -Property @{e={-$_.UsagePage}})) {
        $tag = if ($r.SystemHeld) { 'SYSTEM-HELD (Exclusive)' } elseif ($r.Vendor) { 'VENDOR-DEFINED' } else { 'shared/other' }
        $lvl = if ($r.Vendor) { 'WARN' } else { 'INFO' }
        Write-Log ("  UP=0x{0:X4} U=0x{1:X4}  feat={2,-4} in={3,-4} {4,-24} {5}" -f `
            $r.UsagePage, $r.Usage, $r.Feature, $r.Input, $tag, $r.Product) $lvl
    }

    if ($TargetPath) {
        $script:Target = $rows | Where-Object { $_.Path -eq $TargetPath } | Select-Object -First 1
        if (-not $script:Target) { Write-Log "-TargetPath did not match any enumerated collection." 'FAIL'; return }
        Write-Log ("Target: caller-supplied.") 'WARN'
    } else {
        # Prefer a vendor-defined collection that actually supports feature reports, since
        # that is the closest analogue to ours and the only one check C can exercise.
        # Written as a loop rather than a ?? chain: Windows PowerShell 5.1 has no ??.
        foreach ($pred in @(
            { $_.Vendor -and $_.Feature -gt 0 },
            { $_.Vendor },
            { -not $_.SystemHeld -and $_.Feature -gt 0 },
            { -not $_.SystemHeld }
        )) {
            $script:Target = $rows | Where-Object $pred | Select-Object -First 1
            if ($script:Target) { break }
        }
    }

    if (-not $script:Target) {
        Write-Log "No usable target: every collection on this machine is system-held. Re-run with -TargetPath, or attach any cheap vendor HID device." 'FAIL'
        return
    }
    Write-Log ("TARGET: UP=0x{0:X4} U=0x{1:X4} feat={2} '{3}'" -f `
        $script:Target.UsagePage, $script:Target.Usage, $script:Target.Feature, $script:Target.Product) 'PASS'
    Write-Log ("        {0}" -f $script:Target.Path)
    if ($script:Target.SystemHeld) { Write-Log "        WARNING: target is system-held. Results will describe RIM, not our case." 'FAIL' }
    elseif (-not $script:Target.Vendor) { Write-Log "        Note: not a vendor-defined page. Closest available analogue, slightly weaker evidence." 'WARN' }
}

# ---------------------------------------------------------------------------------------
# CHECK B - does share mode 0 exclude anything?
# ---------------------------------------------------------------------------------------

$script:B3 = $null

function Invoke-CheckB {
    Write-Log "--- CHECK B: does dwShareMode = 0 exclude a second opener? ---" 'HEAD'
    if (-not $script:Target) { Write-Log "no target (run check A first)" 'FAIL'; return }
    $p = $script:Target.Path

    $b1 = Open-Hid -Path $p -Access ([H]::GENERIC_READ -bor [H]::GENERIC_WRITE) -Share 0 -Label 'B1 owner  rw / share 0'
    if (-not $b1.Ok) {
        Write-Log ("B1 could not take the collection at all (err={0}). Share mode 0 is refused here; nothing further is measurable on this target." -f $b1.Error) 'WARN'
        # err 5 = ACCESS_DENIED (someone holds it), err 32 = SHARING_VIOLATION.
        return
    }
    try {
        $b2 = Open-Hid -Path $p -Access ([H]::GENERIC_READ) -Share 0 -Label 'B2 second rw / share 0'
        if ($b2.Ok) {
            Write-Log "B2: a SECOND read handle opened while B1 held share-mode 0. hidclass.sys does not enforce share access at all - worse than the document assumes." 'FAIL'
            [void][H]::CloseHandle($b2.Handle)
        } else {
            Write-Log ("B2: refused (err={0}) - hidclass.sys DOES enforce share access. Research inference 3.3 confirmed." -f $b2.Error) 'PASS'
        }

        $script:B3 = Open-Hid -Path $p -Access 0 -Share 0 -Label 'B3 second ZERO-ACCESS'
        if ($script:B3.Ok) {
            Write-Log "B3: a zero-access handle opened straight through share mode 0. Exclusive ownership is NOT available - this is the documented CreateFile behaviour (dwShareMode value table)." 'FAIL'
        } else {
            Write-Log ("B3: zero-access open REFUSED (err={0}). Unexpected, and good for us - hidclass.sys is stricter than CreateFile documents. Re-read section 3 of the research before relying on it." -f $script:B3.Error) 'PASS'
        }
    } finally {
        [void][H]::CloseHandle($b1.Handle)
        Write-Log "  B1 owner handle closed"
    }
}

# ---------------------------------------------------------------------------------------
# CHECK C - can a zero-access handle move data? THE decisive check.
# ---------------------------------------------------------------------------------------

function Invoke-CheckC {
    Write-Log "--- CHECK C: can a zero-access handle exchange feature reports? ---" 'HEAD'
    Write-Log "Every HID IOCTL is FILE_ANY_ACCESS in hidclass.h, so the I/O manager should permit this."
    Write-Log "hidclass.sys may check more strictly (IoValidateDeviceIoControlAccess). This measures which."
    if (-not $script:Target) { Write-Log "no target (run check A first)" 'FAIL'; return }
    if ($script:Target.Feature -le 0) {
        Write-Log "Target reports FeatureReportByteLength = 0: it has no feature reports, so this check CANNOT run against it. Pick another target with -TargetPath (check A lists feat= per collection)." 'WARN'
        return
    }

    # Fresh zero-access handle, opened while nothing of ours holds the collection - the
    # attacker's position, with no help from us.
    $c = Open-Hid -Path $script:Target.Path -Access 0 -Share ([H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE) -Label 'C zero-access'
    if (-not $c.Ok) { Write-Log ("could not open zero-access (err={0})" -f $c.Error) 'WARN'; return }
    try {
        $buf = New-Object byte[] $script:Target.Feature
        $buf[0] = 0   # report ID 0 when the collection does not use report IDs
        [H]::SetLastError(0)
        $ok  = [H]::HidD_GetFeature($c.Handle, $buf, $buf.Length)
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()

        if ($ok) {
            Write-Log ("HidD_GetFeature SUCCEEDED on a zero-access handle, returned {0} bytes." -f $buf.Length) 'FAIL'
            Write-Log "  => FILE_ANY_ACCESS is effective. A second process needs no access rights to talk to the device."
            Write-Log "  => #34's exclusive-ownership control does NOT survive a move to vendor HID."
        } elseif ($err -eq 5) {
            Write-Log "HidD_GetFeature refused with ACCESS_DENIED (5) on a zero-access handle." 'PASS'
            Write-Log "  => hidclass.sys checks more strictly than the IOCTL access bits. Share mode 0 may be a real control after all; section 3.2 of the research needs correcting."
        } elseif ($err -in 1,31,50,1784) {
            Write-Log ("HidD_GetFeature failed with err={0} - 'not supported / invalid function' shaped. The CHECK did not run; this is not a permission answer. Pick a different target." -f $err) 'WARN'
        } else {
            Write-Log ("HidD_GetFeature failed with err={0}. Ambiguous - record the code and re-run against another vendor collection before concluding anything." -f $err) 'WARN'
        }
    } finally { [void][H]::CloseHandle($c.Handle) }
}

# ---------------------------------------------------------------------------------------
# CHECK D - broadcast or consumed?
# ---------------------------------------------------------------------------------------

function Invoke-CheckD {
    Write-Log "--- CHECK D: do two readers each receive input reports, or does one consume them? ---" 'HEAD'
    if (-not $script:Target) { Write-Log "no target (run check A first)" 'FAIL'; return }
    if ($script:Target.Input -le 0) { Write-Log "Target emits no input reports; this check cannot run against it." 'WARN'; return }

    $d1 = Open-Hid -Path $script:Target.Path -Access ([H]::GENERIC_READ) -Share ([H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE) -Label 'D1 reader one'
    if (-not $d1.Ok) { Write-Log "could not open first reader" 'WARN'; return }
    $d2 = Open-Hid -Path $script:Target.Path -Access ([H]::GENERIC_READ) -Share ([H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE) -Label 'D2 reader two'
    try {
        if (-not $d2.Ok) {
            Write-Log ("A second READ handle was refused (err={0}) even with sharing flags set. Notable - record it." -f $d2.Error) 'WARN'
            return
        }
        Write-Log "Two concurrent read handles on one collection: both opened." 'WARN'
        Write-Log "For the delivery question, drive the device now (press its button / move it) and use"
        Write-Log "the C# FileStream readers below; a bare PowerShell ReadFile here would block the console."
        Write-Log "MARKED INCONCLUSIVE unless a report actually arrives on both handles."
        # Deliberately not automating the read race: on a quiet collection it produces a
        # timeout that reads like a negative result. Two handles opening is the reportable fact.
    } finally {
        if ($d1.Ok) { [void][H]::CloseHandle($d1.Handle) }
        if ($d2.Ok) { [void][H]::CloseHandle($d2.Handle) }
    }
}

if ($Check -contains 'A') { Invoke-CheckA }
if ($Check -contains 'B') { Invoke-CheckB }
if ($Check -contains 'C') { Invoke-CheckC }
if ($Check -contains 'D') { Invoke-CheckD }
if ($script:B3 -and $script:B3.Ok) { [void][H]::CloseHandle($script:B3.Handle) }

Write-Host ''
Write-Host "Log: $LogPath" -ForegroundColor Cyan
Write-Host 'Paste it back and I will finalise the #59 write-up.' -ForegroundColor Cyan
```

**What each outcome means for the decision:**

| Result | Consequence |
| --- | --- |
| B2 refused, B3 succeeds, **C succeeds** | The document's verdict stands. #34's control is dead under vendor HID. |
| B2 refused, B3 succeeds, **C denied** | §3.2's inference is wrong: `hidclass.sys` checks harder than the IOCTL bits, and share mode 0 becomes a genuine control against *active* misuse. Passive eavesdropping (§5) would still need separate testing. Correct this document. |
| **B2 succeeds** | `hidclass.sys` ignores share mode altogether. Worse than described; nothing to salvage. |
| C's target has no feature reports | No result. Re-run against a different vendor collection — do not read the absence as a pass. |

Run checks A and B first and paste the log before running C: A's inventory determines whether the
target is a fair analogue, and a C result against an unfair target is worse than no result.

---

## 8. Verdict

**#34's exclusive-ownership control does not survive a move to vendor HID.**

On a COM port, exclusivity is a documented API contract: `dwShareMode` **must** be zero, so the port
genuinely cannot be opened twice. On a vendor HID collection there is no equivalent. Share mode 0
blocks only opens that *request* read, write or delete access — documented behaviour — and every HID
class IOCTL is declared `FILE_ANY_ACCESS`, so a handle that requested nothing can still exchange
feature reports. The one mechanism Windows provides that would deliver the guarantee,
`IOCTL_HID_ENABLE_SECURE_READ`, requires `SeTcbPrivilege`, which elevation does not confer, and it
covers only input reports in any case.

The sharpest part is the one that needs no cleverness from an attacker: **[INFERENCE]** a second
process opening our collection for read appears to receive a copy of every input report, silently and
concurrently with the helper. On CDC that eavesdropper is simply locked out. This is precisely the
exposure #34 was written about, and vendor HID reopens it.

Two honest caveats. First, the decisive link — that `hidclass.sys` does not check harder than the
IOCTL access bits — is marked **[INFERENCE]**, and check C in §7 measures it on hardware that exists
today. It should be run before #59 is closed either way. Second, the exclusivity CDC gives is not
absolute either: it is first-come, and `docs/research/windows-helper-constraints.md` §4.1 records that
the helper may start tens of seconds after login, so a process that opens the port first still wins.
The difference is one of degree, but a large one — on CDC an attacker must win a race and hold the
port visibly (the helper sees "port busy"); on HID it joins silently at any time and the helper
observes nothing.

**The honest alternatives, if HID is chosen anyway.**

1. **Authenticate and encrypt the channel, in the protocol.** #34 listed this and settled on
   exclusivity as the cheap substitute. Vendor HID removes the cheap substitute, so the cost of the
   real answer comes back. A shared secret established at pairing plus a challenge-response, with
   payload encryption over the link, defends against both the eavesdropper and the injector — and it
   makes the *firmware*, not the OS, the enforcement point. This is the only alternative that
   actually restores the property, and it is a firmware change on an MCU that also has to move the
   bytes. Recommend this if HID wins.
2. **Single-owner semantics in the firmware.** Firmware accepts one authenticated session at a time
   and refuses a second hello. Cheaper than (1) and closes the casual injector case, but does nothing
   about passive eavesdropping, because the eavesdropper never says hello — it just reads.
3. **Keep CDC and accept the `hdlpdbk` exposure.** The Trellix filter registered on Ports is the
   measured cost of staying on CDC. Weigh a filter driver that may or may not enforce against our
   device — unresolved until the device exists — against a documented, unconditional loss of the
   security control. On the current evidence CDC is the safer trade.
4. **Do not treat "vendor HID plus nothing" as an option.** That combination silently deletes a
   locked decision. If #59 lands on HID without (1) or (2), #34 must be reopened and its answer
   changed to "the exposure is accepted", with the documentation obligation #34 already spelled out.

Recommendation for #59: run checks A–C, then take the result to #31. If C succeeds, vendor HID costs
a protocol-level authentication and encryption layer — real firmware work — and that cost belongs in
the three-way comparison alongside macOS's TCC question, not as a footnote.

---

## Sources

Microsoft Learn and WDK unless noted.

- [HID Architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture)
- [Top-Level Collections Opened by Windows for System Use](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections-opened-by-windows-for-system-use)
- [Top-Level Collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections)
- [HID Collections Overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-collections)
- [Finding and Opening a HID Collection](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/finding-and-opening-a-hid-collection)
- [Opening HID Collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/opening-hid-collections)
- [Obtaining HID Reports](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/obtaining-hid-reports)
- [Developing Keyboard and Mouse HID Client Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers)
- [Enforcing a Secure Read for a HID Collection](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/enforcing-a-secure-read-for-a-hid-collection)
- [IOCTL_HID_ENABLE_SECURE_READ (hidclass.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidclass/ni-hidclass-ioctl_hid_enable_secure_read)
- [IOCTL_HID_SET_FEATURE (hidclass.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidclass/ni-hidclass-ioctl_hid_set_feature)
- [HidD_SetFeature (hidsdi.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_setfeature)
- [HidD_GetPreparsedData (hidsdi.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_getpreparseddata)
- [HidD_SetNumInputBuffers (hidsdi.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_setnuminputbuffers)
- [Defining I/O Control Codes](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/defining-i-o-control-codes)
- [IoCheckShareAccess (wdm.h)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocheckshareaccess)
- [IRP_MJ_CREATE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mj-create)
- [CreateFileW (fileapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [CreateFileA (fileapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)
- [Privilege Constants (Winnt.h)](https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants)
- [Supporting human interface devices (XAML)](https://learn.microsoft.com/en-us/previous-versions/windows/apps/dn263140(v=win.10)) — archived; the WinRT HID API's blocked-usage-page list, which excludes vendor pages, corroborating §2 for a different API surface
- Microsoft WDK sample source: [`hid/hclient/pnp.c`](https://github.com/microsoft/Windows-driver-samples/blob/main/hid/hclient/pnp.c) and [`hid/hclient/hclient.c`](https://github.com/microsoft/Windows-driver-samples/blob/main/hid/hclient/hclient.c)
- WDK header `hidclass.h` — read from three independent mirrors (Windows 10 SDK/WDK tree, Wine `include/ddk/hidclass.h`, mingw-w64 `ddk/hidclass.h`), which agree exactly. Microsoft does not host this header's text on Learn.
- **Secondary:** [HIDAPI `windows/hid.c`](https://github.com/libusb/hidapi/blob/master/windows/hid.c) — cited only to show the shared-open assumption is ecosystem-wide.
