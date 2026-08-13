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
    ("a device that falls silent ends the session", testSilenceFromTheDeviceEndsTheSession),
    ("any traffic from the device is liveness", testAnyDeviceTrafficIsLiveness),
    ("the device's beat is traced where it changes, not where it beats",
     testDeviceBeatTransitionsAreTraced),
    ("an announced eviction is acted on at once", testSessionEndIsActedOnImmediately),
    ("a session end outside a session is ignored", testSessionEndOutsideSessionIgnored),
    ("an unpaired helper survives the device saying nothing",
     testUnpairedHelperSurvivesTheDeviceSayingNothing),
    ("a lost session is reported only if it stays lost", testALostSessionIsReportedOnlyIfItStaysLost),
    ("a channel holder is not reported as an absent device",
     testAChannelHolderIsNotReportedAsAnAbsentDevice),
    ("bulk needs a session", testBulkNeedsASession),
    ("an unanswered hello is not left half-open", testUnansweredHello),
    ("a protocol error drops the connection", testProtocolErrorDropsConnection),
    ("a failed write drops the connection", testFailedWriteDropsConnection),
    ("a mismatched helper does not beat at a peer that dropped it",
     testMismatchedHelperDoesNotBeat),
    ("a brief disappearance says nothing", testBriefDisappearanceIsSilent),
    ("a long absence is eventually reported", testLongAbsenceIsReported),
    ("starting before the device is attached is reported", testStartingWithNoDevice),
    ("config mode is distinct from an absent device", testConfigModeIsDistinct),
    ("starting while the device is in config mode keeps saying config mode",
     testStartingWhileTheDeviceIsInConfigModeKeepsSayingConfigMode),
    ("a config-mode round trip reconnects by itself", testConfigModeRoundTrip),
    ("the channels are released when the device goes", testChannelsReleasedOnDeparture),
    ("reconnection backs off to a capped delay", testBackoffIsCapped),
    ("a working session resets the backoff", testWorkingSessionResetsBackoff),
    ("a granted secret is stored and pairs the helper", testPairingRoundTrip),
    ("a stored secret is offered on the next hello", testStoredSecretIsOffered),
    ("a grant outside a session is ignored", testGrantOutsideSessionIgnored),
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

    /// A frame arriving from the device, as the transport would deliver it.
    func deviceFrame(_ type: UInt8, payload: [UInt8] = []) throws -> SessionInput {
        .received(try FrameCodec.encode(Frame(type: type, payload: payload)))
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

        /* The device answers all the while. This test is about the helper's
           own beat, not about the detector watching for the device's
           silence — which would otherwise fire three intervals in. */
        f.send(try f.deviceFrame(MessageType.deviceHeartbeat))
    }

    Check.equal(f.engine.state, .connected, "beating did not keep the session")
}

/*
 * The defect #68 was opened for: the device drops a session on its own — its
 * liveness timeout, a framing error on its reader, the config chord — and
 * with a one-way heartbeat the helper went on beating into a device that
 * ignores beats, reporting a session it did not have.
 */
private func testSilenceFromTheDeviceEndsTheSession() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(f.advance(SessionEngine.deviceBeatTimeout - SessionEngine.heartbeatInterval)
                    .filter { $0 == .closeChannels },
                [], "gave up on the device before the timeout")

    let outputs = f.advance(SessionEngine.heartbeatInterval)
    Check.that(outputs.contains(.closeChannels), "a session the device had dropped was kept")
    Check.that(!f.retries(outputs).isEmpty, "a lost session was not reconnected")

    /* And nothing is beaten at afterwards: the session is gone, not stalled. */
    Check.equal(try f.sentFrames(f.advance(SessionEngine.heartbeatInterval * 3)), [],
                "kept beating after the session was lost")
}

/*
 * Liveness is carried by traffic, not by the beat (ADR-0004) — the beat only
 * fills an idle direction. A placement frame this build does not act on still
 * proves the device is alive and holding a session, because the device does
 * not relay for a peer it has no session with.
 */
private func testAnyDeviceTrafficIsLiveness() throws {
    let f = Fixture()
    try f.establishSession()

    for _ in 0..<6 {
        _ = f.advance(SessionEngine.deviceBeatTimeout - SessionEngine.heartbeatInterval)
        f.send(try f.deviceFrame(0x20, payload: [1, 0, 0, 0x80]))
    }

    Check.equal(f.engine.state, .connected,
                "traffic the engine ignores did not count as the device being alive")
    Check.that(f.engine.canSendBulk, "a live session refused to carry bulk")
}

/*
 * ADR-0004 gates the device's beat on an idle direction, so beats stopping
 * during a transfer is the design working — and it looks identical in a log
 * to a device that has stalled. #88 has to tell those apart on hardware, and
 * the helper had no surface for it at all: the beat arrived and was dropped
 * on the floor.
 *
 * Traced at the edges rather than per beat. A line per arrival would be a
 * line a second, which is precisely the log that hid a live defect for two
 * days during this sitting.
 */
private func testDeviceBeatTransitionsAreTraced() throws {
    let f = Fixture()
    try f.establishSession()

    /* The first beat says so, so "the beat never arrived" is distinguishable
       from "the beat was never worth mentioning". */
    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("device heartbeat") },
               "the first beat of a session was not traced")

    /* On time, it says nothing. */
    for _ in 0..<5 {
        _ = f.advance(SessionEngine.heartbeatInterval)
        Check.equal(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat))), [],
                    "a beat arriving on time was traced")
    }

    /* A transfer: the device keeps sending, so the session holds, while the
       idle-gated beat correctly stops. */
    var duringTransfer: [String] = []
    for _ in 0..<4 {
        duringTransfer += f.notes(f.advance(SessionEngine.heartbeatInterval))
        f.send(try f.deviceFrame(0x20, payload: [1, 0, 0, 0x80]))
    }
    Check.equal(f.engine.state, .connected, "a transfer without beats dropped the session")
    Check.equal(duringTransfer.filter { $0.contains("quiet") }.count, 1,
                "the beat falling silent was not traced exactly once")

    /* And the far side of it, with the measurement attached. */
    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("resumed") },
               "the beat coming back was not traced")
}

/* An eviction the device knows about is announced, so the helper need not
   wait out the timeout. The reason is diagnostic: every one takes the same
   recovery, which is what makes an unknown one safe. */
private func testSessionEndIsActedOnImmediately() throws {
    for reason: UInt8 in [1, 2, 0x7F] {
        let f = Fixture()
        try f.establishSession()

        let outputs = f.send(try f.deviceFrame(MessageType.sessionEnd, payload: [reason]))
        Check.that(outputs.contains(.closeChannels),
                   "an announced eviction (reason \(reason)) kept the connection")
        Check.that(!f.retries(outputs).isEmpty,
                   "an announced eviction (reason \(reason)) did not reconnect")
        Check.that(f.notes(outputs).contains { $0.contains("ended the session") },
                   "an announced eviction (reason \(reason)) was not recorded")
    }
}

/* The tail of a session already dropped. Acting on it would tear down a
   reconnection that is already in flight. */
private func testSessionEndOutsideSessionIgnored() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(.transportFailed("link went away"))

    let outputs = f.send(try f.deviceFrame(MessageType.sessionEnd, payload: [1]))
    Check.that(!outputs.contains(.closeChannels), "a stale session end dropped the connection again")
    Check.that(f.retries(outputs).isEmpty, "a stale session end scheduled a second reconnection")
}

/*
 * The regression this design is most exposed to. A helper refused
 * authentication is deliberately kept live — beating and asking — because
 * #46's window can only provision a helper that is connected when the user
 * presses the chord. The device holds no session for it and so sends it
 * nothing, which means a detector keyed on the phase rather than on the
 * session would tear it down every few seconds and break pairing outright.
 */
private func testUnpairedHelperSurvivesTheDeviceSayingNothing() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    f.send(try f.ack(.authenticationFailed))

    var beats = 0
    var asks = 0
    for _ in 0..<8 {
        let types = try f.sentFrames(f.advance(SessionEngine.heartbeatInterval)).map(\.type)
        Check.that(!f.states(f.advance(0)).contains(.deviceAbsent),
                   "an unpaired helper was reported absent")
        beats += types.filter { $0 == MessageType.heartbeat }.count
        asks += types.filter { $0 == MessageType.pairRequest }.count
    }

    Check.equal(f.engine.state, .notPaired,
                "an unpaired helper was torn down by the detector, so the chord could not reach it")
    Check.that(beats >= 4, "an unpaired helper stopped beating")
    Check.that(asks >= 2, "an unpaired helper stopped asking to be paired")
    Check.that(!f.engine.canSendBulk, "an unpaired helper would carry bulk")
}

/*
 * A lost session that comes back quickly is not worth mentioning; one that
 * does not becomes, from the user's side, a device that is not there. Same
 * window and same machinery as a device that physically disappears — no
 * second vocabulary for a second kind of gone.
 */
private func testALostSessionIsReportedOnlyIfItStaysLost() throws {
    let quick = Fixture()
    try quick.establishSession()
    _ = quick.advance(SessionEngine.deviceBeatTimeout)
    Check.equal(quick.states(quick.advance(SessionEngine.silenceWindow / 2)), [],
                "a session that dropped and returned was announced")

    quick.send(.channelsAcquired(count: 1))
    Check.equal(quick.states(quick.send(try quick.ack(.ok))), [],
                "reconnecting inside the window was visible to the user")
    Check.equal(quick.engine.state, .connected, "the reconnected session was not reported")

    let stuck = Fixture()
    try stuck.establishSession()
    _ = stuck.advance(SessionEngine.deviceBeatTimeout)
    Check.equal(stuck.states(stuck.advance(SessionEngine.silenceWindow)), [.deviceAbsent],
                "a session lost for good went on reading as connected")
}

/*
 * A dropped connection defers "the device is absent"; if the reconnection
 * then finds the channel taken, that is a different state with a different
 * remedy, and the stale deferral must not overwrite it five seconds later.
 */
private func testAChannelHolderIsNotReportedAsAnAbsentDevice() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(.transportFailed("link went away"))

    /* The retry comes back to a channel somebody else now holds. */
    Check.equal(f.states(f.send(.acquisitionRefused(acquired: 0, of: 1))), [.channelHeld],
                "a channel holder was not reported")

    Check.equal(f.states(f.advance(SessionEngine.silenceWindow * 2)), [],
                "a stale absence overwrote the channel holder the user can act on")
    Check.equal(f.engine.state, .channelHeld, "the actionable state did not survive")
}

/* Bulk needs a session, and #52 consumes this rather than inventing it. */
private func testBulkNeedsASession() throws {
    let f = Fixture()
    Check.that(!f.engine.canSendBulk, "an idle helper would carry bulk")

    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    Check.that(!f.engine.canSendBulk, "a helper still awaiting an ack would carry bulk")

    f.send(try f.ack(.ok))
    Check.that(f.engine.canSendBulk, "an established session refused to carry bulk")

    f.send(.transportFailed("link went away"))
    Check.that(!f.engine.canSendBulk, "a dropped connection would still carry bulk")
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

/* A frame written in part leaves the device's reader mid-frame, where the
   padding skip does not apply — so a failed write is a dropped connection,
   not a write to shrug off. */
private func testFailedWriteDropsConnection() throws {
    let f = Fixture()
    try f.establishSession()

    let outputs = f.send(.transportFailed("report write failed"))
    Check.that(outputs.contains(.closeChannels), "a failed write kept the connection")
    Check.that(!f.retries(outputs).isEmpty, "a failed write did not reconnect")

    /* And the session is gone: no heartbeat into a desynchronised reader. */
    Check.equal(try f.sentFrames(f.advance(SessionEngine.heartbeatInterval * 3)), [],
                "kept beating after the connection was dropped")
}

/* The device drops the session on a version mismatch, so there is nothing to
   keep alive — beating at a peer that will never answer only looks like one. */
private func testMismatchedHelperDoesNotBeat() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    f.send(try f.ack(.versionIncompatible, version: 2))

    Check.equal(try f.sentFrames(f.advance(SessionEngine.heartbeatInterval * 3)), [],
                "beat at a device that had refused the session")
    Check.equal(f.engine.state, .versionIncompatible, "the state did not survive the ticks")

    /* An unpaired helper is the opposite case: the session is real, and it
       both keeps beating and keeps asking — the window can only provision a
       helper that is connected when the user presses the chord, and it stays
       connected by beating. */
    let unpaired = Fixture()
    unpaired.send(.deviceAppeared(.normal))
    unpaired.send(.channelsAcquired(count: 1))
    unpaired.send(try unpaired.ack(.authenticationFailed))

    let types = try unpaired.sentFrames(unpaired.advance(SessionEngine.heartbeatInterval))
        .map(\.type)
    Check.that(types.contains(MessageType.heartbeat),
               "an unpaired helper stopped beating, so a pairing window could not reach it")
    Check.that(types.contains(MessageType.pairRequest),
               "an unpaired helper never asked to be paired")
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

    /* And it keeps saying so. Config mode lasts as long as the user leaves
       it — up to minutes — so being right for five seconds is not being
       right. */
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow * 4)), [],
                "config mode was reported and then replaced by something else")
    Check.equal(f.engine.state, .deviceInConfigMode, "config mode did not survive the ticks")
}

/*
 * The sequence recorded on hardware in #73, which is the ordinary one at
 * login: a LaunchAgent starts the helper while the device is already in
 * config mode. Config mode *is* the device being present, under its other
 * USB identity — so a helper that has seen it has seen a device, and the
 * "nothing was ever attached" fallback must not fire behind it.
 */
private func testStartingWhileTheDeviceIsInConfigModeKeepsSayingConfigMode() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.configMode))

    /* Silent at first, exactly as a device that blinks is. */
    Check.equal(f.states(f.advance(SessionEngine.silenceWindow / 2)), [],
                "config mode was announced before the window closed")

    Check.equal(f.states(f.advance(SessionEngine.silenceWindow)), [.deviceInConfigMode],
                "config mode was never reported")

    /* The tick right after the deferred state fired is where this broke:
       the fallback saw a helper that had never seen a device and said so. */
    Check.equal(f.states(f.advance(0.25)), [],
                "the tick after config mode was reported replaced it")

    var reported: [HelperState] = []
    for _ in 0..<40 { reported += f.states(f.advance(0.25)) }
    Check.equal(reported, [], "config mode decayed into another state while it was still on")
    Check.equal(f.engine.state, .deviceInConfigMode,
                "the user was told the device was not connected while it sat in config mode")
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

// MARK: - Pairing

/* The window provisions whoever is connected, and the state changing to
   connected is the confirmation the user was told to expect (#34). */
private func testPairingRoundTrip() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    f.send(try f.ack(.authenticationFailed))
    Check.equal(f.engine.state, .notPaired, "the helper did not report being unpaired")

    /* The user presses the chord; the device grants the rotated secret. */
    let granted = [UInt8](repeating: 0xA5, count: SecretStore.length)
    let grant = try FrameCodec.encode(Frame(type: MessageType.pairGrant, payload: granted))
    let outputs = f.send(.received(grant))

    Check.that(outputs.contains(.storeSecret(granted)),
               "the granted secret was not kept — pairing would not survive a restart")

    /* And the helper immediately says hello again, carrying it. */
    let frames = try f.sentFrames(outputs)
    Check.equal(frames.map(\.type), [MessageType.hello], "no fresh hello after being paired")
    let hello = try Hello.decode(payload: frames[0].payload)
    Check.equal(hello.token, granted, "the new hello did not carry the granted secret")

    /* The device accepts it, and *that* is what the user sees. */
    Check.equal(f.states(f.send(try f.ack(.ok))), [.connected],
                "pairing succeeded but the helper never confirmed it visibly")
    Check.that(!f.engine.state.promptsConfigChord, "a paired helper still prompts the chord")

    /* A grant of the wrong size is not a secret. */
    let short = try FrameCodec.encode(Frame(type: MessageType.pairGrant, payload: [1, 2, 3]))
    Check.that(!f.send(.received(short)).contains(where: {
        if case .storeSecret = $0 { return true } else { return false }
    }), "a malformed grant was stored as a secret")
}

private func testStoredSecretIsOffered() throws {
    /* What the helper does on the next launch: the secret comes off disk and
       goes straight into the hello, so a paired helper never asks again. */
    let stored = [UInt8](repeating: 0x5A, count: SecretStore.length)
    let engine = SessionEngine(secret: stored)

    _ = engine.handle(.deviceAppeared(.normal), at: 0)
    let outputs = engine.handle(.channelsAcquired(count: 1), at: 0)

    let sent = outputs.compactMap { output -> [UInt8]? in
        if case .send(let bytes) = output { return bytes } else { return nil }
    }
    guard let first = sent.first else {
        Check.that(false, "no hello was sent")
        return
    }
    let hello = try Hello.decode(payload: FrameCodec.decode(first).frame.payload)
    Check.equal(hello.token, stored, "a stored secret was not offered in the hello")
}

/* A grant arriving after the connection was dropped must not restart a
   handshake down a channel that is closed: the hello would go nowhere and the
   engine would sit in awaitingAck until it timed out, on top of a
   reconnection already scheduled. */
private func testGrantOutsideSessionIgnored() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(.transportFailed("link went away"))

    let granted = [UInt8](repeating: 0xC3, count: SecretStore.length)
    let grant = try FrameCodec.encode(Frame(type: MessageType.pairGrant, payload: granted))
    let outputs = f.send(.received(grant))

    Check.equal(try f.sentFrames(outputs), [], "a grant outside a session started a handshake")
    Check.that(!outputs.contains(.storeSecret(granted)),
               "a grant outside a session was stored")
    Check.that(f.retries(outputs).isEmpty, "a grant outside a session scheduled another retry")
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
