import AppKit
import DeskhopChannel
import Foundation

func check(_ condition: @autoclosure () -> Bool, _ message: String) {
    if !condition() { fatalError(message) }
}

let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: directory) }
let plist = directory.appendingPathComponent("com.deskhopplus.helper.plist")
let login = LaunchAtLogin(plist: plist, executable: "/Applications/My Helper/deskhop-helper")
check(!login.isEnabled, "a foreground launch does not opt into login startup")
try login.setEnabled(true)
check(login.isEnabled, "enabling registers the helper for login")
let registered = try PropertyListSerialization.propertyList(from: Data(contentsOf: plist), format: nil) as! [String: Any]
check(registered["ProgramArguments"] as? [String] == ["/Applications/My Helper/deskhop-helper"], "the executable path is one argument, including spaces")
check(registered["RunAtLoad"] as? Bool == true, "login starts the helper")
let original = try Data(contentsOf: plist)
try login.setEnabled(false)
check(!login.isEnabled, "disabling removes the login registration")
try login.setEnabled(true)
let restored = try Data(contentsOf: plist)
check(restored == original, "a toggle preserves the installed job")
print("Login registration checks passed")

let menuBar = MenuBar(login: login)
let menu = NSMenu()
for state in HelperState.allCases {
    menuBar.show(state: state)
    menuBar.menuNeedsUpdate(menu)
    let words = menu.items.prefix { !$0.isSeparatorItem }.map(\.title).joined(separator: " ")
    check(words == state.message ?? "Waiting for the device", "the menu must preserve the shared state's remedy")
    check(MenuBar.title(for: state).contains("deskhop"), "every idle title identifies the helper")
}
check(MenuBar.title(for: .connected).contains("paired"), "pairing confirmation is visible without a tooltip")
check(MenuBar.title(for: .listenerDetected).contains("listener"), "listener detection is visible without a tooltip")
check(MenuBar.title(for: .quiet) == "deskhop", "the config round trip stays quiet")
check(HelperState.versionIncompatible.message!.contains("update the helper"), "incompatible versions name the remedy")
menuBar.show(state: .listenerDetected)
menuBar.menuNeedsUpdate(menu)
check(menu.items.filter { !$0.isSeparatorItem }.allSatisfy { $0.title.count <= 65 }, "long remedies wrap into readable lines")
menuBar.show(progress: (25, 100))
menuBar.menuNeedsUpdate(menu)
check(menu.items.contains { $0.title.contains("25%") }, "receiving progress is displayed")
check(menu.items.contains { $0.title == "Cancel this transfer" && $0.isEnabled }, "receiving can be cancelled")
check(menu.items.contains { $0.title == "Start at login" }, "the login action is discoverable")
print("Menu state and action checks passed")

let startup = menu.items.first { $0.title == "Start at login" }!
_ = (startup.target as! NSObject).perform(startup.action!)
check(!login.isEnabled, "the menu action disables login registration")
menuBar.menuNeedsUpdate(menu)
check(menu.items.first { $0.title == "Start at login" }!.state == .off, "the menu reads back login registration")
_ = (startup.target as! NSObject).perform(startup.action!)
check(login.isEnabled, "the menu action enables login registration")
// A conflicting backup must not destroy either job or report success.
let backup = plist.appendingPathExtension("disabled")
try original.write(to: backup)
var refusedConflict = false
do { try login.setEnabled(false) } catch { refusedConflict = true }
check(refusedConflict && login.isEnabled, "a failed toggle preserves the enabled job")
let unchanged = try Data(contentsOf: plist)
check(unchanged == original, "a failed toggle does not damage registration")
menuBar.show(placementProblem: "Cursor placement unavailable — check the display layout.")
menuBar.menuNeedsUpdate(menu)
check(menu.items.contains { $0.title.contains("Cursor placement unavailable") }, "placement degradation has a visible remedy")
check(menu.items.first!.title.contains("Another program"), "placement status does not replace the listener remedy")
menuBar.show(placementProblem: nil)
menuBar.menuNeedsUpdate(menu)
check(!menu.items.contains { $0.title.contains("Cursor placement unavailable") }, "successful placement clears its warning")
print("Login actions, failure preservation, and placement checks passed")
