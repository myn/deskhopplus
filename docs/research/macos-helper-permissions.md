# macOS permissions for the deskhopplus helper

Research for [issue #35](https://github.com/myn/deskhopplus/issues/35) — what a background macOS
helper needs in order to (a) position the mouse cursor and (b) read/write the pasteboard, how those
permissions are requested, and what happens when they are declined.

## Headline

| Capability | Permission needed | If not granted |
| --- | --- | --- |
| `CGWarpMouseCursorPosition` — move the cursor | **None** | n/a — it works |
| `CGEventPost` — synthesise a mouse-moved event | **Accessibility** (TCC) | **Silent no-op.** No error, no return value, cursor does not move |
| Read/write `NSPasteboard` | **None today**; a system alert exists from macOS 15.4 | Contents unreadable when the user denies |
| Open `/dev/cu.*` | **None** — plain POSIX `crw-rw-rw-` | n/a |
| USB CDC device attach | **None on Intel** (Apple-silicon *laptops* only) | n/a on target hardware |

**The design does not need Accessibility.** Standing decision 8 in the
[map](https://github.com/myn/deskhopplus/issues/31) — "macOS needs accessibility permission for
cursor placement" — is **wrong as stated**, provided the helper places the cursor by warping rather
than by posting events. That is the one finding here that should change the plan.

## Test environment

All experiments below were run on the target-class hardware: **Intel Mac (x86_64), macOS 15.7.7
(24G720)**, compiling against the Command Line Tools **macOS 26.2 SDK**, with **unsigned**
binaries (`codesign -dvvv` → "code object is not signed at all"). Header quotations are from that
SDK, at `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`.

Findings are tagged **[doc]** (Apple documentation or SDK header), **[exp]** (measured on this
machine), or **[unverified]**.

---

## 1. Which API positions the cursor, and what it costs permission-wise

Three candidates, all Core Graphics:

**`CGWarpMouseCursorPosition(CGPoint)`** — "Moves the mouse cursor without generating events […]
You can use this function to 'warp' or alter the cursor position without generating or posting an
event." **[doc]**
([Apple](https://developer.apple.com/documentation/coregraphics/cgwarpmousecursorposition(_:)),
`CoreGraphics.framework/Headers/CGRemoteOperation.h:222`). Returns a `CGError`. Available since
macOS 10.0. Neither the reference page nor the header mentions any entitlement, approval, or
sandbox requirement.

**`CGDisplayMoveCursorToPoint(CGDirectDisplayID, CGPoint)`** — the same thing, but in
display-relative coordinates: "Move the mouse cursor to the specified point relative to the origin
(the upper-left corner) of `display`. No events are generated as a result of the move. Points that
lie outside the desktop are clipped to the desktop." **[doc]**
(`CoreGraphics.framework/Headers/CGDirectDisplay.h:412`). Also unmentioned by any permission doc.

**`CGEventPost(kCGHIDEventTap, mouseMovedEvent)`** — "Posts a Quartz event into the event stream at
a specified location." **[doc]**
([Apple](https://developer.apple.com/documentation/coregraphics/cgevent/post(tap:))). This is
*event synthesis*, and it is exactly what the Accessibility TCC service governs. Core Graphics
provides a matching preflight pair, whose header comments are the clearest primary statement that a
permission exists at all:

```c
/* Checks whether the current process already has event synthesizing access */
CG_EXTERN bool CGPreflightPostEventAccess(void) API_AVAILABLE(macos(10.15));

/* Requests event synthesizing access if absent, potentially prompting */
CG_EXTERN bool CGRequestPostEventAccess(void) API_AVAILABLE(macos(10.15));
```

**[doc]** `CoreGraphics.framework/Headers/CGEvent.h:398-408`. (The corresponding Apple reference
pages for
[`CGPreflightPostEventAccess`](https://developer.apple.com/documentation/coregraphics/cgpreflightpostEventaccess())
and
[`CGRequestPostEventAccess`](https://developer.apple.com/documentation/coregraphics/cgrequestpostEventaccess())
are stubs with no abstract or discussion — the header comment is the better source.)

That "Post Event" service is a distinct, user-visible TCC service, listed alongside Accessibility
in Apple's MDM payload documentation **[doc]** ([Privacy Preferences Policy Control payload, Apple
Platform Deployment](https://support.apple.com/guide/deployment/privacy-preferences-policy-control-payload-dep38df53c2a/web)).

### Measured, side by side

A probe binary was run twice from launchd — once with no Accessibility grant, once with one —
calling warp and post against the same starting point:

| | `AXIsProcessTrusted` | `CGPreflightPostEventAccess` | warp moved cursor? | post moved cursor? |
| --- | --- | --- | --- | --- |
| No grant | 0 | 0 | **yes** (`CGError` 0) | **no** |
| Granted | 1 | 1 | yes (`CGError` 0) | yes |

**[exp]** So `CGWarpMouseCursorPosition` succeeds with no TCC approval whatsoever, from an
unsigned binary, under launchd, on an Intel Mac. `CGEventPost` under the same conditions **fails
silently**: `CGEventPost` returns `void`, so there is no error to check — the cursor simply does
not move.

### Recommendation

Use `CGWarpMouseCursorPosition` for entry-point placement. It is permission-free, it is the
cheaper of the two, and it matches the semantics the design wants anyway (teleport the cursor, do
not fabricate motion).

Two consequences worth writing into the spec:

- Warping generates **no events** **[doc]**, so applications tracking `mouseMoved` will not see the
  jump. For placing an entry cursor at a crossing this is the desired behaviour, but anything that
  later wants a hover state to update may need a nudge. **[unverified]** — not tested here.
- `CGAssociateMouseAndMouseCursorPosition(false)` disconnects cursor from hardware mouse and is
  documented as an app-foreground facility **[doc]**
  (`CGRemoteOperation.h:245`). The helper should not need it; noted only so it is not reached for
  by reflex.

**Reading** the cursor position (`CGEventGetLocation(CGEventCreate(NULL))`), which the design needs
for the "report the true cursor position back so the firmware re-anchors" path, also required no
approval **[exp]**.

---

## 2. Accessibility (TCC) for a background agent

Only relevant if the design later needs event synthesis (clicks, keystrokes, scroll). Recorded
because the ticket asks.

### How the check and the prompt work

```
@function AXIsProcessTrustedWithOptions
@abstract Returns whether the current process is a trusted accessibility client.
@param options … KEY: kAXTrustedCheckOptionPrompt
  VALUE: ACFBooleanRef indicating whether the user will be informed if the current process is
  untrusted. This could be used, for example, on application startup to always warn a user if
  accessibility is not enabled for the current process. Prompting occurs asynchronously and does
  not affect the return value.
```

**[doc]**
`ApplicationServices.framework/Frameworks/HIServices.framework/Headers/AXUIElement.h:55-64`.

The two sentences that matter for a background agent: prompting is **asynchronous**, and it **does
not affect the return value**. There is no blocking "wait for the user to decide" API. The helper
calls the check, gets `false`, and must handle that state itself; the user may grant the permission
minutes later.

Apple's user-facing description of the flow: "When a third-party app tries to access and control
your Mac through accessibility features, you receive an alert, and you must specifically grant the
app access to your Mac in Privacy & Security settings" **[doc]** ([Allow accessibility apps to
access your Mac](https://support.apple.com/guide/mac-help/allow-accessibility-apps-to-access-your-mac-mh43185/mac)).
Note the shape: the alert does **not** grant anything. It offers to open System Settings, where the
user flips a switch. Granting always requires a trip to System Settings › Privacy & Security ›
Accessibility.

### Can a UI-less LaunchAgent trigger it?

**[unverified]** — not tested, deliberately. Triggering the prompt would have written a permanent
TCC record on the user's machine for a throwaway binary. What *is* established:

- The prompt is posted by the system, not drawn by the calling process, and the header explicitly
  frames it as a notification rather than a modal the caller waits on **[doc]**. A process with no
  UI therefore has no structural reason to be unable to trigger it.
- **[exp]** A launchd job with no UI and no grant got `AXIsProcessTrusted() == 0` and a TCC row was
  written for it automatically (`auth_value = 0`, denied), with no visible prompt — because the
  probe called only the non-prompting `AXIsProcessTrusted` / `CGPreflight*` variants. Nothing
  surfaced to the user.

The safe design is not to rely on the prompt at all: see §4.

### Does approval survive an update?

**This is the sharp finding for an unsigned project, and it is bad.**

TCC identifies a client either by **bundle ID** or by **file path**, plus a **code requirement**
obtained from `codesign -dr -` **[doc]** ([PPPC
payload](https://support.apple.com/guide/deployment/privacy-preferences-policy-control-payload-dep38df53c2a/web)).
On this machine the `access` table has a `client_type` column distinguishing the two, and a `csreq`
blob **[exp]**.

Measured **[exp]**, all with unsigned binaries:

1. An unsigned binary is recorded by **absolute path** (`client_type = 1`), not bundle ID.
2. A **byte-identical copy at a different path** is a different TCC subject and starts denied.
3. **Rebuilding the binary at the same path revokes the grant.** A granted probe went from
   `AXIsProcessTrusted() == 1` to `0` after a recompile that changed only one `printf` string.
   Same path, same everything else; the grant evaporated.

So for an unsigned helper, **every single rebuild or update makes the user re-grant Accessibility.**
A stable Developer ID signature is what makes a TCC grant survive updates, because the recorded code
requirement then matches across versions. Signing is currently
[out of scope for this map](https://github.com/myn/deskhopplus/issues/31) — which is fine precisely
because §1 says cursor placement does not need the permission. It would not be fine if the design
took a dependency on event synthesis.

### The trap that will waste someone's afternoon

**[exp]** Running the unsigned helper **once from a terminal that itself holds Accessibility**
(iTerm2 and Terminal both do on this machine) caused the system to write an **allowed** TCC row for
the helper binary's path (`auth_value = 2`, `auth_reason = 4`, "system set"). From then on the same
binary was trusted **even when launched from launchd**. A fresh copy at a new path, launched only
from launchd and never from a terminal, stayed denied across repeated runs.

The consequence: **testing the helper from a developer terminal gives a false pass.** It inherits
the terminal's grant, permanently. Any permission testing must be done by launching from launchd
(or by double-clicking the built app), never from the shell where it was compiled.

The exact mechanism was not confirmed against a primary source — `tccd`'s log entries for this are
private-redacted and no Apple document describes the inheritance **[unverified]**. The observed
behaviour is reproducible.

---

## 3. Pasteboard

### Permission today

**None is required.** `NSPasteboard` is documented as "An object that transfers data to and from
the pasteboard server" with no mention of authorization, entitlement, or prompt in its overview
**[doc]** ([Apple](https://developer.apple.com/documentation/appkit/nspasteboard)). Pasteboard does
not appear anywhere in the list of services controllable by the PPPC payload **[doc]**
([Apple](https://support.apple.com/guide/deployment/privacy-preferences-policy-control-payload-dep38df53c2a/web)),
and there is no `kTCCServicePasteboard` in the TCC database on this machine **[exp]**.

**[exp]** An unsigned CLI binary under launchd read `NSPasteboard.generalPasteboard`
(`changeCount`, `types`, and `stringForType:`) successfully, with **no prompt and no user-visible
notification** of any kind.

### The macOS 15.4 change, which is a real risk to this design

macOS 15.4 added `NSPasteboard.accessBehavior` and a per-app pasteboard access alert. From the
header **[doc]** (`AppKit.framework/Headers/NSPasteboard.h:53-70`, `API_AVAILABLE(macos(15.4))`):

> `NSPasteboardAccessBehaviorDefault` — The default behavior for the General pasteboard is **to ask
> upon programmatic access**. All other pasteboards default to always allow access. If an app has
> never triggered a pasteboard access alert, its General pasteboard will report `.default`
> behavior. Such an app is not shown in the corresponding System Settings pane. Once programmatic
> pasteboard access triggers the first pasteboard access alert, the state automatically changes to
> `.ask`. At this point the app starts being shown in System Settings, where the user can toggle
> the behavior between `.ask`, `.alwaysAllow`, and `.alwaysDeny`.

> `NSPasteboardAccessBehaviorAsk` — The system will notify the user and ask for permission before
> granting pasteboard access. However, access that is both **user originated and paste related**
> will always be allowed, and will not result in a notification.

> `NSPasteboardAccessBehaviorAlwaysDeny` — The system will automatically deny all pasteboard access,
> without notifying the user.

A background helper polling the general pasteboard is the textbook definition of "programmatic
access" and is not "user originated". On paper this design is squarely in the alert's path.

**[exp]** It did not fire on this machine — macOS 15.7.7, unsigned, unbundled CLI tool, linked
against the 26.2 SDK, run from launchd. **Do not read that as safety.** The alert is plausibly
gated on the client being a real, identifiable app bundle, which the shipped helper will be; that
gating is **[unverified]**. Reproducing this with a proper `.app` bundle should be the *first*
experiment of the helper implementation ticket, before any clipboard protocol work is committed to.

Two mitigations exist in the same API, both **[doc]** (`NSPasteboard.h:244-278`):

- `detectPatternsForPatterns:completionHandler:` — "Determines whether the first pasteboard item
  matches the specified patterns, **without notifying the person using the app**. This method only
  gives an indication of whether the first pasteboard item matches a particular pattern, and
  doesn't allow the app to access the item's contents. As a result, the system doesn't notify the
  person using the app about reading the contents of the pasteboard." Useful for change detection
  without a full read, though the available patterns (URL, phone number, money amount, …) are a
  poor fit for "did the clipboard change".
- `changeCount` — reading it did not trigger anything **[exp]** and is the natural polling primitive
  regardless. Whether `changeCount` alone is exempt from the alert on a bundled app is
  **[unverified]**.

`NSPasteboardContentsCurrentHostOnly` is also worth knowing about: it "specifies that the pasteboard
contents should not be available to other devices" **[doc]** (`NSPasteboard.h`, macOS 10.12+) —
i.e. it suppresses Universal Clipboard. Relevant because deskhopplus is deliberately a
no-radio project, and a helper writing the received clipboard without this flag would push it back
out over Handoff.

---

## 4. Detecting a missing approval, and guiding the user

Do not infer permission from an operation failing — `CGEventPost` returns `void` and fails silently
**[exp]**. Ask directly, using the non-prompting checks:

- `AXIsProcessTrusted()` / `AXIsProcessTrustedWithOptions(NULL)` — returns whether this process is a
  trusted accessibility client, with no prompt when the options dictionary is `NULL` or the prompt
  key is false **[doc]** (`AXUIElement.h:55-64`).
- `CGPreflightPostEventAccess()` — "Checks whether the current process already has event
  synthesizing access" **[doc]** (`CGEvent.h:405`).
- `CGPreflightListenEventAccess()` — the equivalent for event *listening* (the "Input Monitoring"
  pane) **[doc]** (`CGEvent.h:399`), should the helper ever need to observe input.
- `NSPasteboard.accessBehavior` (macOS 15.4+) — reports `.default` / `.ask` / `.alwaysAllow` /
  `.alwaysDeny` **[doc]** (`NSPasteboard.h:170`). Note `.default` means "never yet prompted", not
  "allowed", so it cannot be treated as a green light.

**[exp]** All three CG/AX checks returned the correct value from a UI-less launchd job, and the
Accessibility grant and `CGPreflightPostEventAccess` moved together — granting Accessibility flipped
the post-event preflight to true.

Guiding the user: `AXIsProcessTrustedWithOptions` with `kAXTrustedCheckOptionPrompt: true` posts the
system alert, which offers to open System Settings **[doc]**; the alert itself grants nothing and the
call returns immediately with the old value **[doc]**. Because prompting "occurs asynchronously and
does not affect the return value", a helper that wants to react to the grant must poll the preflight
rather than await a result.

The `x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility` URL commonly used
to deep-link the settings pane is **[unverified]** — no Apple documentation for it was found, only
secondary sources. Apple's documented path is the alert's own "Open System Settings" button, plus
the manual route: System Settings › Privacy & Security › Accessibility, where the user can also add
an app by hand via the "+" button **[doc]**
([Apple](https://support.apple.com/guide/mac-help/allow-accessibility-apps-to-access-your-mac-mh43185/mac)).

Since §1 removes the Accessibility dependency for the core feature, the helper's degraded state
should be scoped narrowly: per standing decision 7, with no permission and no helper the device
stays a working HID switch, and clipboard is the only thing that can be lost.

---

## 5. Unsigned and ad-hoc-signed binaries

- **Nothing in §1 or §3 requires a signature.** Cursor warping and pasteboard access both worked
  from a completely unsigned binary **[exp]**.
- **Unsigned binaries do get TCC identities**, keyed on absolute path **[exp]**, consistent with
  Apple documenting file path as a valid PPPC identifier type **[doc]**.
- **Grants do not survive a rebuild** (§2) **[exp]**. Ad-hoc signing (`codesign -s -`) does not fix
  this: an ad-hoc signature has a cdhash but no stable signing identity, so each build is a
  different code object. Not separately tested — **[unverified]**, inferred from the mechanism and
  from the observed content-sensitivity of the grant.
- **Gatekeeper is a separate axis** from TCC and was not exercised here, since binaries built
  locally carry no quarantine attribute. A downloaded unsigned helper is a distribution problem,
  and distribution is
  [out of scope for this map](https://github.com/myn/deskhopplus/issues/31). **[unverified]**.
- **App Sandbox is not in play.** The device-access entitlements (`com.apple.security.device.usb`
  and friends) are App Sandbox entitlements **[doc]**
  ([Security entitlements](https://developer.apple.com/documentation/bundleresources/security-entitlements)),
  and a directly-distributed helper is not sandboxed. No entitlement is needed for the serial port.
  There is no documented `com.apple.security.device.serial` on that page **[doc]**.

---

## 6. Serial port access

**No approval of any kind.** **[exp]** On this machine the call-out device nodes are
`crw-rw-rw- root wheel`, world-readable and world-writable; the logged-in user is in no special
group. Opening `/dev/cu.*` is ordinary POSIX file access. Serial ports appear in no TCC service list
**[exp]** and in no PPPC service list **[doc]**.

The one thing to get right is the `cu.` versus `tty.` choice, which is a blocking-behaviour question
rather than a permissions one, and belongs to the CDC ticket.

---

## 7. Intel-specific behaviour

**One thing differs, and it differs in the project's favour.**

The "Allow accessory to connect" alert — the one that would otherwise appear every time the
deskhopplus device is plugged in — is **restricted to Mac laptops with Apple silicon**. Apple:
"When you use a new or unknown USB accessory, Thunderbolt accessory, or SD card with your Mac laptop
with Apple silicon, you get an alert that asks you to allow the accessory to connect" **[doc]**
([Allow USB and other accessories to connect to your
Mac](https://support.apple.com/en-us/102282)). Denying it means "your Mac won't recognize the
accessory or give it access to the data on your Mac", and it re-asks on every reconnect **[doc]**.

On the Intel target this does not apply: the USB CDC device enumerates with no user interaction.
Worth recording as a hazard for Apple-silicon laptop users later, and it interacts unpleasantly with
standing decision 1 — config mode reboots the device under a **different USB identity**, which on an
Apple-silicon laptop would read as a new, unknown accessory and could re-prompt on every config-mode
round trip. **[unverified]** — not testable on this hardware.

Nothing else was Intel-specific. TCC, Accessibility, pasteboard behaviour, and serial access all
behaved as the architecture-neutral documentation describes.

---

## What this changes

1. **Drop the Accessibility dependency for cursor placement.** Standing decision 8 should be
   amended: warping needs no permission. This removes the largest UX obstacle in the macOS helper
   and means an unsigned helper is viable for the cursor half of the channel.
2. **The clipboard half is the one carrying permission risk now**, via the macOS 15.4 pasteboard
   access alert — the reverse of the assumption the map was built on.
3. **Never permission-test from the build terminal.** It silently inherits the terminal's grants.

## Loose ends left for implementation

- Reproduce §3 with a real `.app` bundle on macOS 15.4+ to find out whether the pasteboard alert
  fires for a background helper. This gates the clipboard design.
- Confirm whether a warped cursor needs a follow-up nudge for hover states to update.
- Confirm ad-hoc signing behaves as inferred in §5.

## A note on machine state

Two TCC rows for throwaway probe binaries under
`/private/tmp/claude-501/…/scratchpad/` remain in `/Library/Application Support/com.apple.TCC/TCC.db`
(one allowed, one denied). Both binaries have been deleted. The rows are inert — TCC validates the
recorded code identity, and the grant was already shown to lapse when the binary changed — but they
are residue from this research and can be ignored or cleaned up manually.

## Sources

Apple documentation:

- [`CGWarpMouseCursorPosition(_:)`](https://developer.apple.com/documentation/coregraphics/cgwarpmousecursorposition(_:))
- [`CGEvent.post(tap:)`](https://developer.apple.com/documentation/coregraphics/cgevent/post(tap:))
- [`CGPreflightPostEventAccess()`](https://developer.apple.com/documentation/coregraphics/cgpreflightpostEventaccess()) (stub)
- [`CGRequestPostEventAccess()`](https://developer.apple.com/documentation/coregraphics/cgrequestpostEventaccess()) (stub)
- [`NSPasteboard`](https://developer.apple.com/documentation/appkit/nspasteboard)
- [Security entitlements](https://developer.apple.com/documentation/bundleresources/security-entitlements)
- [`SMAppService`](https://developer.apple.com/documentation/servicemanagement/smappservice) — for registering the LaunchAgent on macOS 13+
- [Privacy Preferences Policy Control payload — Apple Platform Deployment](https://support.apple.com/guide/deployment/privacy-preferences-policy-control-payload-dep38df53c2a/web)
- [Allow accessibility apps to access your Mac](https://support.apple.com/guide/mac-help/allow-accessibility-apps-to-access-your-mac-mh43185/mac)
- [Allow USB and other accessories to connect to your Mac](https://support.apple.com/en-us/102282)

SDK headers (macOS 26.2 SDK, `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`):

- `System/Library/Frameworks/CoreGraphics.framework/Headers/CGRemoteOperation.h`
- `System/Library/Frameworks/CoreGraphics.framework/Headers/CGDirectDisplay.h`
- `System/Library/Frameworks/CoreGraphics.framework/Headers/CGEvent.h`
- `System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/Headers/AXUIElement.h`
- `System/Library/Frameworks/AppKit.framework/Headers/NSPasteboard.h`
