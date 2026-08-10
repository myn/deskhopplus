# Vendor HID on macOS: does `IOHIDDeviceOpen` need Input Monitoring?

Research for [issue #59](https://github.com/myn/deskhopplus/issues/59) — the CDC-serial vs vendor-HID
transport decision. It exists to settle one of the two unknowns that #59's own correction note says
block the choice, and it tests whether the zero-permission finding locked by
[issue #35](https://github.com/myn/deskhopplus/issues/35) survives a move off CDC. The outcome feeds
back to the [map](https://github.com/myn/deskhopplus/issues/31), where standing decisions 1 and 2 —
and the exclusive-ownership control locked by
[#34](https://github.com/myn/deskhopplus/issues/34) — depend on it.

## The question

**Does `IOHIDDeviceOpen` on a vendor-defined usage page, from an unsigned, unbundled, background
helper on current macOS, require Input Monitoring (TCC) approval — or are vendor-defined usage pages
exempt?**

Nothing broader. Cursor placement, clipboard and serial access are settled in
[`macos-helper-permissions.md`](./macos-helper-permissions.md) and are not revisited here.

## Headline

| Question | Answer |
| --- | --- |
| Does a **vendor-page** (`0xFF00`+) HID device require Input Monitoring? | **No.** The kernel never flags it, and the userspace open path never consults TCC |
| Which devices *are* gated? | Exactly three usage pairs: **GenericDesktop/Keyboard**, **GenericDesktop/Mouse**, **Digitizer/TouchPad** |
| Where is that decided? | `IOHIDDevice::handleStart` sets `RequiresTCCAuthorization`; `IOHIDDeviceClass initConnect` reads it and **short-circuits to granted when absent** |
| Measured, unsigned, unbundled, under launchd, with **no** grant | Vendor-page opens returned **`kIOReturnSuccess`** (7 devices, 2 vendor pages) |
| Did a TCC record get written? | **No.** No `kTCCServiceListenEvent` row exists on this machine at all |
| Exclusive open (`kIOHIDOptionsTypeSeizeDevice`)? | **Also succeeds**, permission-free |
| Failure mode when the gate *does* apply | `kIOReturnNotPermitted` (`0xE00002E2`), plus `TCC deny IOHIDDeviceOpen` in the log — distinguishable |
| Version-dependent? | **No.** The gate's source is byte-identical from Catalina (Jan 2020) to macOS 26 (Jun 2026) |
| Does unsigned / unbundled / background matter? | **Not for vendor pages** — TCC is never reached, so there is no grant to lose on rebuild |

**#35's "serial access needs no permission" advantage survives the move to vendor HID.** Vendor HID
does *not* acquire a TCC requirement that CDC lacks. Both are permission-free on macOS.

**But there is one firmware constraint that must be honoured, or the answer flips** — see §4. The
vendor collection must live in its **own USB HID interface**, not be appended to the keyboard or
mouse interface's report descriptor.

## Sourcing note

Apple's *prose* documentation is, as expected, almost silent: it describes Input Monitoring as
covering "your keyboard, mouse, or trackpad" and never mentions HID usage pages. That alone could not
settle this question.

What makes this document unusually well-sourced anyway is that **the gate is implemented in code
Apple publishes**. `IOHIDFamily` is released as open source at
[github.com/apple-oss-distributions/IOHIDFamily](https://github.com/apple-oss-distributions/IOHIDFamily),
and both halves of the decision — the kernel-side flag and the userspace-side check — are in it, in
full. That is a primary source in the strongest sense: it is not a description of the behaviour, it
*is* the behaviour. Findings from it are tagged **[src]**.

Tags used: **[doc]** Apple documentation or SDK header · **[src]** Apple-published `IOHIDFamily`
source · **[exp]** measured on this machine · **[INFERENCE]** reasoned from the above rather than
stated · **[UNVERIFIED]** nothing primary found.

Secondary sources (Karabiner-Elements, hidapi, Stack Overflow, blog posts) are cited **only** in §7,
where they are used to explain why the folklore on this topic is contradictory. They are labelled
secondary and none of them carries a finding.

## Test environment

Same target-class hardware as #35: **Intel Mac (x86_64), macOS 15.7.7 (24G720)**, compiling against
the Command Line Tools **macOS 26.2 SDK**, binaries **unsigned** (`codesign -dvvv` → "code object is
not signed at all") and **unbundled** (a bare Mach-O, no `.app`). All open tests were run **from
launchd**, never from the build terminal — #35 established that a terminal holding a TCC grant leaks
it permanently to binaries launched from it, which would have produced a false pass.

Source inspected at tag `IOHIDFamily-2238.100.59` (the repository default) plus the six older tags
listed in §5.

---

## 1. Where the gate actually is, in Apple's own source

The whole question resolves to seven lines in `IOHIDDevice::handleStart`. **[src]**
([`IOHIDFamily/IOHIDDevice.cpp:526-531`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDDevice.cpp#L526-L531)):

```cpp
    if (conformsTo(kHIDPage_GenericDesktop, kHIDUsage_GD_Keyboard) ||
        conformsTo(kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse) ||
        conformsTo(kHIDPage_Digitizer, kHIDUsage_Dig_TouchPad)) {
        setProperty(kIOHIDRequiresTCCAuthorizationKey, kOSBooleanTrue);
    }
```

That is the entire population of TCC-gated HID devices. Resolving the constants **[doc]**
(`IOHIDUsageTables.h`):

| Constant | Value | Meaning |
| --- | --- | --- |
| `kHIDPage_GenericDesktop` | `0x01` | Generic Desktop page |
| `kHIDUsage_GD_Keyboard` | `0x06` | keyboard |
| `kHIDUsage_GD_Mouse` | `0x02` | mouse |
| `kHIDPage_Digitizer` | `0x0D` | Digitizer page |
| `kHIDUsage_Dig_TouchPad` | `0x05` | touchpad |
| `kHIDPage_VendorDefinedStart` | `0xFF00` | first vendor-defined page |

A vendor-defined page is `0xFF00` or above and cannot equal `0x01` or `0x0D`, so **no vendor-defined
device can ever satisfy this condition.** [src]

The userspace half closes the loop. `IOHIDDeviceOpen` funnels through
`-[IOHIDDeviceClass initConnect]` **[src]**
([`IOHIDLib/IOHIDDeviceClass.m:510-535`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDLib/IOHIDDeviceClass.m#L510-L535)):

```objc
        NSNumber *tcc = CFBridgingRelease(IORegistryEntryCreateCFProperty(
                                    _service,
                                    CFSTR(kIOHIDRequiresTCCAuthorizationKey),
                                    kCFAllocatorDefault,
                                    0));

        if (tcc && [tcc isEqual:@YES]) {
            _tccGranted = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
            [self logProtectedServiceEvent];
        } else {
            _tccGranted = true;
        }
        …
    if (!_tccGranted) {
        HIDLogError("0x%llx: TCC deny IOHIDDeviceOpen", regID);
    }
    __Require_Action(_tccGranted, exit, {
        ret = kIOReturnNotPermitted;
```

Read the `else` branch carefully: when the property is **absent**, `_tccGranted` is set to `true`
**unconditionally, without consulting TCC at all**. There is no fallback path, no secondary check, no
policy daemon consulted. For an unflagged device the TCC subsystem is simply not in the picture.

**`RequiresTCCAuthorization` appears exactly twice in the entire published tree** — once at the set
site above, once at the read site above. **[exp]** There is no third place where something else could
set it for a vendor device.

This is the answer to the whole question, and it is not an inference.

## 2. What Apple's prose documentation says — and does not

The SDK header is the most explicit Apple *documentation* on the subject **[doc]**
(`IOKit.framework/Headers/hidsystem/IOHIDLib.h:155-159`):

> `kIOHIDRequestTypeListenEvent`
> Request to listen to event through IOHIDManager/IOHIDDevice API. Access must be granted by the user
> to use this API. If you do not request access through the IOHIDRequestAccess call, the request will
> be made on the process's behalf in IOHIDManagerOpen/IOHIDDeviceOpen calls.

**Note what this does *not* say.** It states the requirement in blanket terms — "the
IOHIDManager/IOHIDDevice API" — with **no usage-page qualification whatsoever**. Read alone, this
header is the strongest available argument that *all* HID access is gated, and it is wrong, or at
least radically incomplete. It describes the flagged case as though it were the only case.

This is worth flagging because it is the trap the folklore falls into (§7). **A reader with access to
only Apple's headers and prose would reasonably conclude that vendor pages are gated.** Only the
source shows otherwise.

Apple's user-facing documentation is consistent with the source but far too vague to settle anything
**[doc]** ([Control access to input monitoring on
Mac](https://support.apple.com/guide/mac-help/control-access-input-monitoring-mac-mchl4cedafb6/mac)):

> Some apps can monitor your keyboard, mouse, or trackpad even when you're using other apps.

"Keyboard, mouse, or trackpad" maps one-to-one onto the three usage pairs in §1 — a pleasing
corroboration between Apple's prose and Apple's code, but the prose never mentions HID, usage pages,
or `IOHIDDeviceOpen`, so it could not have established the boundary by itself.

`IOHIDDeviceOpen`'s own reference documentation says nothing about permissions **[doc]**
(`IOKit.framework/Headers/hid/IOHIDDevice.h:87-97`):

> Opens a HID device for communication. Before the client can issue commands that change the state of
> the device, it must have succeeded in opening the device. This establishes a link between the
> client's task and the actual device. To establish an exclusive link use the
> `kIOHIDOptionsTypeSeizeDevice` option. […] Returns `kIOReturnSuccess` if successful.

No mention of TCC, Input Monitoring, entitlements or authorization. The `developer.apple.com`
reference page for the symbol returns HTTP 404 **[exp]** — there is no published web reference for
it.

## 3. Measured: it opens, with nothing granted

A probe (`hidprobe.c`, reproduced in §8) enumerated every HID device, printed the kernel's
`RequiresTCCAuthorization` property, and called `IOHIDDeviceOpen` on **only the unflagged ones** —
deliberately skipping flagged devices, since opening one would call `IOHIDRequestAccess` and write a
permanent TCC record on the user's machine.

Run from **launchd**, unsigned, unbundled, no UI, with nothing granted **[exp]**:

```
IOHIDCheckAccess(ListenEvent) = 2  (0=Granted 1=Denied 2=Unknown)

Product                                   VID    PID uPage usage  TCCflag -> IOHIDDeviceOpen
TouchBarUserDevice                       1452  34304     1     6  YES     -> skipped (would prompt)
Touch Bar Display                        1452  33538    13     5  YES     -> skipped (would prompt)
DeskHop Switch                           4617  49152     1     2  YES     -> skipped (would prompt)
Apple Internal Keyboard / Trackpad       1452    636     1     2  YES     -> skipped (would prompt)
Keyboard Backlight                       1452    636 65280    15  no      -> 0x00000000 SUCCESS
Ambient Light Sensor                     1452  33378    32    65  no      -> 0x00000000 SUCCESS
DeskHop Switch                           4617  49152     1     6  YES     -> skipped (would prompt)
Apple Internal Keyboard / Trackpad       1452    636     1     6  YES     -> skipped (would prompt)
Apple Internal Keyboard / Trackpad       1452    636 65280     3  no      -> 0x00000000 SUCCESS
Headset                                  1452  33027    12     1  no      -> 0x00000000 SUCCESS
Touch Bar Backlight                      1452  33026 65298     1  no      -> 0x00000000 SUCCESS
Apple Internal Keyboard / Trackpad       1452    636 65280    11  no      -> 0x00000000 SUCCESS
Apple Internal Keyboard / Trackpad       1452    636 65280    13  no      -> 0x00000000 SUCCESS
```

`65280` is `0xFF00`; `65298` is `0xFF12`. **Five vendor-page devices across two distinct vendor pages
opened successfully.**

The first line is what makes this airtight rather than a possible false pass:
`IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)` returned **`2` = `kIOHIDAccessTypeUnknown`** **[exp]**
— the process held no Input Monitoring grant, and no TCC decision existed for it either way. The
opens succeeded *while unauthorised*.

Corroborating, after the run **[exp]**:

```
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service,client,auth_value from access
   where client like '%hidprobe%' or service='kTCCServiceListenEvent';"
→ (no rows)
```

**No row was created, and this machine has no `kTCCServiceListenEvent` row for any client at all.**
Contrast #35, where merely calling the *non-prompting* `AXIsProcessTrusted` caused a denied
Accessibility row to be written. Here nothing was written because nothing in the code path ever
reached TCC — exactly as §1 predicts.

### Exclusive open is also free

Rebuilt with `kIOHIDOptionsTypeSeizeDevice` and re-run under launchd, every vendor-page device again
returned `kIOReturnSuccess` **[exp]**. So macOS offers vendor HID the same exclusive-ownership
primitive that #34 relies on for the CDC port, at the same price of nothing.

This does **not** rescue #59's Windows-side objection, which is a separate question about Windows
share modes and is out of scope here. It does mean the macOS half of #34's exclusive-access control
survives a move to vendor HID. **[exp]**

## 4. The constraint that would flip the answer: `conformsTo` is not about the *primary* usage

This is the one genuinely dangerous detail, and it is a firmware constraint rather than a helper one.

`conformsTo` does **not** test the device's primary usage. It iterates the device's **complete**
usage-pair list **[src]**
([`IOHIDDevice.cpp:2567-2600`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDDevice.cpp#L2567)):

```cpp
bool IOHIDDevice::conformsTo(UInt32 usagePage, UInt32 usage)
{
    …
    usagePairs = newDeviceUsagePairs();
    …
    for (unsigned int i = 0; i < usagePairs->getCount(); i++) {
```

So **one keyboard or mouse collection anywhere in an `IOHIDDevice`'s report descriptor flags the
whole `IOHIDDevice`** — including any vendor collection sharing it.

This machine provides a clean natural experiment. The **same physical device**, "Apple Internal
Keyboard / Trackpad", presents several `IOHIDDevice` nodes **[exp]** (`ioreg -c IOHIDDevice -r -l`):

| `DeviceUsagePairs` | `RequiresTCCAuthorization` |
| --- | --- |
| `{1/6 keyboard}, {12/1}, {0xFF00/6}, {0xFF00/15}` | **Yes** |
| `{1/2 mouse}, {1/1}, {13/5 touchpad}, {0xFF00/12}` | **Yes** |
| `{0xFF00/11}` | *(absent)* |
| `{0xFF00/13}` | *(absent)* |

The first two rows contain vendor collections (`0xFF00/6`, `0xFF00/15`, `0xFF00/12`) that are gated —
not because they are vendor collections, but because they share an `IOHIDDevice` with a keyboard or
mouse. The last two rows are pure vendor nodes and are ungated. **The boundary is the `IOHIDDevice`,
not the usage page of the collection you happen to want.**

The current DeskHop firmware shows the same pattern **[exp]**:

| Node | `DeviceUsagePairs` | Flag |
| --- | --- | --- |
| interface 1 | `{1/6}, {1/2}, {1/1}, {12/1}, {1/128}` | **Yes** |
| interface 2 | `{1/2}, {1/1}` | **Yes** |

Both existing normal-mode HID interfaces are TCC-flagged today. That is harmless — nothing opens
them — but it sets the trap for the transport change.

**[INFERENCE]** macOS creates one `IOHIDDevice` per USB HID *interface*, so a vendor collection
declared as its own interface gets its own unflagged `IOHIDDevice`. This is inference from the
observed one-node-per-interface structure above rather than a documented guarantee, but the two
DeskHop nodes matching the two existing HID interfaces, and each carrying only its own interface's
collections, is strong evidence for it.

**Concrete firmware constraint for #59:** declare the vendor collection as a **separate USB HID
interface** with its own report descriptor containing *only* vendor-page collections. Appending a
vendor collection to the existing keyboard interface's descriptor — the cheaper-looking change —
would put the vendor collection inside a TCC-flagged `IOHIDDevice` and **acquire the exact Input
Monitoring requirement this document rules out.** Verify after the descriptor change by confirming
the new node has no `RequiresTCCAuthorization` property (§8, check 1).

## 5. Version dependence: none, across six years

The gate's source was fetched at seven release tags spanning Catalina to macOS 26 and the seven lines
from §1 are **byte-identical in all of them** **[src]** [exp]:

| Tag | Tag date | Era **[INFERENCE]** | Gate text |
| --- | --- | --- | --- |
| `IOHIDFamily-1446.11.12` | 2020-01-31 | Catalina 10.15 | identical |
| `IOHIDFamily-1633.40.25` | 2020-12-22 | Big Sur 11 | identical |
| `IOHIDFamily-1787.40.10` | 2022-02-04 | Monterey 12 | identical |
| `IOHIDFamily-1915.140.3` | 2023-08-09 | Ventura 13 | identical |
| `IOHIDFamily-2031.100.16` | 2024-03-25 | Sonoma 14 | identical |
| `IOHIDFamily-2115.140.4` | 2025-08-15 | Sequoia 15 | identical |
| `IOHIDFamily-2238.120.5` | 2026-06-05 | macOS 26 | identical |

The userspace `else { _tccGranted = true; }` short-circuit is likewise unchanged across the same span
**[src]**. Tag dates are facts from the GitHub API; the macOS-version column is **[INFERENCE]** from
those dates, since Apple does not publish a project-version-to-OS-release table.

Catalina is where Input Monitoring was introduced — `IOHIDCheckAccess` and `IOHIDRequestAccess` are
both `__OSX_AVAILABLE(10.15)` **[doc]** (`IOHIDLib.h:193, 214`) — and the usage-pair list has not
been touched since. **[INFERENCE]** Six years of stability across seven releases makes a near-term
change unlikely, though nothing prevents Apple from widening the list in a future release; this is
worth a re-check at each major macOS version, and check 1 in §8 is a five-second re-check.

## 6. The other two gates, and why neither applies

Two further access controls exist on the `IOHIDDeviceOpen` path. Both are worth knowing about; **and
neither one can affect this project.**

**`kIOHIDProtectedAccessKey` — the one gate that *is* keyed on a vendor page.** **[src]**
([`IOHIDDevice.cpp:1703-1770`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDDevice.cpp#L1703)):

```cpp
    static const UsagePair ProtectedAccessUsagePairs[] = {
        {kHIDPage_AppleVendor, 0x004B},
        {kHIDPage_AppleVendor, 0x004D}
    };
    static const AppleVendorID AppleVendorIDs[] = {
        {kIOHIDTransportUSBValue, 1452},
        {kIOHIDTransportBluetoothValue, 76},
        {kIOHIDTransportBluetoothLowEnergyValue, 76}
    };
```

The device must match **both** an Apple vendor ID (`1452` over USB, `76` over Bluetooth) **and** one
of two Apple-vendor usages. Enforcement is an entitlement check, not TCC — a client without the
entitlement is refused at `IOHIDLibUserClient::start` with "Missing entitlement to access protected
service" **[src]**
([`IOHIDLibUserClient.cpp:412-414`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDLibUserClient.cpp#L412)).

deskhopplus's vendor ID is `4617` **[exp]**, so this gate is unreachable for our hardware regardless
of which usages we choose. Recorded because it is the only place a vendor page appears in an access
check, and someone re-deriving this later will find it and needs to know why it does not bite.
(`kHIDPage_AppleVendor` is not defined in the published tree; its value is widely given as `0xFF00`
**[UNVERIFIED]** — immaterial, since the vendor-ID test fails first.)

**`kIOHIDDeviceAccessEntitlementKey`** — a device may name an entitlement string that clients must
hold **[src]** ([`IOHIDLibUserClient.cpp:417`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDLibUserClient.cpp#L417)). This is a property
*of the device's IOKit personality*, set by a matching kernel driver. Our device is claimed by the
generic HID driver and sets no such property **[exp]** — no `DeviceAccessEntitlement` key appears on
any node in `ioreg` on this machine.

## 7. Why the folklore says the opposite

Public discussion of this gate is consistently wrong in the same direction — "HID access needs Input
Monitoring, full stop". The mechanism behind the error is worth recording, because it is a real trap
for the implementation and not merely other people being sloppy.

**`IOHIDManagerOpen` opens every matched device.** The header names it in the same breath as
`IOHIDDeviceOpen` **[doc]** (`IOHIDLib.h:159`), and **[INFERENCE]** it iterates the manager's matched
set calling the per-device open on each. So a manager created with a **`NULL` matching dictionary**,
or any matching dictionary broad enough to catch a keyboard, will open a keyboard, hit the flagged
path, and trigger the prompt — *even if the caller only ever wanted its own vendor device*.

`hidapi` initialises exactly that way, and `hidapi`-based reports of Input Monitoring prompts on
vendor devices are the single largest source of the folklore. **(Secondary:
[hidapi discussion #489](https://github.com/libusb/hidapi/discussions/489),
[John's Blog, "macOS IOHIDManager Permission Issue"](https://nachtimwald.com/2020/11/08/macos-iohidmanager-permission-issue/).)**
These reports are accurate about what their authors observed and wrong about the cause: the prompt
came from the keyboards their enumeration swept up, not from their vendor device.

Karabiner-Elements is genuinely and unavoidably gated — it is a keyboard remapper, so it opens
GenericDesktop/Keyboard devices by design. **(Secondary:
[Karabiner-Elements documentation](https://karabiner-elements.pqrs.org/docs/getting-started/installation/).)** Its
requirement is real and tells us nothing about vendor pages.

**Implementation consequence, and it is a sharp one:** the helper must match **narrowly** — on vendor
ID, product ID and vendor usage page — and must never call `IOHIDManagerOpen` with broad matching. A
lazy `IOHIDManagerSetDeviceMatching(manager, NULL)` followed by `IOHIDManagerOpen` would prompt the
user for Input Monitoring on first run and make this entire document's conclusion moot in practice.
Prefer `IOHIDManagerCopyDevices` + per-device `IOHIDDeviceOpen` on the one device you want, which is
what the probe in §8 does.

**[UNVERIFIED]** — the `IOHIDManagerOpen` implementation is not in the published `IOHIDFamily`
tree, so its per-device iteration is inferred from the header text and from the secondary reports
being consistent with it. Not measured here, because measuring it would mean deliberately opening a
keyboard and writing a TCC record. Check 4 in §8 measures it safely if the answer is ever needed.

## 8. Concrete cheap empirical checks

Four checks, in increasing cost. Checks 1–3 are free and side-effect-free. Check 4 writes a permanent
TCC record and should be run only deliberately.

### Check 1 — five seconds, no code: read the kernel's own verdict

The kernel publishes its decision as an IOKit property. Just read it:

```sh
ioreg -c IOHIDDevice -r -l | grep -E '"(Product|VendorID|PrimaryUsagePage|PrimaryUsage|DeviceUsagePairs|RequiresTCCAuthorization)"'
```

A device with **no** `RequiresTCCAuthorization` line needs no Input Monitoring. This is the check to
run against the deskhopplus device **immediately after** the descriptor change lands — it confirms
§4's constraint was honoured before any helper code is written.

### Check 2 — the probe used in §3

`hidprobe.c`. It enumerates without opening, prints each device's flag, and opens **only** the
unflagged ones, so it cannot trigger a prompt.

```c
// clang -o hidprobe hidprobe.c -framework IOKit -framework CoreFoundation
#include <stdio.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hidsystem/IOHIDLib.h>

static long num(IOHIDDeviceRef d, CFStringRef k) {
    long v = -1;
    CFTypeRef r = IOHIDDeviceGetProperty(d, k);
    if (r && CFGetTypeID(r) == CFNumberGetTypeID()) CFNumberGetValue(r, kCFNumberLongType, &v);
    return v;
}

int main(void) {
    IOHIDAccessType a = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    printf("IOHIDCheckAccess(ListenEvent) = %d  (0=Granted 1=Denied 2=Unknown)\n\n", (int)a);

    // NULL matching: enumerate everything, but DO NOT call IOHIDManagerOpen —
    // that would open every device including keyboards and trip the TCC gate.
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(m, NULL);
    CFSetRef set = IOHIDManagerCopyDevices(m);
    if (!set) { printf("no devices\n"); return 1; }

    CFIndex n = CFSetGetCount(set);
    IOHIDDeviceRef *devs = calloc(n, sizeof(IOHIDDeviceRef));
    CFSetGetValues(set, (const void **)devs);

    printf("%-38s %6s %6s %5s %5s  %s\n", "Product", "VID", "PID", "uPage", "usage",
           "TCCflag -> IOHIDDeviceOpen");
    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef d = devs[i];
        long up = num(d, CFSTR(kIOHIDPrimaryUsagePageKey));
        long u  = num(d, CFSTR(kIOHIDPrimaryUsageKey));
        CFTypeRef tcc = IOHIDDeviceGetProperty(d, CFSTR("RequiresTCCAuthorization"));
        int flagged = (tcc == kCFBooleanTrue);

        char name[64] = "?";
        CFStringRef p = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductKey));
        if (p && CFGetTypeID(p) == CFStringGetTypeID())
            CFStringGetCString(p, name, sizeof name, kCFStringEncodingUTF8);

        printf("%-38.38s %6ld %6ld %5ld %5ld  %-7s", name, num(d, CFSTR(kIOHIDVendorIDKey)),
               num(d, CFSTR(kIOHIDProductIDKey)), up, u, flagged ? "YES" : "no");

        // Only open devices the kernel did NOT flag. Opening a flagged one would
        // call IOHIDRequestAccess and write a permanent TCC record.
        if (flagged) {
            printf(" -> skipped (would prompt)\n");
        } else {
            IOReturn r = IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone);
            printf(" -> 0x%08x %s\n", r, r == kIOReturnSuccess ? "SUCCESS" :
                   r == kIOReturnNotPermitted ? "NotPermitted" :
                   r == kIOReturnExclusiveAccess ? "ExclusiveAccess" : "other");
            if (r == kIOReturnSuccess) IOHIDDeviceClose(d, kIOHIDOptionsTypeNone);
        }
    }
    return 0;
}
```

**Test targets available before deskhopplus hardware exists.** Any Mac laptop with a Touch Bar or an
Apple internal keyboard already exposes pure vendor-page nodes; on this machine, `Keyboard Backlight`
(`0xFF00`/15) and the `0xFF00`/3, /11, /13 collections of `Apple Internal Keyboard / Trackpad` all
work. `ioreg` (check 1) identifies the equivalent on any given machine — look for a
`PrimaryUsagePage` ≥ 65280 with no `RequiresTCCAuthorization` line.

**Run it from launchd, not from the terminal** — per #35, a terminal holding a grant leaks it
permanently to what it launches:

```sh
cat > /tmp/probe.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.deskhopplus.hidprobe</string>
<key>ProgramArguments</key><array><string>/tmp/hidprobe</string></array>
<key>StandardOutPath</key><string>/tmp/probe.out</string>
<key>StandardErrorPath</key><string>/tmp/probe.err</string>
<key>RunAtLoad</key><true/>
</dict></plist>
EOF
launchctl bootstrap gui/$(id -u) /tmp/probe.plist
sleep 3; cat /tmp/probe.out
launchctl bootout gui/$(id -u)/com.deskhopplus.hidprobe
```

For the exclusive-access variant, change `kIOHIDOptionsTypeNone` to `kIOHIDOptionsTypeSeizeDevice` in
the `IOHIDDeviceOpen` call.

### Check 3 — inspect the TCC decision afterwards

Confirm nothing was recorded:

```sh
sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service, client, client_type, auth_value from access where service='kTCCServiceListenEvent';"
sudo sqlite3 /Library/Application\ Support/com.apple.TCC/TCC.db \
  "select service, client, client_type, auth_value from access where service='kTCCServiceListenEvent';"
```

(`auth_value`: 0 denied, 2 allowed. `client_type`: 0 bundle ID, 1 absolute path.) Reading the user
database needs no privilege; the system one needs `sudo`, and Terminal may need Full Disk Access.

Watch the decision live while the probe runs:

```sh
log stream --predicate 'subsystem == "com.apple.TCC"' --info --debug
```

and specifically for the userspace log line from §1, which is the unambiguous signature of a denial:

```sh
log stream --predicate 'eventMessage CONTAINS "IOHIDDeviceOpen" OR eventMessage CONTAINS "TCC deny"' --info
```

To reset a grant between runs (this is what `tccutil` can and cannot do):

```sh
tccutil reset ListenEvent                      # resets Input Monitoring for all clients
tccutil reset ListenEvent com.example.bundleid # bundle-ID clients only
```

**[UNVERIFIED]** `tccutil reset` takes a bundle identifier; there is no documented way to target an
unsigned, unbundled binary recorded by absolute path, so for path-recorded clients the all-clients
form is the only reset. Not exercised here — no `kTCCServiceListenEvent` row existed to reset.

### Check 4 — the deliberate one: what a denial actually looks like

Only if a positive demonstration of the failure mode is wanted. **This writes a permanent TCC record
and shows the user a prompt.** Modify check 2 to open a *flagged* device (any keyboard node) and
observe `IOHIDDeviceOpen` return **`kIOReturnNotPermitted` = `0xE00002E2`** alongside
`TCC deny IOHIDDeviceOpen` in the log **[src]** — the return value and the log line together are what
make a TCC denial distinguishable from `kIOReturnExclusiveAccess` (another client seized the device)
or `kIOReturnNoDevice` (unplugged).

The non-destructive substitute for check 4, and what production code should do, is
`IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)` — it returns `Granted`/`Denied`/`Unknown` **without
prompting** **[doc]** (`IOHIDLib.h:180-193`). Note that for a vendor device this reports the
*process's* Input Monitoring status, which is **irrelevant to whether the open will succeed** — as §3
shows, `Unknown` accompanied seven successful opens. Do not gate the helper's HID open on it.

## 9. Route 5: alternatives to `IOHIDDevice`, and what they cost

Asked by #59 for completeness. **Since `IOHIDDevice` on a vendor page costs nothing, none of these is
needed** — recorded so the option is not reached for later under the mistaken belief that the HID
path is expensive.

| Route | Permission / entitlement cost | Viable unsigned? |
| --- | --- | --- |
| **`IOHIDDevice` + `IOHIDDeviceOpen`, vendor page** | **Nothing** — measured **[exp]** | **Yes** |
| `IOHIDManager` with broad matching | Input Monitoring, via keyboards swept up (§7) | No — avoid |
| `IOUSBHostDevice` / `IOUSBHostInterface` | Must claim a USB interface already claimed by the in-kernel HID driver; needs a matching driver and entitlements | No |
| **DriverKit / `USBDriverKit`** | Entitlements **granted by Apple on request**, tied to a team profile, plus provisioning profiles and code signing | **No** |
| `libusb` against a HID interface | Same claim problem as `IOUSBHostDevice` | No |

DriverKit is decisively out. Apple **[doc]**
([Requesting entitlements for DriverKit development](https://developer.apple.com/documentation/DriverKit/requesting-entitlements-for-driverkit-development)):

> Before you begin developing drivers for your hardware, request the entitlements you need from
> Apple […] Apple ties any requested entitlements to your development team's profile.

and:

> When activating your driver, the system validates the entitlements in your driver's entitlements
> file with the information you used to codesign your driver. If the entitlements don't match, the
> system aborts the activation process.

An Apple-granted, team-tied, code-signature-validated entitlement is incompatible with an unsigned
project, and signing is [out of scope for this map](https://github.com/myn/deskhopplus/issues/31).
Apple also requires "Your company's hardware vendor ID" **[doc]** in the request.

## Verdict

**#35's advantage survives. Vendor HID does not acquire a TCC requirement that CDC lacks.**

On macOS, opening a vendor-defined-usage-page HID device costs **nothing**: no Input Monitoring, no
entitlement, no signature, no bundle, no prompt, and no TCC record — measured from an unsigned,
unbundled launchd job holding no grants, and confirmed against the gate's implementation in Apple's
own published source, where the flag is set for exactly three usage pairs and the userspace open path
short-circuits to "granted" when the flag is absent. The behaviour is unchanged across seven
`IOHIDFamily` releases from Catalina to macOS 26. Exclusive open is free too, so the macOS half of
#34's exclusive-ownership control survives.

So the macOS objection raised in #59's correction — "trading a verified zero-permission path for a
possible TCC prompt is a regression" — **does not hold.** Both transports are permission-free on
macOS, and the macOS platform no longer discriminates between them. **The transport decision now
rests entirely on the Windows question** (#59's unknown 1: whether an unelevated process can open a
vendor HID collection exclusively, the way share mode 0 gives it for a COM port).

Two conditions attach, and the first is not optional:

1. **The vendor collection must be its own USB HID interface.** Put it in the keyboard or mouse
   interface's report descriptor and `conformsTo` flags the whole `IOHIDDevice`, acquiring the exact
   Input Monitoring requirement this document rules out. Verify with check 1 the moment the
   descriptor changes.
2. **The helper must match narrowly and never call `IOHIDManagerOpen` with broad matching**, or it
   will open a keyboard and prompt the user — the mistake behind essentially all the public folklore
   claiming HID needs Input Monitoring.

Residual risk is low but not zero: **[INFERENCE]** the one-`IOHIDDevice`-per-USB-HID-interface
mapping is inferred rather than documented, and Apple could widen the usage-pair list in a future
release. Both are cheap to re-check — check 1 in §8 is a single `ioreg` command and should be run
against the real device as soon as it enumerates, and again at each major macOS version.

## Loose ends

- Run check 1 against deskhopplus hardware once the vendor interface exists, to confirm §4's
  constraint was honoured. This is the one measurement that would retire the last inference in the
  chain.
- Settle #59's Windows unknown; it is now the only thing blocking the transport decision.
- **[UNVERIFIED]** Whether `tccutil reset ListenEvent` can target a path-recorded unsigned binary.
  Only matters if the helper ever takes a dependency on Input Monitoring, which per this document it
  should not.

## A note on machine state

Nothing persistent was created. The launchd jobs (`com.deskhopplus.hidprobe`, `com.deskhopplus.hidseize`)
were booted out after each run and `launchctl list` shows neither remaining **[exp]**. **No TCC rows
were written** — the probe deliberately never opened a flagged device, and check 3 confirms the
`kTCCServiceListenEvent` service has no rows at all. Probe binaries live only in the session
scratchpad. This is a cleaner footprint than #35, which did leave two Accessibility rows behind.

## Sources

**Apple-published source** — [`apple-oss-distributions/IOHIDFamily`](https://github.com/apple-oss-distributions/IOHIDFamily),
tag `IOHIDFamily-2238.100.59` unless noted:

- [`IOHIDFamily/IOHIDDevice.cpp:526-531`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDDevice.cpp#L526-L531) — the TCC flag, set for exactly three usage pairs
- [`IOHIDFamily/IOHIDDevice.cpp:2567`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDDevice.cpp#L2567) — `conformsTo`, which iterates *all* usage pairs
- [`IOHIDFamily/IOHIDDevice.cpp:1703`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDDevice.cpp#L1703) — `newIsAccessProtected`, the Apple-vendor-only protected-access gate
- [`IOHIDLib/IOHIDDeviceClass.m:510-535`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDLib/IOHIDDeviceClass.m#L510-L535) — the open path, and the `else { _tccGranted = true; }` short-circuit
- [`IOHIDFamily/IOHIDLibUserClient.cpp:412`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDLibUserClient.cpp#L412) — entitlement checks for protected services
- Tags `1446.11.12`, `1633.40.25`, `1787.40.10`, `1915.140.3`, `2031.100.16`, `2115.140.4`, `2238.120.5` — version comparison (§5)

**Apple documentation:**

- [Control access to input monitoring on Mac](https://support.apple.com/guide/mac-help/control-access-input-monitoring-mac-mchl4cedafb6/mac)
- [Requesting entitlements for DriverKit development](https://developer.apple.com/documentation/DriverKit/requesting-entitlements-for-driverkit-development)
- [`com.apple.developer.driverkit.transport.usb`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.transport.usb)
- [Privacy Preferences Policy Control payload — Apple Platform Deployment](https://support.apple.com/guide/deployment/privacy-preferences-policy-control-payload-dep38df53c2a/web)
- No reference page exists for `IOHIDDeviceOpen`; `developer.apple.com` returns 404 **[exp]**

**SDK headers** (macOS 26.2 SDK, `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`):

- `System/Library/Frameworks/IOKit.framework/Headers/hidsystem/IOHIDLib.h` — `IOHIDRequestType`, `IOHIDAccessType`, `IOHIDCheckAccess`, `IOHIDRequestAccess`
- `System/Library/Frameworks/IOKit.framework/Headers/hid/IOHIDDevice.h` — `IOHIDDeviceOpen`
- `System/Library/Frameworks/IOKit.framework/Headers/hid/IOHIDKeys.h` — `kIOHIDOptionsTypeSeizeDevice`
- `System/Library/Frameworks/IOKit.framework/Headers/hid/IOHIDUsageTables.h` — usage-page constants

**Secondary** (cited only in §7, to explain the folklore; no finding rests on these):

- [hidapi discussion #489](https://github.com/libusb/hidapi/discussions/489)
- [John's Blog — macOS IOHIDManager Permission Issue](https://nachtimwald.com/2020/11/08/macos-iohidmanager-permission-issue/)
- [Karabiner-Elements installation documentation](https://karabiner-elements.pqrs.org/docs/getting-started/installation/)
