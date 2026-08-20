// What a listener attached to the channel can still make the real helper say.
// (#108 on v1; re-pointed at v2 by #114.)
//
// #95 measured that a second kIOHIDOptionsTypeSeizeDevice open succeeds and
// receives session traffic. On protocol v1 this probe then measured the
// consequence that mattered, and confirmed it on hardware on 2026-08-18:
//
//   1. A listener sends a HELLO carrying a token it made up.
//   2. The device answers DH_HELLO_AUTH_FAILED — correctly, for that hello.
//   3. That answer is an *input report*, so every attached client receives it.
//   4. The real helper, with no field tying an answer to the question that
//      asked it, believed it and told the user "Not paired — press the config
//      chord" — #34's losing sequence of 2026-08-10, with no race won.
//
// ADR-0008 closed that, and this probe now measures whether it stayed closed.
// It asks two questions of a v2 board, in one run:
//
//   A. THE TRAP. A hello whose tag is wrong carries the *probe's* correlation
//      value, so every answer to it carries the probe's value too. The real
//      helper acts only on an answer carrying its own. Expected: the probe
//      sees the answer, and the helper's state does not move.
//
//   B. THE STATE THAT REPLACED "channel held". The board counts frames it
//      could not authenticate and reports the rate as LISTENER_ALERT (#111),
//      which the helper surfaces as "Another program is writing to the device
//      channel" (#114). Expected: a handful of bad-tag heartbeats inside one
//      window produce that alert on the wire and that line in the helper's log.
//
// A is a negative result and B is a positive one, which is the point of running
// them together: a run where the helper says nothing at all proves A only if B
// shows the helper was listening and could still be moved.
//
// Both are read out of the helper's own log as well as off the wire, so the
// whole measurement is one command: a run sheet step that needs a human
// watching two things at once is how #75 and #100 both hid.
//
// Non-destructive. A hello that fails its tag draws no answer and tears down no
// session, and a heartbeat that fails its tag is dropped before it touches
// session state (dh_session.c). What may move is what the *helper* believes.
//
// Usage:
//   swift probe_manufactured_chord_trap.swift [--seconds N] [--interval MS]
//                                             [--once] [--log PATH]
//
// --seconds must outlast the board's listener window (DH_LISTENER_WINDOW_MS,
// 10 s) or question B cannot be answered: the alert is raised when the window
// closes, not when the frames arrive.
import Foundation
import IOKit
import IOKit.hid

// MARK: - Arguments

var seconds = 25.0
var intervalMs = 1000.0
var repeating = true
var logPath = "/tmp/deskhop-helper.log"   // LaunchAgent StandardOutPath

/* The board's own constants (src/core/dh_session.h), repeated rather than
   imported for the reason the encoder below is: a probe that shares its
   constants with the thing it measures cannot catch them being wrong. If a
   board changes its mind, the alert carries the window it actually used and
   the result below reads it from the frame. */
let listenerWindowSeconds = 10.0
let listenerThreshold = 4

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

if repeating && seconds <= listenerWindowSeconds {
    print("probe: NOTE — --seconds \(Int(seconds)) does not outlast the board's \(Int(listenerWindowSeconds))s")
    print("       listener window, so question B will read as 'no alert' whatever the board did.")
}

// MARK: - The wire, by hand

// Kept deliberately independent of src/core: a probe that shares the encoder
// with the thing it is testing cannot catch the encoder being wrong. The
// self-test below is what keeps it honest instead.
let MSG_HELLO: UInt8 = 0x01
let MSG_HELLO_ACK: UInt8 = 0x02
let MSG_LISTENER_ALERT: UInt8 = 0x03
let MSG_HEARTBEAT: UInt8 = 0x05
let MSG_DEVICE_HEARTBEAT: UInt8 = 0x06
let MSG_SESSION_END: UInt8 = 0x07
let MSG_HELLO_REFUSED: UInt8 = 0x0B

let PROTO_VERSION: UInt16 = 2
let OS_MAC: UInt8 = 1
let BUILD_RELEASE: UInt8 = 0

let COUNTER_SIZE = 8
let TAG_SIZE = 16
let PREFIX_SIZE = COUNTER_SIZE + TAG_SIZE

func le16(_ v: UInt16) -> [UInt8] { [UInt8(v & 0xff), UInt8(v >> 8)] }
func le64(_ v: UInt64) -> [UInt8] { (0..<8).map { UInt8((v >> (UInt64($0) * 8)) & 0xff) } }

/// header ‖ counter ‖ tag ‖ body — docs/protocol.md v2. Nothing here computes a
/// tag: the probe has no key and wants a wrong one.
func frame(_ type: UInt8, counter: UInt64, tag: [UInt8], body: [UInt8]) -> [UInt8] {
    let payload = le64(counter) + tag + body
    return [type, 0x00] + le16(UInt16(payload.count)) + payload
}

func helloBody(correlation: UInt64, keyId: [UInt8], nonce: [UInt8]) -> [UInt8] {
    le16(PROTO_VERSION)
        + [OS_MAC, BUILD_RELEASE, 1]      // os, build type, channel count
        + le16(0x0400)                    // max chunk 1024
        + le64(correlation)
        + keyId
        + nonce
}

// The v2 `hello_mac` golden vector, copied from test-vectors/frames.txt. It is
// the shape gate for the encoder above, and its 16-byte tag is reused verbatim
// below: a well-formed tag that no board's key produces is exactly what this
// probe wants to send.
let goldenHelloMac: [UInt8] = [
    0x01, 0x00, 0x3f, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x19, 0x86, 0xc3, 0xf8, 0xfd, 0x48, 0x86, 0x7e,
    0xeb, 0x00, 0x82, 0xa8, 0x4b, 0xce, 0x54, 0xe9,
    0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04,
    0x0d, 0xf0, 0xfe, 0xca, 0xef, 0xbe, 0xad, 0xde,
    0xca, 0x5f, 0x30, 0x15, 0x4a, 0x8f, 0x7c, 0x61,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
]
let goldenTag = Array(goldenHelloMac[(4 + COUNTER_SIZE)..<(4 + PREFIX_SIZE)])

func hex(_ b: ArraySlice<UInt8>) -> String {
    b.map { String(format: "%02x", $0) }.joined(separator: " ")
}

let rebuilt = frame(MSG_HELLO, counter: 0, tag: goldenTag,
                    body: helloBody(correlation: 0xdeadbeef_cafef00d,
                                    keyId: [0xca, 0x5f, 0x30, 0x15, 0x4a, 0x8f, 0x7c, 0x61],
                                    nonce: (0...15).map { UInt8($0) }))
guard rebuilt == goldenHelloMac else {
    print("probe: SELF-TEST FAILED — this probe's hello does not match the v2 hello_mac vector")
    print("       built:  \(hex(rebuilt[...]))")
    print("       golden: \(hex(goldenHelloMac[...]))")
    print("       Refusing to run: a malformed hello would be refused for the wrong reason,")
    print("       and the result would look like the trap does not exist.")
    exit(3)
}
print("probe: self-test ok — hello matches the v2 hello_mac vector")

/* The probe's own correlation value, which is the whole of question A: every
   answer the board gives to these hellos echoes this number, and the real
   helper acts only on answers carrying its own. Fixed rather than random so a
   run is reproducible from its log. */
let probeCorrelation: UInt64 = 0x5052_4f42_4500_0001   // "PROBE\0\0\1"

/* A key id no helper has. It makes the board answer HELLO_REFUSED(unpaired)
   rather than fall silent (#117), which is the louder half of question A: the
   answer is on the wire, this process receives it, and the helper still does
   not move. */
let probeKeyId: [UInt8] = [0x50, 0x52, 0x4f, 0x42, 0x45, 0x00, 0x00, 0x01]
let probeNonce = [UInt8](repeating: 0x5a, count: 16)

let helloFrame = frame(MSG_HELLO, counter: 0, tag: goldenTag,
                       body: helloBody(correlation: probeCorrelation,
                                       keyId: probeKeyId, nonce: probeNonce))

/* Question B's frame: a heartbeat with a tag that cannot verify. It is counted
   towards the listener alert only while a session is live, which is why the
   result below refuses to conclude anything without a device heartbeat. */
let badTagBeat = frame(MSG_HEARTBEAT, counter: 1, tag: goldenTag, body: [])

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
        print("       states, so question A cannot produce a new line and reads as a clean pass")
        print("       whatever happens. Restart the helper first:")
        print("       launchctl kickstart -k gui/$(id -u)/com.deskhopplus.helper")
    }
} else {
    print("probe: NOTE — cannot read \(logPath). The helper's side cannot be read out")
    print("       automatically; watch its own output instead, or pass --log PATH.")
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

var sawRefusedOurs = false
var sawRefusedOther = false
var refusedStatus: UInt8? = nil
var sawOkAck = false
var sawDeviceHeartbeat = false
var sawSessionEnd: UInt8? = nil
var alertWindowMs: UInt32? = nil
var alertRefused: UInt32? = nil

func statusName(_ s: UInt8) -> String {
    switch s {
    case 2: return "version_incompatible"
    case 3: return "unpaired"
    default: return "unknown(\(s))"
    }
}

func readLE32(_ p: UnsafeMutablePointer<UInt8>, _ at: Int) -> UInt32 {
    UInt32(p[at]) | UInt32(p[at + 1]) << 8 | UInt32(p[at + 2]) << 16 | UInt32(p[at + 3]) << 24
}

func readLE64(_ p: UnsafeMutablePointer<UInt8>, _ at: Int) -> UInt64 {
    (0..<8).reduce(UInt64(0)) { $0 | UInt64(p[at + $1]) << (UInt64($1) * 8) }
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
        /* Untagged, so it is read straight from the body. The correlation is
           the first eight bytes — question A in one field. */
        case MSG_HELLO_REFUSED where len >= 11:
            let correlation = readLE64(report, body)
            let status = report[body + 10]
            refusedStatus = status
            if correlation == probeCorrelation {
                if !sawRefusedOurs {
                    print("probe: <- HELLO_REFUSED(\(statusName(status))) echoing the PROBE's"
                          + " correlation value")
                }
                sawRefusedOurs = true
            } else {
                sawRefusedOther = true
                print(String(format: "probe: <- HELLO_REFUSED(%@) for correlation %016llx"
                             + " — not ours; the real helper is retrying",
                             statusName(status), correlation))
            }
        case MSG_HELLO_ACK:
            sawOkAck = true
            print("probe: <- HELLO_ACK — the device accepted a hello whose tag it could not have"
                  + " verified")
        case MSG_LISTENER_ALERT where len >= PREFIX_SIZE + 8:
            /* Tagged under k_b2h, which this process cannot verify — it does
               not need to. That the board emitted one at all is the answer,
               and the window it reports is read from the frame rather than
               assumed, because the threshold is the firmware's and may move. */
            let alertBody = body + PREFIX_SIZE
            alertWindowMs = readLE32(report, alertBody)
            alertRefused = readLE32(report, alertBody + 4)
            print("probe: <- LISTENER_ALERT window=\(alertWindowMs!)ms refused=\(alertRefused!)")
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

// MARK: - Write what a listener can write

var hellosSent = 0
var beatsSent = 0

@Sendable func send(_ frame: [UInt8], what: String) -> Bool {
    var report = frame + [UInt8](repeating: 0x00, count: 64 - frame.count)
    let rc = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0, &report, report.count)
    if rc != kIOReturnSuccess {
        print(String(format: "probe: -> %@ FAILED to send, 0x%08x", what, rc))
        return false
    }
    return true
}

func writeBoth() {
    if send(helloFrame, what: "HELLO (bad tag)") {
        hellosSent += 1
        if hellosSent == 1 {
            print("probe: -> HELLO carrying a well-formed tag no board's key produces,")
            print(String(format: "       under correlation %016llx", probeCorrelation))
        }
    }
    if send(badTagBeat, what: "HEARTBEAT (bad tag)") {
        beatsSent += 1
        if beatsSent == 1 { print("probe: -> HEARTBEAT with a tag that cannot verify") }
    }
}

writeBoth()
if repeating {
    print("probe: repeating every \(Int(intervalMs)) ms for \(Int(seconds))s — one rejection may")
    print("       be corrected by the helper's next hello, and \(listenerThreshold) refused frames")
    print("       inside \(Int(listenerWindowSeconds))s is what the board's alert is measured over.")
    Timer.scheduledTimer(withTimeInterval: intervalMs / 1000, repeats: true) { _ in writeBoth() }
}

RunLoop.current.run(until: Date().addingTimeInterval(seconds))
IOHIDDeviceClose(dev, IOOptionBits(kIOHIDOptionsTypeSeizeDevice))

// MARK: - Read the verdict out of the helper's log

print("\nprobe: ——— result ———")
print("probe: sent \(hellosSent) hello(s) and \(beatsSent) heartbeat(s), none of which could"
      + " authenticate")

var helperLines: [String] = []
var helperSaidNotPaired = false
var helperSaidListener = false
if logReadable, let fh = FileHandle(forReadingAtPath: logPath) {
    fh.seek(toFileOffset: logOffsetAtStart)
    let text = String(data: fh.readDataToEndOfFile(), encoding: .utf8) ?? ""
    fh.closeFile()
    helperLines = text.split(separator: "\n").map(String.init)
    helperSaidNotPaired = helperLines.contains { $0.contains("Not paired") }
    helperSaidListener = helperLines.contains { $0.contains("writing to the device channel") }
}

// ----- A. the trap

print("\nprobe: A — can a listener still manufacture \"press the config chord\"?")
if sawOkAck {
    print("probe: the device answered HELLO_ACK to a tag it cannot have verified. That means a")
    print("       DEVELOPMENT build with authentication compiled out (#44) — reflash a release")
    print("       build before recording anything from this run.")
} else if sawRefusedOurs {
    print("probe: the device answered HELLO_REFUSED(\(statusName(refusedStatus ?? 0))) carrying the")
    print("       PROBE's correlation value, and this process received it without being entitled")
    print("       to it — the v1 reach is unchanged, as ADR-0008 said it would be.")
} else if !sawRefusedOther {
    print("probe: NO ANSWER OBSERVED at all. Nothing is established. Check the board is in normal")
    print("       mode and a helper is running, then re-run.")
}

if !logReadable {
    print("probe: UNREAD — no helper log. Read the helper's output by hand.")
} else if helperSaidNotPaired {
    print("probe: NOT CLOSED — the real helper reported \"Not paired\" during this run, to an")
    print("       answer it did not ask for. ADR-0008's correlation fix is not holding; say so on")
    print("       #108 and #112 before anything else in this file is believed.")
    for line in helperLines where line.contains("Not paired") { print("       | \(line)") }
} else if stateBeforeRun.contains("Not paired") {
    print("probe: UNMEASURABLE — the helper was already showing \"Not paired\" before this run, so")
    print("       there was no state change for it to log. Restart the helper and re-run.")
} else if !helperLines.isEmpty {
    print("probe: CLOSED — the helper logged \(helperLines.count) line(s) and none said \"Not")
    print("       paired\". The answers above carried the probe's correlation value, not the")
    print("       helper's, so the helper discarded them. This is only worth recording alongside")
    print("       a B below that moved the helper: it shows the helper was listening.")
} else {
    print("probe: INCONCLUSIVE — the helper logged nothing at all during this run. It is probably")
    print("       not running; start it and re-run before recording a result.")
}

// ----- B. the state that replaced "channel held"

print("\nprobe: B — is the listener state reachable? (#114)")
if let window = alertWindowMs, let refused = alertRefused {
    print("probe: the board raised LISTENER_ALERT — \(refused) refused frame(s) in \(window)ms.")
    if helperSaidListener {
        print("probe: REACHABLE — the helper reported it:")
        for line in helperLines where line.contains("writing to the device channel") {
            print("       | \(line)")
        }
    } else if !logReadable {
        print("probe: helper side UNREAD — no log. Watch the helper's output by hand.")
    } else {
        print("probe: NOT REPORTED — the board raised the alert and the helper said nothing about")
        print("       it. That is a helper-side defect, not a board one; say so on #114.")
        for line in helperLines.suffix(10) { print("       | \(line)") }
    }
} else if !sawDeviceHeartbeat {
    print("probe: UNMEASURABLE — no device heartbeat was seen, so there was probably no live")
    print("       session. Frames arriving with no session are deliberately not counted")
    print("       (dh_session.c), so this run could not have produced an alert. Pair a helper,")
    print("       let it connect, and re-run.")
} else if seconds <= listenerWindowSeconds {
    print("probe: UNMEASURABLE — the run was shorter than the board's window, which closes before")
    print("       an alert is raised. Re-run with --seconds \(Int(listenerWindowSeconds * 2)).")
} else {
    print("probe: NO ALERT — \(beatsSent) unauthenticated frames arrived into a live session over")
    print("       \(Int(seconds))s and the board raised nothing. Expected at least one alert per")
    print("       \(Int(listenerWindowSeconds))s window above \(listenerThreshold) refusals. Say so on #111 and #114.")
}

if let reason = sawSessionEnd {
    print("\nprobe: CAUTION — the device ended a session (reason \(reason)) during this run. This")
    print("       probe is meant to disturb nothing; a teardown means something else moved too.")
}
