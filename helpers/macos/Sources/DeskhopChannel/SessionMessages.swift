import DHCore

/*
 * The session messages, bound to the shared core's **v2** codecs
 * (dh_session.h, ADR-0008, #112).
 *
 * v2 replaced the bearer token with a key pair on each side, an
 * authentication tag on every frame, and the correlation value that closes
 * the manufactured chord trap (#108). Every hello and every reply echoes the
 * caller's correlation value; a helper acts only on one carrying its own.
 *
 * What was here before: the v1 codecs from dh_session_v1.h, parked for
 * exactly this ticket. They are deleted, along with dh_session_v1.[ch].
 */

public enum BuildType: UInt8, Equatable {
    case release = 0
    case development = 1
}

/*
 * Why the device ended a session. Unlike the hello refusals these are
 * diagnostic — every one takes the same recovery — so an unknown value reads
 * as unspecified rather than as a decode failure.
 */
public enum SessionEndReason: UInt8, Equatable {
    case unspecified = 0
    case livenessTimeout = 1
    case protocolError = 2
    /* The configuration holding the board's registration was wiped, so the
       pairing this session ran on no longer exists. Same recovery as the
       rest — the hello that follows is refused, and the helper reports not
       paired. */
    case unpaired = 3
    /* The board lost an inbound report, so the stream from this helper had a
       hole in it and the reader could not find its way back (#161). Nothing
       this end did is wrong — it is named separately from `protocolError`
       precisely so a log does not send the next reader looking at this end. */
    case streamGap = 4

    public init(wire: UInt8) {
        self = SessionEndReason(rawValue: wire) ?? .unspecified
    }
}

/* Why a hello was refused. Each has a different remedy told to the user.
   Value 1 is reserved (v1's auth_failed) and never sent — a failed tag is
   answered with silence, not a status. */
public enum HelloRefusedStatus: UInt8, Equatable {
    case versionIncompatible = 2
    case unpaired = 3
}

/* Why a pairing request was refused. */
public enum PairRefusedReason: UInt8, Equatable {
    case noWindow = 0
    case alreadyRegistered = 1
}

// MARK: - Hello (h→d, authenticated under k_hello)

public struct Hello: Equatable {
    public var protocolVersion: UInt16
    public var os: UInt8
    public var buildType: BuildType
    public var channelCount: UInt8
    public var maxChunk: UInt16
    public var correlation: UInt64
    public var helperKeyId: [UInt8]
    public var helperNonce: [UInt8]

    public init(protocolVersion: UInt16 = UInt16(DH_PROTO_VERSION),
                os: UInt8 = UInt8(DH_OS_MAC.rawValue),
                buildType: BuildType = .release,
                channelCount: UInt8 = ChannelIdentity.requestedChannelCount,
                maxChunk: UInt16 = ChannelIdentity.requestedMaxChunk,
                correlation: UInt64 = 0,
                helperKeyId: [UInt8] = [],
                helperNonce: [UInt8] = []) {
        self.protocolVersion = protocolVersion
        self.os = os
        self.buildType = buildType
        self.channelCount = channelCount
        self.maxChunk = maxChunk
        self.correlation = correlation
        self.helperKeyId = helperKeyId
        self.helperNonce = helperNonce
    }

    /// A complete frame, tagged under the given key at the given counter.
    public func encoded(key: [UInt8], counter: UInt64 = 0) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        var keyId = helperKeyId
        var nonce = helperNonce
        let rc = keyId.withUnsafeMutableBufferPointer { kid in
            nonce.withUnsafeMutableBufferPointer { n in
                key.withUnsafeBufferPointer { k in
                    out.withUnsafeMutableBufferPointer { buffer in
                        var hello = dh_hello(
                            proto_version: protocolVersion,
                            os: os,
                            build_type: buildType.rawValue,
                            channel_count: channelCount,
                            max_chunk: maxChunk,
                            correlation: correlation,
                            helper_key_id: (0,0,0,0,0,0,0,0),
                            helper_nonce: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
                        withUnsafeMutableBytes(of: &hello.helper_key_id) { dst in
                            let count = min(dst.count, kid.count)
                            dst.copyBytes(from: UnsafeRawBufferPointer(kid)[..<count])
                        }
                        withUnsafeMutableBytes(of: &hello.helper_nonce) { dst in
                            let count = min(dst.count, n.count)
                            dst.copyBytes(from: UnsafeRawBufferPointer(n)[..<count])
                        }
                        return dh_hello_encode(&hello, k.baseAddress, counter,
                                               buffer.baseAddress, buffer.count, &written)
                    }
                }
            }
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }

    public static func decode(body: [UInt8]) throws -> Hello {
        var hello = dh_hello()
        let ok = body.withUnsafeBufferPointer { buf in
            dh_hello_decode(buf.baseAddress, buf.count, &hello)
        }
        guard ok else { throw ChannelError.malformedPayload }
        return Hello(protocolVersion: hello.proto_version,
                     os: hello.os,
                     buildType: BuildType(rawValue: hello.build_type) ?? .release,
                     channelCount: hello.channel_count,
                     maxChunk: hello.max_chunk,
                     correlation: hello.correlation,
                     helperKeyId: withUnsafeBytes(of: hello.helper_key_id) { [UInt8]($0) },
                     helperNonce: withUnsafeBytes(of: hello.helper_nonce) { [UInt8]($0) })
    }
}

// MARK: - HelloAck (d→h, authenticated under k_b2h)

public struct HelloAck: Equatable {
    public var correlation: UInt64
    public var protocolVersion: UInt16
    public var buildType: BuildType
    public var channelCount: UInt8
    public var maxChunk: UInt16
    public var boardNonce: [UInt8]

    public init(correlation: UInt64, protocolVersion: UInt16, buildType: BuildType,
                channelCount: UInt8, maxChunk: UInt16, boardNonce: [UInt8]) {
        self.correlation = correlation
        self.protocolVersion = protocolVersion
        self.buildType = buildType
        self.channelCount = channelCount
        self.maxChunk = maxChunk
        self.boardNonce = boardNonce
    }

    public static func decode(body: [UInt8]) throws -> HelloAck {
        var ack = dh_hello_ack()
        let ok = body.withUnsafeBufferPointer { buf in
            dh_hello_ack_decode(buf.baseAddress, buf.count, &ack)
        }
        guard ok else { throw ChannelError.malformedPayload }
        return HelloAck(correlation: ack.correlation,
                        protocolVersion: ack.proto_version,
                        buildType: BuildType(rawValue: ack.build_type) ?? .release,
                        channelCount: ack.channel_count,
                        maxChunk: ack.max_chunk,
                        boardNonce: withUnsafeBytes(of: ack.board_nonce) { [UInt8]($0) })
    }

    public func encoded(key: [UInt8], counter: UInt64 = 0) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        let rc = key.withUnsafeBufferPointer { k in
            out.withUnsafeMutableBufferPointer { buffer in
                var ack = dh_hello_ack(
                    correlation: correlation,
                    proto_version: protocolVersion,
                    build_type: buildType.rawValue,
                    channel_count: channelCount,
                    max_chunk: maxChunk,
                    board_nonce: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
                withUnsafeMutableBytes(of: &ack.board_nonce) { dst in
                    let count = min(dst.count, boardNonce.count)
                    boardNonce.withUnsafeBufferPointer { src in
                        dst.copyBytes(from: UnsafeRawBufferPointer(src)[..<count])
                    }
                }
                return dh_hello_ack_encode(&ack, k.baseAddress, counter,
                                           buffer.baseAddress, buffer.count, &written)
            }
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }
}

// MARK: - HelloRefused (d→h, untagged — types 0x08-0x0F)

public struct HelloRefused: Equatable {
    public var correlation: UInt64
    public var protocolVersion: UInt16
    public var status: HelloRefusedStatus

    public init(correlation: UInt64, protocolVersion: UInt16 = UInt16(DH_PROTO_VERSION),
                status: HelloRefusedStatus) {
        self.correlation = correlation
        self.protocolVersion = protocolVersion
        self.status = status
    }

    public static func decode(payload: [UInt8]) throws -> HelloRefused {
        var refused = dh_hello_refused()
        let ok = payload.withUnsafeBufferPointer { buf in
            dh_hello_refused_decode(buf.baseAddress, buf.count, &refused)
        }
        guard ok, let status = HelloRefusedStatus(rawValue: refused.status) else {
            throw ChannelError.malformedPayload
        }
        return HelloRefused(correlation: refused.correlation,
                            protocolVersion: refused.proto_version,
                            status: status)
    }

    public func encoded() throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        var msg = dh_hello_refused(correlation: correlation,
                                   proto_version: protocolVersion,
                                   status: status.rawValue)
        let rc = out.withUnsafeMutableBufferPointer { buffer in
            dh_hello_refused_encode(&msg, buffer.baseAddress, buffer.count, &written)
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }
}

// MARK: - PairRequest (h→d, untagged)

public struct PairRequest: Equatable {
    public var correlation: UInt64
    public var helperPublicKey: [UInt8]

    public init(correlation: UInt64, helperPublicKey: [UInt8]) {
        self.correlation = correlation
        self.helperPublicKey = helperPublicKey
    }

    public func encoded() throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        var msg = dh_pair_request(correlation: correlation,
                                  helper_public: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                                  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                                  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                                  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
        withUnsafeMutableBytes(of: &msg.helper_public) { dst in
            helperPublicKey.withUnsafeBufferPointer { src in
                let count = min(dst.count, src.count)
                dst.copyBytes(from: UnsafeRawBufferPointer(src)[..<count])
            }
        }
        let rc = out.withUnsafeMutableBufferPointer { buffer in
            dh_pair_request_encode(&msg, buffer.baseAddress, buffer.count, &written)
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }

    public static func decode(payload: [UInt8]) throws -> PairRequest {
        var msg = dh_pair_request()
        let ok = payload.withUnsafeBufferPointer { buf in
            dh_pair_request_decode(buf.baseAddress, buf.count, &msg)
        }
        guard ok else { throw ChannelError.malformedPayload }
        return PairRequest(correlation: msg.correlation,
                           helperPublicKey: withUnsafeBytes(of: msg.helper_public) { [UInt8]($0) })
    }
}

// MARK: - PairGrant (d→h, untagged)

public struct PairGrant: Equatable {
    public var correlation: UInt64
    public var boardPublicKey: [UInt8]

    public init(correlation: UInt64, boardPublicKey: [UInt8]) {
        self.correlation = correlation
        self.boardPublicKey = boardPublicKey
    }

    public static func decode(payload: [UInt8]) throws -> PairGrant {
        var msg = dh_pair_grant()
        let ok = payload.withUnsafeBufferPointer { buf in
            dh_pair_grant_decode(buf.baseAddress, buf.count, &msg)
        }
        guard ok else { throw ChannelError.malformedPayload }
        return PairGrant(correlation: msg.correlation,
                         boardPublicKey: withUnsafeBytes(of: msg.board_public) { [UInt8]($0) })
    }

    public func encoded() throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        var msg = dh_pair_grant(correlation: correlation,
                                board_public: (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                               0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                               0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                               0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
        withUnsafeMutableBytes(of: &msg.board_public) { dst in
            boardPublicKey.withUnsafeBufferPointer { src in
                let count = min(dst.count, src.count)
                dst.copyBytes(from: UnsafeRawBufferPointer(src)[..<count])
            }
        }
        let rc = out.withUnsafeMutableBufferPointer { buffer in
            dh_pair_grant_encode(&msg, buffer.baseAddress, buffer.count, &written)
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }
}

// MARK: - PairRefused (d→h, untagged)

public struct PairRefused: Equatable {
    public var correlation: UInt64
    public var reason: PairRefusedReason

    public init(correlation: UInt64, reason: PairRefusedReason) {
        self.correlation = correlation
        self.reason = reason
    }

    public static func decode(payload: [UInt8]) throws -> PairRefused {
        var msg = dh_pair_refused()
        let ok = payload.withUnsafeBufferPointer { buf in
            dh_pair_refused_decode(buf.baseAddress, buf.count, &msg)
        }
        guard ok, let reason = PairRefusedReason(rawValue: msg.reason) else {
            throw ChannelError.malformedPayload
        }
        return PairRefused(correlation: msg.correlation, reason: reason)
    }

    public func encoded() throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        var msg = dh_pair_refused(correlation: correlation, reason: reason.rawValue)
        let rc = out.withUnsafeMutableBufferPointer { buffer in
            dh_pair_refused_encode(&msg, buffer.baseAddress, buffer.count, &written)
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }
}

// MARK: - ListenerAlert (d→h, authenticated under k_b2h)

public struct ListenerAlert: Equatable {
    public var windowMs: UInt32
    public var refused: UInt32

    public static func decode(body: [UInt8]) throws -> ListenerAlert {
        var msg = dh_listener_alert()
        let ok = body.withUnsafeBufferPointer { buf in
            dh_listener_alert_decode(buf.baseAddress, buf.count, &msg)
        }
        guard ok else { throw ChannelError.malformedPayload }
        return ListenerAlert(windowMs: msg.window_ms, refused: msg.refused)
    }
}

// MARK: - Authenticated frame helpers

public enum AuthFrame {
    /// Tag a frame body under the given key and counter, producing a complete
    /// frame (header + auth prefix + body).
    public static func wrap(type: UInt8, body: [UInt8] = [],
                            key: [UInt8], counter: UInt64) throws -> [UInt8] {
        let maxPayload = Int(DH_FRAME_AUTH_PREFIX_SIZE) + body.count
        var payload = [UInt8](repeating: 0, count: maxPayload)
        var payloadLen = 0
        let wrapRC = key.withUnsafeBufferPointer { k in
            body.withUnsafeBufferPointer { b in
                payload.withUnsafeMutableBufferPointer { p in
                    dh_auth_wrap(k.baseAddress, type, 0, counter,
                                 b.baseAddress, b.count,
                                 p.baseAddress, p.count, &payloadLen)
                }
            }
        }
        guard wrapRC == DH_AUTH_OK else { throw ChannelError.malformedPayload }

        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        var written = 0
        let encRC = payload.withUnsafeBufferPointer { p in
            out.withUnsafeMutableBufferPointer { o in
                dh_frame_encode(type, 0, p.baseAddress, payloadLen,
                                o.baseAddress, o.count, &written)
            }
        }
        guard encRC == DH_FRAME_OK else { throw ChannelError.from(encRC) }
        return Array(out.prefix(written))
    }

    /// Verify the tag on a received frame and extract its body.
    public static func open(frame: Frame, key: [UInt8],
                            counter: inout dh_auth_counter) throws -> [UInt8] {
        var bodyPtr: UnsafePointer<UInt8>?
        var bodyLen = 0
        var hdr = dh_frame_header(type: frame.type, flags: frame.flags,
                                  len: UInt16(frame.payload.count))
        let result = key.withUnsafeBufferPointer { k in
            frame.payload.withUnsafeBufferPointer { payload in
                dh_auth_open(k.baseAddress, &hdr, payload.baseAddress,
                             &counter, &bodyPtr, &bodyLen)
            }
        }
        switch result {
        case DH_AUTH_OK:
            guard let ptr = bodyPtr else { return [] }
            return Array(UnsafeBufferPointer(start: ptr, count: bodyLen))
        case DH_AUTH_ERR_TAG:
            throw AuthError.badTag
        case DH_AUTH_ERR_COUNTER:
            throw AuthError.replayedCounter
        default:
            throw AuthError.badTag
        }
    }

    /*
     * Peek at the board nonce in a HELLO_ACK payload, before verifying the
     * tag — the one place the helper must read a field it has not yet
     * authenticated, because k_b2h cannot be derived without this nonce and
     * the tag cannot be checked without k_b2h. The value is then checked: a
     * wrong nonce derives a wrong key and the tag fails.
     *
     * The nonce is the last field of the ack body, so its offset is taken
     * from the two sizes the core defines rather than written out here. A
     * literal would be the one wire offset spelled in Swift, and would drift
     * silently if `dh_hello_ack` ever grew a field ahead of it (#64).
     */
    public static func peekBoardNonce(payload: [UInt8]) -> [UInt8]? {
        let offset = Int(DH_FRAME_AUTH_PREFIX_SIZE) + Int(DH_HELLO_ACK_LEN) - Int(DH_NONCE_SIZE)
        let end = offset + Int(DH_NONCE_SIZE)
        guard payload.count >= end else { return nil }
        return Array(payload[offset..<end])
    }
}

public enum AuthError: Error, Equatable {
    case badTag
    case replayedCounter
}

// MARK: - Message type constants

public enum MessageType {
    public static let hello = UInt8(DH_MSG_HELLO.rawValue)
    public static let helloAck = UInt8(DH_MSG_HELLO_ACK.rawValue)
    public static let listenerAlert = UInt8(DH_MSG_LISTENER_ALERT.rawValue)
    public static let heartbeat = UInt8(DH_MSG_HEARTBEAT.rawValue)
    public static let deviceHeartbeat = UInt8(DH_MSG_DEVICE_HEARTBEAT.rawValue)
    public static let sessionEnd = UInt8(DH_MSG_SESSION_END.rawValue)
    public static let pairRequest = UInt8(DH_MSG_PAIR_REQUEST.rawValue)
    public static let pairGrant = UInt8(DH_MSG_PAIR_GRANT.rawValue)
    public static let pairRefused = UInt8(DH_MSG_PAIR_REFUSED.rawValue)
    public static let helloRefused = UInt8(DH_MSG_HELLO_REFUSED.rawValue)

    /* The bulk band. The two that carry the user's bytes are sealed
       helper-to-helper (Seal.swift); the rest carry ids and counts only. */
    public static let clipOffer = UInt8(DH_MSG_CLIP_OFFER.rawValue)
    public static let clipRequest = UInt8(DH_MSG_CLIP_REQUEST.rawValue)
    public static let clipChunk = UInt8(DH_MSG_CLIP_CHUNK.rawValue)
    public static let clipDone = UInt8(DH_MSG_CLIP_DONE.rawValue)
    public static let clipCancel = UInt8(DH_MSG_CLIP_CANCEL.rawValue)
    public static let clipRetransmit = UInt8(DH_MSG_CLIP_RETRANSMIT.rawValue)
    public static let clipCredit = UInt8(DH_MSG_CLIP_CREDIT.rawValue)
    public static let sealOffer = UInt8(DH_MSG_SEAL_OFFER.rawValue)
    public static let sealAccept = UInt8(DH_MSG_SEAL_ACCEPT.rawValue)
    public static let sealStale = UInt8(DH_MSG_SEAL_STALE.rawValue)
}
