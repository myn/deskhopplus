// swift-tools-version: 5.9

/*
 * The macOS helper (#45), and the thin binding through which it consumes the
 * shared C core.
 *
 * The package root is the repository root for one reason: DHCore compiles
 * src/core in place, the same sources the firmware compiles. A copy inside
 * the helper would be a second implementation of the wire format wearing a
 * binding's name, which is exactly what #64 consolidated away.
 *
 * The firmware build (CMakeLists.txt) and the core's host tests (tests/) are
 * untouched by this file.
 */

import PackageDescription

let package = Package(
    name: "deskhopplus",
    platforms: [.macOS(.v13)],
    targets: [
        /* The shared C core, compiled as-is: frame codec, session, transfer,
           and the primitives ADR-0008 needs.

           micro-ecc is excluded from the glob deliberately. src/core/dh_p256.c
           includes uECC.c itself, so that the curve configuration is set in one
           place for all three of this core's toolchains; letting SwiftPM also
           compile uECC.c on its own would be a second, unconfigured copy of
           every symbol in it. */
        .target(
            name: "DHCore",
            path: "src/core",
            exclude: ["micro-ecc"],
            publicHeadersPath: "."
        ),
        /* The binding and the session logic — no IOKit, so it is all testable
           on a laptop with no device attached. */
        .target(
            name: "DeskhopChannel",
            dependencies: ["DHCore"],
            path: "helpers/macos/Sources/DeskhopChannel"
        ),
        /* The background agent: IOKit, run loop, and the state the user sees. */
        .executableTarget(
            name: "deskhop-helper",
            dependencies: ["DeskhopChannel"],
            path: "helpers/macos/Sources/deskhop-helper"
        ),
        /*
         * The host tests, as an executable rather than a .testTarget: XCTest
         * and swift-testing both ship with Xcode, not with the Command Line
         * Tools, and a test suite that needs a 10 GB install to run is a test
         * suite that stops being run. This also matches the C harness next
         * door — an assertion helper, a main, a non-zero exit, no framework.
         *
         *   swift run channel-tests
         */
        .executableTarget(
            name: "channel-tests",
            dependencies: ["DeskhopChannel"],
            path: "helpers/macos/Tests/channel-tests"
        ),
    ]
)
