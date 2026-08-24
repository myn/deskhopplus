import DHCore
import DeskhopChannel
import Foundation

/*
 * The session seam: events in, actions out. No device, no IOKit, no wall
 * clock — every test states the time it is asking about.
 *
 * These describe the **helper**, and the decisions behind them belong to the
 * shared core (`dh_helper`, #80). What each one proves here is that the
 * binding carries the event down and the answer back with nothing lost —
 * every number, every frame, every state. The core's own arithmetic is
 * `tests/helper_test.c`'s subject: the beat trace, the backoff, and the
 * correlation guards moved there with the machine (#81).
 *
 * The fixture plays a board with a real P-256 key pair, so the frames it
 * feeds in are tagged the way the firmware tags them. A test that wants to
 * break authentication has to break it deliberately; nothing is waved through.
 */

let helperSessionTests: [(String, () throws -> Void)] = [
    ("the helper introduces itself once it holds every channel", testHelperIntroducesItself),
    ("an acknowledged hello is a connected session", testAcknowledgedHelloIsASession),
    ("an unpaired board and a version mismatch are different states", testFailuresAreDistinct),
    ("a development device is noted", testDevelopmentDeviceIsNoted),
    ("a grant carrying an unusable key is not kept", testAGrantWithAnUnusableKeyIsNotKept),
    ("a refused open is reported as an unusable device",
     testARefusedOpenIsReportedAsAnUnusableDevice),
    ("a refused open that keeps failing is still reported",
     testARefusedOpenThatKeepsFailingIsStillReported),
    ("a partial acquisition that completes says nothing",
     testAPartialAcquisitionThatCompletesIsSilent),
    ("the heartbeat beats at the interval the device measures", testHeartbeatKeepsBeating),
    ("a device that falls silent ends the session", testSilenceFromTheDeviceEndsTheSession),
    ("any authenticated traffic from the device is liveness", testAnyDeviceTrafficIsLiveness),
    ("an unauthenticated frame is not liveness", testUnauthenticatedTrafficIsNotLiveness),
    ("a listener the board detected is reported", testListenerAlertIsReported),
    ("an announced eviction is acted on at once", testSessionEndIsActedOnImmediately),
    ("a session end outside a session is ignored", testSessionEndOutsideSessionIgnored),
    ("an unpaired helper survives the device saying nothing",
     testUnpairedHelperSurvivesTheDeviceSayingNothing),
    ("a lost session is reported only if it stays lost", testALostSessionIsReportedOnlyIfItStaysLost),
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
    ("a granted board key is pinned and pairs the helper", testPairingRoundTrip),
    ("a pinned board key authenticates the next hello", testPinnedBoardKeyAuthenticatesTheHello),
    ("a board that has forgotten us keeps the pin", testUnpairedRefusalKeepsThePin),
    ("a board whose key changed is not silently accepted",
     testABoardWhoseKeyChangedIsNotSilentlyAccepted),
    ("a grant outside a session is ignored", testGrantOutsideSessionIgnored),
    ("a connection rebuilt over and over is not reported as connected",
     testRepeatedReconnectionIsNotReportedAsConnected),
    ("a link that keeps re-enumerating is the same reading", testAFlappingLinkIsTheSameReading),
    ("a handshake that never completes is reported too",
     testAHandshakeThatNeverCompletesIsReported),
    ("a stuck handshake still asks to be paired", testAStuckHandshakeStillAsksToBePaired),
    ("an occasional reconnection is an ordinary recovery",
     testAnOccasionalReconnectionIsAnOrdinaryRecovery),
    ("a connection that then holds goes back to connected",
     testAConnectionThatHoldsGoesBackToConnected),
    ("every state crosses the seam with its words and its policy",
     testEveryStateCrossesTheSeamIntact),
    ("a hello that cannot be built is still reported",
     testAHelloThatCannotBeBuiltIsStillReported),
    ("a grant nobody asked for is ignored", testAGrantNobodyAskedForIsIgnored),
    ("bulk goes out under the session's own counter", testBulkGoesOutUnderTheSessionsCounter),
]

/// The fixed key pairs the fixtures run on. Fixed so a failure reproduces
/// exactly; a bad one is a typo in this file, not a case to handle.
private func keyPair(_ privateKey: ClosedRange<UInt8>) -> TestIdentity {
    guard let identity = TestIdentity(privateKey: [UInt8](privateKey)) else {
        preconditionFailure("the fixture's private key is not a valid scalar")
    }
    return identity
}

private enum FixtureError: Error {
    /// The board was asked to answer a hello the helper has not sent yet.
    case noHelloOnTheWire
    /// The board was asked to tag a frame before a session existed.
    case noSessionKey
}

/// The body of an authenticated frame, read without verifying its tag. Tests
/// use it to look at what a frame says; the tag is checked where it is the
/// subject, not everywhere a payload is read.
private func unverifiedBody(_ frame: Frame) -> [UInt8] {
    Array(frame.payload.dropFirst(Int(DH_FRAME_AUTH_PREFIX_SIZE)))
}

/// One session, one clock, one board, and the small vocabulary the tests read
/// outputs with.
private final class Fixture {
    let helperIdentity: TestIdentity
    /// The board's key pair. `boardIdentity.publicKey` is what a PAIR_GRANT
    /// carries and what a paired helper has pinned.
    let boardIdentity: TestIdentity
    let session: HelperSession
    var now: TimeInterval = 1000

    /* What the helper last put on the wire. The board needs the correlation
       and the nonce to answer, exactly as the firmware does. */
    private(set) var lastHello: Hello?
    private(set) var lastPairRequest: PairRequest?

    /* The board's side of the session. Reset every time a hello is answered,
       because every hello starts a new session. */
    private var kB2H: [UInt8]?
    private var kH2B: [UInt8]?
    private var boardTxCounter: UInt64 = 0
    private var boardRxCounter = dh_auth_counter()

    /// `paired: false` is a helper with nothing pinned — the state it is in on
    /// a first run, or after the board has forgotten it.
    init(paired: Bool = true) {
        let helper = keyPair(0x01...0x20)
        let board = keyPair(0x21...0x40)
        helperIdentity = helper
        boardIdentity = board

        /* Deterministic entropy: distinct on every call, so two hellos never
           share a nonce or a correlation, and a test can still reproduce a
           failure exactly. */
        var tick = 0
        session = HelperSession(
            identity: helper,
            boardPublicKey: paired ? board.publicKey : nil,
            entropy: { count in
                tick += 1
                return (0..<count).map { UInt8(($0 + tick) & 0xFF) }
            })
        dh_auth_counter_init(&boardRxCounter)
    }

    @discardableResult
    func send(_ input: SessionInput) -> [SessionOutput] {
        observe(session.handle(input, at: now))
    }

    func advance(_ seconds: TimeInterval) -> [SessionOutput] {
        now += seconds
        return observe(session.handle(.tick, at: now))
    }

    /// Everything the helper sends passes through here, so the board always
    /// knows which question it is answering.
    private func observe(_ outputs: [SessionOutput]) -> [SessionOutput] {
        for output in outputs {
            guard case .send(let bytes) = output,
                  let frame = try? FrameCodec.decode(bytes).frame else { continue }
            switch frame.type {
            case MessageType.hello:
                lastHello = try? Hello.decode(body: unverifiedBody(frame))
            case MessageType.pairRequest:
                lastPairRequest = try? PairRequest.decode(payload: frame.payload)
            default:
                break
            }
        }
        return outputs
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

    // MARK: - The board's answers

    /*
     * The board's HELLO_ACK, encoded and tagged the way the firmware does it.
     * The derivation is symmetric — the board runs ECDH against the *helper's*
     * public key and reaches the same shared secret — so `peer` is the
     * helper's key here.
     */
    func ack(channels: UInt8 = 1,
             chunk: UInt16 = 1024,
             build: BuildType = .release,
             version: UInt16 = UInt16(DH_PROTO_VERSION),
             correlation: UInt64? = nil) throws -> SessionInput {
        guard let hello = lastHello else { throw FixtureError.noHelloOnTheWire }

        let boardNonce = (0..<Int(DH_NONCE_SIZE)).map { UInt8(($0 + Int(now)) & 0xFF) }
        guard let keys = boardIdentity.sessionKeys(peer: helperIdentity.publicKey,
                                                   helperNonce: hello.helperNonce,
                                                   boardNonce: boardNonce) else {
            throw FixtureError.noSessionKey
        }

        kB2H = keys.kB2H
        kH2B = keys.kH2B
        boardTxCounter = 0
        dh_auth_counter_init(&boardRxCounter)

        let ack = HelloAck(correlation: correlation ?? hello.correlation,
                           protocolVersion: version,
                           buildType: build,
                           channelCount: channels,
                           maxChunk: chunk,
                           boardNonce: boardNonce)
        let bytes = try ack.encoded(key: keys.kB2H, counter: boardTxCounter)
        boardTxCounter += 1
        return .received(bytes)
    }

    /// The board refusing a hello. Untagged — a refusal has no session behind it.
    func helloRefused(_ status: HelloRefusedStatus,
                      version: UInt16 = UInt16(DH_PROTO_VERSION),
                      correlation: UInt64? = nil) throws -> SessionInput {
        guard let hello = lastHello else { throw FixtureError.noHelloOnTheWire }
        return .received(try HelloRefused(correlation: correlation ?? hello.correlation,
                                          protocolVersion: version,
                                          status: status).encoded())
    }

    /// The board granting a pairing. Untagged, and echoing the correlation the
    /// helper chose — which is what a manufactured grant cannot do (#108).
    func pairGrant(correlation: UInt64? = nil, key: [UInt8]? = nil) throws -> SessionInput {
        let echoed = correlation ?? lastPairRequest?.correlation ?? 0
        return .received(try PairGrant(correlation: echoed,
                                       boardPublicKey: key ?? boardIdentity.publicKey).encoded())
    }

    /// A frame arriving from the device, tagged under the session's k_b2h.
    func deviceFrame(_ type: UInt8, body: [UInt8] = []) throws -> SessionInput {
        guard let key = kB2H else { throw FixtureError.noSessionKey }
        let bytes = try AuthFrame.wrap(type: type, body: body, key: key, counter: boardTxCounter)
        boardTxCounter += 1
        return .received(bytes)
    }

    /// The same frame with no valid tag: the payload a bystander can write
    /// without holding the session key.
    func untaggedDeviceFrame(_ type: UInt8, body: [UInt8] = []) throws -> SessionInput {
        let payload = [UInt8](repeating: 0, count: Int(DH_FRAME_AUTH_PREFIX_SIZE)) + body
        return .received(try FrameCodec.encode(Frame(type: type, payload: payload)))
    }

    /// Verify a frame the *helper* sent, under k_h2b, and return its body.
    /// This is the board's check, and it is what proves the helper tags.
    func openFromHelper(_ frame: Frame) throws -> [UInt8] {
        guard let key = kH2B else { throw FixtureError.noSessionKey }
        return try AuthFrame.open(frame: frame, key: key, counter: &boardRxCounter)
    }

    // MARK: - Sequences

    /// Device present, every channel seized, hello answered.
    func establishSession() throws {
        send(.deviceAppeared(.normal))
        try reacquire()
    }

    /// The recovery the core asks for after a drop: channels back, hello answered.
    @discardableResult
    func reacquire() throws -> [SessionOutput] {
        send(.channelsAcquired(count: 1))
        return send(try ack())
    }

    /*
     * The unpaired path as far as a pair request on the wire. The board
     * refuses the hello because it has no registration for this key id; the
     * helper clears its pin, says so, and starts asking on the next tick.
     */
    func becomeUnpaired() throws {
        send(try helloRefused(.unpaired))
        _ = advance(0)
    }

    /*
     * One turn of the loop #94 is about: a frame this build cannot decode
     * (0xEE is not in the message registry), the gap before the retry, and
     * the reconnection that succeeds — the success being what put `connected`
     * back on the screen each time.
     */
    @discardableResult
    func dropAndReconnect(after gap: TimeInterval = 0.75) throws -> [SessionOutput] {
        var outputs = send(.received([0xEE, 0x00, 0x00, 0x00]))
        outputs += advance(gap)
        return outputs + (try reacquire())
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
    let hello = try Hello.decode(body: unverifiedBody(first))
    Check.equal(hello.protocolVersion, UInt16(DH_PROTO_VERSION),
                "the hello does not carry the protocol version")
    Check.equal(hello.os, 1, "the hello does not carry the platform")

    /* v2 identifies the helper by the id of its key, never by a shared secret
       the wire could carry away. */
    Check.equal(hello.helperKeyId, f.helperIdentity.keyId,
                "the hello does not name the key this helper holds")
    Check.unequal(hello.correlation, 0, "the hello carries no correlation value")

    /* Nothing is claimed to the user until the device has answered. */
    Check.equal(f.session.state, .quiet, "reported a session before the device answered")
}

private func testAcknowledgedHelloIsASession() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    Check.equal(f.states(f.send(try f.ack(channels: 1, chunk: 512))), [.connected],
                "an acknowledged hello did not become a connected session")
    Check.equal(f.session.negotiated?.channelCount, 1, "channel count not taken from the ack")
    Check.equal(f.session.negotiated?.maxChunk, 512,
                "the session must run on the effective value, not the requested one")
}

private func testFailuresAreDistinct() throws {
    let unpaired = Fixture(paired: false)
    unpaired.send(.deviceAppeared(.normal))
    unpaired.send(.channelsAcquired(count: 1))
    Check.equal(unpaired.states(unpaired.send(try unpaired.helloRefused(.unpaired))),
                [.notPaired], "an unpaired board was not reported as unpaired")
    Check.that(unpaired.session.state.promptsConfigChord,
               "an unpaired helper must be told which keystroke fixes it")
    Check.that(unpaired.session.negotiated == nil,
               "an unpaired session must not claim negotiated terms")

    let mismatched = Fixture()
    mismatched.send(.deviceAppeared(.normal))
    mismatched.send(.channelsAcquired(count: 1))
    Check.equal(mismatched.states(mismatched.send(
                    try mismatched.helloRefused(.versionIncompatible, version: 3))),
                [.versionIncompatible], "a version mismatch was not reported as one")
    Check.unequal(mismatched.session.state, .notPaired,
                  "a version mismatch was reported as an unpaired helper")
    Check.that(!mismatched.session.state.allowsBulkTransfers,
               "a mismatched helper must refuse transfers rather than corrupt a file")
}

private func testDevelopmentDeviceIsNoted() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    let outputs = f.send(try f.ack(build: .development))

    Check.equal(f.session.negotiated?.deviceBuild, .development, "the build type was not recorded")
    Check.that(!f.notes(outputs).isEmpty, "a development build must identify itself in the log")
}

// MARK: - Correlation (#108)

/*
 * A grant that echoes the right correlation but carries something that is not
 * a point on the curve — a corrupted frame, or a board that got it wrong. The
 * key must not be kept: a helper holding one it cannot run ECDH against fails
 * every later hello, and nothing in the pairing path would ever replace it.
 */
private func testAGrantWithAnUnusableKeyIsNotKept() throws {
    let f = Fixture(paired: false)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    try f.becomeUnpaired()

    let rubbish = [UInt8](repeating: 0xA5, count: Int(DH_P256_PUBLIC_SIZE))
    let outputs = f.send(try f.pairGrant(key: rubbish))
    Check.that(!outputs.contains(where: {
        if case .storeBoardKey = $0 { return true } else { return false }
    }), "a key that is not on the curve was pinned")
    Check.equal(try f.sentFrames(outputs), [], "an unusable key produced a hello")

    /* And the helper recovers: it is still asking, and the next real grant works. */
    _ = f.advance(HelperSession.pairingRetryInterval)
    Check.that(f.send(try f.pairGrant()).contains(.storeBoardKey(f.boardIdentity.publicKey)),
               "an unusable grant left the helper unable to pair at all")
}

// MARK: - A refused open

/*
 * A refused open is not "another program holds the channel" — on macOS a second
 * seize open succeeds, so nothing else can refuse this one, and the state that
 * said so is gone (#114, ADR-0008). It is a device this helper cannot use, and
 * it is reported as one: nothing at first, because a partial acquisition is
 * ordinary, and an absence if it persists.
 */
private func testARefusedOpenIsReportedAsAnUnusableDevice() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    let outputs = f.send(.acquisitionRefused(acquired: 1, of: 2))

    Check.equal(f.states(outputs), [], "a refusal was reported before it had lasted")
    Check.that(outputs.contains(.closeChannels), "a partially acquired set was not released")
    Check.that(!f.retries(outputs).isEmpty, "a refused acquisition never tries again")

    Check.equal(f.states(f.advance(HelperSession.silenceWindow)), [.deviceAbsent],
                "a device that never opens was never reported at all")
    Check.that(!f.session.state.promptsConfigChord, "a refused open prompted the chord")
}

/*
 * The retry cadence must not push the report out for ever. The backoff caps
 * *below* the silence window, so a deferral re-armed on every refusal would
 * never come due — and a device that can never be opened would say nothing at
 * all, which is the one outcome this deferral exists to prevent.
 */
private func testARefusedOpenThatKeepsFailingIsStillReported() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))

    var reported: [HelperState] = []
    for _ in 0..<10 {
        f.send(.acquisitionRefused(acquired: 0, of: 1))
        reported += f.states(f.advance(HelperSession.backoffCap))
    }

    Check.equal(reported, [.deviceAbsent],
                "a device that never opens was never reported, or was reported over and over")
}

/* The retry that completes clears the deferral before it comes due — #63's
   channel nodes arriving one at a time, which is not a fault to report. */
private func testAPartialAcquisitionThatCompletesIsSilent() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.acquisitionRefused(acquired: 1, of: 2))
    _ = f.advance(HelperSession.silenceWindow / 2)

    /* Only the acquisition clears the deferral here: `deviceAppeared` would
       clear it too, and going through it would prove nothing. */
    try f.reacquire()
    Check.equal(f.states(f.advance(HelperSession.silenceWindow)), [],
                "a partial acquisition that completed was reported as an absent device")
    Check.equal(f.session.state, .connected, "the session that came up was not reported")
}

// MARK: - Liveness

private func testHeartbeatKeepsBeating() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(try f.sentFrames(f.advance(HelperSession.heartbeatInterval / 2)), [],
                "beat before the interval elapsed")

    for _ in 0..<5 {
        let frames = try f.sentFrames(f.advance(HelperSession.heartbeatInterval))
        Check.equal(frames.map(\.type), [MessageType.heartbeat], "missed a beat")
        guard let beat = frames.first else { return }

        /* v2: the board only counts a beat it can verify, so a beat that is
           not tagged under k_h2b is not a beat at all. The board's own check
           is the assertion — and it consumes the counter, so a replayed beat
           would fail the next time round this loop. */
        Check.equal(try f.openFromHelper(beat), [], "the heartbeat body is not empty")

        /* The device answers all the while. This test is about the helper's
           own beat, not about the detector watching for the device's
           silence — which would otherwise fire three intervals in. */
        f.send(try f.deviceFrame(MessageType.deviceHeartbeat))
    }

    Check.equal(f.session.state, .connected, "beating did not keep the session")
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

    Check.equal(f.advance(HelperSession.deviceBeatTimeout - HelperSession.heartbeatInterval)
                    .filter { $0 == .closeChannels },
                [], "gave up on the device before the timeout")

    let outputs = f.advance(HelperSession.heartbeatInterval)
    Check.that(outputs.contains(.closeChannels), "a session the device had dropped was kept")
    Check.that(!f.retries(outputs).isEmpty, "a lost session was not reconnected")

    /* And nothing is beaten at afterwards: the session is gone, not stalled. */
    Check.equal(try f.sentFrames(f.advance(HelperSession.heartbeatInterval * 3)), [],
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
        _ = f.advance(HelperSession.deviceBeatTimeout - HelperSession.heartbeatInterval)
        f.send(try f.deviceFrame(0x20, body: [1, 0, 0, 0x80]))
    }

    Check.equal(f.session.state, .connected,
                "traffic the machine ignores did not count as the device being alive")
    Check.that(f.session.canSendBulk, "a live session refused to carry bulk")
}

/*
 * The other half of that rule, and the defect #95 names. Under v1 *any* frame
 * on the channel reset the liveness clock, so a bystander writing rubbish
 * into the device's endpoint kept a dead session looking alive indefinitely.
 * Under v2 only a frame that verifies counts, and an untagged one is dropped
 * before it can touch the clock.
 */
private func testUnauthenticatedTrafficIsNotLiveness() throws {
    let f = Fixture()
    try f.establishSession()

    /* A single forged beat. `docs/protocol.md` makes this the helper's rule:
       only the device emits device→helper reports, so a failed tag means the
       board is not the one we paired with or the stream is corrupt — the
       connection goes, and nothing is replied. */
    let outputs = f.send(try f.untaggedDeviceFrame(MessageType.deviceHeartbeat))
    Check.that(outputs.contains(.closeChannels), "a frame that failed its tag kept the connection")
    Check.that(!f.retries(outputs).isEmpty, "a frame that failed its tag did not reconnect")
    Check.equal(try f.sentFrames(outputs), [],
                "the helper answered a frame whose tag failed — the answer reaches every "
                + "attached client")
    Check.that(!f.session.canSendBulk, "a session torn down by a bad tag would still carry bulk")

    /* And it never counted as the device being alive: there is no session left
       for a liveness clock to be measuring. This is #95's fix — under v1 any
       frame on the channel reset that clock, so a bystander writing rubbish
       kept a dead session reading as healthy indefinitely. */
    Check.equal(try f.sentFrames(f.advance(HelperSession.heartbeatInterval * 3)), [],
                "kept beating after a forged frame dropped the session")
}

/* The board watches for a process probing the channel and says so. The helper
   has to surface it: a silent detector tells the user nothing. */
private func testListenerAlertIsReported() throws {
    let f = Fixture()
    try f.establishSession()

    /* window_ms = 10000, refused = 4, little-endian, as dh_listener_alert. */
    let body: [UInt8] = [0x10, 0x27, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00]
    let outputs = f.send(try f.deviceFrame(MessageType.listenerAlert, body: body))

    Check.equal(f.states(outputs), [.listenerDetected], "a detected listener was not reported")
    Check.that(f.notes(outputs).contains { $0.contains("4") && $0.contains("10000") },
               "the measurement behind the alert was not recorded")
    Check.that(!f.session.state.promptsConfigChord,
               "a listener prompted the chord, which would provision whoever is listening")

    /* The words, not only the flag. This is the state that replaced
       `channelHeld` (#114), and #38 asks each one for what it is *and* what to
       do — including the one thing the user must not do here. */
    let message = f.session.state.message ?? ""
    Check.that(message.contains("writing to the device channel"),
               "the state does not say what was detected")
    Check.that(message.contains("find and stop it"), "the state names no remedy")
    Check.that(message.lowercased().contains("do not press the config chord"),
               "the state does not warn off the chord, which is what would pair the listener")

    /* The session is untouched. What the board detected is somebody *writing*
       frames it refused, which the tag already keeps out — so this changes
       what the user is told, not what the session may carry, and the two
       seams #52 reads must not disagree. */
    Check.that(f.session.canSendBulk, "a reported listener revoked a session that is still good")
    Check.equal(f.session.state.allowsBulkTransfers, f.session.canSendBulk,
                "allowsBulkTransfers and canSendBulk gave #52 opposite answers")

    /* And it expires like the rate it is. The board measured it over a window
       and said so; nothing further arriving means whatever was writing has
       stopped, and holding the warning up for the rest of the session would
       make it a latch — the mistake #94 corrected for reconnections. */
    var reported: [HelperState] = []
    for _ in 0..<12 {
        reported += f.states(f.advance(HelperSession.heartbeatInterval))
        f.send(try f.deviceFrame(MessageType.deviceHeartbeat))
    }
    Check.equal(reported, [.connected],
                "the listener warning never cleared, though nothing further was refused")
}

/* An eviction the device knows about is announced, so the helper need not
   wait out the timeout. The reason is diagnostic: every one takes the same
   recovery, which is what makes an unknown one safe. */
private func testSessionEndIsActedOnImmediately() throws {
    for reason: UInt8 in [1, 2, 0x7F] {
        let f = Fixture()
        try f.establishSession()

        let outputs = f.send(try f.deviceFrame(MessageType.sessionEnd, body: [reason]))
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
    let stray = try f.deviceFrame(MessageType.sessionEnd, body: [1])
    f.send(.transportFailed("link went away"))

    let outputs = f.send(stray)
    Check.that(!outputs.contains(.closeChannels), "a stale session end dropped the connection again")
    Check.that(f.retries(outputs).isEmpty, "a stale session end scheduled a second reconnection")
}

/*
 * The regression this design is most exposed to. An unpaired helper is
 * deliberately kept live — asking, over and over — because #46's window can
 * only provision a helper that is connected when the user presses the chord.
 * The device holds no session for it and so sends it nothing, which means a
 * detector keyed on the phase rather than on the session would tear it down
 * every few seconds and break pairing outright.
 *
 * v2 changes what it sends while it waits. There is no session key yet, so
 * there is nothing to beat with — the PAIR_REQUEST is untagged by design and
 * is the only thing on the wire.
 */
private func testUnpairedHelperSurvivesTheDeviceSayingNothing() throws {
    let f = Fixture(paired: false)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    f.send(try f.helloRefused(.unpaired))

    var beats = 0
    var asks = 0
    for _ in 0..<8 {
        let types = try f.sentFrames(f.advance(HelperSession.heartbeatInterval)).map(\.type)
        Check.that(!f.states(f.advance(0)).contains(.deviceAbsent),
                   "an unpaired helper was reported absent")
        beats += types.filter { $0 == MessageType.heartbeat }.count
        asks += types.filter { $0 == MessageType.pairRequest }.count
    }

    Check.equal(f.session.state, .notPaired,
                "an unpaired helper was torn down by the detector, so the chord could not reach it")
    Check.that(asks >= 2, "an unpaired helper stopped asking to be paired")
    Check.equal(beats, 0,
                "an unpaired helper beat with a session key it cannot have — a beat it could "
                + "send unauthenticated is a beat anybody could send")
    Check.that(!f.session.canSendBulk, "an unpaired helper would carry bulk")
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
    _ = quick.advance(HelperSession.deviceBeatTimeout)
    Check.equal(quick.states(quick.advance(HelperSession.silenceWindow / 2)), [],
                "a session that dropped and returned was announced")

    quick.send(.channelsAcquired(count: 1))
    Check.equal(quick.states(quick.send(try quick.ack())), [],
                "reconnecting inside the window was visible to the user")
    Check.equal(quick.session.state, .connected, "the reconnected session was not reported")

    let stuck = Fixture()
    try stuck.establishSession()
    _ = stuck.advance(HelperSession.deviceBeatTimeout)
    Check.equal(stuck.states(stuck.advance(HelperSession.silenceWindow)), [.deviceAbsent],
                "a session lost for good went on reading as connected")
}

/* Bulk needs a session, and #52 consumes this rather than inventing it. */
private func testBulkNeedsASession() throws {
    let f = Fixture()
    Check.that(!f.session.canSendBulk, "an idle helper would carry bulk")

    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    Check.that(!f.session.canSendBulk, "a helper still awaiting an ack would carry bulk")

    f.send(try f.ack())
    Check.that(f.session.canSendBulk, "an established session refused to carry bulk")

    f.send(.transportFailed("link went away"))
    Check.that(!f.session.canSendBulk, "a dropped connection would still carry bulk")
}

private func testUnansweredHello() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    Check.equal(f.advance(HelperSession.helloTimeout / 2), [], "gave up on the device early")

    let outputs = f.advance(HelperSession.helloTimeout)
    Check.that(outputs.contains(.closeChannels), "a silent device kept its channels")
    Check.that(!f.retries(outputs).isEmpty, "a silent device was not retried")
    Check.equal(f.session.state, .quiet, "a silent device was reported to the user")
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

    /* The core takes the failure, not the sentence — there is no field on
       `dh_helper_transport_failed` for one. So the reason only reaches the log
       if the binding carries it there itself, and a log that says "transport
       failed" without saying how is the log that hid #93 for two days. */
    Check.that(f.notes(outputs).contains { $0.contains("report write failed") },
               "the transport's own reason never reached the log")

    /* And the session is gone: no heartbeat into a desynchronised reader. */
    Check.equal(try f.sentFrames(f.advance(HelperSession.heartbeatInterval * 3)), [],
                "kept beating after the connection was dropped")
}

/* The device drops the session on a version mismatch, so there is nothing to
   keep alive — beating at a peer that will never answer only looks like one. */
private func testMismatchedHelperDoesNotBeat() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    f.send(try f.helloRefused(.versionIncompatible, version: 3))

    Check.equal(try f.sentFrames(f.advance(HelperSession.heartbeatInterval * 3)), [],
                "beat at a device that had refused the session")
    Check.equal(f.session.state, .versionIncompatible, "the state did not survive the ticks")

    /* An unpaired helper is the opposite case: it is deliberately live and
       keeps asking, because the window can only provision a helper that is
       connected when the user presses the chord. */
    let unpaired = Fixture(paired: false)
    unpaired.send(.deviceAppeared(.normal))
    unpaired.send(.channelsAcquired(count: 1))
    unpaired.send(try unpaired.helloRefused(.unpaired))

    let types = try unpaired.sentFrames(unpaired.advance(HelperSession.heartbeatInterval))
        .map(\.type)
    Check.that(types.contains(MessageType.pairRequest),
               "an unpaired helper never asked to be paired")
}

// MARK: - Coming and going

private func testBriefDisappearanceIsSilent() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(f.states(f.send(.deviceDisappeared)), [],
                "the user was told the moment the device blinked")
    Check.equal(f.states(f.advance(HelperSession.silenceWindow / 2)), [], "told too early")

    /* Back before the window closes: the user never learns it happened. */
    Check.equal(f.states(f.send(.deviceAppeared(.normal))), [],
                "the device returning was itself an announcement")
    Check.equal(f.session.state, .connected, "a brief disappearance changed the reported state")
    Check.equal(f.states(f.advance(HelperSession.silenceWindow * 2)), [],
                "a state deferred before the device returned was still reported")
}

private func testLongAbsenceIsReported() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(.deviceDisappeared)
    Check.equal(f.states(f.advance(HelperSession.silenceWindow)), [.deviceAbsent],
                "a device gone for good was never reported")
}

private func testStartingWithNoDevice() throws {
    /* A LaunchAgent runs at login, which may well be before the device is
       attached — so the same silence applies before anything is said. */
    let f = Fixture()
    Check.equal(f.states(f.advance(HelperSession.silenceWindow / 2)), [],
                "told the user before waiting out the silence window")
    Check.equal(f.states(f.advance(HelperSession.silenceWindow)), [.deviceAbsent],
                "a helper that never saw a device said nothing at all")

    /* And it stops saying it the moment one turns up. */
    f.send(.deviceAppeared(.normal))
    Check.equal(f.session.state, .quiet, "the absence outlived the device arriving")
    Check.equal(f.states(f.advance(HelperSession.silenceWindow * 2)), [],
                "the absence was reported again after the device arrived")
}

private func testConfigModeIsDistinct() throws {
    let f = Fixture()
    try f.establishSession()

    Check.equal(f.states(f.send(.deviceAppeared(.configMode))), [],
                "config mode is something the user did — say nothing at first")
    Check.equal(f.states(f.advance(HelperSession.silenceWindow)), [.deviceInConfigMode],
                "the config-mode identity was never reported")
    Check.unequal(f.session.state, .deviceAbsent,
                  "seeing the config-mode identity was reported as an absent device")
    Check.that(!f.session.state.promptsConfigChord, "config mode prompted the chord")

    /* And it keeps saying so. Config mode lasts as long as the user leaves
       it — up to minutes — so being right for five seconds is not being
       right. */
    Check.equal(f.states(f.advance(HelperSession.silenceWindow * 4)), [],
                "config mode was reported and then replaced by something else")
    Check.equal(f.session.state, .deviceInConfigMode, "config mode did not survive the ticks")
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
    Check.equal(f.states(f.advance(HelperSession.silenceWindow / 2)), [],
                "config mode was announced before the window closed")

    Check.equal(f.states(f.advance(HelperSession.silenceWindow)), [.deviceInConfigMode],
                "config mode was never reported")

    /* The tick right after the deferred state fired is where this broke:
       the fallback saw a helper that had never seen a device and said so. */
    Check.equal(f.states(f.advance(0.25)), [],
                "the tick after config mode was reported replaced it")

    var reported: [HelperState] = []
    for _ in 0..<40 { reported += f.states(f.advance(0.25)) }
    Check.equal(reported, [], "config mode decayed into another state while it was still on")
    Check.equal(f.session.state, .deviceInConfigMode,
                "the user was told the device was not connected while it sat in config mode")
}

private func testConfigModeRoundTrip() throws {
    let f = Fixture()
    try f.establishSession()

    f.send(.deviceAppeared(.configMode))
    _ = f.advance(HelperSession.silenceWindow)
    Check.equal(f.session.state, .deviceInConfigMode, "config mode was not reported")

    /* Config mode reboots back under the normal identity, minutes later. */
    f.now += 300
    let outputs = f.send(.deviceAppeared(.normal))
    Check.that(outputs.contains(.openChannels), "the helper did not re-acquire by itself")
    Check.equal(f.session.state, .quiet, "a stale state survived the device returning")

    f.send(.channelsAcquired(count: 1))
    Check.equal(f.states(f.send(try f.ack())), [.connected],
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
    let f = Fixture(paired: false)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    try f.becomeUnpaired()
    Check.equal(f.session.state, .notPaired, "the helper did not report being unpaired")

    /* The helper's request carries its public key — never a private half, and
       never a secret the board has to keep. */
    Check.equal(f.lastPairRequest?.helperPublicKey, f.helperIdentity.publicKey,
                "the pair request did not offer this helper's public key")

    /* The user presses the chord; the device grants its own public key. */
    let outputs = f.send(try f.pairGrant())
    Check.that(outputs.contains(.storeBoardKey(f.boardIdentity.publicKey)),
               "the board's key was not pinned — pairing would not survive a restart")

    /* And the helper immediately says hello again, under the pinned key. */
    let frames = try f.sentFrames(outputs)
    Check.equal(frames.map(\.type), [MessageType.hello], "no fresh hello after being paired")
    guard let hello = frames.first else { return }
    guard let key = try f.boardIdentity.helloKey(
        peer: f.helperIdentity.publicKey,
        helperNonce: Hello.decode(body: unverifiedBody(hello)).helperNonce) else {
        Check.that(false, "the board could not derive a key against the hello it was sent")
        return
    }
    var counter = dh_auth_counter()
    dh_auth_counter_init(&counter)
    _ = Check.doesNotThrow("the hello after pairing was not authenticated under the granted key") {
        try AuthFrame.open(frame: hello, key: key, counter: &counter)
    }

    /* The device accepts it, and *that* is what the user sees. */
    Check.equal(f.states(f.send(try f.ack())), [.connected],
                "pairing succeeded but the helper never confirmed it visibly")
    Check.that(!f.session.state.promptsConfigChord, "a paired helper still prompts the chord")
}

/*
 * What the helper does on the next launch: the pinned key comes off disk and
 * is what the hello is authenticated under, so a paired helper never asks
 * again. The board's own check is the assertion — and the unpaired helper
 * beside it fails that same check, which is what makes it meaningful.
 */
private func testPinnedBoardKeyAuthenticatesTheHello() throws {
    func helloFrom(_ f: Fixture) throws -> Frame {
        f.send(.deviceAppeared(.normal))
        let frames = try f.sentFrames(f.send(.channelsAcquired(count: 1)))
        return frames[0]
    }

    let paired = Fixture(paired: true)
    let pairedHello = try helloFrom(paired)
    guard let pairedKey = try paired.boardIdentity.helloKey(
        peer: paired.helperIdentity.publicKey,
        helperNonce: Hello.decode(body: unverifiedBody(pairedHello)).helperNonce) else {
        Check.that(false, "the board could not derive a key against a paired helper's hello")
        return
    }
    var counter = dh_auth_counter()
    dh_auth_counter_init(&counter)
    _ = Check.doesNotThrow("a pinned board key did not authenticate the hello") {
        try AuthFrame.open(frame: pairedHello, key: pairedKey, counter: &counter)
    }

    let fresh = Fixture(paired: false)
    let freshHello = try helloFrom(fresh)
    guard let freshKey = try fresh.boardIdentity.helloKey(
        peer: fresh.helperIdentity.publicKey,
        helperNonce: Hello.decode(body: unverifiedBody(freshHello)).helperNonce) else {
        Check.that(false, "the board could not derive a key against a fresh helper's hello")
        return
    }
    var freshCounter = dh_auth_counter()
    dh_auth_counter_init(&freshCounter)
    var verified = true
    do {
        _ = try AuthFrame.open(frame: freshHello, key: freshKey, counter: &freshCounter)
    } catch {
        verified = false
    }
    Check.that(!verified,
               "a helper with nothing pinned produced a hello the board would accept")
}

/*
 * The board says it has no registration for this key id — a pairing revoked,
 * or a board re-flashed. The helper reports it and starts asking, but it
 * **keeps the pin**: that pin is the only record of which board it trusts,
 * and a control a restart clears is not a control. Nothing on the channel can
 * make the helper forget which board is its own.
 */
private func testUnpairedRefusalKeepsThePin() throws {
    let f = Fixture(paired: true)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    f.send(try f.helloRefused(.unpaired))
    Check.equal(f.session.state, .notPaired, "the refusal did not leave the helper unpaired")

    /* The pin is still there, and it is what a different board is measured
       against: a grant from something else is refused, not accepted. */
    _ = f.advance(0)
    let other = keyPair(0x41...0x60)
    f.send(try f.pairGrant(key: other.publicKey))
    Check.equal(f.session.state, .boardIdentityChanged,
                "the refusal dropped the pin, so a different board was accepted silently")
}

/*
 * A board wiped past its identity sector, re-flashed, or swapped for another.
 * The grant is genuine — the chord was pressed, and the correlation is this
 * helper's own — but it comes from something that is not the board this
 * helper was paired with, and accepting it silently is how a swapped board
 * inherits the trust of the one it replaced.
 *
 * The chord is deliberately not offered: pressing it is the act that would
 * accept the new board.
 */
private func testABoardWhoseKeyChangedIsNotSilentlyAccepted() throws {
    let f = Fixture(paired: true)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    /* The board no longer knows this helper, so the pin is dropped. */
    f.send(try f.helloRefused(.unpaired))
    _ = f.advance(0)

    /* The chord is pressed, and what answers grants a different identity. */
    let other = keyPair(0x41...0x60)
    let outputs = f.send(try f.pairGrant(key: other.publicKey))

    Check.that(!outputs.contains(where: {
        if case .storeBoardKey = $0 { return true } else { return false }
    }), "a different board's key was pinned without a word")
    Check.equal(f.session.state, .boardIdentityChanged,
                "a board with a new identity key was accepted as the paired one")
    Check.that(!f.session.state.promptsConfigChord,
               "a changed board identity prompted the chord — pressing it is what accepts it")
    Check.that(!f.session.state.allowsBulkTransfers,
               "a board that may not be ours would carry bulk")
    Check.equal(try f.sentFrames(outputs), [], "a different board's grant restarted the handshake")

    /* The same board re-granting is not this case, and pairs normally. */
    let same = Fixture(paired: true)
    same.send(.deviceAppeared(.normal))
    same.send(.channelsAcquired(count: 1))
    same.send(try same.helloRefused(.unpaired))
    _ = same.advance(0)
    Check.that(same.send(try same.pairGrant()).contains(.storeBoardKey(same.boardIdentity.publicKey)),
               "the board re-granting its own key was refused as a different board")
}

/* A grant arriving after the connection was dropped must not restart a
   handshake down a channel that is closed: the hello would go nowhere and the
   machine would sit awaiting an ack until it timed out, on top of a
   reconnection already scheduled. */
private func testGrantOutsideSessionIgnored() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(.transportFailed("link went away"))

    let outputs = f.send(try f.pairGrant(correlation: 0x1234))
    Check.equal(try f.sentFrames(outputs), [], "a grant outside a session started a handshake")
    Check.that(!outputs.contains(where: {
        if case .storeBoardKey = $0 { return true } else { return false }
    }), "a grant outside a session was pinned")
    Check.that(f.retries(outputs).isEmpty, "a grant outside a session scheduled another retry")
}

// MARK: - Reconnection

/*
 * The defect #94 was opened for. A helper too old to decode a frame the
 * device had started sending tore the connection down and rebuilt it about
 * 1.4 times a second for two days, and reported `Connected and paired`
 * throughout: every cycle is correctly too brief for the silence window, and
 * nothing anywhere measured how often they happened.
 *
 * The rate is the missing quantity. One reconnection is a recovery; a
 * reconnection a second is a fault with no other symptom the user can see.
 */
private func testRepeatedReconnectionIsNotReportedAsConnected() throws {
    let f = Fixture()
    try f.establishSession()

    for _ in 0..<(HelperSession.reconnectLimit - 1) { try f.dropAndReconnect() }
    Check.equal(f.session.state, .connected,
                "a couple of reconnections is an ordinary recovery, not a fault")

    /* The rate itself goes in the log, where the operator finds it. */
    Check.that(f.notes(try f.dropAndReconnect()).contains { $0.contains("reconnections") },
               "the measurement behind the state was never recorded")
    Check.equal(f.session.state, .reconnectingRepeatedly,
                "a connection rebuilt \(HelperSession.reconnectLimit) times in a few seconds "
                + "went on reading as connected")
    Check.that(!f.session.state.promptsConfigChord,
               "a connection being rebuilt prompted the chord: the chord provisions whoever is "
               + "connected, and this helper barely is")
    Check.that(f.session.canSendBulk,
               "reporting the rate also revoked the session — this is what the user is told, "
               + "not what the session may carry")

    /* And it stays said, once. Each cycle's successful hello_ack is exactly
       what flapped the state back to connected, which is the whole defect —
       and a line per cycle is the log that hid it (#94). */
    var reported: [HelperState] = []
    var recorded: [String] = []
    for _ in 0..<HelperSession.reconnectLimit {
        let outputs = try f.dropAndReconnect()
        reported += f.states(outputs)
        recorded += f.notes(outputs)
    }
    Check.equal(reported, [], "the state flapped back to connected on every reconnection")
    Check.equal(recorded.filter { $0.contains("reconnections") }, [],
                "the measurement was repeated on every cycle rather than where it changed")
}

/* The same false-healthy reading reached the other way: a link that
   re-enumerates once a second is never gone long enough for the silence
   window either, and the device returning is not itself an announcement. */
private func testAFlappingLinkIsTheSameReading() throws {
    let f = Fixture()
    try f.establishSession()

    for _ in 0..<HelperSession.reconnectLimit {
        f.send(.deviceDisappeared)
        _ = f.advance(0.5)
        f.send(.deviceAppeared(.normal))
        try f.reacquire()
    }

    Check.equal(f.session.state, .reconnectingRepeatedly,
                "a link re-enumerating once a second went on reading as connected")
}

/*
 * The same loop one step further out: a helper that never finishes the
 * handshake at all — a device that takes the hello and says nothing, or a
 * first frame it cannot decode. A rate read only where a session comes up
 * would never be read here at all.
 *
 * And the deferred "device not connected" cannot cover it either: every
 * re-acquisition clears the deferral a second or so before it comes due, so
 * a helper looping on this says nothing whatever — for ever, holding
 * whatever it last said.
 */
private func testAHandshakeThatNeverCompletesIsReported() throws {
    let f = Fixture()
    try f.establishSession()

    /* Acquire, hello, no answer, timeout, and round again. */
    for _ in 0..<HelperSession.reconnectLimit {
        f.send(.channelsAcquired(count: 1))
        _ = f.advance(HelperSession.helloTimeout)
    }
    Check.equal(f.session.state, .reconnectingRepeatedly,
                "a helper that never got past hello went on reading as connected")

    /* And from a standing start, where there is no stale `connected` to
       replace: this helper has never had a session, and saying nothing at
       all is the reading that sent someone looking at the wrong thing. */
    let fresh = Fixture()
    fresh.send(.deviceAppeared(.normal))
    for _ in 0..<HelperSession.reconnectLimit {
        fresh.send(.channelsAcquired(count: 1))
        _ = fresh.advance(HelperSession.helloTimeout)
    }
    Check.equal(fresh.session.state, .reconnectingRepeatedly,
                "a helper that never once got a session reported nothing at all")
}

/*
 * The dead end the review of #112 found, and the reason a chord has to reach
 * a helper that is *not* in `notPaired`.
 *
 * Before #117, `answer_hello` (src/core/dh_session.c) refused with `unpaired`
 * only when the board held **no** registration. When it held one for a
 * different key id — this helper's identity regenerated, a home directory
 * restored onto another Mac — it fell through to the tag check, failed, and
 * stayed silent by design. The helper then looped: hello, no answer, timeout,
 * drop, retry, for ever, never reaching `notPaired`, so it never asked to be
 * paired and the chord had nothing to provision. ADR-0008's "recovery is one
 * chord press" did not hold.
 *
 * #117 refuses per key id, so a board on current firmware answers and this
 * helper reaches `notPaired` without any of the below. The behaviour under test
 * is what keeps the chord working against a board that predates that fix —
 * firmware and helper ship separately, so this helper still meets those.
 */
private func testAStuckHandshakeStillAsksToBePaired() throws {
    let f = Fixture(paired: true)
    f.send(.deviceAppeared(.normal))

    /* The board says nothing at all, however many times it is asked. */
    var asked = 0
    for _ in 0..<(HelperSession.reconnectLimit * 2) {
        asked += try f.sentFrames(f.send(.channelsAcquired(count: 1)))
            .filter { $0.type == MessageType.pairRequest }.count
        _ = f.advance(HelperSession.helloTimeout)
    }

    Check.equal(f.session.state, .reconnectingRepeatedly,
                "a handshake that never completes was not reported")
    Check.that(asked >= 1,
               "a helper looping on an unanswerable hello never asked to be paired, so the "
               + "config chord could not rescue it")

    /* And the chord works. The next retry acquires the channels and asks
       again; the user presses the chord while that request is in flight. The
       pin is unchanged, so this is the same board re-granting, not a swap. */
    f.send(.channelsAcquired(count: 1))
    Check.that(f.send(try f.pairGrant()).contains(.storeBoardKey(f.boardIdentity.publicKey)),
               "the chord could not pair a helper stuck on a silent board")
    /* The session comes up. The state stays `reconnectingRepeatedly` for now
       and correctly so — the rate it measures really was that high — and it
       ages back to connected once the link holds, which
       `testAConnectionThatHoldsGoesBackToConnected` covers. */
    f.send(try f.ack())
    Check.that(f.session.canSendBulk,
               "pairing did not recover the helper from the stuck handshake")
}

/*
 * The other side of the judgement. A re-enumeration, a config-mode round
 * trip and a laptop waking up each cost a reconnection, and a helper that
 * called any of those a fault would be the more annoying defect.
 */
private func testAnOccasionalReconnectionIsAnOrdinaryRecovery() throws {
    let f = Fixture()
    try f.establishSession()

    /* Well past the count, spread past the window. */
    for _ in 0..<(HelperSession.reconnectLimit * 3) {
        try f.dropAndReconnect(after: HelperSession.reconnectWindow)
        Check.equal(f.session.state, .connected,
                    "an occasional reconnection was reported as a connection that will not hold")
    }
}

/* The state is not a latch: a connection that then holds for the whole
   window is a connected one again, and says so once. */
private func testAConnectionThatHoldsGoesBackToConnected() throws {
    let f = Fixture()
    try f.establishSession()
    for _ in 0..<HelperSession.reconnectLimit { try f.dropAndReconnect() }
    Check.equal(f.session.state, .reconnectingRepeatedly,
                "the repeated rebuilding was never reported")

    /* The device beats all the while, so the only thing that changes is the
       window passing. */
    var reported: [HelperState] = []
    func hold(_ seconds: Int) throws {
        for _ in 0..<seconds {
            reported += f.states(f.advance(HelperSession.heartbeatInterval))
            f.send(try f.deviceFrame(MessageType.deviceHeartbeat))
        }
    }

    try hold(Int(HelperSession.reconnectWindow / 2))
    Check.equal(f.session.state, .reconnectingRepeatedly,
                "the state cleared before the window it is measured over had passed")

    try hold(Int(HelperSession.reconnectWindow / 2) + 2)
    Check.equal(reported, [.connected],
                "a connection that then held for the whole window did not go back to connected")
}

// MARK: - The seam itself

/*
 * The one place the binding can drift silently. `HelperState` pairs with
 * `dh_helper_state` by raw value rather than by a switch, so a state renumbered
 * in the core would quietly become a different state here — the user would be
 * shown the wrong sentence, and `promptsConfigChord` would answer for the
 * wrong case, which is the #34 property.
 *
 * The two predicates are read off the core deliberately (`dh_helper.h` says
 * why: the chord provisions whatever is attached during its window, so a
 * second helper must not get to re-decide it). What is asserted here is that
 * each answer arrives against the state it was meant for, and that every state
 * the user can be shown has words.
 */
private func testEveryStateCrossesTheSeamIntact() throws {
    let pairing: [(HelperState, dh_helper_state)] = [
        (.quiet, DH_HELPER_QUIET),
        (.connected, DH_HELPER_CONNECTED),
        (.reconnectingRepeatedly, DH_HELPER_RECONNECTING_REPEATEDLY),
        (.notPaired, DH_HELPER_NOT_PAIRED),
        (.deviceInConfigMode, DH_HELPER_DEVICE_IN_CONFIG_MODE),
        (.deviceAbsent, DH_HELPER_DEVICE_ABSENT),
        (.versionIncompatible, DH_HELPER_VERSION_INCOMPATIBLE),
        (.listenerDetected, DH_HELPER_LISTENER_DETECTED),
        (.boardIdentityChanged, DH_HELPER_BOARD_IDENTITY_CHANGED),
    ]
    Check.equal(pairing.count, HelperState.allCases.count,
                "a state was added without being paired with the core's")

    /*
     * The list above is written by hand, so on its own it can only catch a
     * state added *here*. `DH_HELPER_STATE_COUNT` is what lets the core be
     * counted too: without it a tenth `dh_helper_state` — #49's Windows helper
     * is the likely source — left this suite green while `HelperState(core:)`
     * returned nil, `state` fell back to `.quiet`, and the user was shown
     * nothing at all.
     */
    Check.equal(HelperState.allCases.count, Int(DH_HELPER_STATE_COUNT.rawValue),
                "the core carries a different number of states than this helper does")

    for raw in 0 ..< DH_HELPER_STATE_COUNT.rawValue {
        /* `HelperState(core:)` is a one-line forward to this initialiser, and
           is internal, so this is the same question asked through the public
           half of the seam. */
        Check.that(HelperState(rawValue: raw) != nil,
                   "the core's state \(raw) has no case on this side, so it would be "
                   + "reported as quiet and shown to nobody")
    }

    for (swift, core) in pairing {
        Check.equal(swift.rawValue, core.rawValue, "\(swift) is not the core's \(core.rawValue)")
        Check.equal(swift.promptsConfigChord, dh_helper_prompts_config_chord(core),
                    "\(swift) answers the chord question against the wrong state")
        Check.equal(swift.allowsBulkTransfers, dh_helper_allows_bulk(core),
                    "\(swift) answers the bulk question against the wrong state")

        /* Every state but `quiet` is something to say. `quiet` is the one that
           says nothing on purpose: a device that blinks is ordinary. */
        if swift == .quiet {
            Check.that(swift.message == nil, "quiet put something in the menu bar")
        } else {
            Check.that(!(swift.message ?? "").isEmpty, "\(swift) has no words")
        }
    }

    /* The chord, from exactly one state — the rule #34 buys. */
    Check.equal(HelperState.allCases.filter(\.promptsConfigChord), [.notPaired],
                "the chord is offered from somewhere other than an unpaired helper")

    /* And the two that must warn against it say so in words, not only by
       withholding the prompt (#38): a user who has heard "press the chord"
       once will press it again unless told not to. */
    let listener = HelperState.listenerDetected.message ?? ""
    Check.that(listener.lowercased().contains("do not press the config chord"),
               "a detected listener does not warn off the chord")
    let swapped = HelperState.boardIdentityChanged.message ?? ""
    Check.that(swapped.contains("remove the pinned board key"),
               "a swapped board names no remedy the user can actually reach")
}

/*
 * The first of the two defects the Swift machine took to its grave (#81).
 *
 * It used to clear the deferred report on the way *into* an acquisition and
 * never re-arm it when the hello could not be built, and it recorded no drop
 * either — so an enclave that would not answer, or a stored board key that is
 * not a point on the curve, left the menu bar showing a working helper over a
 * dead device, for ever. `dh_helper` clears the deferral only once the hello
 * exists, and this is the binding's check that it still does.
 */
private func testAHelloThatCannotBeBuiltIsStillReported() throws {
    let session = HelperSession(identity: RefusingIdentity(),
                                boardPublicKey: keyPair(0x21...0x40).publicKey,
                                entropy: { count in [UInt8](repeating: 0x7C, count: count) })
    var now: TimeInterval = 1000

    /* The device is seen first, deliberately. Without that the "nothing was
       ever attached" fallback would report the absence on its own, and this
       test would pass whatever the deferral did. */
    _ = session.handle(.deviceAppeared(.normal), at: now)

    let opened = session.handle(.channelsAcquired(count: 1), at: now)
    Check.that(opened.contains(.closeChannels), "a channel nothing can be said down was kept")
    Check.that(!opened.contains(where: { if case .send = $0 { return true } else { return false } }),
               "something went out under a key that cannot be derived")
    Check.that(opened.contains(where: {
        if case .note(let note) = $0 { return note.contains("not a point on the curve") }
        return false
    }), "the log does not say why the hello could not be built")

    /* It keeps failing, and the retry cadence must not push the report out for
       ever: the backoff caps below the silence window on purpose. */
    var reported: [HelperState] = []
    for _ in 0..<10 {
        now += HelperSession.backoffCap
        reported += session.handle(.channelsAcquired(count: 1), at: now)
            .compactMap { if case .state(let s) = $0 { return s } else { return nil } }
        reported += session.handle(.tick, at: now)
            .compactMap { if case .state(let s) = $0 { return s } else { return nil } }
    }
    Check.equal(reported, [.deviceAbsent],
                "a device that could never be used said nothing at all, or said it over and over")
}

/*
 * The second one. `pairingRequestedAt` was set to nil on success but never
 * *tested*, and the correlation guard could not stand in for it — the second
 * copy of a grant carries the value this helper really did ask with.
 *
 * Replayed two seconds into the session it produced, it re-pinned the key,
 * sent a fresh hello and dropped the session, while the menu bar still read
 * *Connected and paired*. `dh_helper` requires a request to be outstanding.
 */
private func testAGrantNobodyAskedForIsIgnored() throws {
    let f = Fixture(paired: false)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    try f.becomeUnpaired()

    /* The chord lands. The grant is kept, because the point is what happens
       when the *same* one arrives twice. */
    let grant = try f.pairGrant()
    Check.that(f.send(grant).contains(.storeBoardKey(f.boardIdentity.publicKey)),
               "the genuine grant did not pair the helper")
    f.send(try f.ack())
    Check.equal(f.session.state, .connected, "pairing did not end in a session")

    /* Two seconds later, the same bytes again. */
    _ = f.advance(2)
    let outputs = f.send(grant)

    Check.that(f.session.canSendBulk, "a repeated grant tore down the live session")
    Check.equal(f.session.state, .connected,
                "a repeated grant changed what the user is told about a session that is fine")
    Check.that(!outputs.contains(where: {
        if case .storeBoardKey = $0 { return true } else { return false }
    }), "a repeated grant re-pinned the board key")
    Check.equal(try f.sentFrames(outputs), [], "a repeated grant sent a fresh hello")
}

/*
 * The seam #52 consumes. The counter space belongs to the session key, and the
 * heartbeat is already writing into it — so the frames are built here rather
 * than by a platform keeping a counter of its own beside it. Two writers in one
 * space means the board refuses whichever frame loses the race, silently, at
 * the far end, with nothing at either end able to say why.
 *
 * The board's own check is the assertion: `openFromHelper` consumes the
 * counter, so a reused one fails on the next call.
 */
private func testBulkGoesOutUnderTheSessionsCounter() throws {
    /* A bulk type the core routes and this machine does not decide anything
       about — exactly what #52 will be handing it. */
    let clipChunk = UInt8(DH_MSG_CLIP_CHUNK.rawValue)

    let f = Fixture()
    Check.that(f.session.emit(type: clipChunk, body: [1, 2, 3]) == nil,
               "a frame was tagged with no session to carry it")

    try f.establishSession()
    for i: UInt8 in 0..<2 {
        guard let bytes = f.session.emit(type: clipChunk, body: [1, 2, 3, i]) else {
            Check.that(false, "a live session would not build a bulk frame")
            return
        }
        let frame = try FrameCodec.decode(bytes).frame
        Check.equal(frame.type, clipChunk, "the frame is not the one that was asked for")
        Check.equal(try f.openFromHelper(frame), [1, 2, 3, i],
                    "the board refused a bulk frame this session built")
        f.session.noteSent(at: f.now)
    }

    /* ADR-0004: the transfer filled the direction, so no beat is owed — and
       the machine only knows that because the platform said the frame went. */
    Check.equal(try f.sentFrames(f.advance(HelperSession.heartbeatInterval * 0.9)), [],
                "beat into a direction carrying a transfer")

    /* And the beat that does follow keeps the counter moving forwards. */
    let frames = try f.sentFrames(f.advance(HelperSession.heartbeatInterval))
    Check.equal(frames.map(\.type), [MessageType.heartbeat], "no beat once the direction went idle")
    guard let beat = frames.first else { return }
    Check.equal(try f.openFromHelper(beat), [],
                "the beat reused a counter the transfer had already spent")
}
