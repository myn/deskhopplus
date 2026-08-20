import DHCore
import Foundation

/*
 * The helper's side of the session — as a **binding onto the shared core's
 * machine** (`dh_helper`, #79/#80/#81), not a machine of its own.
 *
 * Nothing here decides anything. The hello exchange, negotiation, ADR-0004's
 * liveness, pairing, all-or-nothing acquisition, the capped backoff and the
 * states a user is shown are all `src/core/dh_helper.c` — the same object code
 * the firmware compiles, and the same machine #49's Windows helper will drive.
 * This file carries events in, carries outputs back, and owns the two halves
 * the core deliberately refuses: the *wording* (HelperNotes, HelperState) and
 * the Secure Enclave, which cannot hand a private key to C at all.
 *
 * What it replaced was a second implementation of that machine, 761 lines of
 * Swift only macOS could run. Two of them would have given ADR-0004's
 * traffic-gated liveness two chances to be got right, failing differently on
 * two operating systems under load, in a way that looks like a hardware fault.
 *
 * The transport (HIDChannelSet) does what it is told and reports what it sees.
 */

public enum DeviceIdentity: Equatable {
    case normal
    /* Config mode reboots the device under a different USB identity. Seeing
       it tells the helper exactly what happened, which is why it is a state
       of its own and not "the device is gone". */
    case configMode

    var core: dh_device_identity { self == .configMode ? DH_DEVICE_CONFIG_MODE : DH_DEVICE_NORMAL }
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
       dropped connection, not a retryable write.

       The reason is carried for the log only: the core takes the failure, not
       the sentence. */
    case transportFailed(String)
    case tick

    var transportReason: String? {
        if case .transportFailed(let reason) = self { return reason }
        return nil
    }
}

public enum SessionOutput: Equatable {
    /* The board's public key, from a PAIR_GRANT this helper asked for.
       Storing it is the runtime's job; the core only decides it is worth
       keeping — and never emits one for a board whose key has changed. */
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

/*
 * What the core is handed back on every callback. Held strongly by the
 * session, because `dh_helper_identity.ctx` is a bare pointer to it and the
 * core calls through that for the life of the machine.
 */
private final class IdentityBox {
    let identity: HelperIdentity
    let entropy: (Int) -> [UInt8]

    init(identity: HelperIdentity, entropy: @escaping (Int) -> [UInt8]) {
        self.identity = identity
        self.entropy = entropy
    }
}

public final class HelperSession {
    /* The timings, read off the core rather than restated. Two of them are
       protocol and come from dh_session.h; the rest are dh_helper.h's policy. */
    public static let heartbeatInterval = seconds(DH_SESSION_HEARTBEAT_MS)
    public static let deviceBeatTimeout = seconds(UInt32(DH_SESSION_ABSENT_MS))
    public static let silenceWindow = seconds(DH_HELPER_SILENCE_MS)
    public static let helloTimeout = seconds(DH_HELPER_HELLO_TIMEOUT_MS)
    public static let reconnectWindow = seconds(DH_HELPER_RECONNECT_WINDOW_MS)
    public static let reconnectLimit = Int(DH_HELPER_RECONNECT_LIMIT)
    public static let pairingRetryInterval = seconds(DH_HELPER_PAIRING_RETRY_MS)
    /* The reconnection delay's ceiling. It sits *below* the silence window on
       purpose: a retry cadence slower than the window would push a deferred
       report out for ever. */
    public static let backoffCap = seconds(DH_HELPER_BACKOFF_CAP_MS)

    private static func seconds(_ ms: UInt32) -> TimeInterval { TimeInterval(ms) / 1000 }

    /* All three are heap-allocated with stable addresses: the core keeps a
       pointer to the identity for the life of the machine, and `dh_helper`
       is far too large to keep copying in and out of a Swift value. */
    private let machine = UnsafeMutablePointer<dh_helper>.allocate(capacity: 1)
    private let identity = UnsafeMutablePointer<dh_helper_identity>.allocate(capacity: 1)
    private let outputs = UnsafeMutablePointer<dh_helper_outputs>.allocate(capacity: 1)
    private let box: IdentityBox

    /// `boardPublicKey` is what the platform had stored, or nil for a helper
    /// that has never paired.
    ///
    /// `entropy` must return exactly the number of bytes it is asked for. It
    /// feeds nonces and correlation values, so a short draw would leave the
    /// core keying on bytes nobody chose — checked rather than padded.
    public init(identity helperIdentity: HelperIdentity, boardPublicKey: [UInt8]? = nil,
                entropy: @escaping (Int) -> [UInt8]) {
        box = IdentityBox(identity: helperIdentity, entropy: entropy)

        identity.initialize(to: dh_helper_identity())
        identity.pointee.ctx = Unmanaged.passUnretained(box).toOpaque()
        identity.pointee.os = UInt8(DH_OS_MAC.rawValue)
        identity.pointee.build_type = UInt8(DH_BUILD_RELEASE.rawValue)
        copy(helperIdentity.publicKey, into: &identity.pointee.public_key)
        copy(helperIdentity.keyId, into: &identity.pointee.key_id)
        identity.pointee.ecdh = { ctx, peer, out in
            guard let ctx, let peer, let out else { return false }
            let box = Unmanaged<IdentityBox>.fromOpaque(ctx).takeUnretainedValue()
            let peerKey = Array(UnsafeBufferPointer(start: peer, count: Int(DH_P256_PUBLIC_SIZE)))
            guard let shared = box.identity.sharedSecret(with: peerKey),
                  shared.count == Int(DH_P256_SHARED_SIZE) else { return false }
            shared.withUnsafeBufferPointer { out.update(from: $0.baseAddress!, count: $0.count) }
            return true
        }
        identity.pointee.entropy = { ctx, out, len in
            guard let ctx, let out else { return }
            let box = Unmanaged<IdentityBox>.fromOpaque(ctx).takeUnretainedValue()
            let bytes = box.entropy(len)
            precondition(bytes.count == len, "the entropy source returned \(bytes.count) "
                                             + "bytes where \(len) were asked for")
            bytes.withUnsafeBufferPointer { out.update(from: $0.baseAddress!, count: len) }
        }

        outputs.initialize(to: dh_helper_outputs())
        if let boardPublicKey, boardPublicKey.count == Int(DH_P256_PUBLIC_SIZE) {
            boardPublicKey.withUnsafeBufferPointer {
                dh_helper_init(machine, identity, $0.baseAddress)
            }
        } else {
            dh_helper_init(machine, identity, nil)
        }
    }

    deinit {
        machine.deallocate()
        identity.deallocate()
        outputs.deallocate()
    }

    /* `.quiet` is the fallback for a state this helper has no case for, which
       the state table in the tests exists to keep impossible. It is the safe
       wrong answer: it shows nothing, offers no chord and allows no bulk. The
       output path says so out loud rather than falling back silently. */
    public var state: HelperState { HelperState(core: machine.pointee.state) ?? .quiet }

    public var negotiated: Negotiated? {
        guard machine.pointee.have_negotiated else { return nil }
        let n = machine.pointee.negotiated
        return Negotiated(channelCount: n.channel_count,
                          maxChunk: n.max_chunk,
                          deviceBuild: BuildType(rawValue: n.device_build) ?? .release)
    }

    /// Whether a bulk transfer may go out right now — the seam #52 consumes.
    /// It answers for the *session*, where `HelperState.allowsBulkTransfers`
    /// answers for what the user is being told; the two must not disagree.
    public var canSendBulk: Bool { dh_helper_can_send_bulk(machine) }

    public func handle(_ input: SessionInput, at now: TimeInterval) -> [SessionOutput] {
        let ms = Self.milliseconds(now)
        dh_helper_outputs_reset(outputs)

        switch input {
        case .deviceAppeared(let which):
            dh_helper_device_appeared(machine, which.core, ms, outputs)
        case .deviceDisappeared:
            dh_helper_device_disappeared(machine, ms, outputs)
        case .channelsAcquired(let count):
            dh_helper_channels_acquired(machine, UInt8(clamping: count), ms, outputs)
        case .acquisitionRefused(let acquired, let total):
            dh_helper_acquisition_refused(machine, UInt8(clamping: acquired),
                                          UInt8(clamping: total), ms, outputs)
        case .received(let bytes):
            bytes.withUnsafeBufferPointer {
                dh_helper_received(machine, $0.baseAddress, $0.count, ms, outputs)
            }
        case .transportFailed:
            dh_helper_transport_failed(machine, ms, outputs)
        case .tick:
            dh_helper_tick(machine, ms, outputs)
        }

        return collect(transportReason: input.transportReason)
    }

    /*
     * Build one authenticated frame to send to the board — a bulk chunk, a
     * cursor placement — under the session key and the next counter in its
     * space. `nil` when there is no session to carry it.
     *
     * The counter space belongs to the key, and the heartbeat is already
     * writing into this one. A platform keeping a counter of its own beside it
     * would give one space two writers, and the board refuses anything not
     * strictly greater — so whichever frame lost the race would be dropped
     * silently, at the far end, with nothing at either end able to say why.
     * #52 consumes this rather than counting for itself.
     *
     * The idle timer is **not** charged here. A frame the transport then
     * refused would have suppressed a beat that was owed; call `noteSent`
     * when it actually went out.
     */
    public func emit(type: UInt8, flags: UInt8 = 0, body: [UInt8] = []) -> [UInt8]? {
        var frame = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        let capacity = frame.count
        let rc = body.withUnsafeBufferPointer { b in
            frame.withUnsafeMutableBufferPointer { out in
                dh_helper_emit(machine, type, flags, b.baseAddress, b.count,
                               out.baseAddress, capacity, &written)
            }
        }
        guard rc == DH_FRAME_OK else { return nil }
        return Array(frame.prefix(written))
    }

    /// The transport wrote a frame this machine did not produce. ADR-0004's
    /// heartbeat fills a direction that has carried *nothing* for a full
    /// interval, and traffic the core never saw would otherwise have it
    /// beating into a direction that is far from idle.
    public func noteSent(at now: TimeInterval) {
        dh_helper_note_sent(machine, Self.milliseconds(now))
    }

    // MARK: - Crossing the seam

    /*
     * The core's clock is milliseconds in a `uint32_t`, compared as unsigned
     * differences — so the wrap every 49 days is arithmetic rather than a
     * dropped session, and truncating here is safe for the same reason.
     */
    private static func milliseconds(_ now: TimeInterval) -> UInt32 {
        UInt32(truncatingIfNeeded: Int64((now * 1000).rounded()))
    }

    private func collect(transportReason: String?) -> [SessionOutput] {
        var result: [SessionOutput] = []
        let count = min(outputs.pointee.count, Int(DH_HELPER_OUTPUTS_MAX))

        withUnsafePointer(to: &outputs.pointee.items) { tuple in
            tuple.withMemoryRebound(to: dh_helper_output.self,
                                    capacity: Int(DH_HELPER_OUTPUTS_MAX)) { items in
                for index in 0..<count {
                    result.append(translate(items[index], transportReason: transportReason))
                }
            }
        }

        /*
         * An output that did not fit would look exactly like nothing having
         * happened — a dropped `send`, or a state the user is never told. The
         * core counts them rather than letting them pass, and this is where
         * that count becomes visible instead of silent.
         */
        if outputs.pointee.overflow > 0 {
            result.append(.note("\(outputs.pointee.overflow) output(s) did not fit and were lost"))
        }
        return result
    }

    private func translate(_ item: dh_helper_output, transportReason: String?) -> SessionOutput {
        switch dh_helper_output_kind(rawValue: UInt32(item.kind)) {
        case DH_HELPER_OUT_STORE_BOARD_KEY:
            return .storeBoardKey(bytes(of: item))
        case DH_HELPER_OUT_OPEN_CHANNELS:
            return .openChannels
        case DH_HELPER_OUT_CLOSE_CHANNELS:
            return .closeChannels
        case DH_HELPER_OUT_SEND:
            return .send(bytes(of: item))
        case DH_HELPER_OUT_STATE:
            guard let state = HelperState(core: dh_helper_state(rawValue: UInt32(item.state)))
            else { return .note("the core reported state \(item.state), which this helper "
                                + "has no words for") }
            return .state(state)
        case DH_HELPER_OUT_RETRY:
            return .retry(after: TimeInterval(item.a) / 1000)
        case DH_HELPER_OUT_NOTE:
            return .note(HelperNotes.line(dh_helper_note(rawValue: UInt32(item.note)),
                                          a: item.a, b: item.b,
                                          transportReason: transportReason))
        default:
            /* Unreachable while both sides come out of one build — and said
               rather than dropped, because an output that vanishes looks
               exactly like nothing having happened. */
            return .note("the core produced output kind \(item.kind), which this helper does "
                         + "not carry")
        }
    }

    private func bytes(of item: dh_helper_output) -> [UInt8] {
        withUnsafeBytes(of: item.bytes) { Array($0.prefix(item.len)) }
    }
}

/* A Swift array into one of the core's fixed-size byte fields. Short sources
   leave the tail as it was, which is why every one of them is zeroed first. */
private func copy<T>(_ source: [UInt8], into destination: inout T) {
    withUnsafeMutableBytes(of: &destination) { field in
        let count = min(field.count, source.count)
        source.withUnsafeBufferPointer { src in
            field.copyBytes(from: UnsafeRawBufferPointer(src)[..<count])
        }
    }
}
