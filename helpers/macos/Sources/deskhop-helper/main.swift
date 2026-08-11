/*
 * The macOS helper: a background agent that finds the device, seizes every
 * channel, introduces itself, and keeps the session alive (#45).
 *
 * No payloads yet — clipboard (#52, #55, #56) and cursor placement (#51)
 * arrive on the session this establishes. It runs from a LaunchAgent that
 * restarts it after a crash; see helpers/macos/README.md.
 */

HelperRuntime().run()
