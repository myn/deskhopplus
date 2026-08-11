import DeskhopChannel
import Foundation

/*
 * The session seam: events in, actions out. No device, no IOKit, no wall
 * clock — every test states the time it is asking about.
 */

let sessionEngineTests: [(String, () throws -> Void)] = [
    ("the helper introduces itself once it holds every channel", testHelperIntroducesItself),
    ("an acknowledged hello is a connected session", testAcknowledgedHelloIsASession),
    ("auth failure and version mismatch are different states", testFailuresAreDistinct),
    ("a development device is noted", testDevelopmentDeviceIsNoted),
    ("a partial acquisition never prompts the chord", testPartialAcquisitionNeverPromptsChord),
    ("a wholly refused acquisition is the same state", testWhollyRefusedAcquisition),
    ("the heartbeat beats at the interval the device measures", testHeartbeatKeepsBeating),
    ("an unanswered hello is not left half-open", testUnansweredHello),
    ("a protocol error drops the connection", testProtocolErrorDropsConnection),
    ("a brief disappearance says nothing", testBriefDisappearanceIsSilent),
    ("a long absence is eventually reported", testLongAbsenceIsReported),
    ("starting before the device is attached is reported", testStartingWithNoDevice),
    ("config mode is distinct from an absent device", testConfigModeIsDistinct),
    ("a config-mode round trip reconnects by itself", testConfigModeRoundTrip),
    ("the channels are released when the device goes", testChannelsReleasedOnDeparture),
    ("reconnection backs off to a capped delay", testBackoffIsCapped),
    ("a working session resets the backoff", testWorkingSessionResetsBackoff),
]

/// One engine, one clock, and the small vocabulary the tests read outputs with.
private final class Fixture {
    let engine = SessionEngine()
    var now: TimeInterval = 1000

    @discardableResult
    func send(_ input: SessionInput) -> [SessionOutput] {
        engine.handle(input, at: now)
    }

    func advance(_ seconds: TimeInterval) -> [SessionOutput] {
        now += seconds
        return engine.handle(.tick, at: now)
    }

    /// The frames a run of outputs asked to be written.
    func sentFrames(_ outputs: [SessionOutput]) throws -> [Frame] {
        try outputs.compactMap { output -> Frame? in
            guard case .send(let bytes) = output else { return nil }
            return try FrameCodec.decode(bytes).frame
        }
    }

    func states(_ outputs: [SessionOutput]) -> [HelperState] {
        outputs.compactMap { if case .state(let state) = $0 { return state } else { return nil } }
    }

    func retries(_ outputs: [SessionOutput]) -> [TimeInterval] {
        outputs.compactMap { if case .retry(let after) = $0 { return after } else { return nil } }
    }

    func notes(_ outputs: [SessionOutput]) -> [String] {
        outputs.compactMap { if case .note(let note) = $0 { return note } else { return nil } }
    }

    /// The device's own answer, encoded through the shared core.
    func ack(_ status: HelloStatus,
             channels: UInt8 = 1,
             chunk: UInt16 = 1024,
             build: BuildType = .release,
             version: UInt16 = 1) throws -> SessionInput {
        let ack = HelloAck(protocolVersion: version, status: status, buildType: build,
                           channelCount: status == .ok ? channels : 0,
                           maxChunk: status == .ok ? chunk : 0)
        return .received(try ack.encoded())
    }

    /// Device present, every channel seized, hello answered.
    func establishSession() throws {
        send(.deviceAppeared(.normal))
        send(.channelsAcquired(count: 1))
        send(try ack(.ok))
    }
}

// MARK: - The handshake

private func testHelperIntroducesItself() throws {
    let f = Fixture()
    Check.equal(f.send(.deviceAppeared(.normal)).filter { $0 == .openChannels }.count, 1,
                "seeing the device did not start an acquisition")

    let frames = try f.sentFrames(f.send(.channelsAcquired(count: 1)))
    Check.equal(frames.count, 1, "acquiring the channels did not produce a hello")
    guard let first = frames.first else { return }

    Check.equal(first.type, MessageType.hello, "the helper did not introduce itself with a hello")
    let hello = try Hello.decode(payload: first.payload)
    Check.equal(hello.protocolVersion, 1, "the hello does not carry the protocol version")
    Check.equal(hello.os, 1, "the hello does not carry the platform")

    /* Nothing is claimed to the user until the device has answered. */
    Check.equal(f.engine.state, .quiet, "reported a session before the device answered")
}

private func testAcknowledgedHelloIsASession() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    Check.equal(f.states(f.send(try f.ack(.ok, channels: 1, chunk: 512))), [.connected],
                "an acknowledged hello did not become a connected session")
    Check.equal(f.engine.negotiated?.channelCount, 1, "channel count not taken from the ack")
    Check.equal(f.engine.negotiated?.maxChunk, 512,
                "the session must run on the effective value, not the requested one")
}

private func testFailuresAreDistinct() throws {
    let unpaired = Fixture()
    unpaired.send(.deviceAppeared(.normal))
    unpaired.send(.channelsAcquired(count: 1))
    Check.equal(unpaired.states(unpaired.send(try unpaired.ack(.authenticationFailed))),
                [.notPaired], "authentication failure was not reported as unpaired")
    Check.that(unpaired.engine.state.promptsConfigChord,
               "an unpaired helper must be told which keystroke fixes it")
    Check.that(unpaired.engine.negotiated == nil,
               "an unpaired session must not claim negotiated terms")

    let mismatched = Fixture()
    mismatched.send(.deviceAppeared(.normal))
    mismatched.send(.channelsAcquired(count: 1))
    Check.equal(mismatched.states(mismatched.send(try mismatched.ack(.versionIncompatible,
                                                                    version: 2))),
                [.versionIncompatible], "a version mismatch was not reported as one")
    Check.unequal(mismatched.engine.state, .notPaired,
                  "a version mismatch was reported as an unpaired helper")
    Check.that(!mismatched.engine.state.allowsBulkTransfers,
               "a mismatched helper must refuse transfers rather than corrupt a file")
}

private func testDevelopmentDeviceIsNoted() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    let outputs = f.send(try f.ack(.ok, build: .development))

    Check.equal(f.engine.negotiated?.deviceBuild, .development, "the build type was not recorded")
    Check.that(!f.notes(outputs).isEmpty, "a development build must identify itself in the log")
}

// MARK: - Exclusivity

private func testPartialAcquisitionNeverPromptsChord() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    let outputs = f.send(.acquisitionRefused(acquired: 1, of: 2))

    Check.equal(f.states(outputs), [.channelHeld],
                "a partial acquisition was not reported as a held channel")
    Check.that(outputs.contains(.closeChannels), "a partially acquired set was not released")
    Check.that(!f.engine.state.promptsConfigChord,
               "a refused open must never prompt the chord: the chord would provision the "
               + "program holding the channel")
    Check.equal(f.engine.state.message, "Another program holds the channel — find and stop it",
                "the remedy does not name the cause")
    Check.unequal(f.engine.state, .notPaired, "a refused open was reported as unpaired")
    Check.that(!f.retries(outputs).isEmpty, "a refused acquisition never tries again")
}

private func testWhollyRefusedAcquisition() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    Check.equal(f.states(f.send(.acquisitionRefused(acquired: 0, of: 1))), [.channelHeld],
                "a wholly refused acquisition is a different state from a partial one")
}

// MARK: - Liveness

private func testHeartbeatKeepsBeating() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(try f.sentFrames(f.advance(SessionEngine.heartbeatInterval / 2)), [],
                "beat before the interval elapsed")

    for _ in 0..<5 {
        let frames = try f.sentFrames(f.advance(SessionEngine.heartbeatInterval))
        Check.equal(frames.map(\.type), [MessageType.heartbeat], "missed a beat")
        Check.equal(frames.first?.payload, [], "the heartbeat is not empty")
    }

    Check.equal(f.engine.state, .connected, "beating did not keep the session")
}

private func testUnansweredHello() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    Check.equal(f.advance(SessionEngine.helloTimeout / 2), [], "gave up on the device early")

    let outputs = f.advance(SessionEngine.helloTimeout)
    Check.that(outputs.contains(.closeChannels), "a silent device kept its channels")
    Check.that(!f.retries(outputs).isEmpty, "a silent device was not retried")
    Check.equal(f.engine.state, .quiet, "a silent device was reported to the user")
}

private func testProtocolErrorDropsConnection() throws {
    let f = Fixture()
    try f.establishSession()

    /* 0xEE is not in the message registry — the stream is no longer
       trustworthy, so the connection goes rather than resynchronising. */
    let outputs = f.send(.received([0xEE, 0x00, 0x00, 0x00]))
    Check.that(outputs.contains(.closeChannels), "a protocol error kept the connection")
    Check.that(!f.retries(outputs).isEmpty, "a protocol error did not reconnect")
}

// MARK: - Coming and going

private func testBriefDisappearanceIsSilent() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(f.states(f.send(.deviceDisappeared)), [],
                "the user was told the moment the device blinked")
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow / 2)), [], "told too early")

    /* Back before the window closes: the user never learns it happened. */
    Check.equal(f.states(f.send(.deviceAppeared(.normal))), [],
                "the device returning was itself an announcement")
    Check.equal(f.engine.state, .connected, "a brief disappearance changed the reported state")
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow * 2)), [],
                "a state deferred before the device returned was still reported")
}

private func testLongAbsenceIsReported() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(.deviceDisappeared)
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow)), [.deviceAbsent],
                "a device gone for good was never reported")
}

private func testStartingWithNoDevice() throws {
    /* A LaunchAgent runs at login, which may well be before the device is
       attached — so the same silence applies before anything is said. */
    let f = Fixture()
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow / 2)), [],
                "told the user before waiting out the silence window")
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow)), [.deviceAbsent],
                "a helper that never saw a device said nothing at all")

    /* And it stops saying it the moment one turns up. */
    f.send(.deviceAppeared(.normal))
    Check.equal(f.engine.state, .quiet, "the absence outlived the device arriving")
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow * 2)), [],
                "the absence was reported again after the device arrived")
}

private func testConfigModeIsDistinct() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(f.states(f.send(.deviceAppeared(.configMode))), [],
                "config mode is something the user did — say nothing at first")
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow)), [.deviceInConfigMode],
                "the config-mode identity was never reported")
    Check.unequal(f.engine.state, .deviceAbsent,
                  "seeing the config-mode identity was reported as an absent device")
    Check.that(!f.engine.state.promptsConfigChord, "config mode prompted the chord")
}

private func testConfigModeRoundTrip() throws {
    let f = Fixture()
    try f.establishSession()

    f.send(.deviceAppeared(.configMode))
    _ = f.advance(SessionEngine.silenceWindow)
    Check.equal(f.engine.state, .deviceInConfigMode, "config mode was not reported")

    /* Config mode reboots back under the normal identity, minutes later. */
    f.now += 300
    let outputs = f.send(.deviceAppeared(.normal))
    Check.that(outputs.contains(.openChannels), "the helper did not re-acquire by itself")
    Check.equal(f.engine.state, .quiet, "a stale state survived the device returning")

    f.send(.channelsAcquired(count: 1))
    Check.equal(f.states(f.send(try f.ack(.ok))), [.connected],
                "the session did not come back after a config-mode round trip")
}

private func testChannelsReleasedOnDeparture() throws {
    let f = Fixture()
    try f.establishSession()
    Check.that(f.send(.deviceDisappeared).contains(.closeChannels),
               "the device went and the handles stayed")

    /* And nothing is released twice — there is nothing to release. */
    Check.that(!f.send(.deviceDisappeared).contains(.closeChannels),
               "channels were released twice")
}

// MARK: - Reconnection

private func testBackoffIsCapped() throws {
    let f = Fixture()
    var delays: [TimeInterval] = []
    f.send(.deviceAppeared(.normal))
    for _ in 0..<10 {
        delays += f.retries(f.send(.acquisitionRefused(acquired: 0, of: 1)))
    }

    Check.equal(delays.count, 10, "not every refusal produced a retry")
    Check.that(zip(delays, delays.dropFirst()).allSatisfy { $0 <= $1 },
               "the delay did not grow monotonically: \(delays)")
    Check.that((delays.last ?? 0) > (delays.first ?? 0), "the delay never grew at all")
    Check.that((delays.max() ?? 0) <= 5,
               "the backoff is not capped at a few seconds: \(delays)")
}

private func testWorkingSessionResetsBackoff() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    var delays: [TimeInterval] = []
    for _ in 0..<5 { delays += f.retries(f.send(.acquisitionRefused(acquired: 0, of: 1))) }
    Check.equal(delays.max(), delays.last, "the delay did not grow before the session")

    f.send(.channelsAcquired(count: 1))
    f.send(try f.ack(.ok))
    f.send(.deviceDisappeared)
    f.send(.deviceAppeared(.normal))

    Check.equal(f.retries(f.send(.acquisitionRefused(acquired: 0, of: 1))).first, delays.first,
                "a working session did not reset the reconnection delay")
}
