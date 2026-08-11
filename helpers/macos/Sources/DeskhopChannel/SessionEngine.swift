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
     * A helper beats at the interval the shared core defines, so the device's
     * "absent after a couple of missed intervals" is measured against the
     * same number at both ends.
     *
     * Known gap (#68): the heartbeat has no acknowledgement in v1, so if the
     * device drops the session on its side — its liveness timeout, or a
     * framing error on its reader — this helper cannot tell, and goes on
     * reporting a session that is gone. Closing it means a wire-format change
     * (an acknowledged beat, or a periodic device-to-helper frame) and
     * belongs in a protocol decision rather than here.
     */
    public static let heartbeatInterval = TimeInterval(DH_SESSION_HEARTBEAT_MS) / 1000

    /*
     * How long the device may be gone before the user is told anything. A
     * config-mode round trip and an ordinary re-enumeration both look like
     * this at first, and neither is worth a notification.
     */
    public static let silenceWindow: TimeInterval = 5

    /* A device that takes the hello and says nothing is not a working
       session; give up and re-acquire rather than sit on a half-open one. */
    public static let helloTimeout: TimeInterval = 2

    private enum Phase {
        case idle
        case awaitingAck
        case live
    }

    private var phase: Phase = .idle
    private var backoff = Backoff()
    private var stream = FrameStream()
    private var helloSentAt: TimeInterval = 0
    private var lastHeartbeatAt: TimeInterval = 0
    private var holdingChannels = false
    /* A state the user will be told about once the silence window passes. */
    private var deferredState: (state: HelperState, at: TimeInterval)?
    /* The token every hello carries. nil until the device grants one. */
    private var secret: [UInt8]?
    private var pairingRequestedAt: TimeInterval?
    /* A helper that starts before the device is attached — the ordinary case
       at login — reports the absence on the same delay as one that loses it. */
    private var startedAt: TimeInterval?
    private var everSawDevice = false

    public private(set) var state: HelperState = .quiet
    public private(set) var negotiated: Negotiated?

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
            return dropConnection(note: "transport failed: \(reason)")
        case .tick:
            return tick(at: now)
        }
    }

    // MARK: - Device presence

    private func deviceAppeared() -> [SessionOutput] {
        deferredState = nil
        everSawDevice = true
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
        phase = .idle
        negotiated = nil
        stream.reset()
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
        deferredState = nil

        do {
            return [.send(try Hello(token: secret ?? []).encoded())]
        } catch {
            /* Encoding a hello cannot fail against a working core; if it ever
               does, say so rather than sit silently in awaitingAck. */
            phase = .idle
            return [.note("hello could not be encoded: \(error)"),
                    .closeChannels,
                    .retry(after: backoff.next())]
        }
    }

    private func acquisitionRefused(acquired: Int, of total: Int) -> [SessionOutput] {
        phase = .idle
        holdingChannels = false
        negotiated = nil

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
            return dropConnection(note: "protocol error on the channel: \(error)")
        }

        var outputs: [SessionOutput] = []
        for frame in frames {
            switch frame.type {
            case MessageType.helloAck:
                outputs += helloAck(frame, at: now)

            case MessageType.pairGrant:
                outputs += pairGranted(frame, at: now)
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
            return dropConnection(note: "hello_ack payload could not be decoded")
        }

        lastHeartbeatAt = now
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
            return outputs + emit(.connected)

        case .authenticationFailed:
            /* The session stays up and asking: the pairing window (#46)
               provisions a helper that is connected when it opens, and this
               helper is only connected if it keeps a session alive. */
            phase = .live
            negotiated = nil
            pairingRequestedAt = nil
            return emit(.notPaired)

        case .versionIncompatible:
            /*
             * The device dropped the session on its side, so there is nothing
             * to keep alive — beating at a peer that will never answer would
             * only look like a session. The channels stay held so nothing
             * else takes them, and a firmware or helper update re-enumerates,
             * which is what starts the next attempt.
             */
            phase = .idle
            negotiated = nil
            return [.note("device speaks protocol version \(ack.protocolVersion), "
                          + "this helper speaks \(DH_PROTO_VERSION)")]
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
        guard frame.payload.count == SecretStore.length else {
            return [.note("ignoring a pair grant carrying \(frame.payload.count) bytes")]
        }

        secret = frame.payload
        pairingRequestedAt = nil

        guard let hello = try? Hello(token: frame.payload).encoded() else {
            return [.note("paired, but the hello could not be encoded")]
        }
        helloSentAt = now
        phase = .awaitingAck
        return [.storeSecret(frame.payload), .note("paired by the device"), .send(hello)]
    }

    private func tick(at now: TimeInterval) -> [SessionOutput] {
        var outputs: [SessionOutput] = []

        if let deferred = deferredState, now >= deferred.at {
            deferredState = nil
            outputs += emit(deferred.state)
        } else if !everSawDevice, let startedAt, now - startedAt >= Self.silenceWindow {
            /* Nothing has ever been attached. The helper may well have
               started before the device — a LaunchAgent runs at login — so
               this waits out the same window before saying so. */
            outputs += emit(.deviceAbsent)
        }

        switch phase {
        case .awaitingAck where now - helloSentAt >= Self.helloTimeout:
            outputs += dropConnection(note: "no hello_ack within \(Self.helloTimeout)s")

        case .live:
            /*
             * Beating and asking are independent, not alternatives. An
             * unpaired helper must keep the session alive *and* keep asking:
             * the window can only provision a helper that is connected when
             * the user presses the chord, and it stays connected by beating.
             */
            if now - lastHeartbeatAt >= Self.heartbeatInterval {
                lastHeartbeatAt = now
                if let beat = try? FrameCodec.encode(Frame(type: MessageType.heartbeat)) {
                    outputs.append(.send(beat))
                }
            }

            /* The device answers with silence outside a window, so this costs
               one frame every couple of seconds and no state on either side. */
            if state == .notPaired,
               now - (pairingRequestedAt ?? 0) >= Self.pairingRetryInterval {
                pairingRequestedAt = now
                if let request = try? FrameCodec.encode(Frame(type: MessageType.pairRequest)) {
                    outputs.append(.send(request))
                }
            }

        default:
            break
        }

        return outputs
    }

    private func dropConnection(note: String) -> [SessionOutput] {
        phase = .idle
        holdingChannels = false
        negotiated = nil
        stream.reset()
        return [.note(note), .closeChannels, .retry(after: backoff.next())]
    }

    /* State is reported on change only — a menu bar item that rewrites itself
       every tick is noise, and the transport logs on every output. */
    private func emit(_ next: HelperState) -> [SessionOutput] {
        guard next != state else { return [] }
        state = next
        return [.state(next)]
    }
}
