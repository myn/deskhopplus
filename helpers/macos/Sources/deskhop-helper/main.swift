/*
 * The macOS helper: a background agent that finds the device, seizes every
 * channel, introduces itself, keeps the session alive (#45), and carries the
 * clipboard across it (#52).
 *
 * Clipboard text, images and files (#52, #55, #56); cursor placement is #51.
 * It runs from a LaunchAgent that restarts it after a crash; see
 * helpers/macos/README.md.
 *
 * There is a menu-bar item, so this is an AppKit application rather than a
 * bare run loop — `HelperRuntime.run` sets the activation policy and turns
 * `NSApplication`'s loop. It stays a background agent: `.accessory` means no
 * Dock icon and no app-switcher entry.
 */

HelperRuntime().run()
