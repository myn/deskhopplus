import DHCore
import Foundation

/*
 * The helper's side of the session, as pure logic: events in, actions out, no
 * IOKit and no clock of its own. Everything the acceptance criteria care about
 * — the hello exchange, the heartbeat, all-or-nothing exclusivity, the states
 * the user is shown, and reconnection with a capped backoff — is decided here
 * and therefore tested on a laptop with no device attached.
 *
 * v2 (#112, ADR-0008): the bearer token is gone. Every hello and every reply
 * echoes a correlation value that ties an answer to its question, every
 * session-band frame is tagged under a per-session key, and the helper holds
 * a Secure Enclave identity rather than a 16-byte secret.
 *
 * The transport (HIDChannelSet) does what it is told and reports what it sees.
 */

public enum DeviceIdentity: Equatable {
    case normal
    /* Config mode reboots the device under a different USB identity. Seeing
       it tells the helper exactly what happened, which is why it is a state
       of its own and not "the device is gone". */
    case configMode
}

public enum SessionInput: Equatable {
    case deviceAppeared(DeviceIdentity)
    case deviceDisappeared
    /* Every channel was opened exclusively. Partial acquisition is not this
       input — it is `acquisitionRefused`. */
    case channelsAcquired(count: Int)
    case acquisitionRefused(acquired: Int, of: Int)
    case received([UInt8])
    /* The transport could not carry something it was given. A frame written
       in part leaves the device's reader mid-frame, where the padding skip
       does not apply and the next frame is eaten as its tail — so this is a
       dropped connection, not a retryable write. */
    case transportFailed(String)
    case tick
}

public enum SessionOutput: Equatable {
    /* The board's public key, from a PAIR_GRANT this helper asked for.
       Storing it is the runtime's job; the engine only decides it is worth
       keeping — and, per `boardPublicKey`, never emits a second one for a
       board whose key has changed. */
    case storeBoardKey([UInt8])
    case openChannels
    case closeChannels
    case send([UInt8])
    case state(HelperState)
    case retry(after: TimeInterval)
    /* Diagnostics, never shown to the user. */
    case note(String)
}

public struct Negotiated: Equatable {
    public let channelCount: UInt8
    public let maxChunk: UInt16
    public let deviceBuild: BuildType
}

public final class SessionEngine {
    public static let heartbeatInterval = TimeInterval(DH_SESSION_HEARTBEAT_MS) / 1000
    public static let deviceBeatTimeout = TimeInterval(DH_SESSION_ABSENT_MS) / 1000
    public static let silenceWindow: TimeInterval = 5
    public static let helloTimeout: TimeInterval = 2
    public static let reconnectWindow: TimeInterval = 30
    public static let reconnectLimit = 4
    public static let pairingRetryInterval: TimeInterval = 2

    private enum Phase {
        case idle
        case awaitingAck
        case live
    }

    private let identity: HelperIdentity
    private let entropy: (Int) -> [UInt8]

    private var phase: Phase = .idle
    private var backoff = Backoff()
    private var stream = FrameStream()
    private var helloSentAt: TimeInterval = 0
    private var lastSentAt: TimeInterval = 0
    private var lastDeviceFrameAt: TimeInterval = 0
    private var lastDeviceBeatAt: TimeInterval?
    private var deviceBeatQuietNoted = false
    private var holdingChannels = false
    private var recentDrops: [TimeInterval] = []
    private var deferredState: (state: HelperState, at: TimeInterval)?
    private var pairingRequestedAt: TimeInterval?
    private var startedAt: TimeInterval?
    private var everSawDevice = false

    /*
     * The board's identity key, pinned at pairing. It is deliberately **not**
     * dropped when the board says it does not know us: it is the only record
     * of which board this helper trusts, and a control that a restart clears
     * is not a control. A stale pin costs nothing — the board checks its
     * registration before it checks the tag, so the hello is still refused
     * with `unpaired` — and keeping it is what lets a grant carrying a
     * *different* key be recognised as a different board (#112).
     *
     * Only the user clears it, by removing the file. See the README.
     */
    private var boardPublicKey: [UInt8]?

    /// When the board's last listener alert stops meaning anything. See
    /// `listenerAlertReceived`.
    private var listenerAlertUntil: TimeInterval?

    // v2 session crypto — per session, cleared by forgetCryptoState()
    private var helperNonce: [UInt8]?
    private var helloCorrelation: UInt64 = 0
    private var pairCorrelation: UInt64 = 0
    private var kH2B: [UInt8]?
    private var kB2H: [UInt8]?
    private var txCounter: UInt64 = 0
    private var rxCounter = dh_auth_counter()

    public private(set) var state: HelperState = .quiet
    public private(set) var negotiated: Negotiated?

    public var canSendBulk: Bool { phase == .live && negotiated != nil }

    public init(identity: HelperIdentity, boardPublicKey: [UInt8]? = nil,
                entropy: @escaping (Int) -> [UInt8]) {
        self.identity = identity
        self.boardPublicKey = boardPublicKey
        self.entropy = entropy
        dh_auth_counter_init(&rxCounter)
    }

    public func handle(_ input: SessionInput, at now: TimeInterval) -> [SessionOutput] {
        if startedAt == nil { startedAt = now }
        if case .deviceAppeared = input { everSawDevice = true }

        switch input {
        case .deviceAppeared(.normal):
            return deviceAppeared()
        case .deviceAppeared(.configMode):
            return deviceLeft(for: .deviceInConfigMode, at: now)
        case .deviceDisappeared:
            return deviceLeft(for: .deviceAbsent, at: now)
        case .channelsAcquired(let count):
            return channelsAcquired(count: count, at: now)
        case .acquisitionRefused(let acquired, let total):
            return acquisitionRefused(acquired: acquired, of: total, at: now)
        case .received(let bytes):
            return received(bytes, at: now)
        case .transportFailed(let reason):
            return dropConnection(note: "transport failed: \(reason)", at: now)
        case .tick:
            return tick(at: now)
        }
    }

    // MARK: - Device presence

    private func forgetSession() {
        phase = .idle
        negotiated = nil
        stream.reset()
        forgetBeatTrace()
        forgetCryptoState()
    }

    private func forgetCryptoState() {
        kH2B = nil
        kB2H = nil
        helperNonce = nil
        listenerAlertUntil = nil
        txCounter = 0
        dh_auth_counter_init(&rxCounter)
    }

    private func forgetBeatTrace() {
        lastDeviceBeatAt = nil
        deviceBeatQuietNoted = false
    }

    private func deviceAppeared() -> [SessionOutput] {
        deferredState = nil
        backoff.reset()
        let stale: [HelperState] = [.deviceAbsent, .deviceInConfigMode]
        let outputs = stale.contains(state) ? emit(.quiet) : []
        return outputs + [.openChannels]
    }

    private func deviceLeft(for reason: HelperState, at now: TimeInterval) -> [SessionOutput] {
        recordDrop(at: now)
        forgetSession()
        deferredState = (reason, now + Self.silenceWindow)

        var outputs: [SessionOutput] = []
        if holdingChannels {
            holdingChannels = false
            outputs.append(.closeChannels)
        }
        return outputs
    }

    // MARK: - Acquisition

    private func channelsAcquired(count: Int, at now: TimeInterval) -> [SessionOutput] {
        holdingChannels = true
        phase = .awaitingAck
        helloSentAt = now
        lastSentAt = now
        lastDeviceFrameAt = now
        deferredState = nil

        do {
            var outputs: [SessionOutput] = [.send(try buildHello(at: now))]

            /*
             * A handshake that keeps failing, with the board answering nothing.
             *
             * #117 fixed this in the firmware: `answer_hello`
             * (src/core/dh_session.c) now refuses with `unpaired` per key id
             * rather than per board, so a board registered to *someone else*
             * says so and this helper reaches `notPaired` on its own. On a
             * board running that firmware the ask below never fires, because
             * the refusal arrives long before the rate says anything.
             *
             * **Kept anyway, deliberately.** Firmware and helper ship
             * separately and a user updates one without the other, so this
             * helper still meets boards that predate #117 — and on those, the
             * old behaviour is exactly as it was: the board holds a
             * registration for a key id that is no longer this helper's, falls
             * through to the tag check, fails it, and stays silent. Silence
             * never reaches `notPaired`, so without this the config chord has
             * nothing to provision and the machine reconnects for ever.
             *
             * The cost of keeping it is one untagged frame per acquisition,
             * and only once the rate already says retrying is not working. A
             * board outside a pairing window ignores it.
             */
            if state == .reconnectingRepeatedly, let ask = pairRequestFrame(at: now) {
                outputs.append(.note("asking to be paired: the handshake is not completing"))
                outputs.append(.send(ask))
            }
            return outputs
        } catch {
            forgetSession()
            return [.note("hello could not be encoded: \(error)"),
                    .closeChannels,
                    .retry(after: backoff.next())]
        }
    }

    /*
     * The open failed. Under v1 this reported `channelHeld` — *"Another program
     * holds the channel — find and stop it"* — which on macOS asserts something
     * that cannot happen: a second `kIOHIDOptionsTypeSeizeDevice` open succeeds,
     * measured. The state is gone with the claim (#72, #114, ADR-0008), and no
     * state replaces it here, because a refused open names no remedy of its own.
     *
     * What is left is a device this helper cannot use, reported the way every
     * other unusable device is: a deferred absence, so a failure that persists
     * says *"Device not connected"* instead of saying nothing for ever, with the
     * real reason in the log. A partial acquisition — the ordinary shape when
     * #63's channel nodes arrive one at a time — clears the deferral on the
     * retry that completes, well inside the window.
     */
    private func acquisitionRefused(acquired: Int, of total: Int,
                                    at now: TimeInterval) -> [SessionOutput] {
        forgetSession()
        holdingChannels = false
        deferredState = (.deviceAbsent, now + Self.silenceWindow)

        let note = acquired > 0
            ? "released \(acquired) of \(total) channels: a partial acquisition is not a session"
            : "every channel refused"
        return [.closeChannels, .note(note), .retry(after: backoff.next())]
    }

    // MARK: - Traffic

    private func received(_ bytes: [UInt8], at now: TimeInterval) -> [SessionOutput] {
        let frames: [Frame]
        do {
            frames = try stream.push(bytes)
        } catch {
            return dropConnection(note: "protocol error on the channel: \(error)", at: now)
        }

        var outputs: [SessionOutput] = []
        for frame in frames {
            let isAuth = frame.type < 0x08 || frame.type > 0x0F
            if isAuth {
                outputs += handleAuthenticated(frame, at: now)
            } else {
                outputs += handleUnauthenticated(frame, at: now)
            }
        }
        return outputs
    }

    /* Frames that carry an authentication prefix (everything outside 0x08-0x0F).
       Liveness is only updated when the tag verifies — v2's fix for #95. */
    private func handleAuthenticated(_ frame: Frame, at now: TimeInterval) -> [SessionOutput] {
        if frame.type == MessageType.helloAck {
            return helloAck(frame, at: now)
        }

        guard let key = kB2H else {
            return [.note("dropping authenticated frame (type 0x\(String(frame.type, radix: 16)))"
                          + " with no session key")]
        }

        let body: [UInt8]
        do {
            body = try AuthFrame.open(frame: frame, key: key, counter: &rxCounter)
        } catch AuthError.badTag {
            /*
             * docs/protocol.md, "the helper's side of the same rule": only the
             * device emits device→helper reports, so a tag that fails here
             * means the board is not the board this helper paired with, or the
             * byte stream is corrupt. Either way the connection goes, and
             * nothing is replied — an answer would reach every attached client.
             */
            return dropConnection(note: "a device→helper frame failed its tag", at: now)
        } catch AuthError.replayedCounter {
            /*
             * Not the same case. The tag verified, so this came from the board;
             * the counter is merely not greater than one already accepted. The
             * device's outbound path is a bounded queue (ADR-0005) where gaps
             * are ordinary, so this costs the frame, not the session.
             */
            return [.note("dropping frame with a counter already seen")]
        } catch {
            return [.note("dropping frame: \(error)")]
        }

        lastDeviceFrameAt = now

        switch frame.type {
        case MessageType.sessionEnd:
            return sessionEnded(body, at: now)
        case MessageType.deviceHeartbeat:
            return deviceBeat(at: now)
        case MessageType.listenerAlert:
            return listenerAlertReceived(body, at: now)
        default:
            return []
        }
    }

    /* Frames in the unauthenticated band (0x08-0x0F): pairing, hello refusals.
       No liveness update — these are not session traffic. */
    private func handleUnauthenticated(_ frame: Frame, at now: TimeInterval) -> [SessionOutput] {
        switch frame.type {
        case MessageType.helloRefused:
            return helloRefused(frame.payload, at: now)
        case MessageType.pairGrant:
            return pairGranted(frame.payload, at: now)
        case MessageType.pairRefused:
            return pairRefusedReceived(frame.payload, at: now)
        default:
            return []
        }
    }

    // MARK: - Hello exchange

    /* HELLO_ACK is special: we peek the board nonce from the payload before we
       can derive k_b2h and verify the tag. */
    private func helloAck(_ frame: Frame, at now: TimeInterval) -> [SessionOutput] {
        guard phase == .awaitingAck else {
            return [.note("ignoring a hello_ack outside a session")]
        }
        guard let boardKey = boardPublicKey else {
            return dropConnection(note: "received hello_ack but have no board key", at: now)
        }
        guard let nonce = helperNonce else {
            return dropConnection(note: "received hello_ack with no stored nonce", at: now)
        }

        guard let boardNonce = AuthFrame.peekBoardNonce(payload: frame.payload) else {
            return dropConnection(note: "hello_ack too short to carry a board nonce", at: now)
        }

        let keys: (kH2B: [UInt8], kB2H: [UInt8])
        do {
            keys = try identity.deriveSessionKeys(
                boardPublicKey: boardKey, helperNonce: nonce, boardNonce: boardNonce)
        } catch {
            return dropConnection(note: "could not derive session keys: \(error)", at: now)
        }

        var counter = dh_auth_counter()
        dh_auth_counter_init(&counter)
        let body: [UInt8]
        do {
            body = try AuthFrame.open(frame: frame, key: keys.kB2H, counter: &counter)
        } catch {
            return dropConnection(note: "hello_ack tag did not verify: \(error)", at: now)
        }

        guard let ack = try? HelloAck.decode(body: body) else {
            return dropConnection(note: "hello_ack body could not be decoded", at: now)
        }
        guard ack.correlation == helloCorrelation else {
            return [.note("ignoring hello_ack with wrong correlation")]
        }

        self.kH2B = keys.kH2B
        self.kB2H = keys.kB2H
        self.rxCounter = counter
        self.txCounter = 0

        backoff.reset()
        phase = .live
        lastDeviceFrameAt = now
        negotiated = Negotiated(channelCount: ack.channelCount,
                                maxChunk: ack.maxChunk,
                                deviceBuild: ack.buildType)

        var outputs: [SessionOutput] = []
        if ack.buildType == .development {
            outputs.append(.note("device is a development build: channel authentication "
                                 + "is compiled out"))
        }

        guard reconnectingRepeatedly(at: now) else { return outputs + emit(.connected) }
        return outputs + emitRepeatedReconnection(at: now)
    }

    private func helloRefused(_ payload: [UInt8], at now: TimeInterval) -> [SessionOutput] {
        guard phase == .awaitingAck else {
            return [.note("ignoring a hello_refused outside a session")]
        }
        guard let refused = try? HelloRefused.decode(payload: payload) else {
            return dropConnection(note: "hello_refused could not be decoded", at: now)
        }
        guard refused.correlation == helloCorrelation else {
            return [.note("ignoring hello_refused with wrong correlation")]
        }

        backoff.reset()

        switch refused.status {
        case .unpaired:
            phase = .live
            negotiated = nil
            pairingRequestedAt = nil
            forgetBeatTrace()
            forgetCryptoState()
            /* The pin stays. See `boardPublicKey`: it is the record of which
               board this helper trusts, and dropping it here would mean a
               swapped board is accepted silently after any restart. */
            return emit(.notPaired)

        case .versionIncompatible:
            forgetSession()
            return [.note("device speaks protocol version \(refused.protocolVersion), "
                          + "this helper speaks \(DH_PROTO_VERSION)")]
                + emit(.versionIncompatible)
        }
    }

    // MARK: - Pairing

    private func pairGranted(_ payload: [UInt8], at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            return [.note("ignoring a pair grant outside a session")]
        }
        guard let grant = try? PairGrant.decode(payload: payload) else {
            return [.note("ignoring a pair grant that could not be decoded")]
        }
        guard grant.correlation == pairCorrelation else {
            return [.note("ignoring pair grant with wrong correlation")]
        }

        /*
         * A different board. The chord was pressed and the grant is genuine —
         * but it is a genuine grant from something that is not the board this
         * helper was paired with, and accepting it silently is how a swapped
         * board inherits the trust of the one it replaced.
         *
         * Deliberately not offered the chord: pressing it is the very act
         * that would accept the new board. The way through is for the user to
         * remove the pinned key, which says "I re-flashed it" in the one place
         * a bystander on the channel cannot reach.
         */
        if let pinned = boardPublicKey, pinned != grant.boardPublicKey {
            pairingRequestedAt = nil
            return [.note("the board granted pairing under a different identity key — "
                          + "re-flashed, wiped past its identity sector, or swapped")]
                + emit(.boardIdentityChanged)
        }

        /*
         * Held until the key has at least produced a hello. A grant carrying
         * something that is not a point on the curve must leave the helper as
         * it found it, not holding a key every later hello fails on.
         *
         * It is persisted at that point rather than after the ack, because the
         * ack is what the *next* hello needs the key to verify: a helper that
         * waited would have nothing to authenticate the hello it is about to
         * send after a restart.
         */
        let previous = boardPublicKey
        boardPublicKey = grant.boardPublicKey

        do {
            let hello = try buildHello(at: now)
            pairingRequestedAt = nil
            helloSentAt = now
            lastSentAt = now
            phase = .awaitingAck
            return [.storeBoardKey(grant.boardPublicKey), .note("paired by the device"), .send(hello)]
        } catch {
            boardPublicKey = previous
            return [.note("paired, but the hello could not be encoded: \(error)")]
        }
    }

    private func pairRefusedReceived(_ payload: [UInt8], at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            return [.note("ignoring a pair refused outside a session")]
        }
        guard let refused = try? PairRefused.decode(payload: payload) else {
            return [.note("ignoring a pair refused that could not be decoded")]
        }
        guard refused.correlation == pairCorrelation else {
            return [.note("ignoring pair refused with wrong correlation")]
        }

        switch refused.reason {
        case .noWindow:
            return [.note("pairing refused: no window open")]
        case .alreadyRegistered:
            return [.note("pairing refused: board already has a registration")]
        }
    }

    // MARK: - Session traffic

    private func deviceBeat(at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            return [.note("ignoring a device heartbeat outside a session")]
        }
        defer { lastDeviceBeatAt = now }

        guard let last = lastDeviceBeatAt else {
            return [.note("device heartbeat: first beat of the session")]
        }
        guard deviceBeatQuietNoted else { return [] }

        deviceBeatQuietNoted = false
        return [.note(String(format: "device heartbeat resumed after %.1fs", now - last))]
    }

    private func sessionEnded(_ body: [UInt8], at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            return [.note("ignoring a session end outside a session")]
        }
        let reason = SessionEndReason(wire: body.first ?? 0)
        return dropConnection(note: "the device ended the session: \(reason)", at: now)
    }

    private func listenerAlertReceived(_ body: [UInt8],
                                       at now: TimeInterval) -> [SessionOutput] {
        guard let alert = try? ListenerAlert.decode(body: body) else {
            return [.note("listener_alert could not be decoded")]
        }

        /* The alert is a rate, so it expires like one. The board measured it
           over a window and says so; if nothing further arrives within another
           such window, whatever was writing has stopped, and leaving the
           warning up for the rest of the session would make it a latch rather
           than a reading — the mistake #94 corrected for reconnections. */
        listenerAlertUntil = now + TimeInterval(alert.windowMs) / 1000

        return [.note("listener detected: \(alert.refused) refused frames in "
                       + "\(alert.windowMs)ms")]
            + emit(.listenerDetected)
    }

    // MARK: - Tick

    private func tick(at now: TimeInterval) -> [SessionOutput] {
        var outputs: [SessionOutput] = []

        if let deferred = deferredState, now >= deferred.at {
            deferredState = nil
            outputs += emit(deferred.state)
        } else if !everSawDevice, let startedAt, now - startedAt >= Self.silenceWindow {
            outputs += emit(.deviceAbsent)
        }

        switch phase {
        case .awaitingAck where now - helloSentAt >= Self.helloTimeout:
            outputs += dropConnection(note: "no hello_ack within \(Self.helloTimeout)s", at: now)

        case .live:
            /* Liveness — scoped to a session. An unpaired helper (negotiated
               == nil, phase == .live) has no session to time out. */
            if negotiated != nil, now - lastDeviceFrameAt >= Self.deviceBeatTimeout {
                return outputs + dropConnection(
                    note: "nothing from the device in \(Self.deviceBeatTimeout)s", at: now)
            }

            /* The repeated reconnection aged out — the link is holding. */
            if state == .reconnectingRepeatedly, negotiated != nil,
               !reconnectingRepeatedly(at: now) {
                outputs += emit(.connected)
            }

            /* The listener alert aged out — nothing further was refused. */
            if state == .listenerDetected, negotiated != nil,
               let until = listenerAlertUntil, now >= until {
                listenerAlertUntil = nil
                outputs += emit(.connected)
            }

            /* The beat stopped while the session did not. Scoped to a session
               for the same reason liveness is. */
            if negotiated != nil, let last = lastDeviceBeatAt, !deviceBeatQuietNoted,
               now - last >= Self.deviceBeatTimeout {
                deviceBeatQuietNoted = true
                outputs.append(.note(String(format: "device heartbeat quiet for %.1fs",
                                            now - last)))
            }

            /* Heartbeat — fills an idle direction with an authenticated frame.
               Only possible when we hold session keys. */
            if let key = kH2B, now - lastSentAt >= Self.heartbeatInterval {
                if let beat = try? AuthFrame.wrap(type: MessageType.heartbeat,
                                                  key: key, counter: txCounter) {
                    txCounter += 1
                    lastSentAt = now
                    outputs.append(.send(beat))
                }
            }

            /* Pair request — when unpaired, periodically ask. Untagged, no key
               needed. The board answers with silence outside a pairing window. */
            if state == .notPaired,
               now - (pairingRequestedAt ?? 0) >= Self.pairingRetryInterval,
               let request = pairRequestFrame(at: now) {
                outputs.append(.send(request))
            }

        default:
            break
        }

        return outputs
    }

    /// A fresh PAIR_REQUEST, and the correlation value a grant must echo back
    /// before this helper will act on it (#108).
    private func pairRequestFrame(at now: TimeInterval) -> [UInt8]? {
        pairCorrelation = freshCorrelation()
        guard let request = try? PairRequest(correlation: pairCorrelation,
                                             helperPublicKey: identity.publicKey).encoded() else {
            return nil
        }
        pairingRequestedAt = now
        lastSentAt = now
        return request
    }

    // MARK: - Hello builder

    /* One place for the hello encoding — channelsAcquired and pairGranted both
       need it. Fresh nonce and correlation each time. */
    private func buildHello(at now: TimeInterval) throws -> [UInt8] {
        let nonce = entropy(Int(DH_NONCE_SIZE))
        try checkNonce(nonce)
        let correlation = freshCorrelation()

        self.helperNonce = nonce
        self.helloCorrelation = correlation

        let kHello: [UInt8]
        if let boardKey = boardPublicKey {
            kHello = try identity.deriveHelloKey(boardPublicKey: boardKey, helperNonce: nonce)
        } else {
            /* Unpaired: the board checks registration before the tag, so a
               dummy key produces a frame the board refuses with
               HELLO_REFUSED(unpaired) without ever verifying the tag. */
            kHello = deriveHelloKeyFromSharedSecret(
                [UInt8](repeating: 0, count: Int(DH_P256_SHARED_SIZE)),
                helperNonce: nonce)
        }

        return try Hello(
            correlation: correlation,
            helperKeyId: identity.keyId,
            helperNonce: nonce
        ).encoded(key: kHello)
    }

    /* `entropy` is injected, so its result is treated as input rather than
       trusted: `copyBytes` traps on an over-long source and reads uninitialised
       bytes on a short one. Assembled a byte at a time instead, which is
       correct for any length the closure returns. */
    private func freshCorrelation() -> UInt64 {
        let bytes = entropy(8)
        var value: UInt64 = 0
        for (i, byte) in bytes.prefix(8).enumerated() {
            value |= UInt64(byte) << (UInt64(i) * 8)
        }
        return value
    }

    // MARK: - Reconnection rate

    private func recordDrop(at now: TimeInterval) {
        guard holdingChannels || phase != .idle else { return }
        recentDrops.append(now)
        if recentDrops.count > Self.reconnectLimit { recentDrops.removeFirst() }
    }

    private func reconnectingRepeatedly(at now: TimeInterval) -> Bool {
        guard recentDrops.count >= Self.reconnectLimit, let oldest = recentDrops.first else {
            return false
        }
        return now - oldest <= Self.reconnectWindow
    }

    private func emitRepeatedReconnection(at now: TimeInterval) -> [SessionOutput] {
        let reported = emit(.reconnectingRepeatedly)
        guard !reported.isEmpty, let oldest = recentDrops.first else { return reported }
        return [.note(String(format: "the last %d reconnections came inside %.1fs",
                             recentDrops.count, now - oldest))] + reported
    }

    private func dropConnection(note: String, at now: TimeInterval) -> [SessionOutput] {
        recordDrop(at: now)
        forgetSession()
        holdingChannels = false
        deferredState = (.deviceAbsent, now + Self.silenceWindow)

        let falsifiedByTheRate: [HelperState] = [.connected, .quiet]
        let rate = reconnectingRepeatedly(at: now) && falsifiedByTheRate.contains(state)
            ? emitRepeatedReconnection(at: now)
            : []

        return [.note(note), .closeChannels] + rate + [.retry(after: backoff.next())]
    }

    private func emit(_ next: HelperState) -> [SessionOutput] {
        guard next != state else { return [] }
        state = next
        return [.state(next)]
    }
}
