# Windows constraints for an unsigned, unelevated background helper

Research for [issue #36](https://github.com/myn/deskhopplus/issues/36), under
[map #31](https://github.com/myn/deskhopplus/issues/31).

**Question.** What limits an unsigned, unelevated, background helper application on a
corporate-managed Windows 11 laptop, for: CDC serial port access, cursor positioning via
`SetCursorPos`, clipboard read/write from the background, and running at login?

**Scope note.** The user's managed work laptop already runs mkroamer's unsigned executable, so
*unsigned execution itself* is established as permitted on that machine. This document establishes
what else is permitted, and — more usefully — which enterprise policies could turn each capability
off, so the design can degrade rather than break.

**Sourcing.** Every claim below cites Microsoft Learn or a Microsoft open specification. Where a
conclusion is an inference from primary sources rather than a documented statement, it is marked
**[INFERENCE]** and the reasoning is shown. Where nothing primary could be found, it is marked
**[UNVERIFIED]**.

---

## Summary of findings

| Capability | Verdict for unelevated, unsigned, per-user helper |
| --- | --- |
| Open CDC serial port | **Permitted.** No elevation required; documented `CreateFile` constraints only (exclusive access). Blockable by device-installation policy, not by a port-access policy. |
| `SetCursorPos` | **Permitted at medium integrity, with two documented preconditions.** Not listed among UIPI's blocked operations. Fails whenever the input desktop is not the caller's desktop — i.e. UAC prompt, Ctrl+Alt+Del, lock screen, secure screen saver. |
| Clipboard read/write from background | **Permitted.** Clipboard is explicitly a shared USER resource *not* isolated by UIPI. `OpenClipboard` fails whenever another window holds it open — the real constraint is contention, not privilege. |
| Run at login | **Permitted without admin** via `HKCU\...\Run`, the per-user Startup folder, or a non-boot-trigger scheduled task. All three are commonly restricted by enterprise policy. |
| SmartScreen / Defender on first run | **Warning, or a hard block, depending on policy.** Driven by Mark of the Web plus reputation. Several enterprise settings turn the warning into a non-bypassable block. |
| In-box CDC driver bind | **Automatic, no admin required** — compatible-ID match to `usbser.inf`. Directly blockable by Device Installation Restrictions policy. |
| Console vs windowed vs service | **A service is the wrong shape.** Session 0 isolation puts services on a different window station with no access to the user's clipboard or cursor. |

---

## 1. Serial port (CDC) access from an unelevated process

### 1.1 The API contract

A COM port is opened with `CreateFile`. The documented constraints are shape constraints, not
privilege constraints:

> The **CreateFile** function can create a handle to a communications resource, such as the serial
> port COM1. For communications resources, the *dwCreationDisposition* parameter must be
> **OPEN_EXISTING**, the *dwShareMode* parameter must be zero (exclusive access), and the
> *hTemplateFile* parameter must be **NULL**. Read, write, or read/write access can be specified,
> and the handle can be opened for overlapped I/O.
>
> To specify a COM port number greater than 9, use the following syntax: `"\\.\COM10"`. This syntax
> works for all port numbers and hardware that allows COM port numbers to be specified.

— [CreateFileA (fileapi.h), "Communications Resources"](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)

Nothing in that page, nor in
[Communications Resources](https://learn.microsoft.com/en-us/windows/win32/devio/communications-resources),
states any requirement for administrator rights or elevation.

**[INFERENCE]** An unelevated medium-integrity process can open the port. The documentation
specifies no privilege requirement, and access is governed by the ordinary device-object DACL,
which for a PnP serial device grants the interactive user access. No Microsoft page was found that
states this in so many words, so it is inference from the absence of a stated requirement plus the
general access-control model — but it is consistent with mkroamer already working on the machine.

**Design consequences from the documented parts, which are firm:**

- `dwShareMode` **must be zero**. The port is exclusive. Two helper instances, or a helper plus a
  stray terminal program, cannot both hold it. The helper needs a single-instance guard and a
  clear "port busy" state.
- Port number ≥ 10 requires the `\\.\COMnn` form. Given standing decision 1 in map #31 (locate the
  device by identifier + serial, never by port name), the helper will be resolving a device
  interface to a port name anyway — it must emit the `\\.\` form.

### 1.2 Does any common policy restrict serial port access?

No policy was found that restricts *opening* an already-installed serial port for a standard user.
The enforcement point on Windows is one layer earlier: whether the device is allowed to install at
all. See §5.

**[UNVERIFIED]** Whether any specific third-party endpoint-security product (as distinct from
Windows itself) intercepts COM port handles. Out of reach of Microsoft primary sources; would need
testing on the actual managed laptop.

---

## 2. Cursor positioning — `SetCursorPos`, UIPI, and the secure desktop

This is the load-bearing section. The short version: **UIPI is not the obstacle; the input desktop
is.**

### 2.1 What `SetCursorPos` documents as its requirements

> The cursor is a shared resource. A window should move the cursor only when the cursor is in the
> window's client area.
>
> The calling process must have **WINSTA_WRITEATTRIBUTES** access to the window station.
>
> The input desktop must be the current desktop when you call **SetCursorPos**. Call
> [OpenInputDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openinputdesktop)
> to determine whether the current desktop is the input desktop. If it is not, call
> [SetThreadDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddesktop)
> with the **HDESK** returned by **OpenInputDesktop** to switch to that desktop.

— [SetCursorPos (winuser.h)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos)

Note what is *absent*: no mention of UIPI, integrity levels, foreground window, or elevation.

Also documented on the same page, and relevant: if the coordinates fall outside a rectangle set by
a prior `ClipCursor` call, "the system automatically adjusts the coordinates so that the cursor
stays within the rectangle."

### 2.2 `WINSTA_WRITEATTRIBUTES` — is it granted?

Yes, to the interactive user. The generic access rights for the interactive window station (the one
assigned to the interactive user's logon session) are documented as:

> | GENERIC_WRITE | STANDARD_RIGHTS_WRITE WINSTA_ACCESSCLIPBOARD WINSTA_CREATEDESKTOP WINSTA_WRITEATTRIBUTES |

— [Window Station Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/winstation/window-station-security-and-access-rights)

Two things fall out of that one table. `WINSTA_WRITEATTRIBUTES` (the right `SetCursorPos` needs) and
`WINSTA_ACCESSCLIPBOARD` ("Required to use the clipboard") are **both** in `GENERIC_WRITE` on the
interactive window station. A normal interactive user process has both. Elevation is not what grants
them; being the interactive user is.

The same page confirms the flip side: **noninteractive** window stations (assigned to "all logon
sessions other than that of the interactive user") get `WINSTA_ACCESSCLIPBOARD` in `GENERIC_WRITE`
but **not** `WINSTA_WRITEATTRIBUTES`, and not `WINSTA_READSCREEN`. This is the mechanism behind the
service caveat in §7.

### 2.3 UIPI: what it actually blocks

The authoritative enumeration:

> User Interface Privilege Isolation (UIPI) implements restrictions in the windows subsystem that
> prevents lower-privilege applications from sending window messages or installing hooks in
> higher-privilege processes. […] The restrictions are implemented in the **SendMessage** and
> related window message functions.
>
> UIPI does not interfere with or change the behavior of window messaging between applications at
> the same privilege (or integrity) level. UIPI prevents lower-privilege processes from accessing
> higher-privilege processes by blocking the following behavior. A lower-privilege process cannot:
>
> - Perform a window handle validation of a process running with higher rights.
> - Use SendMessage or PostMessage to application windows running with higher rights. These APIs
>   return success but silently drop the window message.
> - Use thread hooks to attach to a process running with higher rights.
> - Use journal hooks to monitor a process running with higher rights.
> - Perform dynamic link library (DLL) injection to a process running with higher rights.
>
> With UIPI enabled, the following shared USER resources are still shared between processes at
> different privilege levels.
>
> - Desktop window, which actually owns the screen surface
> - Desktop heap read-only shared memory
> - Global atom table
> - **Clipboard**

— [Windows Integrity Mechanism Design, "User Interface Privilege Isolation (UIPI) and integrity"](https://learn.microsoft.com/en-us/previous-versions/dotnet/articles/bb625963(v=msdn.10))

`SetCursorPos` appears on neither list of blocked operations. The "desktop window, which actually
owns the screen surface" is explicitly still shared across integrity levels, and the same section
notes that painting to the screen is likewise not blocked by UIPI.

**Contrast this with `SendInput`, which *is* explicitly subject to UIPI:**

> This function fails when it is blocked by UIPI. Note that neither GetLastError nor the return
> value will indicate the failure was caused by UIPI blocking.
>
> This function is subject to UIPI. Applications are permitted to inject input only into
> applications that are at an equal or lesser integrity level.

— [SendInput (winuser.h)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)

That contrast is the strongest available evidence. Microsoft documents UIPI subjection where it
applies (`SendInput`) and does not document it where it does not (`SetCursorPos`) — and `SetCursorPos`
instead documents an entirely different, desktop-scoped precondition.

**[INFERENCE] Can an unelevated helper move the cursor while an elevated window has focus?**
**Yes**, provided the elevated window is on the same (Default) desktop and the input desktop is the
Default desktop. Reasoning: (a) `SetCursorPos` is absent from the enumerated UIPI blocks; (b) it is
absent from `SendInput`-style "subject to UIPI" language on its own reference page; (c) its only
documented requirements — `WINSTA_WRITEATTRIBUTES` and input-desktop identity — are both satisfied
in that scenario; (d) the cursor lives on the shared desktop window, which UIPI explicitly does not
isolate. Microsoft nowhere states "SetCursorPos works against an elevated foreground window" in
those words, so this remains inference, and it is worth a five-minute empirical check on the target
laptop before the design leans on it.

A caveat that *is* documented: an elevated app that has called `ClipCursor` will clamp the helper's
requested coordinates ([SetCursorPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos)).
Games and remote-desktop clients do this routinely. The helper cannot detect the clamp from the
return value — `SetCursorPos` returns success — so it must read the cursor back to know where it
actually landed. This dovetails with standing decision 6 in map #31 (the helper reports the true
cursor position back so the firmware re-anchors): that reporting is not merely a nicety, it is the
only way to detect a clip.

### 2.4 Lock screen, Ctrl+Alt+Del, and the UAC secure desktop

This is where cursor positioning genuinely stops working, and it is documented plainly:

> The desktops associated with the interactive window station, Winsta0, can be made to display a
> user interface and receive user input, but only one of these desktops at a time is active. This
> active desktop, also known as the *input desktop*, is the one that is currently visible to the
> user and that receives user input.
>
> By default, there are three desktops in the interactive window station: Default, ScreenSaver, and
> Winlogon.
>
> […] During the user's session, the system switches to the Winlogon desktop when the user presses
> the CTRL+ALT+DEL key sequence, or when the User Account Control (UAC) dialog box is open.
>
> The Winlogon desktop's security descriptor allows access to a very restricted set of accounts,
> including the LocalSystem account. **Applications generally do not carry any of these accounts'
> SIDs in their tokens and therefore cannot access the Winlogon desktop or switch to a different
> desktop while the Winlogon desktop is active.**
>
> Whenever a secure screen saver activates, the system automatically switches to the ScreenSaver
> desktop, which protects the processes on the default desktop from unauthorized users.

— [Desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)

Combine that with the `SetCursorPos` requirement that "the input desktop must be the current desktop
when you call SetCursorPos", and the conclusion is firm rather than inferential:

- **UAC prompt up** → input desktop is Winlogon → the helper's thread desktop is Default → the
  precondition is violated, and the helper cannot switch to Winlogon (it lacks the SID).
- **Ctrl+Alt+Del** → same.
- **Locked workstation / secure screen saver** → input desktop is Winlogon or ScreenSaver → same.

**Design consequence.** Cursor placement must be treated as *best-effort and revocable*, not as a
guaranteed operation. Concretely:

1. The helper must check `SetCursorPos`'s BOOL return and not assume success.
2. It should verify with `GetCursorPos` afterward — this catches both `ClipCursor` clamping and a
   silently ineffective call.
3. There must be a defined behaviour for "the placement failed": the firmware's fallback placement
   stands and the helper reports the discrepancy. Standing decision 7 (degrade quietly) already
   covers the no-helper case; this is the narrower case of *helper present but momentarily
   powerless*, and it needs its own state, because it is transient and self-healing (the desktop
   switches back).
4. Crossing *into* a locked machine is a real scenario for this project — the user switches to
   computer B, which is locked. The entry cursor will not be placed. Placement should be deferred
   or simply abandoned; a queued "place it when the desktop comes back" behaviour would fire at an
   arbitrary later moment and move the user's cursor unexpectedly. Recommend abandon.

`OpenInputDesktop` is the documented way to test this before trying
([SetCursorPos remarks](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos)),
and is cheaper than a failed placement plus readback.

### 2.5 UIAccess is not available to us

The documented escape hatch from UIPI is the `uiAccess="true"` manifest attribute, but its policy
checks rule it out:

> - The application must have a digital signature that can be verified using a digital certificate
>   that chains up to a trusted root in the local machine Trusted Root Certification Authorities
>   certificate store.
> - The application must be installed in a local folder application directory that is writeable only
>   by administrators, such as the Program Files directory.

— [Windows Integrity Mechanism Design, "UIAccess for UI automation applications"](https://learn.microsoft.com/en-us/previous-versions/dotnet/articles/bb625963(v=msdn.10))

Both requirements are out of reach for an unsigned helper installed per-user, and the second needs
administrator rights. Note this does not cost us anything, because §2.3 concludes we do not need
UIPI bypass for `SetCursorPos` — and UIAccess would not help with the Winlogon desktop anyway, which
is a desktop-ACL matter, not a UIPI matter.

---

## 3. Clipboard access from a background process

### 3.1 Privilege: not a constraint

Two independent primary sources say the clipboard is available:

- `WINSTA_ACCESSCLIPBOARD` ("Required to use the clipboard") is in `GENERIC_WRITE` for both the
  interactive **and** noninteractive window station
  ([Window Station Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/winstation/window-station-security-and-access-rights)).
- The clipboard is explicitly listed as a shared USER resource "still shared between processes at
  different privilege levels" with UIPI enabled
  ([Windows Integrity Mechanism Design](https://learn.microsoft.com/en-us/previous-versions/dotnet/articles/bb625963(v=msdn.10))).

Each window station has its own clipboard — "The clipboard for each window station has an associated
clipboard sequence number"
([About the Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/about-the-clipboard)).
The helper must therefore run in the *user's* session and window station, which it will, as a
per-user login item. This is the second reason a service is wrong (§7).

### 3.2 Contention: the actual constraint

> **OpenClipboard** fails if another window has the clipboard open.

— [OpenClipboard (winuser.h)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard)

This is the practical failure mode and it has nothing to do with privilege or being in the
background. Any application — a clipboard manager, Office, a browser — can hold the clipboard open
and the helper's call fails. The documentation offers no built-in wait, no timeout, and no
queueing. `GetOpenClipboardWindow` will identify the holder, but not always:

> If an application or DLL specifies a **NULL** window handle when calling the **OpenClipboard**
> function, the clipboard is opened but is not associated with a window. In such a case,
> **GetOpenClipboardWindow** returns **NULL**.

— [GetOpenClipboardWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getopenclipboardwindow)

**Design consequence.** Every clipboard operation in the helper needs a bounded retry-with-backoff
around `OpenClipboard`, and a defined give-up that surfaces as a transfer failure the protocol can
report. Given map #31's chunked transfer and progress/abort UI, this belongs in the clipboard
adapter, not in the transfer state machine — the transfer should see a clean "could not read the
clipboard" error, not a raw Win32 failure.

Two more documented rules that constrain the design:

> If an application calls **OpenClipboard** with hwnd set to **NULL**, **EmptyClipboard** sets the
> clipboard owner to **NULL**; this causes **SetClipboardData** to fail.

— [OpenClipboard](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard)

and

> Places data on the clipboard in a specified clipboard format. The window must be the current
> clipboard owner, and the application must have called the **OpenClipboard** function.

— [SetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata)

**So the helper needs a real `HWND`** — a hidden message-only window is sufficient, but "no window
at all" is not an option for *writing* the clipboard. That also settles the app-shape question in
§7: this is a windowed app with a hidden window, not a console app.

### 3.3 Change notification

The helper needs to know when the local clipboard changes, to offer it across the link. The modern
API needs a window too:

> Places the given window in the system-maintained clipboard format listener list. […] When a window
> has been added to the clipboard format listener list, it is posted a **WM_CLIPBOARDUPDATE** message
> whenever the contents of the clipboard have changed.

— [AddClipboardFormatListener](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-addclipboardformatlistener)

Prefer this over the legacy `SetClipboardViewer` chain, which
[About the Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/about-the-clipboard)
documents as requiring every participant to correctly forward `WM_DRAWCLIPBOARD` and to unlink
itself with `ChangeClipboardChain` — a chain any misbehaving app can break. `GetClipboardSequenceNumber`
is available as a cheap poll/verify (same page).

### 3.4 Data handling rules that affect the codec

> **Caution** Clipboard data is not trusted. Parse the data carefully before using it in your
> application.
>
> The clipboard controls the handle that the **GetClipboardData** function returns, not the
> application. The application should copy the data immediately. The application must not free the
> handle nor leave it locked. The application must not use the handle after the **EmptyClipboard**
> or **CloseClipboard** function is called […]

— [GetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclipboarddata)

The helper must **copy out under the open-clipboard window** and close promptly — it cannot hold the
clipboard open for the duration of a chunked transfer over the CDC link. Given map #31's ~256 KB
eager threshold and 10–64 MB cap, this means: open, snapshot to the helper's own buffer, close, then
transfer. Holding the clipboard open across a multi-second USB+UART relay would block every other
app on the machine from copying or pasting.

Delayed rendering (`SetClipboardData` with `hMem == NULL`, then servicing `WM_RENDERFORMAT` /
`WM_RENDERALLFORMATS`) is documented as available and is the natural fit for map #31's *lazy* mode
for files and large payloads — the helper advertises the format and only pulls the bytes across the
link when a paste actually happens. The obligation it creates is firm: "If a window delays rendering,
it must process the WM_RENDERFORMAT and WM_RENDERALLFORMATS messages"
([SetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata)).
`WM_RENDERALLFORMATS` fires when the helper is about to exit, so shutdown must service it or the
pasted data evaporates.

### 3.5 Enterprise policies that can affect the clipboard

- **Clipboard history / cross-device clipboard** are separately controllable:
  `Experience/AllowClipboardHistory` — "This policy setting determines whether history of Clipboard
  contents can be stored in memory" — and `Experience/AllowCrossDeviceClipboard`
  ([Experience Policy CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-experience)).
  These govern Windows' own features, not third-party clipboard access, so they do not block the
  helper. They are worth knowing about because a managed laptop with cross-device clipboard disabled
  is exactly a laptop where IT has an opinion about clipboard data leaving the machine.
- **Microsoft Purview Endpoint DLP** can restrict clipboard and removable-device egress of matched
  sensitive content, with configurable activities including "Paste to supported browsers", "Copy to
  a removable USB device", and "Copy or move using RDP"
  ([Get started with collecting files that match DLP policies from devices](https://learn.microsoft.com/en-us/purview/dlp-copy-matched-items-get-started)).
  **[UNVERIFIED]** whether Endpoint DLP specifically intercepts a third-party app calling
  `GetClipboardData`, and whether a USB CDC device counts as "a removable USB device" for the
  copy-to-USB restriction. The documentation enumerates the restrictable activities but does not
  describe the interception mechanism at API granularity. This is the single most plausible way a
  corporate policy silently breaks clipboard relay on a managed laptop, and it deserves a real test.

---

## 4. Running at login without administrator rights

Three viable per-user mechanisms, all documented, none requiring elevation.

### 4.1 `HKEY_CURRENT_USER` Run key

> Use `Run` or `RunOnce` registry keys to make a program run when a user logs on. The `Run` key makes
> the program run every time the user logs on […] These keys can be set for the user or the machine.
>
> - **HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run**

— [Run and RunOnce Registry Keys](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

Constraints from the same page: the value is "a command line no longer than **260 characters**";
ordering among multiple Run entries "is indeterminate"; the keys are ignored in Safe Mode by default;
and —

> The system does not provide guarantees about how promptly the programs in the `Run` key are run. To
> improve the user experience, the system may choose to delay the execution of programs in the `Run`
> key and in the Startup group to a time when they are less likely to interfere with the foreground
> user experience or with each other.

**Design consequence.** The helper may start seconds or tens of seconds after the desktop appears.
The firmware must not assume a helper is present just because the user has logged in — which
standing decision 7 already handles, but it means the *arrival* of a helper mid-session is normal
traffic, not an anomaly. Whatever hello/capability handshake the protocol defines has to be
initiatable by the helper at any time, not only at device attach.

### 4.2 Per-user Startup folder

`FOLDERID_Startup`, GUID `{B97D20BB-F46A-4C97-BA10-5E3608430854}`, folder type **PERUSER**, default
path `%APPDATA%\Microsoft\Windows\Start Menu\Programs\StartUp`
([KNOWNFOLDERID](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid)).
Per-user and user-writable, so no elevation. (`FOLDERID_CommonStartup` is the machine-wide one under
`%ALLUSERSPROFILE%`, which would need admin.) Subject to the same "may choose to delay" caveat quoted
above, which names the Startup group explicitly.

### 4.3 Scheduled task with a logon trigger

A logon trigger runs a task when a specified user logs on
([Logon Trigger Example](https://learn.microsoft.com/en-us/windows/win32/taskschd/logon-trigger-example--scripting-)).
Registration is via `ITaskFolder::RegisterTaskDefinition`, whose Remarks state:

> Only a member of the Administrators group can create a task with a **boot** trigger.

— [ITaskFolder::RegisterTaskDefinition](https://learn.microsoft.com/en-us/windows/win32/api/taskschd/nf-taskschd-itaskfolder-registertaskdefinition) (emphasis added)

**[INFERENCE]** Since the documentation calls out administrator membership as required *specifically*
for boot triggers, a **logon**-triggered task registered to run as the current user does not require
administrator rights. This is inference from a targeted restriction implying the absence of a general
one; no page was found stating "standard users can register tasks" outright.

Use `TASK_LOGON_INTERACTIVE_TOKEN` — "User must already be logged on. The task will be run only in an
existing interactive session" (same page). That is exactly the requirement from §3.1: the helper must
be in the user's interactive session to reach that window station's clipboard. Note also the
documented pitfall: registering with a *group* in `userId` plus `TASK_LOGON_INTERACTIVE_TOKEN`
"can successfully register […] but the task will not run."

### 4.4 Which are commonly blocked by corporate policy

**[UNVERIFIED, partially.]** Group Policy settings named "Do not process the run once list" and "Do
not process the legacy run list" exist and are widely deployed, but a canonical Microsoft Learn
reference page for them could not be located during this research — only community and Q&A pages,
which the brief excludes. Treat the existence of Run-key suppression as likely but unconfirmed here.

What *is* confirmed as a blocking mechanism is application control rather than autostart control:

- **App Control for Business (WDAC) / AppLocker.** "Traditional Win32 apps on Windows can run without
  being digitally signed. This practice can expose Windows devices to malicious or tampered code and
  presents a security vulnerability" — organizations that "enforce codesigning for all executable
  code are best-positioned to protect their Windows computers." Policies can be built around a
  managed installer, path rules where "admin-only file path rules can be used to allow apps installed
  by admin users", or reputation via the Intelligent Security Graph
  ([Understand App Control for Business policy design decisions](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/understand-appcontrol-policy-design-decisions)).
  An "admin-only path rule" policy is precisely one that permits `C:\Program Files\…` and denies
  `%LOCALAPPDATA%\…` — which is where an unelevated per-user install has to live.

**Recommendation.** Support **all three** autostart mechanisms and let the install step pick whichever
succeeds, rather than hard-coding one. They are each a few lines of code, they fail in different
ways, and on a managed machine you will not know in advance which survives. Verify success by
reading back what was written, not by assuming the write took.

---

## 5. The in-box CDC driver and device installation policy

### 5.1 Binding is automatic and needs no administrator

> *Usbser.inf* is located in the `%Systemroot%\INF` directory. This setup information (INF) file
> loads *Usbser.sys* as the functional device object (FDO) in the device stack. If your device
> belongs to the communications and CDC control device class, *Usbser.sys* loads automatically. You
> don't need to write your own INF file to reference the driver. Windows loads the driver based on a
> compatible ID match […]
>
> `USB\Class_02`
> `USB\Class_02&SubClass_02`
>
> To load *Usbser.sys* automatically, set the class code to 02 and subclass code to 02 in the Device
> Descriptor.

and

> If you're trying to install a USB device class driver included in Windows, you don't need to
> download the driver. Windows installs these drivers automatically.

— [USB Serial Driver (Usbser.sys)](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids)

This confirms standing decision 2 in map #31. It also carries a hard firmware requirement: **class
02 / subclass 02 in the device descriptor**. The same page warns that "if your device specifies class
code 02 but a subclass code other than 02, *Usbser.sys* doesn't load automatically."

### 5.2 Device installation policy can block it, and can do so retroactively

The Device Installation Restrictions policies match on hardware ID, compatible ID, device instance
ID, or device setup class GUID. Directly relevant:

> **PreventInstallationOfMatchingDeviceIDs** — This policy setting allows you to specify a list of
> Plug and Play hardware IDs and compatible IDs for devices that Windows is prevented from
> installing. By default, this policy setting takes precedence over any other policy setting that
> allows Windows to install a device.

> **PreventInstallationOfDevicesNotDescribedByOtherPolicySettings** — This policy setting allows you
> to prevent the installation of devices that aren't specifically described by any other policy
> setting.

— [DeviceInstallation Policy CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-deviceinstallation)

Three sharp points from that page:

1. The worked example for `PreventInstallationOfMatchingDeviceIDs` blocks `USB\Composite` and
   `USB\Class_FF`. A composite device — which deskhopplus is, presenting HID plus CDC — matches
   `USB\Composite`. An organization using the documented example verbatim blocks the whole device,
   not just the serial function.
2. Blocking can be **retroactive**: `DeviceInstall_IDs_Deny_Retroactive` set to `true` "appl[ies] the
   policy to matching device classes that are already installed." A device that works today can stop
   working after a policy refresh.
3. The **allowlist** posture — `PreventInstallationOfDevicesNotDescribedByOtherPolicySettings`, i.e.
   default-deny — is a documented and common lockdown configuration. On such a machine an unlisted
   VID/PID never installs, full stop, and no amount of helper cleverness helps.

Diagnosis is documented: check `C:\windows\INF\setupapi.dev.log` for the
`[Device Installation Restrictions Policy Check]` section (same page). Worth building into whatever
diagnostics map #31 eventually specifies — it turns "the device doesn't work" into "policy blocked
it, here's the log line", which is what the user needs to take to IT.

**Design consequence.** The helper must distinguish *device not present* from *device present but not
bound to a COM port*. The second is the policy-blocked case and needs a different message. It also
argues for the firmware choosing a distinctive, stable VID/PID pair, since the practical remedy on a
managed machine is asking IT to allowlist a specific hardware ID.

**Also note:** none of these policies apply to keyboard/mouse HID separately from the composite
device unless targeted that way. Under a default-deny policy the whole device including HID is
blocked, so the "degrade to a plain HID switch" fallback in standing decision 7 does not save you
from *this* particular failure. Worth stating plainly in the map.

---

## 6. SmartScreen and Defender against an unsigned executable

### 6.1 The trigger is Mark of the Web plus absent reputation

> Checking downloaded files against a list of files that are well known and downloaded frequently. If
> the file isn't on that list, Microsoft Defender SmartScreen shows a warning, advising caution.
>
> **Reputation-based URL and app protection:** […] It also provides reputation checks for apps,
> checking downloaded programs and the digital signature used to sign a file. If a URL, a file, an
> app, or a certificate has an established reputation, users don't see any warnings. **If there's no
> reputation, the item is marked as a higher risk and presents a warning to the user.**

and, critically for scope:

> **Important** SmartScreen protects against malicious files from the internet. It doesn't protect
> against malicious files on internal locations or network shares, such as shared folders with UNC
> paths or SMB/CIFS shares.

— [Microsoft Defender SmartScreen overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/)

The "from the internet" marker is the Zone.Identifier alternate data stream:

> Windows Internet Explorer uses the stream name Zone.Identifier for storage of URL security zones.
> The fully qualified form is `sample.txt:Zone.Identifier:$DATA`. The stream is a simple text stream
> of the form: `[ZoneTransfer]` / `ZoneId=3`

— [\[MS-FSCC\]: Zone.Identifier Stream Name](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6e3f7352-d11c-4d76-8c39-2516a9df36e8)

**Expected UX, and it recurs.** An unsigned helper downloaded from GitHub carries MOTW and has no
reputation, so it draws the "unrecognized app" warning at first run. Reputation accrues per binary,
so **every new build draws it again** — which makes this a per-release friction, not a one-time
hurdle. This is a strong practical argument for signing eventually, even though map #31 puts signing
out of scope. Worth recording as a known cost rather than a surprise.

### 6.2 An administrator can make it a hard block

> If you enable this setting, it turns on Microsoft Defender SmartScreen and your users are unable to
> turn it off. When enabling this feature, you must pick whether users may choose to ignore warnings
> and run an unknown or malicious program.

and Microsoft's own recommendation to enterprises:

> **Enable with the Warn and prevent bypass option.** Stops users from ignoring warning messages about
> malicious files downloaded from the Internet.

with the MDM equivalents `SmartScreen/EnableSmartScreenInShell` = 1 and
`SmartScreen/PreventOverrideForFilesInShell` = 1.

— [Available Microsoft Defender SmartScreen settings](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/available-settings)

So on a laptop configured per Microsoft's own enterprise recommendation, an unsigned downloaded
helper is **blocked with no bypass**. Since mkroamer already runs on this machine, that configuration
is evidently not in force there — but it is the documented default recommendation, so it is the
thing most likely to change under the design's feet.

### 6.3 Smart App Control

> Malware, Potentially Unwanted Apps (PUA), and unknown, unsigned code are blocked by default.
>
> In *enforcement mode*, Smart App Control is actively protecting your device. Apps cannot be run
> unless they are recognized by Microsoft's app intelligence services, or they are signed with a
> trusted certificate.
>
> [It] can only be enabled on a clean install of a version of Windows that contains the Smart App
> Control feature.
>
> […] there are some legitimate tasks that corporate users, developers, or others do regularly that
> might not be a great experience with Smart App Control running. **If we detect that you're one of
> those users, we automatically turn Smart App Control off** so you can work with fewer interruptions.

— [Smart App Control](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/overview)

Smart App Control would categorically block an unsigned helper. The mitigating facts: it requires a
clean install to ever turn on, and Windows turns it off automatically for users whose behaviour looks
like corporate/developer usage. Since mkroamer runs on the target machine, Smart App Control is not
in enforcement mode there.

### 6.4 Defender ASR rules

Two rules in the ASR reference are directly relevant:

> **Block executable files from running unless they meet a prevalence, age, or trusted list criterion**
> — This ASR rule blocks executable files (for example, .exe, .dll, or .scr, from launching).
> Launching untrusted or unknown executable files can be risky […] GUID
> `01443614-cd74-433a-b99e-2ecdc07bfc25`

> **Block untrusted and unsigned processes that run from USB** — This ASR rule prevents unsigned or
> untrusted executable files (for example, .exe, .dll, or .scr) from running from USB removable
> drives, including SD cards. This ASR rule doesn't block the files from being copied from the USB
> drive to disk. It blocks the copied files from running from disk. GUID
> `b2b3f03d-6a65-4f7b-a9c7-1c7ef74a9ba4`

— [ASR rules reference](https://learn.microsoft.com/en-us/defender-endpoint/attack-surface-reduction-rules-reference)

The first is the dangerous one for a freshly built, low-prevalence binary — a brand-new unsigned
release has neither prevalence nor age by construction. The second matters if anyone ever thinks of
shipping the helper on the device itself as a mass-storage payload: read it carefully — copying to
disk does not launder it, the copy is still blocked from running.

Both are Block-mode capable and both surface user notifications, so failures are at least visible
rather than silent (same page, "Alerts and notifications" table).

---

## 7. Console app vs windowed app vs service

**Service: rejected, on two independent documented grounds.**

> **Important** Services cannot directly interact with a user as of Windows Vista.
>
> By default, services use a noninteractive window station and cannot interact with the user.
>
> The **NoInteractiveServices** value defaults to 1, which means that no service is allowed to run
> interactively, regardless of whether it has SERVICE_INTERACTIVE_PROCESS.
>
> All services run in Terminal Services session 0.

— [Interactive Services](https://learn.microsoft.com/en-us/windows/win32/services/interactive-services)

Ground one: a noninteractive window station does not grant `WINSTA_WRITEATTRIBUTES`
([Window Station Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/winstation/window-station-security-and-access-rights)),
so `SetCursorPos` fails its documented precondition. Ground two: each window station has its own
clipboard ([About the Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/about-the-clipboard)),
so a session-0 service reads and writes a clipboard nobody is using. A service would also need admin
rights to install, which the premise excludes.

Microsoft's documented pattern if a service were ever wanted is exactly the split we would have to
build: "Create a separate hidden GUI application and use CreateProcessAsUser to run the application
within the context of the interactive user […] Design the GUI application to communicate with the
service through some method of interprocess communication" (same page). That is strictly more moving
parts than a plain per-user app, for no gain here.

**Console app: workable but wrong.** A console app can open the serial port fine, but §3.2 showed
`SetClipboardData` requires the calling window to be the clipboard owner, and §3.3 showed
`AddClipboardFormatListener` requires an `HWND`. A console app would have to create a hidden window
and pump messages anyway — at which point it is a windowed app that also flashes a console window at
login.

**Windowed app with a hidden (message-only) window: correct.** It satisfies the clipboard owner
requirement, receives `WM_CLIPBOARDUPDATE`, `WM_RENDERFORMAT`, and `WM_RENDERALLFORMATS`, runs in the
user's session and window station, needs no elevation, and can be started by any of the three
mechanisms in §4. This matches standing decision 3 in map #31 (C++ on Windows, reusing mkroamer's
Win32 clipboard implementation).

One consequence worth stating: **the helper needs a message pump on a dedicated thread, and that
thread must stay responsive.** Delayed rendering means Windows will send `WM_RENDERFORMAT`
synchronously when another app pastes; if that thread is blocked on a slow CDC/UART round trip, the
pasting application hangs. The serial I/O must be off the UI thread, and the render handler needs a
timeout that gives up cleanly rather than wedging the paste.

---

## 8. What would force a design change

1. **Cursor placement is not available at the lock screen, during Ctrl+Alt+Del, or while a UAC
   prompt is up** (§2.4) — documented, not inferred. Standing decision 6 needs a failure branch:
   the helper must detect this, abandon rather than queue the placement, and report back so the
   firmware keeps its own fallback anchor. This is a real, routine, self-healing state, not an edge
   case: crossing to a locked machine is an everyday occurrence for this product.
2. **The helper must own a real window** (§3.2, §3.3) — `SetClipboardData` requires it. This rules
   out a pure console helper and makes the message pump a first-class part of the design, with the
   delayed-render handler under a timeout so a slow link cannot hang the app that is pasting.
3. **The clipboard cannot be held open across a transfer** (§3.4) — snapshot and close. Combined with
   `OpenClipboard`'s documented failure when another window holds it, every clipboard operation needs
   bounded retry and a clean error path into the transfer state machine.
4. **Device Installation Restrictions can block the whole composite device, retroactively** (§5.2).
   The "degrade to plain HID switch" fallback does not rescue this case, because HID goes away too.
   The map should say so, and diagnostics should read `setupapi.dev.log` so the user gets an
   actionable message instead of silence.
5. **Autostart should be tri-modal, not single-mode** (§4.4) — Run key, Startup folder, logon task,
   whichever takes, verified by readback. Plus: the helper can arrive tens of seconds after login,
   so the protocol handshake must be helper-initiated at arbitrary times.

## 9. Open questions worth an empirical check on the actual laptop

Small, cheap, and they retire the inferences:

- Does `SetCursorPos` in fact succeed while an elevated window has focus? (§2.3 — inference)
- Does registering a logon-trigger scheduled task succeed as the standard user? (§4.3 — inference)
- Is Purview Endpoint DLP intercepting third-party clipboard reads on this tenant? (§3.5 —
  unverified, and the highest-risk unknown)
- Does the device enumerate and bind `usbser.sys` at all under the machine's device-installation
  policy? (§5.2 — determines whether anything else matters)

---

## Sources

All Microsoft Learn / Microsoft open specifications.

- [SetCursorPos (winuser.h)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos)
- [SendInput (winuser.h)](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
- [Windows Integrity Mechanism Design](https://learn.microsoft.com/en-us/previous-versions/dotnet/articles/bb625963(v=msdn.10))
- [Mandatory Integrity Control](https://learn.microsoft.com/en-us/windows/win32/secauthz/mandatory-integrity-control)
- [Window Station Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/winstation/window-station-security-and-access-rights)
- [Desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)
- [OpenClipboard](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard)
- [GetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclipboarddata)
- [SetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata)
- [GetOpenClipboardWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getopenclipboardwindow)
- [AddClipboardFormatListener](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-addclipboardformatlistener)
- [About the Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/about-the-clipboard)
- [CreateFileA (fileapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)
- [Communications Resources](https://learn.microsoft.com/en-us/windows/win32/devio/communications-resources)
- [USB Serial Driver (Usbser.sys)](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids)
- [DeviceInstallation Policy CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-deviceinstallation)
- [Run and RunOnce Registry Keys](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)
- [KNOWNFOLDERID](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid)
- [Logon Trigger Example (Scripting)](https://learn.microsoft.com/en-us/windows/win32/taskschd/logon-trigger-example--scripting-)
- [ITaskFolder::RegisterTaskDefinition](https://learn.microsoft.com/en-us/windows/win32/api/taskschd/nf-taskschd-itaskfolder-registertaskdefinition)
- [Interactive Services](https://learn.microsoft.com/en-us/windows/win32/services/interactive-services)
- [Microsoft Defender SmartScreen overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/)
- [Available Microsoft Defender SmartScreen settings](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/available-settings)
- [Smart App Control](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/overview)
- [ASR rules reference](https://learn.microsoft.com/en-us/defender-endpoint/attack-surface-reduction-rules-reference)
- [Understand App Control for Business policy design decisions](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/understand-appcontrol-policy-design-decisions)
- [\[MS-FSCC\]: Zone.Identifier Stream Name](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/6e3f7352-d11c-4d76-8c39-2516a9df36e8)
- [Experience Policy CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-experience)
- [Get started with collecting files that match DLP policies from devices](https://learn.microsoft.com/en-us/purview/dlp-copy-matched-items-get-started)
