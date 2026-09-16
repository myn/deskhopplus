// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Derek Reynolds

import DeskhopChannel
import Foundation

/*
 * The Swift side of the bundle (#195): the copy side's rule for when one is
 * built, and the binding over `dh_bundle`.
 *
 * The format itself is `tests/bundle_test.c`'s. What the binding can get wrong
 * is different — a pointer into a buffer that has already gone, a length in
 * the wrong units — so this drives a round trip rather than restating the
 * layout. The rule is tested against a limit of 100, not the real threshold,
 * so that the case on each side of it is a number and not the constant
 * asserted to itself.
 */

let bundleTests: [(String, () throws -> Void)] = [
    ("text and a picture that fit together are bundled", testTheSendRule),
    ("a bundle survives the round trip", testABundleRoundTrips),
    ("a bundle that will not unpack is refused", testABadBundleIsRefused),
]

private func testABundleRoundTrips() {
    let parts = [
        ClipBundle.Part(kind: ClipKind.text.rawValue, bytes: Array("Ünïcödé".utf8)),
        ClipBundle.Part(kind: ClipKind.png.rawValue, bytes: [0x89, 0x50, 0x4e, 0x47]),
    ]
    guard let packed = ClipBundle.pack(parts) else {
        Check.that(false, "text and a PNG would not pack")
        return
    }
    /* The layout is tests/bundle_test.c's; here only that the header counts
       bytes and not characters. */
    Check.equal(Array(packed.prefix(5)), [0, 11, 0, 0, 0], "the text part's length is not its byte count")
    Check.equal(ClipBundle.unpack(packed), parts, "the parts did not survive the round trip")
    Check.equal(ClipBundle.pack([]), nil, "a bundle of no parts packed")
}

/* The refusals are tests/bundle_test.c's; this is only that the core's
   `false` comes back as nil. */
private func testABadBundleIsRefused() {
    Check.equal(ClipBundle.unpack([0, 9, 0, 0, 0, 0x68, 0x69]), nil,
                "a part longer than the payload was accepted")
}

private func testTheSendRule() {
    Check.equal(ClipboardSend.select(textBytes: 5, imageBytes: 0, bundleLimit: 100), .text,
                "text alone was not sent as text")
    Check.equal(ClipboardSend.select(textBytes: 0, imageBytes: 50, bundleLimit: 100), .image,
                "a picture alone was not sent as a picture")
    Check.equal(ClipboardSend.select(textBytes: 5, imageBytes: 95, bundleLimit: 100), .bundle,
                "text and a picture that fit together were not bundled")
    Check.equal(ClipboardSend.select(textBytes: 5, imageBytes: 96, bundleLimit: 100), .text,
                "text beside a picture too big to bundle did not travel alone")
    Check.equal(ClipboardSend.select(textBytes: 0, imageBytes: 0, bundleLimit: 100), .nothing,
                "an empty read sent something")
}
