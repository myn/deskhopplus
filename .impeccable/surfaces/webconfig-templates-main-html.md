---
version: 1
slug: "webconfig-templates-main-html"
primary_target: "webconfig/templates/main.html"
related_targets: ["webconfig/templates/style.css","webconfig/templates/form.html","webconfig/templates/script.js"]
---

# Config page — surface brief

Scope: the whole config page (`webconfig/templates/main.html` and its includes). Visitor mode: Operate. Spec: GitHub issue #225. Product truth: `PRODUCT.md`.

Audience and job: the maintainer at his own desk, in config mode, in Chrome. Task: Connect, drag the desk, set a few options, Save, Exit. Proof: the picture matches the desk; the helper says "Connected and paired — config mode".

Constraints: packed page ≤ 40 960 bytes; no font, image, CDN or network; `form.py`, the HID protocol code and `layout.js` logic untouched; Save is the only batch send; Read restores. Desktop only.

Open decisions (not the builder's to make): number inputs still send on change; no brand mark beyond the plain name; helper state per side waits on firmware (#50).

## Direction contract

THESIS: The config page is the OS's own display-arrangement pane, stretched over two computers: one settings window, a sidebar, and a picture you click. It refuses the category default, a long two-column admin form with a picture bolted on top.

OWN-WORLD: System window grey ground (light: `#ececec` sidebar / `#f5f5f7` content; dark: `#1e1e1e` / `#2a2a2c`), white or near-black grouped boxes with 10px radius and hairline separators, label-left control-right rows at 13px system sans, one accent: system selection blue `#0a60ff` (dark `#3b82f6`) for the selected computer, focus rings, switches and the default button. Checkboxes are switches; selects are pop-up buttons with a chevron; ranges use the accent. Refusal is a full-width strip in warning yellow-on-dark-text with a leading "!" glyph and a linked list. No shadows deeper than 1px, no gradients, no icons beyond a few-path SVG chevron and glyph.

STORY: The visitor sees their desk first, knows at once whether the board is connected, clicks a computer to see only its settings, saves once, and reads a refusal in words at the top when Save could not go.

FIRST VIEWPORT (1440×900): Sidebar 220px full height, "deskhopplus" as the window title at top-left, six section names beneath (Desk selected). Content column left-aligned, max 760px, 24px padding. Toolbar row 44px: connection state in words at left ("Not connected"), then Connect · Read · Save (with unsaved count) · Exit at right, Save filled blue. Section title "Desk" at 20px semibold. Below it the arrangement well: a recessed grouped box, ~320px tall, the layout SVG centred at ~1.6× today's scale, the selected computer's monitors filled blue with white numbers, the other grey; +/− per computer as small buttons on the well's bottom edge. Under the well: "Output A · macOS" as a group title, then its rows in one grouped box, then a closed "Advanced" disclosure row. Signature interaction: click a computer in the picture; its thumbnails turn blue in 120ms and the rows beneath swap to that computer with no animation. Motion grammar: 120ms ease on highlight and focus only; section switches and the refusal strip are instant; `prefers-reduced-motion` removes the 120ms.

FORM: OS display-arrangement pane. Candidate 1 of my ordered grounded list (the roll assigned candidate 5, a rack-switch faceplate; the user chose the pick). Seed key `fceb5e39`. Build path: code-led (no image generation here).

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance.
