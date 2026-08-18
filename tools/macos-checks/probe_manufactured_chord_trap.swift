// Does a listener's rejected hello reach the *real* helper, and does the real
// helper believe it? (#108, deciding evidence for ADR-0008 finding 2.)
//
// #95 measured that a second kIOHIDOptionsTypeSeizeDevice open succeeds and
// receives session traffic. This probe measures the consequence that matters:
//
//   1. A listener sends a HELLO carrying a token it made up.
//   2. The device answers DH_HELLO_AUTH_FAILED — correctly, for that hello.
//   3. That ack is an *input report*, so every attached client receives it,
//      including the legitimate helper, which has no field tying an ack to the
//      hello that asked for it (dh_hello_ack carries no correlation value).
//   4. SessionEngine.swift moves the real helper to .notPaired and tells the
//      user "Not paired — press the config chord" — the gesture that would
//      provision whatever is attached, which is #34's losing sequence of
//      2026-08-10 reached without winning any race.
//
// Steps 1-3 this probe observes directly. Step 4 is read out of the helper's
// own log, so the whole measurement is one command: a run sheet step that
// needs a human watching two things at once is how #75 and #100 both hid.
//
// Non-destructive. answer_hello() does not tear down a session that is already
// present when a hello fails authentication (dh_session.c), so the device's
// own state survives this. What may move is what the *helper* believes.
//
// Usage:
//   swift probe_manufactured_chord_trap.swift [--seconds N] [--interval MS]
//                                             [--once] [--log PATH]
import Foundation
import IOKit
import IOKit.hid

// MARK: - Arguments

var seconds = 12.0
var intervalMs = 1000.0
var repeating = true
var logPath = "/tmp/deskhop-helper.log"   // LaunchAgent StandardOutPath

var args = Array(CommandLine.arguments.dropFirst())
func value(for flag: String) -> String {
    guard !args.isEmpty else {
        FileHandle.standardError.write("\(flag) needs a value\n".data(using: .utf8)!)
        exit(64)
    }
    return args.removeFirst()
}
while let flag = args.first {
    args.removeFirst()
    switch flag {
    case "--seconds":  seconds = Double(value(for: flag)) ?? seconds
    case "--interval": intervalMs = Double(value(for: flag)) ?? intervalMs
    case "--once":     repeating = false
    case "--log":      logPath = value(for: flag)
    default:
        FileHandle.standardError.write("unknown argument: \(flag)\n".data(using: .utf8)!)
        exit(64)
    }
}

// MARK: - The wire, by hand

// Kept deliberately independent of src/core: a probe that shares the encoder
// with the thing it is testing cannot catch the encoder being wrong. The
// self-test below is what keeps it honest instead.
let MSG_HELLO: UInt8 = 0x01
let MSG_HELLO_ACK: UInt8 = 0x02
let MSG_DEVICE_HEARTBEAT: UInt8 = 0x06
let MSG_SESSION_END: UInt8 = 0x07

let PROTO_VERSION: UInt16 = 1
let OS_MAC: UInt8 = 1
let BUILD_RELEASE: UInt8 = 0

func encodeHello(token: [UInt8]) -> [UInt8] {
    var payload: [UInt8] = []
    payload += [UInt8(PROTO_VERSION & 0xff), UInt8(PROTO_VERSION >> 8)]
    payload += [OS_MAC, BUILD_RELEASE, 1]          // os, build type, channel count
    payload += [0x00, 0x04]                        // max chunk 1024, LE
    payload += token
    let len = UInt16(payload.count)
    return [MSG_HELLO, 0x00, UInt8(len & 0xff), UInt8(len >> 8)] + payload
}

// The v1 `hello_mac` golden vector, frozen here. It left
// test-vectors/frames.txt when #109 rewrote that file for protocol v2, but the
// shipped firmware still speaks v1 (#111 is what moves it), so this is still
// the hello the board on the desk expects. If this ever stops matching what
// the board speaks, the probe is speaking a protocol the device does not, and
// a run that found nothing would mean nothing.
//
// WHEN #111 LANDS: this probe measures a trap v2 closes — a hello that does
// not authenticate gets no answer at all. Re-point it at the v2 vectors and it
// should find nothing, which is then the result worth recording.
let goldenHelloMac: [UInt8] = [
    0x01, 0x00, 0x17, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04,
    0xef, 0xbe, 0xad, 0xde, 0xef, 0xbe, 0xad, 0xde,
    0xef, 0xbe, 0xad, 0xde, 0xef, 0xbe, 0xad, 0xde,
]

func hex(_ b: ArraySlice<UInt8>) -> String {
    b.map { String(format: "%02x", $0) }.joined(separator: " ")
}

let deadbeef: [UInt8] = Array(repeating: [0xef, 0xbe, 0xad, 0xde], count: 4).flatMap { $0 }
guard encodeHello(token: deadbeef) == goldenHelloMac else {
    print("probe: SELF-TEST FAILED — this probe's hello does not match the frozen v1 hello_mac")
    print("       built:  \(hex(encodeHello(token: deadbeef)[...]))")
    print("       golden: \(hex(goldenHelloMac[...]))")
    print("       Refusing to run: a malformed hello would be refused for the wrong reason,")
    print("       and the result would look like the trap does not exist.")
    exit(3)
}
print("probe: self-test ok — hello matches the frozen v1 hello_mac")

/* A token the device cannot have issued. Well-formed at 16 bytes, so it is
   rejected on the comparison rather than on the length — the path a real
   listener would take, and the only one that produces AUTH_FAILED. */
let bogusToken = [UInt8](repeating: 0x00, count: 16)
let helloFrame = encodeHello(token: bogusToken)

// MARK: - Where the helper's log stands now

var logOffsetAtStart: UInt64 = 0
var logReadable = false
var stateBeforeRun = ""
if let fh = FileHandle(forReadingAtPath: logPath) {
    logOffsetAtStart = fh.seekToEndOfFile()
    fh.closeFile()
    logReadable = true
    /* The helper logs a state *change*, not a state. So "no Not-paired line
       appeared" means nothing unless we know what it was showing beforehand —
       a helper already sitting in Not paired has no change to log, and the run
       reads as a clean miss. That misread this probe's own second run. */
    if let text = try? String(contentsOfFile: logPath, encoding: .utf8) {
        stateBeforeRun = text.split(separator: "\n")
            .last { $0.contains("state:") }
            .map { String($0.split(separator: "state:").last ?? "").trimmingCharacters(in: .whitespaces) } ?? ""
    }
    print("probe: helper log at \(logPath), reading from byte \(logOffsetAtStart)")
    print("probe: helper state before this run: \(stateBeforeRun.isEmpty ? "unknown" : stateBeforeRun)")
    if stateBeforeRun.contains("Not paired") {
        print("probe: WARNING — the helper is ALREADY showing \"Not paired\". It logs changes, not")
        print("       states, so this run cannot produce a new line and would read as a miss.")
        print("       Restart the helper first:  launchctl kickstart -k gui/$(id -u)/com.deskhopplus.helper")
    }
} else {
    print("probe: NOTE — cannot read \(logPath). Step 4 cannot be read out automatically;")
    print("       watch the helper's own output instead, or pass --log PATH.")
}

// MARK: - Find and seize

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [
    kIOHIDVendorIDKey: 0x1209, kIOHIDProductIDKey: 0xC000,
    kIOHIDDeviceUsagePageKey: 0xFF00, kIOHIDDeviceUsageKey: 0x20,
] as CFDictionary)
IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))

guard let set = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice>, let dev = set.first else {
    print("probe: no matching vendor node found — is the board in normal mode?")
    exit(2)
}
let serial = IOHIDDeviceGetProperty(dev, kIOHIDSerialNumberKey as CFString) as? String ?? "?"
print("probe: found vendor node on serial \(serial)")

let opened = IOHIDDeviceOpen(dev, IOOptionBits(kIOHIDOptionsTypeSeizeDevice))
print(String(format: "probe: IOHIDDeviceOpen(seize) -> 0x%08x  %@", opened,
             opened == kIOReturnSuccess ? "SUCCESS — not refused" : "refused"))
guard opened == kIOReturnSuccess else {
    print("probe: refused, so there is no listener and nothing to measure. On macOS this")
    print("       would contradict #95 — say so on the ticket rather than assuming a fluke.")
    exit(1)
}

// MARK: - Listen

var sawAuthFailedAck = false
var sawOkAck = false
var sawDeviceHeartbeat = false
var sawSessionEnd: UInt8? = nil

func statusName(_ s: UInt8) -> String {
    switch s {
    case 0: return "DH_HELLO_OK"
    case 1: return "DH_HELLO_AUTH_FAILED"
    case 2: return "DH_HELLO_VERSION_INCOMPATIBLE"
    default: return "unknown(\(s))"
    }
}

/* One report, one or more whole frames, 0x00 padding between them
   (dh_frame.h). Session frames are far shorter than a 64-byte report, so a
   frame split across reports is not handled here and does not arise for the
   types this probe reads. */
func handle(report: UnsafeMutablePointer<UInt8>, length: Int) {
    var i = 0
    while i < length {
        if report[i] == 0x00 { i += 1; continue }          // padding
        guard i + 4 <= length else { return }
        let type = report[i]
        let len = Int(report[i + 2]) | (Int(report[i + 3]) << 8)
        let body = i + 4
        guard body + len <= length else { return }

        switch type {
        case MSG_HELLO_ACK where len >= 4:
            let status = report[body + 2]
            let build = report[body + 3]
            sawAuthFailedAck = sawAuthFailedAck || status == 1
            sawOkAck = sawOkAck || status == 0
            print("probe: <- HELLO_ACK status=\(statusName(status))"
                  + (build == 1 ? "  [device is a DEVELOPMENT build]" : ""))
        case MSG_DEVICE_HEARTBEAT:
            if !sawDeviceHeartbeat { print("probe: <- DEVICE_HEARTBEAT — a session is live here") }
            sawDeviceHeartbeat = true
        case MSG_SESSION_END where len >= 1:
            sawSessionEnd = report[body]
            print("probe: <- SESSION_END reason=\(report[body])")
        default:
            print("probe: <- frame type 0x\(String(format: "%02x", type)) len=\(len)")
        }
        i = body + len
    }
}

let inputBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
/* Argument order is (context, result, sender, type, reportID, report, length).
   Reading the 5th as the length is a silent zero, and a run that observed
   nothing would read as a device that sent nothing. */
IOHIDDeviceRegisterInputReportCallback(dev, inputBuffer, 64, { _, _, _, _, _, report, length in
    handle(report: report, length: Int(length))
}, nil)
IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)

// MARK: - Send the manufactured rejection

var sent = 0
func sendBogusHello() {
    var report = helloFrame + [UInt8](repeating: 0x00, count: 64 - helloFrame.count)
    let rc = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0, &report, report.count)
    sent += 1
    if rc != kIOReturnSuccess {
        print(String(format: "probe: -> HELLO (bogus token) FAILED to send, 0x%08x", rc))
    } else if sent == 1 {
        print("probe: -> HELLO carrying a 16-byte token of zeros — a token no device issued")
    }
}

sendBogusHello()
if repeating {
    print("probe: repeating every \(Int(intervalMs)) ms for \(Int(seconds))s — a single rejection")
    print("       may be corrected by the helper's next hello, and holding the state is the")
    print("       point: a listener can send this as often as it likes.")
    Timer.scheduledTimer(withTimeInterval: intervalMs / 1000, repeats: true) { _ in sendBogusHello() }
}

RunLoop.current.run(until: Date().addingTimeInterval(seconds))
IOHIDDeviceClose(dev, IOOptionBits(kIOHIDOptionsTypeSeizeDevice))

// MARK: - Read the verdict out of the helper's log

print("\nprobe: ——— result ———")
print("probe: sent \(sent) hello(s) with a token the device never issued")

var helperSaidNotPaired = false
var helperLines: [String] = []
if logReadable, let fh = FileHandle(forReadingAtPath: logPath) {
    fh.seek(toFileOffset: logOffsetAtStart)
    let text = String(data: fh.readDataToEndOfFile(), encoding: .utf8) ?? ""
    fh.closeFile()
    helperLines = text.split(separator: "\n").map(String.init)
    helperSaidNotPaired = helperLines.contains { $0.contains("Not paired") }
}

if sawAuthFailedAck {
    print("probe: CONFIRMED, step 2-3 — the device answered DH_HELLO_AUTH_FAILED, and this")
    print("       process received that answer without being the one entitled to it.")
} else if sawOkAck {
    print("probe: the device answered DH_HELLO_OK to a token it never issued. That means a")
    print("       DEVELOPMENT build with authentication compiled out (#44) — reflash a release")
    print("       build before recording anything from this run.")
} else {
    print("probe: NO HELLO_ACK OBSERVED. Nothing is established. Check the board is in normal")
    print("       mode and a helper is running, then re-run.")
}

if !logReadable {
    print("probe: step 4 UNREAD — no helper log. Read the helper's output by hand.")
} else if helperSaidNotPaired {
    print("probe: CONFIRMED, step 4 — the real helper reported \"Not paired\" during this run,")
    print("       to a hello it did not send. ADR-0008 finding 2 holds: a listener can")
    print("       manufacture the state whose documented remedy is the config chord.")
    for line in helperLines where line.contains("Not paired") { print("       | \(line)") }
} else if stateBeforeRun.contains("Not paired") {
    print("probe: step 4 UNMEASURABLE — the helper was already showing \"Not paired\" before this run,")
    print("       so there was no state change for it to log. Restart the helper and re-run.")
} else if !helperLines.isEmpty {
    print("probe: NOT CONFIRMED, step 4 — the helper logged \(helperLines.count) line(s) and none")
    print("       said \"Not paired\". Say so on #95 and on #108 just as loudly as a confirmation:")
    print("       ADR-0008 finding 2 would need correcting, and SessionEngine's handling of an")
    print("       uncorrelated ack would deserve a second look for *why* it did not.")
    for line in helperLines.suffix(10) { print("       | \(line)") }
} else {
    print("probe: step 4 INCONCLUSIVE — the helper logged nothing at all during this run. It is")
    print("       probably not running; start it and re-run before recording a result.")
}

if !sawDeviceHeartbeat {
    print("probe: CAUTION — no device heartbeat was seen, so there may have been no live session")
    print("       to disturb. A run without a paired helper attached proves nothing about step 4.")
}
