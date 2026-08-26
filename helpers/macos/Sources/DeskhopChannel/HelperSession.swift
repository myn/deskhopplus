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
    /* The board's clipboard direction policy (#52); the flags are
       DH_CLIP_MAY_SEND / DH_CLIP_MAY_RECEIVE. The device is the single source
       of truth for settings, so this is the only place a helper learns it. */
    case clipPolicy(flags: UInt8)
}

public struct Negotiated: Equatable {
    public let channelCount: UInt8
    public let maxChunk: UInt16
    public let deviceBuild: BuildType
}

/*
 * What the board says it has dropped on the channel, since it booted (#133).
 *
 * Nine totals rather than one, because each names a different seam and each
 * seam has a different remedy. Read live: these used to be readable only from
 * the config page, which is reachable only in config mode, which is entered by
 * rebooting the board that holds them — so they could only ever read zero, and
 * a row of zeros was taken three times as evidence the seams were clean.
 */
public struct BoardDrops: Equatable {
    public let reports: UInt32
    public let inbound: UInt32
    public let outbound: UInt32
    public let unsent: UInt32
    public let orphans: UInt32
    public let truncated: UInt32
    public let relayQueue: UInt32

    /// Why the outbound queue refused, which the total alone cannot say (#142).
    /// The queue has two bands and #141 deepened only one of them, so a total
    /// still climbing was equally consistent with that change having worked
    /// and with it not having.
    public let outboundPriority: UInt32
    public let outboundBadHeader: UInt32

    /// **Not a drop.** Frames the board took from this helper and
    /// authenticated, counted on the line that refreshes the liveness
    /// deadline (#107). Rendered separately below, because a stall note that
    /// listed it among the losses would be naming a seam that is working.
    public let framesIn: UInt32
    /// The rest of the same chain: reports the board's USB callback took, and
    /// frames that reached authentication and failed it (#107).
    public let reportsIn: UInt32
    public let framesRefused: UInt32

    /// The band #141 deepened. Derived, because the board sends the total and
    /// the two parts that are not it — appending two fields rather than three
    /// keeps the frame inside the board's reply buffer.
    public var outboundBulk: UInt32 {
        outbound >= outboundPriority + outboundBadHeader
            ? outbound - outboundPriority - outboundBadHeader
            : 0
    }

    public init(_ d: dh_device_drops) {
        reports = d.reports
        inbound = d.inbound
        outbound = d.outq
        unsent = d.unsent
        orphans = d.orphans
        truncated = d.truncated
        relayQueue = d.relay_q
        outboundPriority = d.outq_priority
        outboundBadHeader = d.outq_bad_header
        framesIn = d.frames_in
        reportsIn = d.reports_in
        framesRefused = d.frames_refused
    }

    /// Every seam that has lost something, named. Seams that have lost nothing
    /// are left out, so the ordinary line says so in three words and a line
    /// with anything in it is all signal.
    public var line: String {
        let seams: [(String, UInt32)] = [
            ("reports not taken", reports),
            ("from peer board", inbound),
            ("outbound refused", outbound),
            ("not handed on", unsent),
            ("peer orphan packets", orphans),
            ("peer frames truncated", truncated),
            ("relay queue refused", relayQueue),
        ]
        let lost = seams.filter { $0.1 > 0 }.map { seam -> String in
            /* The outbound total carries its breakdown, because which band
               refused decides what to do about it and the total says only
               that something did (#142). Printed whole, zeros included: a
               zero here is the finding, not an absence. */
            guard seam.0 == "outbound refused" else { return "\(seam.0) \(seam.1)" }
            return "\(seam.0) \(seam.1) (priority \(outboundPriority), bulk \(outboundBulk)"
                + ", bad header \(outboundBadHeader))"
        }
        /* The accepted-frame count rides along whatever the losses say, and
           is never one of them: on a liveness eviction it is the number that
           decides whether the board was hearing this helper at all (#107). */
        /* The inbound chain, head to tail, so a loss can be located in one
           reading instead of one per rebuild (#107). */
        let heard = "board inbound: \(reportsIn) report(s) in, \(framesIn) frame(s) accepted, "
                  + "\(framesRefused) refused"
        return (lost.isEmpty ? "board reports no drops" : "board drops: " + lost.joined(separator: ", "))
             + "; " + heard
    }
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
    /* The slow reading of the same fault: sessions lost with the board still
       attached, which is what #107 spent sixteen hours doing invisibly. */
    public static let sessionLossLimit = Int(DH_HELPER_SESSION_LOSS_LIMIT)
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

        machine.initialize(to: dh_helper())
        outputs.initialize(to: dh_helper_outputs())
        if let boardPublicKey, boardPublicKey.count == Int(DH_P256_PUBLIC_SIZE) {
            boardPublicKey.withUnsafeBufferPointer {
                dh_helper_init(machine, identity, $0.baseAddress)
            }
        } else {
            dh_helper_init(machine, identity, nil)
        }

        /*
         * Unretained on purpose: this object owns the machine it is handing a
         * pointer to itself to, so the machine cannot outlive it, and a
         * retained reference here would be a cycle nothing breaks.
         */
        dh_helper_set_payload_sink(machine, { ctx, type, body, len in
            guard let ctx else { return }
            let session = Unmanaged<HelperSession>.fromOpaque(ctx).takeUnretainedValue()
            guard let handler = session.onPayload else { return }
            let bytes = body.map { Array(UnsafeBufferPointer(start: $0, count: len)) } ?? []
            handler(type, bytes)
        }, Unmanaged.passUnretained(self).toOpaque())
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

    /*
     * Bulk and placement frames the core authenticates but does not decide
     * about, handed over as a verified body (#52).
     *
     * Set this rather than reading frames off the transport: everything
     * upstream — decode, tag, replay counter — has already happened by the
     * time a body reaches here, and a platform that read the stream itself
     * would be doing all of it again, differently.
     */
    public var onPayload: ((UInt8, [UInt8]) -> Void)?

    /// What the board last said about the clipboard's two directions (#52).
    /// Both allowed until it has said anything, matching the stored default.
    public var clipFlags: UInt8 { dh_helper_clip_flags(machine) }

    /// What the board says it has dropped, or nil if it has stated nothing
    /// this session (#133). Nil is not the same answer as all-zero, and a
    /// caller that conflates them is back to reading silence as evidence.
    public var boardDrops: BoardDrops? {
        var d = dh_device_drops()
        guard dh_helper_device_drops(machine, &d) else { return nil }
        return BoardDrops(d)
    }

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

    /// A frame this helper actually got out, whoever produced it — including
    /// the beat the core built a moment ago. ADR-0004's heartbeat fills a
    /// direction that has carried *nothing* for a full interval, so anything
    /// that did carry has to say so.
    ///
    /// Called once the transport has taken the frame, never before: charging
    /// for a refused one buys an interval of silence the helper has not
    /// earned, and the board evicts after three (#107). See `dh_helper.h` for
    /// the hello and pair-request exception.
    public func noteSent(at now: TimeInterval) {
        dh_helper_note_sent(machine, Self.milliseconds(now))
    }

    /// The transport would not take a frame. Counted, so "writing" and "being
    /// refused" are different readings rather than the same silence (#107).
    public func noteSendRefused() {
        dh_helper_note_send_refused(machine)
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
        case DH_HELPER_OUT_CLIP_POLICY:
            return .clipPolicy(flags: UInt8(truncatingIfNeeded: item.a))
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

/* A Swift array into one of the core's fixed-size byte fields. The sizes must
   match exactly: a `HelperIdentity` handing over CryptoKit's 65-byte
   `x963Representation` instead of the 64 raw bytes the wire wants would
   otherwise lose its last byte here and fail every tag check on the board,
   with nothing naming the cause. */
private func copy<T>(_ source: [UInt8], into destination: inout T) {
    precondition(source.count == MemoryLayout<T>.size,
                 "\(source.count) bytes into a \(MemoryLayout<T>.size)-byte field")
    withUnsafeMutableBytes(of: &destination) { field in
        source.withUnsafeBufferPointer { src in
            field.copyBytes(from: UnsafeRawBufferPointer(src))
        }
    }
}
