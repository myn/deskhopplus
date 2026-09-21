---
name: deskhopplus config page
description: An OS display-arrangement pane stretched over two computers. One settings window, a sidebar, one accent, and a picture you click.
colors:
  ground: "#ececec"
  pane: "#f5f5f7"
  box: "#ffffff"
  well: "#e4e4e8"
  line: "#d2d2d7"
  ink: "#1d1d1f"
  ink-2: "#6e6e73"
  accent: "#0a60ff"
  on-accent: "#ffffff"
  monitor: "#c7c7cc"
  monitor-ink: "#3a3a3c"
  handle: "#4a4a4e"
  warn: "#ffd60a"
  warn-ink: "#1d1d1f"
  bad: "#d70015"
  focus: "#0a60ff66"
  ground-dark: "#1e1e1e"
  pane-dark: "#2a2a2c"
  box-dark: "#353538"
  well-dark: "#232325"
  line-dark: "#48484c"
  ink-dark: "#f2f2f7"
  ink-2-dark: "#a1a1a6"
  accent-dark: "#2f6fe0"
  monitor-dark: "#5a5a5f"
  monitor-ink-dark: "#f2f2f7"
  handle-dark: "#5a5a5f"
  bad-dark: "#ff6961"
  focus-dark: "#2f6fe080"
typography:
  headline:
    fontFamily: '-apple-system, "Segoe UI", system-ui, sans-serif'
    fontSize: "20px"
    fontWeight: 600
    lineHeight: 1.45
    letterSpacing: "-0.01em"
  title:
    fontFamily: '-apple-system, "Segoe UI", system-ui, sans-serif'
    fontSize: "15px"
    fontWeight: 600
    lineHeight: 1.45
    letterSpacing: "-0.01em"
  subtitle:
    fontFamily: '-apple-system, "Segoe UI", system-ui, sans-serif'
    fontSize: "13px"
    fontWeight: 600
    lineHeight: 1.45
  body:
    fontFamily: '-apple-system, "Segoe UI", system-ui, sans-serif'
    fontSize: "13px"
    fontWeight: 400
    lineHeight: 1.45
  label:
    fontFamily: '-apple-system, "Segoe UI", system-ui, sans-serif'
    fontSize: "11px"
    fontWeight: 600
    lineHeight: 1.45
    letterSpacing: "0.04em"
  hint:
    fontFamily: '-apple-system, "Segoe UI", system-ui, sans-serif'
    fontSize: "12px"
    fontWeight: 400
    lineHeight: 1.45
  mono:
    fontFamily: "ui-monospace, Menlo, Consolas, monospace"
    fontSize: "12px"
    fontWeight: 400
    lineHeight: 1.45
    fontFeature: "tnum"
  trace:
    fontFamily: "ui-monospace, Menlo, Consolas, monospace"
    fontSize: "11px"
    fontWeight: 400
    lineHeight: 1.5
rounded:
  sm: "6px"
  md: "8px"
  lg: "10px"
  full: "50%"
spacing:
  xs: "4px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "24px"
  xxl: "40px"
components:
  sidebar:
    backgroundColor: "{colors.ground}"
    width: "220px"
    padding: "16px 12px"
  sidebar-item:
    textColor: "{colors.ink}"
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    padding: "6px 10px"
  sidebar-item-current:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.on-accent}"
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    padding: "6px 10px"
  toolbar:
    backgroundColor: "{colors.pane}"
    height: "44px"
    padding: "0 24px"
  button:
    backgroundColor: "{colors.box}"
    textColor: "{colors.ink}"
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    padding: "4px 12px"
  button-primary:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.on-accent}"
    typography: "{typography.subtitle}"
    rounded: "{rounded.sm}"
    padding: "4px 12px"
  button-armed:
    backgroundColor: "{colors.warn}"
    textColor: "{colors.warn-ink}"
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    padding: "4px 12px"
  unsaved-badge:
    backgroundColor: "{colors.on-accent}"
    textColor: "{colors.accent}"
    rounded: "{rounded.md}"
    padding: "0 6px"
  strip:
    backgroundColor: "{colors.warn}"
    textColor: "{colors.warn-ink}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: "10px 14px"
  group:
    backgroundColor: "{colors.box}"
    rounded: "{rounded.lg}"
    padding: "0"
  row:
    typography: "{typography.body}"
    height: "40px"
    padding: "6px 14px"
  sub-heading:
    textColor: "{colors.ink-2}"
    typography: "{typography.label}"
    padding: "12px 14px 4px"
  input:
    backgroundColor: "{colors.pane}"
    textColor: "{colors.ink}"
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    padding: "4px 8px"
  input-reading:
    textColor: "{colors.ink-2}"
    typography: "{typography.mono}"
  switch:
    backgroundColor: "{colors.monitor}"
    rounded: "{rounded.lg}"
    width: "34px"
    height: "20px"
  switch-on:
    backgroundColor: "{colors.accent}"
    rounded: "{rounded.lg}"
    width: "34px"
    height: "20px"
  well:
    backgroundColor: "{colors.well}"
    rounded: "{rounded.lg}"
    padding: "16px"
  trace:
    textColor: "{colors.ink-2}"
    typography: "{typography.trace}"
    padding: "10px 14px"
---

# Design System: deskhopplus config page

Scope: this file covers the config page only (`webconfig/templates/` rendered to `webconfig/config.htm`). The macOS menu bar item and the Windows tray item are native and out of scope. Every value here is read from `webconfig/templates/style.css` as shipped on 2026-09-21. Light values sit on `:root`. Dark values are the `-dark` tokens and apply under `prefers-color-scheme: dark`; a token with no `-dark` twin is the same in both schemes.

## Overview

**Creative North Star: "The Display-Arrangement Pane"**

The page looks like the OS's own display settings, stretched over two computers. It is one settings window: a grey sidebar of section names on the left, a toolbar with the connection words and four buttons, and a content column of grouped boxes. The picture of the desk sits in a recessed well at the top of the Desk section. Click a computer in the picture and its rows appear under it. The page refuses the category default: a long two-column admin form with a picture bolted on.

The material is flat. Depth comes from tonal steps (sidebar darker than content, boxes lighter, the well recessed) and 1px hairlines, not shadows. Type is the system face at 13px in every row. One accent, system selection blue, marks what is selected or what acts: the current section, the chosen computer, the Save button, switches that are on, the range thumb, the focus ring. Yellow is the only other fill and it means "stop and read": the refusal strip and an armed service button.

The page ships inline on a 64 kB disk with no network. That is a design fact, not a limit to work around. No web fonts, no images, no icon library. Icons are few-path inline SVG, and there are two of them.

**Key Characteristics:**
- One window: 220px sidebar, 44px sticky toolbar, 760px content column.
- Grouped boxes with 10px corners, hairline row separators, label left and control right.
- One accent hue. Grey does the rest.
- Flat: tonal layers and hairlines; one 1px shadow under the switch thumb.
- System type only: 13px body, 20px section titles.
- Dark scheme follows the OS through `color-scheme: light dark`.
- Motion is 120ms ease-out on the switch and the monitor fill, nothing else.

## Colors

The palette is system greys plus one blue, with yellow and red held back for refusal and error.

### Primary
- **Selection Blue** (`accent`, dark `accent-dark`): the current sidebar item, the selected computer's monitors and handle, the Save button, a switch that is on, the range track and thumb through `accent-color`, and text selection. The dark value is deeper than the OS default so white text passes contrast on it.
- **On Accent** (`on-accent`): white text and numbers on any accent fill. Also the Main monitor's thick stroke when its computer is selected, and the fill of the unsaved-count badge.
- **Focus Ring** (`focus`, dark `focus-dark`): the accent at 40% (light) or 50% (dark) alpha, drawn as a 3px outline with 1px offset on every `:focus-visible`.

### Secondary
- **Refusal Yellow** (`warn`) with **Refusal Ink** (`warn-ink`): the same pair in both schemes. Fills the refusal strip, the no-WebHID strip, and a service button armed for its second click.

### Tertiary
- **Error Red** (`bad`, dark `bad-dark`): the border of an invalid input, the error line under it, the layout status line when a drop is refused, and the "dev build" reading. Never a fill.

### Neutral
- **Ground** (`ground`, dark `ground-dark`): the sidebar.
- **Pane** (`pane`, dark `pane-dark`): the body, the toolbar, and every editable input's background.
- **Box** (`box`, dark `box-dark`): grouped boxes and default buttons. The lightest surface in light mode; the top layer in dark mode.
- **Well** (`well`, dark `well-dark`): the arrangement well, recessed below the pane. Also the digit fill on the seam bands.
- **Line** (`line`, dark `line-dark`): every hairline: sidebar edge, toolbar edge, box borders, row separators, input borders.
- **Ink** (`ink`, dark `ink-dark`): body text and labels.
- **Ink 2** (`ink-2`, dark `ink-2-dark`): secondary text: hints, the toolbar words when not connected, read-only readings, slider readouts, the trace, sub-headings, seam input labels.
- **Monitor** (`monitor`, dark `monitor-dark`): the fill of an unselected monitor in the picture and the off state of a switch.
- **Monitor Ink** (`monitor-ink`, dark `monitor-ink-dark`): monitor strokes and labels, the seam bands, and the halo around band digits.
- **Handle** (`handle`, dark `handle-dark`): the label bar you drag to move a computer, when that computer is not selected.

Three literal colours sit outside the token set and are the same in both schemes: the select chevron stroke (`#888`), the switch thumb (`#fff`) and the handle text (`#fff`). Use `on-accent` for new white-on-fill text; do not add more literals.

### Named Rules
**The One Accent Rule.** Blue means selected or primary. It appears on the current section, the chosen computer, Save, switches that are on, the range, focus and selection, and on nothing else. A second hue never enters the picture; the seam bands are ink, not colour.

**The Yellow Means Stop Rule.** Yellow fills only a strip the user must read (a refusal, a missing WebHID) and a service button waiting for its second click. Red never fills; it strokes an invalid input and colours the line that says why.

**The Tonal Step Rule.** Surfaces are told apart by lightness, not by shadow: ground under pane, pane under box, well recessed below pane. In dark mode the order flips so box is the lightest layer.

## Typography

**Display Font:** none.
**Body Font:** system sans (`-apple-system`, "Segoe UI", `system-ui`, `sans-serif`).
**Label/Mono Font:** system mono (`ui-monospace`, Menlo, Consolas, `monospace`) for hex readings, keymap text areas and the cursor trace.

**Character:** the page reads as the OS's own settings because it uses the OS's own face. There is nothing to load and nothing to choose. Weight does the hierarchy: 600 for titles and headings, 400 for everything else. Tabular numerals on every reading and readout.

### Hierarchy
- **Headline** (600, 20px, -0.01em): the section title (`h2`) at the top of the content column, one per section.
- **Title** (600, 15px, -0.01em): the window title "deskhopplus" at the top of the sidebar.
- **Subtitle** (600, 13px): a group title above a grouped box ("Output A · MacOS", "Cursor transition trace"), the Advanced summary, and the Save button's label.
- **Body** (400, 13px, 1.45): every row label, every control, every button, the sidebar items, the toolbar words.
- **Label** (600, 11px, 0.04em, uppercase, Ink 2): the sub-heading inside a grouped box (`h3.sub`), such as "SCREENSAVER". It heads the rows beneath it. It never sits above a title.
- **Hint** (400, 12px, Ink 2, max 72ch): the paragraphs under a section title or the well. Error lines and seam input labels share the size.
- **Mono** (400, 12px, tabular): hex readings and the keymap text areas.
- **Trace** (400, 11px, 1.5): the cursor transition trace in `pre`, wrapped, in Ink 2.

### Named Rules
**The System Face Rule.** No font file is ever shipped or fetched. The two stacks above are the whole type system.

**The 13px Row Rule.** A row's label and its control are both 13px. Only the section title (20px) and the window title (15px) are larger. Smaller sizes (12px, 11px) exist only for secondary text. The desk picture's labels (10 and 9 in `layout.js`) are SVG viewBox units that scale with the picture, about 14-16px on screen; they are not steps on this ramp.

## Layout

The window is a flex row: a 220px sidebar (`ground`, 1px right hairline, padding 16px 12px, 2px gap between items) and a pane that fills the rest. The pane holds a sticky 44px toolbar (padding 0 24px, 1px bottom hairline), then any strips (margin 16px 24px 0), then `main` (padding 8px 24px 40px, max-width 760px, left-aligned).

One section is visible at a time. The others are hidden, not removed, so every field stays in the document. The section title has 20px above and 12px below. A group title (`h3`) has 24px above and 8px below. A hint has 8px above. The Advanced disclosure box has 12px above.

Inside a grouped box, rows stack with a hairline between them. A row is a wrapping flex line: min-height 40px, padding 6px 14px, gap 8px vertical and 16px horizontal, label left (`flex: 1 1 160px`) and control right. An error line (`small`) takes the full width under them. The arrangement well has 16px padding; the picture's SVG is centred, full width, capped at 360px tall; the +/- counts sit 10px under it with a 24px gap between computers.

The spacing scale is 4px-based: 4, 8, 12, 16, 24, 40. Row padding (6px 14px), strip padding (10px 14px) and button padding (4px 12px) are the exceptions and are recorded on their components.

Responsive: at 800px and below the sidebar folds into a wrapping row of section names under the title (1px bottom hairline), the toolbar unsticks and wraps, content padding drops to 16px, and text inputs go full width. At 480px and below the slider and seam inputs wrap onto their own line and number inputs shrink to 80px. The page is desktop-first; the narrow layouts keep it usable, not designed for.

## Elevation & Depth

The page is flat. Depth is tonal layering plus hairlines: the sidebar is darker than the pane, grouped boxes are lighter than the pane, the well is darker than the pane, and each edge is a 1px `line` border. There are no gradients.

### Shadow Vocabulary
- **Thumb lift** (`box-shadow: 0 1px 2px rgba(0,0,0,0.3)`): under the switch thumb only. It is the one shadow in the page.

### Named Rules
**The Hairline Rule.** Every boundary is a 1px `line` border. Boxes, rows, inputs, the sidebar edge and the toolbar edge share it. Nothing is separated by space alone or by a shadow.

**The One Shadow Rule.** The switch thumb carries the only shadow. Nothing else lifts.

## Shapes

Corners are small and consistent: 6px on buttons, inputs, selects, text areas and sidebar items; 8px on the strips and the unsaved-count badge; 10px on grouped boxes, the Advanced disclosure and the well. The switch is a 34×20 pill (10px radius); its thumb and the sidebar's unsaved dot are circles.

In the picture, monitors are rounded rectangles filled `monitor` with a 1.5px `monitor-ink` stroke; the Main monitor's stroke is 3px. Seam bands are 6px round-capped strokes. Handles (the label bars) are filled rectangles with white 10px text. Icons: one chevron (10×6, stroke `#888`) as a data-URI on every select; one ring-and-exclamation (16×16, `currentColor`) leading every strip.

## Components

### Sidebar
Character: the section list of a settings window.
- **Style:** `ground` background, 220px, window title at top, six section buttons stacked with 2px gap. Each button is 13px body text, left-aligned, padding 6px 10px, 6px radius, transparent.
- **Hover:** 8% ink over transparent.
- **Current:** accent fill, on-accent text (`aria-current="page"`).
- **Unsaved:** a 7px dot in `currentColor` at the right edge when the section holds an unsaved or invalid field.
- **Mobile (≤800px):** a wrapping row under the title.

### Toolbar
Character: the window's action bar.
- **Style:** sticky, 44px, `pane` background, bottom hairline. Connection words at left in Ink 2 ("Not connected"), Ink when connected ("Connected — config mode"); they fall back to "Not connected" when the device drops, as after Exit. Buttons at right with 8px gap: Connect · Read · Save · Exit.

### Buttons
- **Shape:** softly rounded (6px), padding 4px 12px, 13px text.
- **Default:** `box` fill, 1px `line` border, Ink text. Hover mixes 15% ink into the fill.
- **Primary (Save):** accent fill and border, on-accent text, weight 600. Hover mixes 15% black into the accent. Carries the unsaved count as a badge: on-accent fill, accent text, 11px, 8px radius, padding 0 6px, 6px to the left of it. Hidden at zero.
- **Armed (Service):** a Bootloader or Wipe button takes two clicks. After the first it turns `warn` with `warn-ink` text and its label changes to "Click again to …". Any other click disarms it.
- **Count (+ / −):** the same button at padding 0 8px, line-height 20px, on the well's bottom edge.
- **Disabled:** 45% opacity, default cursor, no hover change. Every board-facing button starts disabled; Connect turns them on.

### Strips
Character: a full-width band the user must read before going on.
- **Style:** `warn` fill, `warn-ink` text, 8px radius, padding 10px 14px, margin 16px 24px 0, `role="alert"`. A 16px ring-and-exclamation glyph leads, then a bold first sentence ("Save refused."), then a bulleted list of links. Links inherit the ink and are weight 600; clicking one reveals and focuses the field.
- **Variants:** refusal (after a Save that could not go) and no-WebHID (browser warning). Both appear at once; no motion.

### Grouped rows (Cards / Containers)
- **Corner Style:** 10px.
- **Background:** `box`.
- **Shadow Strategy:** none; see Elevation.
- **Border:** 1px `line`; rows inside are separated by 1px `line` top borders.
- **Internal Padding:** rows are 6px 14px with min-height 40px; a sub-heading is 12px 14px 4px.
- **Advanced:** a `details` styled as a group. The summary is 10px 14px, weight 600; when open it gains a bottom hairline.

### Inputs / Fields
- **Text / number / textarea / select:** `pane` fill, 1px `line` border, 6px radius, padding 4px 8px, 13px. Text inputs are 220px wide, number 100px, seam numbers 72px, text areas 280px and mono 12px with vertical resize.
- **Select:** native appearance removed; a 10×6 chevron data-URI at right 8px; 24px right padding.
- **Reading (read-only):** no border, no fill, right-aligned, Ink 2. Hex readings are mono 12px tabular. A dev-build reading is Error Red.
- **Focus:** 3px `focus` outline, 1px offset (global).
- **Invalid:** `bad` border; a 12px `bad` error line appears under the row and the sidebar section gets a dot. Editing the field clears both at once.
- **Disabled:** the whole fieldset is disabled until Connect.

### Switch
A checkbox drawn as the OS draws one: 34×20 pill in `monitor`, 16px white thumb with the thumb lift shadow. Checked: accent fill, thumb slides right. Both transitions are 120ms ease-out and drop to none under reduced motion.

### Slider
A native range (160px, accent through `accent-color`) with a 48px read-only readout to its left in Ink 2, tabular, 10px gap. At ≤800px the range is 120px.

### Seam inputs
Three 72px number inputs (Screen, Start, End) with 12px Ink 2 labels, 8px apart, on the right of a "Segment n" row.

### Trace
A `pre` inside a grouped box: 11px/1.5 mono, Ink 2, wrapped, padding 10px 14px, top hairline.

### Arrangement well and picture (signature)
The well is a recessed box (`well` fill, 10px radius, 16px padding, centred). The picture is layout.js's SVG, drawn at pane scale (max 360px tall). Each computer has a label bar handle ("Output A · MacOS") and its monitors below. Unselected: handle in `handle`, monitors in `monitor` with `monitor-ink` 1.5px strokes and 13px 600 labels; Main has a 3px stroke. Selected (`data-selected`): handle and monitors turn accent, labels and Main's stroke turn on-accent, in 120ms. The seam between the computers is a row of `monitor-ink` bands, 6px round-capped, with 9px 700 digits filled `well` and haloed by a 3px `monitor-ink` stroke. A refused drop writes one Error Red line under the picture; an accepted one writes Ink 2. A focused monitor box gets an accent stroke. Click, focus or key on a computer selects it and swaps the rows beneath with no animation.

## Do's and Don'ts

### Do:
- **Do** put every setting in a grouped box row: label left, control right, hairline between rows.
- **Do** use the accent only for what is selected or primary, and grey for the rest.
- **Do** keep the two schemes on the same tokens; dark redefines `:root`, never a component.
- **Do** draw any new icon as a few-path inline SVG in `currentColor` or a data-URI, at 16px or less.
- **Do** keep the packed `config.htm` at or under 40 960 bytes; `disk/capacity.py` is the hard gate.
- **Do** make a refusal a yellow strip at the top with the field names as links, and mark the field red at the same time.
- **Do** keep an action that reboots or wipes behind a second click, shown in yellow.
- **Do** make a hover a colour mix (8% ink on transparent, 15% ink on `box`, 15% black on accent), not a new colour.

### Don't:
- **Don't** load a font, an image, an icon set or anything else over the network. The page opens from a drive.
- **Don't** add a shadow beyond the switch thumb's 1px lift, or a gradient anywhere.
- **Don't** introduce a second hue. Bands, handles and unselected monitors stay in the grey ramp.
- **Don't** fill anything red; red is a border and a line of text.
- **Don't** animate a section switch, a row swap or a strip. Only the switch and the monitor fill move, and only for 120ms.
- **Don't** let a corner exceed 10px or drop below 6px, except the pill switch and the dots.
- **Don't** widen the content column past 760px or add a second column.
