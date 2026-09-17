// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Derek Reynolds

/*
 * Renders every icon file the helpers ship from the one glyph the macOS menu
 * bar draws in code (`MenuBar.image(for:)`), so there is one geometry and not
 * two (#208). Run through render.sh, which compiles this against the built
 * modules and writes into helpers/icon/:
 *
 *   paired.ico, off.ico, attention.ico — the Windows tray, one per look, at
 *       the four sizes the taskbar asks for between 100 % and 200 % DPI.
 *       The Mac menu bar tints a template image for free; Windows gets colour
 *       instead, which does not care whether the taskbar is light or dark.
 *   app.ico — the .exe in Explorer, and deskhop.icns — the .app in Finder:
 *       the paired glyph in white on a blue rounded square.
 *
 * The rendered files are committed. CI never runs this; it is for the next
 * time the glyph or a colour changes.
 */

import AppKit
import DeskhopChannel

let blue = NSColor(srgbRed: 0.18, green: 0.44, blue: 0.92, alpha: 1)
let grey = NSColor(srgbRed: 0.55, green: 0.55, blue: 0.58, alpha: 1)
let orange = NSColor(srgbRed: 0.96, green: 0.58, blue: 0.10, alpha: 1)

/// A bitmap of exactly `size` pixels, drawn by `body` on the 16-unit grid.
func bitmap(_ size: Int, _ body: () -> Void) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    let context = NSGraphicsContext(bitmapImageRep: rep)!
    NSGraphicsContext.current = context
    context.cgContext.scaleBy(x: CGFloat(size) / 16, y: CGFloat(size) / 16)
    body()
    NSGraphicsContext.restoreGraphicsState()
    return rep
}

/// The template glyph, tinted, on its own clear canvas — the tint is a
/// source-atop fill, which would also paint anything already underneath.
/// Drawn unflipped, so the badge rect (which MenuBar states with y running
/// down) is mirrored to y running up. Resolution-independent, like the source.
func tinted(_ look: MenuBar.Look, _ colour: NSColor, badge badgeColour: NSColor? = nil) -> NSImage {
    NSImage(size: NSSize(width: 16, height: 16), flipped: false) { rect in
        let image = MenuBar.image(for: look)
        image.isTemplate = false
        image.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1)
        colour.set()
        rect.fill(using: .sourceAtop)
        if let badgeColour {
            let b = MenuBar.badge
            NSBezierPath(ovalIn: NSRect(x: b.minX, y: 16 - b.maxY, width: b.width, height: b.height)).addClip()
            badgeColour.set()
            rect.fill(using: .sourceAtop)
        }
        return true
    }
}

/// One tray-sized look, filling the 16-unit canvas.
func glyph(_ look: MenuBar.Look, _ colour: NSColor, badge badgeColour: NSColor? = nil) {
    tinted(look, colour, badge: badgeColour)
        .draw(in: NSRect(x: 0, y: 0, width: 16, height: 16), from: .zero, operation: .sourceOver, fraction: 1)
}

/// The app tile: a blue rounded square with the paired glyph in white.
func tile() {
    blue.set()
    NSBezierPath(roundedRect: NSRect(x: 0.5, y: 0.5, width: 15, height: 15), xRadius: 3.5, yRadius: 3.5).fill()
    tinted(.paired, .white)
        .draw(in: NSRect(x: 3, y: 3, width: 10, height: 10), from: .zero, operation: .sourceOver, fraction: 1)
}

func png(_ rep: NSBitmapImageRep) -> Data { rep.representation(using: .png, properties: [:])! }

/// An .ico is a small directory in front of one PNG per size — the form
/// Windows has read since Vista.
func ico(_ sizes: [Int], _ draw: (Int) -> NSBitmapImageRep) -> Data {
    let images = sizes.map { png(draw($0)) }
    var data = Data()
    func u16(_ v: Int) { data.append(contentsOf: [UInt8(v & 0xff), UInt8(v >> 8)]) }
    func u32(_ v: Int) { u16(v & 0xffff); u16(v >> 16) }
    u16(0); u16(1); u16(sizes.count)
    var offset = 6 + 16 * sizes.count
    for (size, image) in zip(sizes, images) {
        data.append(UInt8(size == 256 ? 0 : size)); data.append(UInt8(size == 256 ? 0 : size))
        data.append(0); data.append(0)
        u16(1); u16(32); u32(image.count); u32(offset)
        offset += image.count
    }
    for image in images { data.append(image) }
    return data
}

let out = URL(fileURLWithPath: CommandLine.arguments[1])
let tray = [16, 20, 24, 32]
let looks: [(String, MenuBar.Look, NSColor, NSColor?)] = [
    ("paired", .paired, blue, nil), ("off", .off, grey, nil), ("attention", .attention, blue, orange),
]
for (name, look, colour, badge) in looks {
    try ico(tray) { size in bitmap(size) { glyph(look, colour, badge: badge) } }
        .write(to: out.appendingPathComponent("\(name).ico"))
}
try ico([16, 32, 48, 256]) { size in bitmap(size) { tile() } }.write(to: out.appendingPathComponent("app.ico"))

/* iconutil wants a folder of named PNGs and makes the .icns from it. */
let iconset = out.appendingPathComponent("deskhop.iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    try png(bitmap(base) { tile() }).write(to: iconset.appendingPathComponent("icon_\(base)x\(base).png"))
    try png(bitmap(base * 2) { tile() }).write(to: iconset.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", out.appendingPathComponent("deskhop.icns").path]
try iconutil.run(); iconutil.waitUntilExit()
try FileManager.default.removeItem(at: iconset)
print("wrote paired.ico off.ico attention.ico app.ico deskhop.icns to \(out.path)")
