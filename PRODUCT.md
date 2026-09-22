# Product

<!-- impeccable:product-schema 1 -->

This record covers the **config page** only: `webconfig/templates/` rendered to
`webconfig/config.htm` and shipped on the board's `DESKHOP` drive. The macOS
menu bar item and the Windows tray item are native, tiny, and out of scope for
design work (confirmed 2026-09-21).

## Platform

web

## Users

Derek, at his own desk: one Mac and one Windows PC, more than one monitor on
each, joined by a DeskHopPlus board pair. He opens the page rarely — at first
setup and when the desk changes — and knows the firmware inside out.

Other DeskHop owners who flash the fork from the release page are welcome but
are not the design target (confirmed 2026-09-21).

## Product Purpose

The page is the only place the boards are configured. It sets where the two
computers sit (the layout), how many monitors each drives and which meets which
(segments), key remapping and the Cmd/Ctrl swap, every hotkey chord, and the
common settings. It shows device status (firmware versions, pairing, the cursor
transition trace) and offers maintenance actions (blink, bootloader, wipe).

Success: a desk with multiple monitors on both sides is described correctly in
one Connect → drag → Save round, and the cursor lands where the desk says it
should.

## Positioning

The layout is set by dragging a picture of the desk, not by typing counts,
directions and a four-row segment table by hand. The picture derives every
field the board needs; the fields stay reachable under Advanced.

The page ships inside the firmware. There is no installer, no server and no
network: the board presents a drive, the page opens from it, and WebHID talks to
the board directly.

## Operating Context

Opening the page is a ritual, in this order:

1. Press the chord, `Left Ctrl + Right Shift + C + O`, on the keyboard plugged
   into the board. The board reboots into config mode and its LED blinks.
2. A drive named `DESKHOP` appears. Open `config.htm` from it in Chrome or
   another Chromium browser. Firefox and Safari have no WebHID.
3. Click **Connect** and pick the board in the browser's WebHID prompt.
4. **Read** loads the board's values. Change things. **Save** sends them and
   writes flash. **Exit** reboots the board out of config mode.

Config mode ends by itself after five minutes. Each entry and exit reboots the
board and starts a fresh helper session. While the page stays connected the
helper can say **Connected and paired — config mode**, which lets multi-monitor
hops be tried live; **Device in config mode** means there is no usable helper
channel yet.

The page's words: **Output A** / **Output B** are the two computers; **board A**
and **board B** are the two halves; the keyboard and mouse always start on board
A's computer after a replug. The user guide (`docs/user-guide.md`, *Layout,
remapping and hotkeys*) documents the page and must stay in step with it.

## Capabilities and Constraints

**Sections today.** Topbar with Connect / Read / Save / Exit and the
maintenance buttons; Layout (drag picture with a status line); Output A and
Output B columns; a closed Advanced panel holding Screen Count, Border
Direction, Chain Direction, four seam segments and the legacy seam start/end for
both outputs; Common Config; Device Status with the cursor transition trace.

**Save is the boundary.** A page gesture or edit changes the page's fields
only. **Save** sends them to the board; **Read** throws unsaved changes away.
The board keeps a sent value in RAM and only Save writes flash, so a value sent
early makes Read return it and the user cannot tell what is saved. Derek called
sending on drop a "big bug" (#212). The plain number inputs still send on
change; that is upstream behaviour he has not ruled on yet — ask before changing
it, do not copy it.

**The fields are the truth.** The Advanced inputs are the settings. The layout
is a view of them, drawn by pure functions; it stores nothing of its own.

**Size.** The page must fit the 64 kB config disk the firmware stores. The
file may use 59 392 bytes of it (the rest is FAT overhead); `disk/capacity.py`
turns an overflow into a build failure. Today `config.htm` is 30 336 bytes
(51%), a self-extracting DEFLATE bundle of one HTML file with its CSS and JS
inline (`webconfig/render.py`). A second cap sits in the browser, not the
board: `packer.j2` inflates into a 100 000-byte buffer, and the unpacked page
is 85 328 bytes today; that number can grow at no cost to the board. Design
budget for the packed page (set 2026-09-21): at most 40 960 bytes, so 18 kB
stays free for future fields. Everything the page needs
must be inline: it opens from a drive, with no network, so no CDN, no web fonts,
no external images. Rendering the templates needs Python 3.10+ and Jinja2;
`webconfig/config.htm` and `disk/disk.img` are committed and CI regenerates and
compares them.

**Browser.** Chromium only, because of WebHID. The page shows a warning
otherwise.

**Firmware limits.** Up to seven monitors per computer. Four seam segments,
fixed — growing them is a config version bump that costs every board its
settings and its pairing. Values are read and written per field over 64-byte
HID reports; the page must never send a layout the board cannot run.

**Terminology.** The page says **monitor**; the firmware says screen. **Main**
is the monitor the OS calls main and the page cannot change that. **Segment**
is a touching pair of monitors across the seam. **Layout** is the picture.
**The chord** always works whatever the page sets.

**Open product work** (GitHub issues): page-level banner when Save is refused
(#155); make validation errors prominent (#154); field 94 label and duplicate
presentation (#140); show helper connectivity on the page (#50); flash the
main screen from config mode (#216). ADR-0014 is still unanswered (see the
layout editor memory).

## Brand Commitments

DeskHopPlus is its own product (confirmed 2026-09-21). The upstream DeskHop
name, logo, the "DeskHop Config" title and the green `#5e9f41` **Hop** are not
binding; later design work may replace them. Nothing has been chosen to replace
them yet — that is a new-work decision, not a fact.

Words a person reads spell the product **DeskHopPlus** (#233). Every identifier
stays lowercase `deskhopplus`: file names, folders, paths, URLs and release
assets.

Voice, from the README and user guide: short plain sentences, active voice, one
idea per sentence, no marketing. The guide states rules at the moment they
matter ("one line under the picture says why").

## Evidence on Hand

- The live page: `webconfig/templates/main.html`, `form.html`, `style.css`
  (Milligram 1.4.1, MIT, vendored inline), `script.js`, `layout.js`.
- `img/config-page.gif`: this page in the README. A screen recording of the
  page connected to a board in config mode, taken with CleanShot in light
  mode at 800 px wide and saved as a GIF. It sits at the top of the README. Record it again the same way when
  the page changes; keep it under GitHub's 10 MB image limit.
- `img/connect-dialog.png`: upstream DeskHop's connect dialog, before this
  fork's changes.
- `docs/user-guide.md` — the page's documentation. `README.md` — product
  description and release flow.
- Spec #210 (closed): the layout editor's user stories, model, gestures and
  refusals. #211 and #212 shipped it.
- Layout logic tests run in node's `vm` with a fake DOM; one smoke test loads
  the rendered page in real Chrome.
- No testimonials, customers, benchmarks or pricing exist. Do not invent any.

## Product Principles

1. **Nothing reaches the board before Save.** Every gesture edits the page;
   Save sends; Read restores.
2. **The picture derives the numbers, the numbers stay reachable.** Advanced
   holds the truth; the layout is a view of it.
3. **Refuse loudly, in one line, at the moment it matters.** A layout the board
   cannot run is not made; the page says why and stays as it was.
4. **Fit the disk.** 64 kB, inline, no network. Size is a build gate, not a
   wish.
5. **Match the guide.** The page and `docs/user-guide.md` use the same words
   and change together.

## Accessibility & Inclusion

No formal standard is set. Two requirements exist in the shipped page and must
survive: the layout is not drag-only (Tab to a box, arrow keys move it), and
status and error lines use `aria-live="polite"` so a refusal is announced.
