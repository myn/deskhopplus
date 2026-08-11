// Minimal, dependency-free: does a second kIOHIDOptionsTypeSeizeDevice open
// succeed while another process already holds one? Prints the raw IOReturn and
// then reports whatever arrives, so we can also tell who receives.
import Foundation
import IOKit
import IOKit.hid

let vendorID = 0x1209, productID = 0xC000, usagePage = 0xFF00, usage = 0x20
let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1])! : 8

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [
    kIOHIDVendorIDKey: vendorID, kIOHIDProductIDKey: productID,
    kIOHIDDeviceUsagePageKey: usagePage, kIOHIDDeviceUsageKey: usage,
] as CFDictionary)
IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))

guard let set = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice>, let dev = set.first else {
    print("probe: no matching vendor node found"); exit(2)
}
let serial = IOHIDDeviceGetProperty(dev, kIOHIDSerialNumberKey as CFString) as? String ?? "?"
print("probe: found vendor node on serial \(serial)")

let r = IOHIDDeviceOpen(dev, IOOptionBits(kIOHIDOptionsTypeSeizeDevice))
print(String(format: "probe: IOHIDDeviceOpen(seize) -> 0x%08x  %@", r,
             r == kIOReturnSuccess ? "SUCCESS — not refused" : "refused"))
guard r == kIOReturnSuccess else { exit(1) }

let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
IOHIDDeviceRegisterInputReportCallback(dev, buf, 64, { _, _, _, _, _, report, len in
    let bytes = (0..<min(Int(len), 12)).map { String(format: "%02x", report[$0]) }.joined(separator: " ")
    print("probe: RECEIVED \(len) bytes: \(bytes) ...")
}, nil)
IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)

print("probe: holding a seize for \(Int(seconds))s, listening…")
RunLoop.current.run(until: Date().addingTimeInterval(seconds))
print("probe: done")
