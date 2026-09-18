# Multi-monitor macOS without a helper: where an absolute pointer lands

Research for [issue #214](https://github.com/myn/deskhopplus/issues/214) — in config mode the helper
channel is off (`src/channel.c:470`, `channel_pump_out` returns when `config_mode_active`), so a hop
onto the Mac has only absolute coordinates to place the cursor, and on 2026-09-18 a hop aimed at Main
landed on the other monitor. It also revisits the macOS half of
[#11](https://github.com/myn/deskhopplus/issues/11), whose resolution said absolute coordinates anchor
to the OS's primary display.

## The question

**Can a DeskHop-class board put the cursor on a chosen monitor of a multi-monitor Mac with HID
reports alone — no helper on the Mac?** And, as background: how does macOS map the board's
0–32767 absolute range across several displays, what did upstream DeskHop and its forks do, and what
does the helper path do that HID cannot.

## Headline

| Question | Answer |
| --- | --- |
| Where does macOS map a 0–32767 absolute mouse report? | Onto **the display the cursor is currently on** — not the main display, not the union. Apple's last published implementation of the mapping says so; every field report since agrees |
| Does a Digitizer (pen/touch) collection behave differently? | **No path to a display.** In Apple's driver a Mouse collection with absolute X/Y *is* a digitizer stylus transducer; both produce the same normalized `(0..1, 0..1)` event with no display field |
| Any HID feature to target a display? | **None.** The Digitizer usage page (HUT 1.6 §16) has no display usage; `Device Identifier` names a collection, not a screen. Extra collections or interfaces each get the same per-display mapping |
| What did upstream do? | Absolute report to the edge, then five **relative** nudges so macOS itself moves the cursor to the next display; the board then counts `screen_index`. The digitizer-pen macro in `usb_descriptors.h` was a **Windows** experiment and was never wired in |
| What did forks do? | Tune the same walk: bigger nudge ([#179](https://github.com/hrvach/deskhop/pull/179)), nudge from the vertical middle ([#227](https://github.com/hrvach/deskhop/pull/227)), nudge from the current Y ([#312](https://github.com/hrvach/deskhop/pull/312)), vertical directions ([#231](https://github.com/hrvach/deskhop/pull/231), Windows). None places a cursor on a chosen display; all walk it there and trust the count |
| Helper-free placement? | **Only by walking**: absolute-to-edge + nudge, one display at a time, from a display the board must already know. It breaks the moment the cursor moved by other means |
| With a helper? | `CGWarpMouseCursorPosition` — one call, global coordinates, any display. That is what `helpers/macos/.../CursorPlacement.swift:65` does |

**The #214 landing reads as "current display", not "union"** [INFERENCE]: the Mac cursor was last
left on A2 (the only Mac monitor that reaches Windows B1); the hop back sent Main's coordinates, and
macOS mapped them into A2. A main-only rule would have landed on A1. This also corrects #11's macOS
reading: absolute does not anchor to the primary on macOS.

## Sourcing note

Tags follow [`hid-transport-macos-tcc.md`](./hid-transport-macos-tcc.md): **[doc]** Apple
documentation, SDK header or the USB-IF HID Usage Tables · **[src]** Apple-published `IOHIDFamily`
source · **[up]** upstream DeskHop code, issue or PR · **[exp]** observed on hardware, by this project
or by a named reporter · **[INFERENCE]** reasoned from the above · **[UNVERIFIED]** nothing primary.

One limit is fixed and is stated up front. On this Mac (macOS 15.8) the DeskHop interfaces are driven
by `AppleUserHIDEventDriver`, a DriverKit dext from `com.apple.AppleUserHIDDrivers` **[exp]**
(`ioreg -c IOHIDDevice -r -l`), and the userspace translator that turns a normalized point into a
screen point (`IOHIDPointerEventTranslator`, `IOHIDEventTranslation.h`) is **not** in the published
tree **[exp]**. Apple publishes the in-kernel `IOHIDEventDriver` that the dext ports, and the
pre-Sierra kernel that did the mapping. §1 is built from those; the modern mapping is [INFERENCE]
from that code plus five independent field reports.

## 1. What macOS does with an absolute HID pointer

### 1.1 An absolute mouse is parsed as a digitizer stylus [src]

`IOHIDEventDriver::parseElements` tries the parsers in a fixed order
([`IOHIDEventDriver.cpp:624-643`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp#L624)).
An X/Y element with the **Absolute** flag under a **Generic Desktop / Mouse** collection fails all of
them:

- `parseRelativeElement` requires `kIOHIDElementFlagsRelativeMask`
  ([`:2220`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp#L2220)).
- `checkMultiAxisElement` refuses any element that `conformsTo(GenericDesktop, Mouse)`
  ([`:3129`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp#L3129)).

It lands in `pendingElements` and is handed to `parseDigitizerTransducerElement(element, NULL)`
([`:2032-2113`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp#L2032)),
which accepts it (it is not relative, `:2047`), creates a transducer of type
`kDigitizerTransducerTypeStylus` because there is no digitizer parent (`:2083`), and calibrates it
(`:2107-2108`). The live device confirms the classification: the absolute-mouse interface publishes
`"SurfaceDimensions" = {"Width"=32767,"Height"=32767}` — the digitizer surface property — **[exp]**.

Per report, X and Y are read as `getScaledFixedValue(kIOHIDValueScaleTypeCalibrated)`
([`:4006-4013`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp#L4006)),
i.e. a fixed-point `0.0–1.0` fraction of the logical range, and dispatched
([`:3914`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp#L3914))
into `IOHIDEventService::dispatchDigitizerEventWithOrientation`
([`IOHIDEventService.cpp:1955-2014`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventService.cpp#L1955)),
which emits a tablet event **and** `dispatchAbsolutePointerEvent` (`:2014`). That builds an
`IOHIDEventTypePointer` event with `kIOHIDEventOptionIsAbsolute` whose `position.x/y` are the same
`0..1` fractions
([`IOHIDEvent.cpp:731-736`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEvent.cpp#L731)).
The DriverKit interface documents the same contract **[doc]**: `DispatchAbsolutePointerEvent` takes
"An X value between 0 and 1" and "A Y value between 0 and 1"
([`IOHIDEventService.iig:180-205`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/HIDDriverKit/IOHIDEventService.iig#L180)).

**So the whole of what a device can say is one normalized point.** There is no display index, no
bounds, nothing a report could carry to pick a screen. Absolute events also skip pointer acceleration
([`IOHIDPointerScrollFilter.cpp:457`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDEventSystemPlugIns/IOHIDPointerScrollFilter.cpp#L457)).

A side note the firmware may care about: the driver has a machinery to cut a dead band off each end
of an absolute axis (`AbsoluteAxisBoundsRemovalPercentage`, `calibrateJustifiedPreferredStateElement`,
`:3162-3171`). The constant `kDefaultAbsoluteAxisRemovalPercentage 15` (`:74`) is **never assigned**;
`init` sets only the *preferred*-axis default (`:305-309`), so the band is 0 unless a driver
personality sets the property (`:420-424`). On this Mac only the "VMWare Legacy Work-around"
personality does **[exp]** (`IOHIDEventDriverSafeBoot.kext/Contents/Info.plist`). The board's
0..32767 maps to the full `0..1` — no dead band. [src] [exp]

### 1.2 Which display gets the `0..1` point: the current one

The mapping from `0..1` to a screen point is the one step Apple no longer publishes. The last version
that did is in the OS X 10.10–10.11 kernel (`IOHIDFamily-606` … `-701`; gone by `-870`, Sierra)
**[src]** [INFERENCE on the OS mapping]:

- `absolutePointerEventGated` calls `_scaleLocationToCurrentScreen(newLoc, bounds)`
  ([`IOHIDSystem.cpp:3570`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-606.20.4/IOHIDSystem/IOHIDSystem.cpp#L3570)).
- That function maps the device bounds onto **`cursorPin`**
  ([`:3181-3219`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-606.20.4/IOHIDSystem/IOHIDSystem.cpp#L3181)).
- `cursorPin` is the `desktopBounds` of **the screen the cursor is on**, reset whenever the cursor
  changes screens
  ([`:5501-5507`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-606.20.4/IOHIDSystem/IOHIDSystem.cpp#L5501)).

Every later observation is consistent with exactly that rule and with nothing else:

| Report | Observation | Fits |
| --- | --- | --- |
| [hrvach/deskhop#56](https://github.com/hrvach/deskhop/issues/56), opener, Sonoma 14.3.1 **[exp]** | "Either it's stuck to the primary screen, or if I use the trackpad to move the mouse to the secondary screen, it just allows this one" | current display |
| [#56, Fallingonion](https://github.com/hrvach/deskhop/issues/56#issuecomment-1984992574) **[exp]** | cursor on B: B↔C works, cannot reach A; after the trackpad moves it to A, the mouse "jumps from A to C" at A's edge | current display |
| [#56, hrvach 2024-11-27](https://github.com/hrvach/deskhop/issues/56#issuecomment-2504760157) **[up]** | "macOS has a thing where absolute coords apply to the current screen so a 'helper mouse' could be used to nudge it between the two" | current display |
| [myn/deskhopplus#11](https://github.com/myn/deskhopplus/issues/11), second result **[exp]** | with `screen_count = 2` "the Mac now spans both its monitors smoothly" via the nudge | current display (a primary-only rule would snap back after each nudge) |
| [#214](https://github.com/myn/deskhopplus/issues/214) **[exp]** | hop aimed at A1 lands on A2, after the Mac was last left from A2 [INFERENCE on the last monitor] | current display (main-only would have landed on A1) |

Windows is the primary-only case, and it is a different mechanism: since KB5003637 `mouhid` maps an
absolute pointer without the `MOUSE_VIRTUAL_DESKTOP` flag onto the primary display
([hrvach/deskhop#3](https://github.com/hrvach/deskhop/issues/3#issuecomment-1885795190),
[OSR thread cited there](https://community.osr.com/discussion/74220/absolute-mouse-data-on-multi-monitors)) **[up]**.
#11's Test A moved the *Windows* primary and drew the "anchors to primary" conclusion for both
computers; the Mac half of that conclusion does not hold.

### 1.3 Digitizer collections, multiple collections, `DisplayIntegrated`

- The Digitizer page **[doc]** (HUT 1.6, §16, table 16.1) has no display, screen or monitor usage.
  `Device Identifier` (0x53) "uniquely identifies the digitizer top-level collection" for devices with
  several collections, and in a settings collection "allows the host to select the device it wants to
  configure" — a collection selector, not a display selector (§16.7).
- A Pen / Touch Screen / Digitizer collection goes through `parseDigitizerElement` into the same
  transducer path (`:1904-2030`, then `:2032`), so it produces the same normalized event. The only
  difference is that Pen, Light Pen and Touch Screen set `DisplayIntegrated = true` on the service
  ([`IOHIDEventService.cpp:783`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventService.cpp#L783)) **[src]**.
  Whatever consumes that property on macOS is unpublished; there is no evidence it lets a device pick
  a display, and a touch screen "integrated" with a display could at most be pinned to *one*
  **[UNVERIFIED]**.
- Several absolute collections or interfaces each become their own service and each get the same
  per-display rule, so they add nothing **[INFERENCE]**.

## 2. What upstream DeskHop does [up]

README (`upstream/main:README.md`, "Multiple screens per output"):

> Windows and Mac have issues with multiple screens and absolute positioning, so workarounds are
> needed (still experimental). There is a better workaround under construction, but for now you have
> to set the operating system for each output and number of screens.
>
> Your main screens need to be in the middle, and secondary screen(s) on the edges.

and under "Shortcomings": "MacOS has issues with more than one screens, latest firmware offers an
experimental workaround that fixes it."

The workaround is `switch_virtual_desktop_macos` (`upstream/main:src/mouse.c:205-236`, introduced in
[`9c69dc3cd`](https://github.com/hrvach/deskhop/commit/9c69dc3cd), 2024-03-22): one **absolute**
report to the edge (`x = MIN or MAX`, `y = MAX/2`), then `MACOS_SWITCH_MOVE_COUNT = 5` **relative**
reports of `MACOS_SWITCH_MOVE_X = 10` on the second, relative-mouse interface
(`desc_hid_report_relmouse`, `usb_descriptors.c:47`), buttons forced to 0. macOS's own cursor logic
carries the cursor across; the board then sets `screen_index` and keeps sending absolute reports,
which now map into the new display (§1.2). Left/right only: the nudge has no vertical form.

The digitizer-pen macro `TUD_HID_REPORT_DESC_DIGITIZER_PEN` (`usb_descriptors.h:155`, `REPORT_ID_DIGITIZER 7`,
`digitizer_active`) arrived in v0.64
([`97a2cea91`](https://github.com/hrvach/deskhop/commit/97a2cea91), 2024-08-24). It is **not in any
report descriptor and nothing sets the flag** — `git grep DIGITIZER_PEN( upstream/main -- '*.c'`
is empty. It was a Windows experiment: "a potential workaround with the digitizer pen … will apply to
a virtual desktop stretching through all of the screens … I was able to move the cursor perfectly
accurate on multi-screen windows, but couldn't figure out buttons properly"
([hrvach, #56](https://github.com/hrvach/deskhop/issues/56#issuecomment-2504760157)). Windows
secondary monitors instead run in relative mode (`state->relative_mouse = (new_index > 1)`,
`mouse.c:245`).

## 3. What forks and PRs did [up]

| Where | What | Places the cursor? |
| --- | --- | --- |
| [PR #179](https://github.com/hrvach/deskhop/pull/179) eltariel, merged 2024-11 | nudge 25→50 units: "There seems to be a minimum amount of movement needed to convince Mac OS to switch the active screen" | no — same walk, bigger step |
| [PR #227](https://github.com/hrvach/deskhop/pull/227) rygwdn, merged 2025-02 | "macos will only allow the mouse to move between the screens where it thinks they overlap. This change bumps the cursor to the vertical middle of the edge … before using relative movement" | no — walk from a safer Y |
| [PR #312](https://github.com/hrvach/deskhop/pull/312) / [#315](https://github.com/hrvach/deskhop/pull/315) BrianPugh, 2026-01 | use the current Y, not the centre; four-point Y remap per transition; keep dragging across screens | no — walk with a better Y |
| [PR #231](https://github.com/hrvach/deskhop/pull/231) AsocPro, open, Windows user | adds `TOP`/`BOTTOM` to `is_screen_switch_needed`; custom `do_screen_switch` per layout; "x scaling not currently supported" | no — vertical walk, `switch_virtual_desktop_macos` untouched |
| [#56, jalmeroth](https://github.com/hrvach/deskhop/issues/56#issuecomment-2504689657) | relative helper mouse with no buttons, to survive drags | no |
| [#87](https://github.com/hrvach/deskhop/issues/87) crablab, open | asks for top/bottom switching on a Mac with a vertical monitor; "This would be possible, but is not currently supported" | request only |

All of them keep the OS as the only thing that moves the cursor between displays, and keep the board
counting where it thinks the cursor is. The failure eltariel found is the general one
([#56](https://github.com/hrvach/deskhop/issues/56#issuecomment-2507107697)): when displays are
offset, a nudge where they do not touch "will cause it to wrap back to the same screen", and the
board "tallies it internally as being on the leftmost screen 2" — the count and the cursor part ways,
exactly as in #214.

## 4. With a helper: one call

Deskflow (and Synergy/Barrier before it) place the cursor with `CGWarpMouseCursorPosition`
([`OSXScreen.mm:269-283`](https://github.com/deskflow/deskflow/blob/master/src/lib/platform/OSXScreen.mm#L269),
"move cursor without generating events"). The SDK header **[doc]**
(`CoreGraphics/CGRemoteOperation.h:219-222`): "Move the mouse cursor to the desired position in
global display coordinates without generating events." Global coordinates span every display, so
one call reaches any monitor. This project's helper does the same
(`helpers/macos/Sources/deskhop-helper/CursorPlacement.swift:65`) after resolving the layout's
monitor index to a display rect. Nothing on the HID side has an equivalent.

## What this means for deskhopplus

1. **Reword the rule.** On macOS an absolute report addresses *the display the cursor is on*. With a
   helper this never shows, because `CGWarpMouseCursorPosition` moves the cursor first. Without a
   helper the board must know which display the cursor is on, and it cannot read that back — it can
   only remember where it last left it, which the trackpad or a failed nudge silently invalidates.
2. **#214 is expected, not a #212 defect**, and the board's `screen_index = Main` is the wrong half of
   the pair: the cursor stayed on A2. Any helper-free fix is a *walk*, not a placement: on arrival,
   if the target monitor differs from the monitor the board last left the cursor on, do
   absolute-to-edge + nudge per step along the chain (§2), then the absolute entry position. The fork
   already has a direction-aware nudge (`src/mouse_logic.c:150-187`, `dh_mouse_nudge`) for chain
   moves; arrivals in config mode do not use it. The walk must start from the *remembered* monitor
   and nudge where the two Mac monitors actually touch (the layout grid knows that), or it wraps back
   (§3).
3. **Of #214's three options**, only option 3 (keep the channel alive in config mode) gives real
   placement; option 2 (treat every arrival as Main, keep `screen_index = 1`) is wrong on macOS —
   the cursor is *not* on Main, so the board and the OS would disagree on every hop from a non-main
   monitor; a walk-on-arrival is the correct form of option 2. Option 1 (document it) costs nothing
   and is true under every OS rule.
4. **Update #11's macOS line.** "Absolute anchors to the primary display" is a Windows fact. The
   configuration contract there (primary must be border-adjacent) is still needed on Windows, and on
   macOS only because the *board* assumes arrivals land on Main.

### One cheap check that would retire the inference

Board in config mode, no helper: use the trackpad to put the Mac cursor on the non-main monitor, then
move the DeskHop mouse without crossing. If the cursor moves on that monitor and cannot leave it,
§1.2 holds on macOS 15.8. Repeat from Main. Two minutes, no code.

## Loose ends

- **[UNVERIFIED]** what macOS does with `DisplayIntegrated` for a Pen / Touch Screen collection; even
  the best case is "pinned to one display", not "selectable".
- **[UNVERIFIED]** that the DriverKit dext keeps the in-kernel parse order; the live
  `SurfaceDimensions` property says it classifies the same way. The check above measures the
  `0..1` → screen mapping directly, which is otherwise inferred from 10.11 source plus field reports.

## Sources

**Apple-published source** — [`apple-oss-distributions/IOHIDFamily`](https://github.com/apple-oss-distributions/IOHIDFamily):

- [`IOHIDFamily/IOHIDEventDriver.cpp`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventDriver.cpp) tag `2238.100.59` — `parseElements` `:588-643`, `parseRelativeElement` `:2209`, `checkMultiAxisElement` `:3124`, `parseDigitizerTransducerElement` `:2032`, calibration `:3162`, dead-band default `:74`, `:305-309`, `:420-424`, X/Y read `:4006`, dispatch `:3914`
- [`IOHIDFamily/IOHIDEventService.cpp`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEventService.cpp) — `DisplayIntegrated` `:783`, `dispatchAbsolutePointerEvent` `:1609`, `dispatchDigitizerEventWithOrientation` `:1955-2014`
- [`IOHIDFamily/IOHIDEvent.cpp:731`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDFamily/IOHIDEvent.cpp#L731) — `kIOHIDEventOptionIsAbsolute`
- [`IOHIDEventSystemPlugIns/IOHIDPointerScrollFilter.cpp:457`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/IOHIDEventSystemPlugIns/IOHIDPointerScrollFilter.cpp#L457) — no acceleration on absolute
- [`HIDDriverKit/IOHIDEventService.iig:180-205`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-2238.100.59/HIDDriverKit/IOHIDEventService.iig#L180) — "An X value between 0 and 1"
- [`IOHIDSystem/IOHIDSystem.cpp`](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-606.20.4/IOHIDSystem/IOHIDSystem.cpp) tag `606.20.4` — `_scaleLocationToCurrentScreen` `:3181`, call `:3570`, `cursorPin` `:5501-5507`; the function is present through `701.60.2` and absent from `870.1.10` on

**Documentation:** [HID Usage Tables 1.6](https://usb.org/sites/default/files/hut1_6.pdf) §16 (table 16.1, §16.7); macOS 26.2 SDK `CoreGraphics/CGRemoteOperation.h:219-222`.

**Upstream and forks:** `upstream/main` at `e5f8ae8ce` (`README.md`, `src/mouse.c`, `src/usb_descriptors.c`, `src/include/usb_descriptors.h`, commits `9c69dc3cd`, `97a2cea91`);
[hrvach/deskhop#3](https://github.com/hrvach/deskhop/issues/3), [#56](https://github.com/hrvach/deskhop/issues/56), [#87](https://github.com/hrvach/deskhop/issues/87),
PRs [#179](https://github.com/hrvach/deskhop/pull/179), [#227](https://github.com/hrvach/deskhop/pull/227), [#231](https://github.com/hrvach/deskhop/pull/231), [#312](https://github.com/hrvach/deskhop/pull/312), [#315](https://github.com/hrvach/deskhop/pull/315);
[deskflow `OSXScreen.mm`](https://github.com/deskflow/deskflow/blob/master/src/lib/platform/OSXScreen.mm).

**This project:** [#11](https://github.com/myn/deskhopplus/issues/11), [#214](https://github.com/myn/deskhopplus/issues/214); `src/channel.c:470`, `src/mouse_logic.c:150-187`, `helpers/macos/Sources/deskhop-helper/CursorPlacement.swift:65`; `ioreg` on macOS 15.8 (24H23).
