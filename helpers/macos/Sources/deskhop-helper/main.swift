/*
 * The macOS helper: a background agent that finds the device, seizes every
 * channel, introduces itself, keeps the session alive (#45), and carries the
 * clipboard across it (#52).
 *
 * Clipboard text and images — files are #56, and cursor
 * placement is #51. It runs from a LaunchAgent that restarts it after a crash;
 * see helpers/macos/README.md.
 */

HelperRuntime().run()
