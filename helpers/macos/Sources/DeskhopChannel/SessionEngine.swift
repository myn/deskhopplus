import DHCore
import Foundation

/*
 * The helper's side of the session, as pure logic: events in, actions out, no
 * IOKit and no clock of its own. Everything the acceptance criteria care about
 * — the hello exchange, the heartbeat, all-or-nothing exclusivity, the states
 * the user is shown, and reconnection with a capped backoff — is decided here
 * and therefore tested on a laptop with no device attached.
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
    /* A secret the device just granted. Storing it is the transport's job;
       the engine only decides that it is worth keeping. */
    case storeSecret([UInt8])
    case openChannels
    case closeChannels
    case send([UInt8])
    case state(HelperState)
    case retry(after: TimeInterval)
    /* Diagnostics, never shown to the user. */
    case note(String)
}

/* The effective parameters the device replied with — not what was asked for. */
public struct Negotiated: Equatable {
    public let channelCount: UInt8
    public let maxChunk: UInt16
    public let deviceBuild: BuildType
}

public final class SessionEngine {
    /*
     * A helper beats at the interval the shared core defines, so liveness is
     * measured against the same number at both ends. The beat only fills a
     * direction that has carried nothing for a full interval — traffic is the
     * measurement, and the beat exists so that silence is unambiguous
     * (ADR-0004).
     */
    public static let heartbeatInterval = TimeInterval(DH_SESSION_HEARTBEAT_MS) / 1000

    /*
     * How long the *device* may say nothing at all before this helper
     * concludes the session is gone. The device applies the same deadline to
     * this helper, from the same constant, so neither end is more patient
     * than the other.
     *
     * Deliberately not called a silence window: `silenceWindow` below is a
     * different quantity for a different purpose, and conflating a 3-second
     * protocol deadline with a 5-second cosmetic one is how this gets broken.
     */
    public static let deviceBeatTimeout = TimeInterval(DH_SESSION_ABSENT_MS) / 1000

    /*
     * How long the device may be gone before the user is told anything. A
     * config-mode round trip, an ordinary re-enumeration and a session
     * reconnecting all look like this at first, and none is worth a
     * notification.
     */
    public static let silenceWindow: TimeInterval = 5

    /* A device that takes the hello and says nothing is not a working
       session; give up and re-acquire rather than sit on a half-open one. */
    public static let helloTimeout: TimeInterval = 2

    /*
     * How often the connection may be rebuilt before that is worth reporting
     * in its own right. Every individual cycle passes the silence window
     * above — the connection is back long before it closes — so without a
     * rate nothing is left to notice a helper reconnecting once a second
     * (#94), which is the shape a wire-format mismatch takes.
     *
     * Both numbers are a judgement call, unlike the absence of any threshold.
     * One reconnection is the ordinary recovery, and a re-enumeration, a
     * config-mode round trip and a laptop waking up each cost one; four
     * inside half a minute is not a link doing its job.
     */
    public static let reconnectWindow: TimeInterval = 30
    public static let reconnectLimit = 4

    private enum Phase {
        case idle
        case awaitingAck
        case live
    }

    private var phase: Phase = .idle
    private var backoff = Backoff()
    private var stream = FrameStream()
    private var helloSentAt: TimeInterval = 0
    /* When this helper last had something to say, and when the device last
       did. The beat fills the gaps in the first; the second is the detector. */
    private var lastSentAt: TimeInterval = 0
    private var lastDeviceFrameAt: TimeInterval = 0
    /*
     * The device's own beat, tracked apart from lastDeviceFrameAt because
     * ADR-0004 gates it on an idle direction: through a transfer, frames keep
     * arriving while beats correctly stop, so the two quantities diverge and
     * only this one answers "is the beat running". nil until the first beat
     * of a session. Diagnostic — nothing decides on it.
     */
    private var lastDeviceBeatAt: TimeInterval?
    private var deviceBeatQuietNoted = false
    private var holdingChannels = false
    /* When the connection was last lost, oldest first and never more than
       reconnectLimit of them — only the reconnectLimit-th most recent decides
       anything, so the rest are not worth keeping. */
    private var recentDrops: [TimeInterval] = []
    /* A state the user will be told about once the silence window passes. */
    private var deferredState: (state: HelperState, at: TimeInterval)?
    /* The token every hello carries. nil until the device grants one. */
    private var secret: [UInt8]?
    private var pairingRequestedAt: TimeInterval?
    /* A helper that starts before the device is attached — the ordinary case
       at login — reports the absence on the same delay as one that loses it. */
    private var startedAt: TimeInterval?
    /* Under any identity, config mode included — see handle(_:at:). */
    private var everSawDevice = false

    public private(set) var state: HelperState = .quiet
    public private(set) var negotiated: Negotiated?

    /*
     * May bulk be sent? Only a helper that actually holds a session may put
     * clipboard payloads on it — otherwise it offers content that never
     * arrives and the paste side waits on a transfer nobody is sending.
     *
     * This is the same predicate the liveness detector is scoped to, exposed
     * once: a helper refused authentication is deliberately `.live` and must
     * not be mistaken for one with a session. #52 consumes this rather than
     * inventing its own condition.
     */
    public var canSendBulk: Bool { phase == .live && negotiated != nil }

    /*
     * How often an unpaired helper asks to be paired. The window is about a
     * minute and the user may press the chord at any point in it, so asking
     * periodically is what makes provisioning silent — the alternative is
     * telling the user to press the chord *and then* restart the helper.
     */
    public static let pairingRetryInterval: TimeInterval = 2

    public init(secret: [UInt8]? = nil) {
        self.secret = secret
    }

    public func handle(_ input: SessionInput, at now: TimeInterval) -> [SessionOutput] {
        if startedAt == nil { startedAt = now }

        /*
         * Any identity at all means a device is attached. Config mode is the
         * same board under a different one (#33), so a helper that has seen
         * it has seen a device, and the "nothing was ever attached" fallback
         * in tick() must not fire behind it (#73) — that told the user the
         * device was not connected for the whole of a config session, which
         * is exactly the window in which they are looking for reassurance.
         *
         * Set here rather than in the per-identity handlers so that a third
         * identity cannot reintroduce this by forgetting to.
         */
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
            return acquisitionRefused(acquired: acquired, of: total)
        case .received(let bytes):
            return received(bytes, at: now)
        case .transportFailed(let reason):
            return dropConnection(note: "transport failed: \(reason)", at: now)
        case .tick:
            return tick(at: now)
        }
    }

    // MARK: - Device presence

    /*
     * Everything a session leaves behind, in the one place every way out of
     * one reaches: the device going away, the connection dying underneath us,
     * an acquisition refused, a version the device will not speak, and a
     * hello that could not be encoded. Kept together so a sixth cannot
     * reintroduce the leak by forgetting a field — which is exactly how the
     * beat trace broke (#98). It survived a dropped connection and not a
     * config-mode round trip, and the round trip is the common path.
     *
     * The frame stream goes with it: a half-frame read from a session that
     * has ended must never be glued to the front of the next one.
     *
     * Deliberately not `holdingChannels`, and not the deferred state. Those
     * two differ between the callers — one releases handles it may not hold,
     * another keeps them on purpose, and each names its own reason for the
     * silence — so folding them in would make this lie about one caller.
     */
    private func forgetSession() {
        phase = .idle
        negotiated = nil
        stream.reset()
        forgetBeatTrace()
    }

    /*
     * The beat belongs to a session: the next one's first beat is worth a
     * line of its own, and a quiet spell does not outlive the session it was
     * measured in.
     *
     * Split out of forgetSession() for the one path that loses a session
     * without going idle — a helper refused authentication stays live,
     * beating and asking, because #46 can only provision one that is
     * connected when the chord lands. A teardown keyed on the phase steps
     * straight past it, and then the session pairing establishes has its
     * first beat swallowed: not the first any more, and with no quiet spell
     * behind it to make it a resumption, it says nothing at all.
     */
    private func forgetBeatTrace() {
        lastDeviceBeatAt = nil
        deviceBeatQuietNoted = false
    }

    private func deviceAppeared() -> [SessionOutput] {
        deferredState = nil
        backoff.reset()

        /*
         * Clear only a state the device's return has just falsified. A helper
         * that was showing "connected" when the device blinked keeps showing
         * it: the whole point of the silence window is that a brief
         * disappearance produces no visible change at all, and clearing to
         * nothing here would be a visible change.
         */
        let stale: [HelperState] = [.deviceAbsent, .deviceInConfigMode, .channelHeld]
        let outputs = stale.contains(state) ? emit(.quiet) : []
        return outputs + [.openChannels]
    }

    private func deviceLeft(for reason: HelperState, at now: TimeInterval) -> [SessionOutput] {
        recordDrop(at: now)
        forgetSession()
        /* Silence at first: the state is held back until the disappearance
           has lasted long enough to be worth mentioning. */
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
            return [.send(try Hello(token: secret ?? []).encoded())]
        } catch {
            /* Encoding a hello cannot fail against a working core; if it ever
               does, say so rather than sit silently in awaitingAck. */
            forgetSession()
            return [.note("hello could not be encoded: \(error)"),
                    .closeChannels,
                    .retry(after: backoff.next())]
        }
    }

    private func acquisitionRefused(acquired: Int, of total: Int) -> [SessionOutput] {
        forgetSession()
        holdingChannels = false
        /*
         * We now know something more useful than whatever a preceding drop
         * deferred. A dropped connection arms "the device is absent" against
         * the silence window; if the retry then finds the channel taken, the
         * device is plainly not absent and the remedy is a different one —
         * letting the stale deferral fire would replace an actionable state
         * with a misleading one, minutes after the fact.
         */
        deferredState = nil

        /*
         * All or nothing (ADR-0002). A partial acquisition is worse than an
         * outright failure: the other holder would silently receive part of
         * every transfer while both sides looked healthy. And this state
         * never prompts the config chord — the program holding the channel is
         * exactly what the chord would provision (#34).
         */
        let note = acquired > 0
            ? "released \(acquired) of \(total) channels: a partial acquisition is not a session"
            : "every channel refused"
        return [.closeChannels, .note(note)] + emit(.channelHeld) + [.retry(after: backoff.next())]
    }

    // MARK: - Traffic

    private func received(_ bytes: [UInt8], at now: TimeInterval) -> [SessionOutput] {
        let frames: [Frame]
        do {
            frames = try stream.push(bytes)
        } catch {
            /* A protocol error means the byte stream is no longer
               trustworthy: drop the connection and reconnect. */
            return dropConnection(note: "protocol error on the channel: \(error)", at: now)
        }

        /*
         * Any frame at all proves the device is alive *and* holding a session
         * for this helper, since it relays nothing and answers nothing for a
         * peer it has no session with. That is what liveness is measured on;
         * the device's beat merely fills the silence when there is no other
         * traffic to read it from (ADR-0004).
         */
        if !frames.isEmpty { lastDeviceFrameAt = now }

        var outputs: [SessionOutput] = []
        for frame in frames {
            switch frame.type {
            case MessageType.helloAck:
                outputs += helloAck(frame, at: now)

            case MessageType.pairGrant:
                outputs += pairGranted(frame, at: now)

            case MessageType.sessionEnd:
                outputs += sessionEnded(frame, at: now)

            case MessageType.deviceHeartbeat:
                /* Liveness is already recorded above, by virtue of the frame
                   having arrived at all. What is left is the trace. */
                outputs += deviceBeat(at: now)
            default:
                /* Placement and the bulk band belong to later tickets; a
                   frame this build does not act on is not an error. */
                continue
            }
        }
        return outputs
    }

    private func helloAck(_ frame: Frame, at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            /* An answer to a hello this helper is no longer waiting on — the
               tail of a session that was already dropped. Acting on it would
               report a session that does not exist. */
            return [.note("ignoring a hello_ack outside a session")]
        }
        guard let ack = try? HelloAck.decode(payload: frame.payload) else {
            return dropConnection(note: "hello_ack payload could not be decoded", at: now)
        }

        backoff.reset()

        switch ack.status {
        case .ok:
            phase = .live
            negotiated = Negotiated(channelCount: ack.channelCount,
                                    maxChunk: ack.maxChunk,
                                    deviceBuild: ack.buildType)
            var outputs: [SessionOutput] = []
            if ack.buildType == .development {
                outputs.append(.note("device is a development build: channel authentication "
                                     + "is compiled out"))
            }

            /*
             * The successful reconnection is exactly what used to put
             * `connected` back on the screen once a second. A helper that has
             * been rebuilding this connection all along says so instead, and
             * goes on saying it until the window passes quietly — see tick().
             */
            guard reconnectingRepeatedly(at: now) else { return outputs + emit(.connected) }
            return outputs + emitRepeatedReconnection(at: now)

        case .authenticationFailed:
            /* The session stays up and asking: the pairing window (#46)
               provisions a helper that is connected when it opens, and this
               helper is only connected if it keeps a session alive. */
            phase = .live
            negotiated = nil
            pairingRequestedAt = nil
            /* The session is gone even though the phase is not, so the beat
               measured in it goes too — see forgetBeatTrace(). */
            forgetBeatTrace()
            return emit(.notPaired)

        case .versionIncompatible:
            /*
             * The device dropped the session on its side, so there is nothing
             * to keep alive — beating at a peer that will never answer would
             * only look like a session. The channels stay held so nothing
             * else takes them, and a firmware or helper update re-enumerates,
             * which is what starts the next attempt. The channels staying held
             * is why holdingChannels is not forgetSession()'s business.
             */
            forgetSession()
            return [.note("device speaks protocol version \(ack.protocolVersion), "
                          + "this helper speaks \(DH_PROTO_VERSION_V1)")]
                + emit(.versionIncompatible)
        }
    }

    /*
     * The window provisioned us. Storing the secret and saying hello again is
     * the whole of pairing — and the state changing to connected is the
     * confirmation the user was told to expect (#34): a chord press that does
     * not produce it is the signal that something else may have been
     * provisioned instead.
     */
    private func pairGranted(_ frame: Frame, at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            /* A grant for a session this helper has already dropped. Acting
               on it would send a hello down a channel that is closed and then
               sit in awaitingAck until it timed out, on top of a reconnection
               already scheduled. */
            return [.note("ignoring a pair grant outside a session")]
        }
        guard frame.payload.count == SecretStore.length else {
            return [.note("ignoring a pair grant carrying \(frame.payload.count) bytes")]
        }

        secret = frame.payload
        pairingRequestedAt = nil

        guard let hello = try? Hello(token: frame.payload).encoded() else {
            return [.note("paired, but the hello could not be encoded")]
        }
        helloSentAt = now
        lastSentAt = now
        phase = .awaitingAck
        return [.storeSecret(frame.payload), .note("paired by the device"), .send(hello)]
    }

    /*
     * The device's beat, traced at its edges only: the first of a session,
     * and a return after it had gone quiet. ADR-0004 stops the beat on a
     * direction that is carrying traffic, so a gap is ordinarily the design
     * working — but it is also what a stalled device looks like, and the two
     * are indistinguishable without this. Nothing decides on it; a beat that
     * never arrives is caught by the liveness deadline in tick().
     *
     * Per-beat logging is deliberately not offered. At a beat a second it
     * buries everything else in the log, which is exactly how a helper
     * thrashing on an unknown frame type went unnoticed for two days (#94).
     */
    private func deviceBeat(at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            /* A beat already in the read queue when the connection went, like
               the tail of any other frame type. Acting on it would announce
               the first beat of a session that does not exist — and then
               swallow the real one, which is only first while nothing has
               claimed it. */
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

    /*
     * The device evicted us and said so, rather than leaving it to the
     * timeout. The reason is a log line and nothing more: every one takes the
     * same recovery, and an unrecognised one is treated as unspecified so a
     * later device can end a session for a reason this build predates.
     *
     * The backoff is deliberately *not* reset here. An orderly end is still a
     * reason to reconnect at the current delay — a device ending sessions
     * repeatedly would otherwise spin this helper in a tight loop, and a
     * successful hello_ack is the right and only place to declare things
     * well again.
     */
    private func sessionEnded(_ frame: Frame, at now: TimeInterval) -> [SessionOutput] {
        guard phase != .idle else {
            /* The tail of a session already dropped. Acting on it would tear
               down a reconnection that is already in flight. */
            return [.note("ignoring a session end outside a session")]
        }
        let reason = SessionEndReason(wire: frame.payload.first ?? 0)
        return dropConnection(note: "the device ended the session: \(reason)", at: now)
    }

    private func tick(at now: TimeInterval) -> [SessionOutput] {
        var outputs: [SessionOutput] = []

        if let deferred = deferredState, now >= deferred.at {
            deferredState = nil
            outputs += emit(deferred.state)
        } else if !everSawDevice, let startedAt, now - startedAt >= Self.silenceWindow {
            /* No device has ever been seen, under *either* identity — see
               where everSawDevice is set. The helper may well have started
               before the device, since a LaunchAgent runs at login, so this
               waits out the same window before saying so. */
            outputs += emit(.deviceAbsent)
        }

        switch phase {
        case .awaitingAck where now - helloSentAt >= Self.helloTimeout:
            outputs += dropConnection(note: "no hello_ack within \(Self.helloTimeout)s", at: now)

        case .live:
            /*
             * The device has gone quiet for longer than it is allowed to.
             * Scoped to a session rather than to the phase: a helper refused
             * authentication is deliberately live, beating and asking, and
             * the device holds no session for it and so sends it nothing —
             * tearing that down here would break #46's provisioning outright.
             */
            if negotiated != nil, now - lastDeviceFrameAt >= Self.deviceBeatTimeout {
                return outputs + dropConnection(
                    note: "nothing from the device in \(Self.deviceBeatTimeout)s", at: now)
            }

            /*
             * The rebuilding has aged out of the window and this connection
             * held throughout it, so it is a connected one again — the state
             * reports a rate, and a rate recovers. Only ever from this state:
             * every other one is a more specific diagnosis that outlives the
             * window on its own.
             */
            if state == .reconnectingRepeatedly, negotiated != nil,
               !reconnectingRepeatedly(at: now) {
                outputs += emit(.connected)
            }

            /*
             * The beat stopped while the session did not — the device is
             * still sending, so the check above is satisfied, but the
             * idle-gated beat has dried up. Expected under a transfer and
             * suspicious otherwise, which is a distinction only the operator
             * can make, so it is reported rather than acted on. Once per
             * quiet spell; deviceBeat() arms it again.
             *
             * Scoped to a session, exactly as the check above it is and for
             * the same reason: a helper refused authentication is deliberately
             * live, and the device holds no session for it and so sends it
             * nothing. Tracing the absence of beats that are not supposed to
             * exist is noise in the log this trace exists to keep readable
             * (#98).
             */
            if negotiated != nil, let last = lastDeviceBeatAt, !deviceBeatQuietNoted,
               now - last >= Self.deviceBeatTimeout {
                deviceBeatQuietNoted = true
                outputs.append(.note(String(format: "device heartbeat quiet for %.1fs", now - last)))
            }

            /*
             * Beating and asking are independent, not alternatives. An
             * unpaired helper must keep the session alive *and* keep asking:
             * the window can only provision a helper that is connected when
             * the user presses the chord, and it stays connected by beating.
             *
             * The beat fills an idle direction only — anything else this
             * helper sent has already told the device the same thing.
             */
            if now - lastSentAt >= Self.heartbeatInterval {
                if let beat = try? FrameCodec.encode(Frame(type: MessageType.heartbeat)) {
                    lastSentAt = now
                    outputs.append(.send(beat))
                }
            }

            /* The device answers with silence outside a window, so this costs
               one frame every couple of seconds and no state on either side. */
            if state == .notPaired,
               now - (pairingRequestedAt ?? 0) >= Self.pairingRetryInterval {
                pairingRequestedAt = now
                if let request = try? FrameCodec.encode(Frame(type: MessageType.pairRequest)) {
                    lastSentAt = now
                    outputs.append(.send(request))
                }
            }

        default:
            break
        }

        return outputs
    }

    // MARK: - How often the connection is rebuilt

    /*
     * Every loss of a connection this helper actually had, whatever took it —
     * a protocol error, a failed write, the device ending the session, or the
     * link itself going away. All of them are recovered the same way and all
     * of them are invisible one at a time, so all of them are counted here.
     *
     * The guard is what keeps the count honest: a device that disappears
     * twice, or a write that fails after the connection is already gone,
     * loses nothing the second time.
     *
     * Call this *before* forgetSession(), which clears the state the guard
     * reads — reversed, every drop would look like the second one and the
     * rate would never rise.
     */
    private func recordDrop(at now: TimeInterval) {
        guard holdingChannels || phase != .idle else { return }
        recentDrops.append(now)
        if recentDrops.count > Self.reconnectLimit { recentDrops.removeFirst() }
    }

    /* Whether the last reconnectLimit drops all fall inside the window. */
    private func reconnectingRepeatedly(at now: TimeInterval) -> Bool {
        guard recentDrops.count >= Self.reconnectLimit, let oldest = recentDrops.first else {
            return false
        }
        return now - oldest <= Self.reconnectWindow
    }

    /*
     * The rate, said once and with its measurement attached — a line per
     * cycle is how #94 stayed invisible for two days to begin with.
     *
     * "The last N" rather than a total, because that is what is measured:
     * only the most recent reconnectLimit drops are kept, so a running count
     * would read the same number however many there had been.
     */
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
        /*
         * Held back, exactly as a device that physically disappears is: a
         * connection that comes back inside the window produces no visible
         * change at all, and one that does not has become, from the user's
         * side, a device that is not there. No second vocabulary for a
         * second kind of gone — and no leaving the menu bar reading
         * *connected* for a session that ended, which is the whole of #68.
         */
        deferredState = (.deviceAbsent, now + Self.silenceWindow)

        /*
         * Reported here as well as on a session coming up, because a
         * connection that keeps dying may never reach another hello_ack: a
         * device that takes the hello and says nothing loops on the timeout
         * above, and the deferral just armed is cleared by the next
         * acquisition a second before it comes due. A rate read only where a
         * session establishes would never be read at all in that loop.
         *
         * Only where the rate falsifies what is on the screen: `connected`
         * claims a link that is holding, and `quiet` claims there is nothing
         * to say. Every other state is a more specific diagnosis with its own
         * remedy and keeps its place — a channel somebody else holds does not
         * become this.
         */
        let falsifiedByTheRate: [HelperState] = [.connected, .quiet]
        let rate = reconnectingRepeatedly(at: now) && falsifiedByTheRate.contains(state)
            ? emitRepeatedReconnection(at: now)
            : []

        return [.note(note), .closeChannels] + rate + [.retry(after: backoff.next())]
    }

    /* State is reported on change only — a menu bar item that rewrites itself
       every tick is noise, and the transport logs on every output. */
    private func emit(_ next: HelperState) -> [SessionOutput] {
        guard next != state else { return [] }
        state = next
        return [.state(next)]
    }
}
