import DHCore
import DeskhopChannel
import Foundation

/*
 * The session seam: events in, actions out. No device, no IOKit, no wall
 * clock — every test states the time it is asking about.
 *
 * v2 (#112, ADR-0008): the fixture now plays a board with a real P-256 key
 * pair, so the frames it feeds the engine are tagged the way the firmware
 * tags them. A test that wants to break authentication has to break it
 * deliberately; nothing here is waved through.
 */

let sessionEngineTests: [(String, () throws -> Void)] = [
    ("the helper introduces itself once it holds every channel", testHelperIntroducesItself),
    ("an acknowledged hello is a connected session", testAcknowledgedHelloIsASession),
    ("an unpaired board and a version mismatch are different states", testFailuresAreDistinct),
    ("a development device is noted", testDevelopmentDeviceIsNoted),
    ("an ack that echoes somebody else's question is ignored", testAckWithWrongCorrelationIgnored),
    ("a grant this helper never asked for is ignored", testGrantWithWrongCorrelationIgnored),
    ("a grant carrying an unusable key is not kept", testAGrantWithAnUnusableKeyIsNotKept),
    ("a partial acquisition never prompts the chord", testPartialAcquisitionNeverPromptsChord),
    ("a wholly refused acquisition is the same state", testWhollyRefusedAcquisition),
    ("the heartbeat beats at the interval the device measures", testHeartbeatKeepsBeating),
    ("a device that falls silent ends the session", testSilenceFromTheDeviceEndsTheSession),
    ("any authenticated traffic from the device is liveness", testAnyDeviceTrafficIsLiveness),
    ("an unauthenticated frame is not liveness", testUnauthenticatedTrafficIsNotLiveness),
    ("a listener the board detected is reported", testListenerAlertIsReported),
    ("the device's beat is traced where it changes, not where it beats",
     testDeviceBeatTransitionsAreTraced),
    ("the beat trace starts afresh after a config-mode round trip",
     testTheBeatTraceStartsAfreshAfterAConfigModeRoundTrip),
    ("a beat outside a session is ignored", testABeatOutsideASessionIsIgnored),
    ("the beat trace starts afresh after a hello is refused",
     testTheBeatTraceStartsAfreshAfterAHelloIsRefused),
    ("an unpaired helper traces no beat it cannot get",
     testAnUnpairedHelperTracesNoBeatItCannotGet),
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
]

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

/// One engine, one clock, one board, and the small vocabulary the tests read
/// outputs with.
private final class Fixture {
    let helperIdentity: SoftwareIdentity
    /// The board's key pair. `boardIdentity.publicKey` is what a PAIR_GRANT
    /// carries and what a paired helper has pinned.
    let boardIdentity: SoftwareIdentity
    let engine: SessionEngine
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
        let helper = try! SoftwareIdentity(privateKey: [UInt8](0x01...0x20))
        let board = try! SoftwareIdentity(privateKey: [UInt8](0x21...0x40))
        helperIdentity = helper
        boardIdentity = board

        /* Deterministic entropy: distinct on every call, so two hellos never
           share a nonce or a correlation, and a test can still reproduce a
           failure exactly. */
        var tick = 0
        engine = SessionEngine(
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
        observe(engine.handle(input, at: now))
    }

    func advance(_ seconds: TimeInterval) -> [SessionOutput] {
        now += seconds
        return observe(engine.handle(.tick, at: now))
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
     * `deriveSessionKeys` is symmetric — the board runs ECDH against the
     * *helper's* public key and gets the same shared secret — so the parameter
     * named `boardPublicKey` carries the helper's key here.
     */
    func ack(channels: UInt8 = 1,
             chunk: UInt16 = 1024,
             build: BuildType = .release,
             version: UInt16 = UInt16(DH_PROTO_VERSION),
             correlation: UInt64? = nil) throws -> SessionInput {
        guard let hello = lastHello else { throw FixtureError.noHelloOnTheWire }

        let boardNonce = (0..<Int(DH_NONCE_SIZE)).map { UInt8(($0 + Int(now)) & 0xFF) }
        let keys = try boardIdentity.deriveSessionKeys(
            boardPublicKey: helperIdentity.publicKey,
            helperNonce: hello.helperNonce,
            boardNonce: boardNonce)

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

    /// The recovery the engine asks for after a drop: channels back, hello answered.
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
    Check.equal(f.engine.state, .quiet, "reported a session before the device answered")
}

private func testAcknowledgedHelloIsASession() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))

    Check.equal(f.states(f.send(try f.ack(channels: 1, chunk: 512))), [.connected],
                "an acknowledged hello did not become a connected session")
    Check.equal(f.engine.negotiated?.channelCount, 1, "channel count not taken from the ack")
    Check.equal(f.engine.negotiated?.maxChunk, 512,
                "the session must run on the effective value, not the requested one")
}

private func testFailuresAreDistinct() throws {
    let unpaired = Fixture(paired: false)
    unpaired.send(.deviceAppeared(.normal))
    unpaired.send(.channelsAcquired(count: 1))
    Check.equal(unpaired.states(unpaired.send(try unpaired.helloRefused(.unpaired))),
                [.notPaired], "an unpaired board was not reported as unpaired")
    Check.that(unpaired.engine.state.promptsConfigChord,
               "an unpaired helper must be told which keystroke fixes it")
    Check.that(unpaired.engine.negotiated == nil,
               "an unpaired session must not claim negotiated terms")

    let mismatched = Fixture()
    mismatched.send(.deviceAppeared(.normal))
    mismatched.send(.channelsAcquired(count: 1))
    Check.equal(mismatched.states(mismatched.send(
                    try mismatched.helloRefused(.versionIncompatible, version: 3))),
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
    let outputs = f.send(try f.ack(build: .development))

    Check.equal(f.engine.negotiated?.deviceBuild, .development, "the build type was not recorded")
    Check.that(!f.notes(outputs).isEmpty, "a development build must identify itself in the log")
}

// MARK: - Correlation (#108)

/*
 * The trap #108 describes, on the hello path. An attacker who can write to
 * the channel can produce a well-formed ack; what it cannot produce is the
 * random value this helper put in the question it is answering. A helper that
 * acts on any ack it can decode is a helper an unrelated frame can walk into a
 * session with.
 *
 * The tag is *correct* here — the point is that a correct tag on the wrong
 * conversation is still the wrong conversation.
 */
private func testAckWithWrongCorrelationIgnored() throws {
    let f = Fixture()
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    guard let asked = f.lastHello?.correlation else {
        Check.that(false, "no hello was sent")
        return
    }

    let outputs = f.send(try f.ack(correlation: asked &+ 1))
    Check.equal(f.states(outputs), [], "an ack answering a different question started a session")
    Check.unequal(f.engine.state, .connected, "the helper reported a session it never negotiated")
    Check.that(!f.engine.canSendBulk, "a session built on somebody else's ack would carry bulk")
    Check.that(f.notes(outputs).contains { $0.contains("correlation") },
               "the mismatched correlation was not recorded")

    /* And the real answer still works: refusing one ack must not poison the
       handshake that is still legitimately in flight. */
    Check.equal(f.states(f.send(try f.ack())), [.connected],
                "the genuine ack was refused after a forged one had been dropped")
}

/*
 * The same trap on the pairing path, which is the one #108 was actually
 * opened for: a manufactured PAIR_GRANT arriving without any chord pins an
 * attacker's key as the board's. The correlation value in the helper's own
 * PAIR_REQUEST is the thing the attacker has to guess.
 */
private func testGrantWithWrongCorrelationIgnored() throws {
    let f = Fixture(paired: false)
    f.send(.deviceAppeared(.normal))
    f.send(.channelsAcquired(count: 1))
    try f.becomeUnpaired()

    guard let asked = f.lastPairRequest?.correlation else {
        Check.that(false, "an unpaired helper never asked to be paired")
        return
    }

    /* A key that is entirely valid — a real point on the curve, which ECDH
       would happily accept. The correlation is the only thing wrong with this
       grant, and it has to be enough on its own. */
    let attacker = try SoftwareIdentity(privateKey: [UInt8](0x41...0x60))
    let outputs = f.send(try f.pairGrant(correlation: asked &+ 1, key: attacker.publicKey))

    Check.that(!outputs.contains(where: {
        if case .storeBoardKey = $0 { return true } else { return false }
    }), "a key nobody asked for was pinned as the board's")
    Check.equal(try f.sentFrames(outputs), [],
                "a manufactured grant restarted the handshake")
    Check.equal(f.engine.state, .notPaired, "a manufactured grant paired the helper")

    /* The genuine grant, echoing what this helper asked, is acted on. */
    Check.that(f.send(try f.pairGrant()).contains(.storeBoardKey(f.boardIdentity.publicKey)),
               "the board's own grant was refused along with the forged one")
}

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
    _ = f.advance(SessionEngine.pairingRetryInterval)
    Check.that(f.send(try f.pairGrant()).contains(.storeBoardKey(f.boardIdentity.publicKey)),
               "an unusable grant left the helper unable to pair at all")
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
        f.send(try f.deviceFrame(0x20, body: [1, 0, 0, 0x80]))
    }

    Check.equal(f.engine.state, .connected,
                "traffic the engine ignores did not count as the device being alive")
    Check.that(f.engine.canSendBulk, "a live session refused to carry bulk")
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
    Check.that(!f.engine.canSendBulk, "a session torn down by a bad tag would still carry bulk")

    /* And it never counted as the device being alive: there is no session left
       for a liveness clock to be measuring. This is #95's fix — under v1 any
       frame on the channel reset that clock, so a bystander writing rubbish
       kept a dead session reading as healthy indefinitely. */
    Check.equal(try f.sentFrames(f.advance(SessionEngine.heartbeatInterval * 3)), [],
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
    Check.that(!f.engine.state.promptsConfigChord,
               "a listener prompted the chord, which would provision whoever is listening")

    /* The session is untouched. What the board detected is somebody *writing*
       frames it refused, which the tag already keeps out — so this changes
       what the user is told, not what the session may carry, and the two
       seams #52 reads must not disagree. */
    Check.that(f.engine.canSendBulk, "a reported listener revoked a session that is still good")
    Check.equal(f.engine.state.allowsBulkTransfers, f.engine.canSendBulk,
                "allowsBulkTransfers and canSendBulk gave #52 opposite answers")

    /* And it expires like the rate it is. The board measured it over a window
       and said so; nothing further arriving means whatever was writing has
       stopped, and holding the warning up for the rest of the session would
       make it a latch — the mistake #94 corrected for reconnections. */
    var reported: [HelperState] = []
    for _ in 0..<12 {
        reported += f.states(f.advance(SessionEngine.heartbeatInterval))
        f.send(try f.deviceFrame(MessageType.deviceHeartbeat))
    }
    Check.equal(reported, [.connected],
                "the listener warning never cleared, though nothing further was refused")
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
       from "the beat was never worth mentioning". The wording is asserted, not
       just the subject: `quiet for …` and `resumed after …` both contain
       "device heartbeat", so a looser check passes on any of the three and
       would stay green if the first beat printed `resumed after 0.0s` (#98). */
    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("first beat") },
               "the first beat of a session was not traced as the first")

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
        f.send(try f.deviceFrame(0x20, body: [1, 0, 0, 0x80]))
    }
    Check.equal(f.engine.state, .connected, "a transfer without beats dropped the session")
    Check.equal(duringTransfer.filter { $0.contains("quiet") }.count, 1,
                "the beat falling silent was not traced exactly once")

    /* And the far side of it, with the measurement attached. */
    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("resumed") },
               "the beat coming back was not traced")
}

/*
 * The trace is measured inside one session and must not outlive it. A
 * config-mode round trip is the common path that proves it: the board leaves
 * under its other identity for minutes, which is `deviceLeft` and not
 * `dropConnection`, and a beat remembered from before the chord makes both
 * edges of the next session wrong — a `quiet for 300.0s` about a session one
 * tick old, and a genuine first beat announcing itself as `resumed` (#98).
 */
private func testTheBeatTraceStartsAfreshAfterAConfigModeRoundTrip() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(try f.deviceFrame(MessageType.deviceHeartbeat))

    /* The chord: config mode, minutes away, then back as itself. */
    f.send(.deviceAppeared(.configMode))
    _ = f.advance(SessionEngine.silenceWindow)
    f.now += 300
    f.send(.deviceAppeared(.normal))
    try f.reacquire()

    Check.equal(f.notes(f.advance(0.25)).filter { $0.contains("quiet") }, [],
                "a quiet spell measured before the chord outlived the session it belonged to")
    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("first beat") },
               "the first beat after a config-mode round trip was not traced as the first")
}

/*
 * Every other frame type refuses one that arrives outside a session and says
 * so; the beat did not. A beat still in the read queue when the connection
 * went announced the first beat of a session that does not exist — and then
 * swallowed the real one, since the real first beat is only first while
 * nothing has claimed it (#98).
 *
 * Under v2 it is refused a step earlier, at the tag: the session key went
 * with the session, so there is nothing to verify the frame against. The
 * property the test is about is unchanged — the stale beat must not consume
 * the next session's first.
 */
private func testABeatOutsideASessionIsIgnored() throws {
    let f = Fixture()
    try f.establishSession()
    let stray = try f.deviceFrame(MessageType.deviceHeartbeat)
    f.send(.transportFailed("link went away"))

    let stale = f.notes(f.send(stray))
    Check.that(stale.contains { $0.contains("dropping") },
               "a beat outside a session was acted on rather than refused")
    Check.that(!stale.contains { $0.contains("first beat") },
               "a beat outside a session announced the first beat of one")

    try f.reacquire()
    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("first beat") },
               "the stale beat consumed the new session's first")
}

/*
 * The same leak on the pairing path, which the engine reaches without ever
 * going idle. A HELLO_REFUSED(unpaired) ends the session while deliberately
 * keeping the phase — #46 needs the helper live and asking — so it is the one
 * place where losing a session is not going quiet, and the one a teardown
 * keyed on the phase steps straight past.
 *
 * A beat remembered across it silences the *next* session's first beat
 * entirely: it is no longer the first, and no quiet spell was noted to make
 * it a resumption, so it says nothing at all.
 */
private func testTheBeatTraceStartsAfreshAfterAHelloIsRefused() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(try f.deviceFrame(MessageType.deviceHeartbeat))

    /* The link goes, and this time the board has forgotten the registration:
       the fresh hello is refused, and the helper is left unpaired and asking. */
    f.send(.transportFailed("link went away"))
    f.send(.channelsAcquired(count: 1))
    try f.becomeUnpaired()
    Check.equal(f.engine.state, .notPaired, "the refused hello did not leave the helper unpaired")

    /* The chord lands, and the grant restarts the handshake. */
    f.send(try f.pairGrant())
    f.send(try f.ack())
    Check.equal(f.engine.state, .connected, "pairing did not establish a session")

    Check.that(f.notes(f.send(try f.deviceFrame(MessageType.deviceHeartbeat)))
                   .contains { $0.contains("first beat") },
               "the first beat of the session pairing established was not traced at all")
}

/*
 * The quiet note is scoped to a session, exactly as the liveness check three
 * lines above it is. An unpaired helper is deliberately live and the device
 * holds no session for it, so it is sent nothing by design — tracing the
 * absence of beats that are not supposed to exist is noise in the log the
 * trace exists to keep readable (#98).
 */
private func testAnUnpairedHelperTracesNoBeatItCannotGet() throws {
    let f = Fixture()
    try f.establishSession()
    f.send(try f.deviceFrame(MessageType.deviceHeartbeat))

    f.send(.transportFailed("link went away"))
    f.send(.channelsAcquired(count: 1))
    try f.becomeUnpaired()
    Check.equal(f.engine.state, .notPaired, "the refused hello did not leave the helper unpaired")

    var notes: [String] = []
    for _ in 0..<8 { notes += f.notes(f.advance(SessionEngine.heartbeatInterval)) }

    Check.equal(notes.filter { $0.contains("quiet") }, [],
                "an unpaired helper traced beats the device is designed not to send it")
    Check.equal(f.engine.state, .notPaired, "the unpaired helper was torn down")
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
        let types = try f.sentFrames(f.advance(SessionEngine.heartbeatInterval)).map(\.type)
        Check.that(!f.states(f.advance(0)).contains(.deviceAbsent),
                   "an unpaired helper was reported absent")
        beats += types.filter { $0 == MessageType.heartbeat }.count
        asks += types.filter { $0 == MessageType.pairRequest }.count
    }

    Check.equal(f.engine.state, .notPaired,
                "an unpaired helper was torn down by the detector, so the chord could not reach it")
    Check.that(asks >= 2, "an unpaired helper stopped asking to be paired")
    Check.equal(beats, 0,
                "an unpaired helper beat with a session key it cannot have — a beat it could "
                + "send unauthenticated is a beat anybody could send")
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
    Check.equal(quick.states(quick.send(try quick.ack())), [],
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

    f.send(try f.ack())
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
    f.send(try f.helloRefused(.versionIncompatible, version: 3))

    Check.equal(try f.sentFrames(f.advance(SessionEngine.heartbeatInterval * 3)), [],
                "beat at a device that had refused the session")
    Check.equal(f.engine.state, .versionIncompatible, "the state did not survive the ticks")

    /* An unpaired helper is the opposite case: it is deliberately live and
       keeps asking, because the window can only provision a helper that is
       connected when the user presses the chord. */
    let unpaired = Fixture(paired: false)
    unpaired.send(.deviceAppeared(.normal))
    unpaired.send(.channelsAcquired(count: 1))
    unpaired.send(try unpaired.helloRefused(.unpaired))

    let types = try unpaired.sentFrames(unpaired.advance(SessionEngine.heartbeatInterval))
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
    Check.equal(f.engine.state, .notPaired, "the helper did not report being unpaired")

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
    let key = try f.boardIdentity.deriveHelloKey(
        boardPublicKey: f.helperIdentity.publicKey,
        helperNonce: Hello.decode(body: unverifiedBody(hello)).helperNonce)
    var counter = dh_auth_counter()
    dh_auth_counter_init(&counter)
    _ = Check.doesNotThrow("the hello after pairing was not authenticated under the granted key") {
        try AuthFrame.open(frame: hello, key: key, counter: &counter)
    }

    /* The device accepts it, and *that* is what the user sees. */
    Check.equal(f.states(f.send(try f.ack())), [.connected],
                "pairing succeeded but the helper never confirmed it visibly")
    Check.that(!f.engine.state.promptsConfigChord, "a paired helper still prompts the chord")
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
    let pairedKey = try paired.boardIdentity.deriveHelloKey(
        boardPublicKey: paired.helperIdentity.publicKey,
        helperNonce: Hello.decode(body: unverifiedBody(pairedHello)).helperNonce)
    var counter = dh_auth_counter()
    dh_auth_counter_init(&counter)
    _ = Check.doesNotThrow("a pinned board key did not authenticate the hello") {
        try AuthFrame.open(frame: pairedHello, key: pairedKey, counter: &counter)
    }

    let fresh = Fixture(paired: false)
    let freshHello = try helloFrom(fresh)
    let freshKey = try fresh.boardIdentity.deriveHelloKey(
        boardPublicKey: fresh.helperIdentity.publicKey,
        helperNonce: Hello.decode(body: unverifiedBody(freshHello)).helperNonce)
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
    Check.equal(f.engine.state, .notPaired, "the refusal did not leave the helper unpaired")

    /* The pin is still there, and it is what a different board is measured
       against: a grant from something else is refused, not accepted. */
    _ = f.advance(0)
    let other = try SoftwareIdentity(privateKey: [UInt8](0x41...0x60))
    f.send(try f.pairGrant(key: other.publicKey))
    Check.equal(f.engine.state, .boardIdentityChanged,
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
    let other = try SoftwareIdentity(privateKey: [UInt8](0x41...0x60))
    let outputs = f.send(try f.pairGrant(key: other.publicKey))

    Check.that(!outputs.contains(where: {
        if case .storeBoardKey = $0 { return true } else { return false }
    }), "a different board's key was pinned without a word")
    Check.equal(f.engine.state, .boardIdentityChanged,
                "a board with a new identity key was accepted as the paired one")
    Check.that(!f.engine.state.promptsConfigChord,
               "a changed board identity prompted the chord — pressing it is what accepts it")
    Check.that(!f.engine.state.allowsBulkTransfers,
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
   engine would sit in awaitingAck until it timed out, on top of a
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
    f.send(try f.ack())
    f.send(.deviceDisappeared)
    f.send(.deviceAppeared(.normal))

    Check.equal(f.retries(f.send(.acquisitionRefused(acquired: 0, of: 1))).first, delays.first,
                "a working session did not reset the reconnection delay")
}

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

    for _ in 0..<(SessionEngine.reconnectLimit - 1) { try f.dropAndReconnect() }
    Check.equal(f.engine.state, .connected,
                "a couple of reconnections is an ordinary recovery, not a fault")

    /* The rate itself goes in the log, where the operator finds it. */
    Check.that(f.notes(try f.dropAndReconnect()).contains { $0.contains("reconnections") },
               "the measurement behind the state was never recorded")
    Check.equal(f.engine.state, .reconnectingRepeatedly,
                "a connection rebuilt \(SessionEngine.reconnectLimit) times in a few seconds "
                + "went on reading as connected")
    Check.that(!f.engine.state.promptsConfigChord,
               "a connection being rebuilt prompted the chord: the chord provisions whoever is "
               + "connected, and this helper barely is")
    Check.that(f.engine.canSendBulk,
               "reporting the rate also revoked the session — this is what the user is told, "
               + "not what the session may carry")

    /* And it stays said, once. Each cycle's successful hello_ack is exactly
       what flapped the state back to connected, which is the whole defect —
       and a line per cycle is the log that hid it (#94). */
    var reported: [HelperState] = []
    var recorded: [String] = []
    for _ in 0..<SessionEngine.reconnectLimit {
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

    for _ in 0..<SessionEngine.reconnectLimit {
        f.send(.deviceDisappeared)
        _ = f.advance(0.5)
        f.send(.deviceAppeared(.normal))
        try f.reacquire()
    }

    Check.equal(f.engine.state, .reconnectingRepeatedly,
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
    for _ in 0..<SessionEngine.reconnectLimit {
        f.send(.channelsAcquired(count: 1))
        _ = f.advance(SessionEngine.helloTimeout)
    }
    Check.equal(f.engine.state, .reconnectingRepeatedly,
                "a helper that never got past hello went on reading as connected")

    /* And from a standing start, where there is no stale `connected` to
       replace: this helper has never had a session, and saying nothing at
       all is the reading that sent someone looking at the wrong thing. */
    let fresh = Fixture()
    fresh.send(.deviceAppeared(.normal))
    for _ in 0..<SessionEngine.reconnectLimit {
        fresh.send(.channelsAcquired(count: 1))
        _ = fresh.advance(SessionEngine.helloTimeout)
    }
    Check.equal(fresh.engine.state, .reconnectingRepeatedly,
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
    for _ in 0..<(SessionEngine.reconnectLimit * 2) {
        asked += try f.sentFrames(f.send(.channelsAcquired(count: 1)))
            .filter { $0.type == MessageType.pairRequest }.count
        _ = f.advance(SessionEngine.helloTimeout)
    }

    Check.equal(f.engine.state, .reconnectingRepeatedly,
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
    Check.that(f.engine.canSendBulk,
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
    for _ in 0..<(SessionEngine.reconnectLimit * 3) {
        try f.dropAndReconnect(after: SessionEngine.reconnectWindow)
        Check.equal(f.engine.state, .connected,
                    "an occasional reconnection was reported as a connection that will not hold")
    }
}

/* The state is not a latch: a connection that then holds for the whole
   window is a connected one again, and says so once. */
private func testAConnectionThatHoldsGoesBackToConnected() throws {
    let f = Fixture()
    try f.establishSession()
    for _ in 0..<SessionEngine.reconnectLimit { try f.dropAndReconnect() }
    Check.equal(f.engine.state, .reconnectingRepeatedly,
                "the repeated rebuilding was never reported")

    /* The device beats all the while, so the only thing that changes is the
       window passing. */
    var reported: [HelperState] = []
    func hold(_ seconds: Int) throws {
        for _ in 0..<seconds {
            reported += f.states(f.advance(SessionEngine.heartbeatInterval))
            f.send(try f.deviceFrame(MessageType.deviceHeartbeat))
        }
    }

    try hold(Int(SessionEngine.reconnectWindow / 2))
    Check.equal(f.engine.state, .reconnectingRepeatedly,
                "the state cleared before the window it is measured over had passed")

    try hold(Int(SessionEngine.reconnectWindow / 2) + 2)
    Check.equal(reported, [.connected],
                "a connection that then held for the whole window did not go back to connected")
}
