import DHCore

/*
 * The session messages, bound to the shared core's codecs (dh_session.h) —
 * the same ones the firmware answers with, so the negotiated fields cannot
 * drift between the two ends.
 */

public enum HelloStatus: UInt8, Equatable {
    case ok = 0
    /* Distinct because the remedies differ, and each is told to the user
       verbatim: re-pair with the config chord, versus update the helper. */
    case authenticationFailed = 1
    case versionIncompatible = 2
}

public enum BuildType: UInt8, Equatable {
    case release = 0
    case development = 1
}

/*
 * Why the device ended a session. Unlike HelloStatus these are diagnostic
 * rather than behavioural — every one of them takes the same recovery — which
 * is exactly what makes an unrecognised value safe to carry on with, rather
 * than a decode failure. A later device may end a session for a reason this
 * build predates.
 */
public enum SessionEndReason: UInt8, Equatable {
    case unspecified = 0
    case livenessTimeout = 1
    case protocolError = 2

    public init(wire: UInt8) {
        self = SessionEndReason(rawValue: wire) ?? .unspecified
    }
}

public struct Hello: Equatable {
    public var protocolVersion: UInt16
    public var os: UInt8
    public var buildType: BuildType
    public var channelCount: UInt8
    public var maxChunk: UInt16
    public var token: [UInt8]

    /* What this helper is: macOS, this protocol version, asking for the
       channel count and chunk size it was built against. The device replies
       with the effective values. */
    public init(protocolVersion: UInt16 = UInt16(DH_PROTO_VERSION),
                os: UInt8 = UInt8(DH_OS_MAC.rawValue),
                buildType: BuildType = .release,
                channelCount: UInt8 = ChannelIdentity.requestedChannelCount,
                maxChunk: UInt16 = ChannelIdentity.requestedMaxChunk,
                token: [UInt8] = []) {
        self.protocolVersion = protocolVersion
        self.os = os
        self.buildType = buildType
        self.channelCount = channelCount
        self.maxChunk = maxChunk
        self.token = token
    }

    /* A complete frame, encoded by the core. */
    public func encoded() throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        let capacity = out.count
        var written = 0
        let rc = token.withUnsafeBufferPointer { token -> dh_frame_result in
            var hello = dh_hello(proto_version: protocolVersion,
                                 os: os,
                                 build_type: buildType.rawValue,
                                 channel_count: channelCount,
                                 max_chunk: maxChunk,
                                 token: token.baseAddress,
                                 token_len: UInt16(token.count))
            return out.withUnsafeMutableBufferPointer { buffer in
                dh_hello_encode(&hello, buffer.baseAddress, capacity, &written)
            }
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }

    /* The decoded token points into `payload`, whose pointer is only
       guaranteed for the length of the closure — so it is copied inside it. */
    public static func decode(payload: [UInt8]) throws -> Hello {
        var hello = dh_hello()
        var decoded: Hello?
        payload.withUnsafeBufferPointer { buffer in
            guard dh_hello_decode(buffer.baseAddress, buffer.count, &hello) else { return }
            decoded = Hello(protocolVersion: hello.proto_version,
                            os: hello.os,
                            buildType: BuildType(rawValue: hello.build_type) ?? .release,
                            channelCount: hello.channel_count,
                            maxChunk: hello.max_chunk,
                            token: hello.token.map {
                                Array(UnsafeBufferPointer(start: $0, count: Int(hello.token_len)))
                            } ?? [])
        }
        guard let decoded else { throw ChannelError.malformedPayload }
        return decoded
    }
}

public struct HelloAck: Equatable {
    public var protocolVersion: UInt16
    public var status: HelloStatus
    public var buildType: BuildType
    /* Effective, not requested — and zero on any non-ok status. */
    public var channelCount: UInt8
    public var maxChunk: UInt16

    public init(protocolVersion: UInt16, status: HelloStatus, buildType: BuildType,
                channelCount: UInt8, maxChunk: UInt16) {
        self.protocolVersion = protocolVersion
        self.status = status
        self.buildType = buildType
        self.channelCount = channelCount
        self.maxChunk = maxChunk
    }

    public static func decode(payload: [UInt8]) throws -> HelloAck {
        var ack = dh_hello_ack()
        let ok = payload.withUnsafeBufferPointer { buffer in
            dh_hello_ack_decode(buffer.baseAddress, buffer.count, &ack)
        }
        guard ok, let status = HelloStatus(rawValue: ack.status) else {
            throw ChannelError.malformedPayload
        }
        return HelloAck(protocolVersion: ack.proto_version,
                        status: status,
                        buildType: BuildType(rawValue: ack.build_type) ?? .release,
                        channelCount: ack.channel_count,
                        maxChunk: ack.max_chunk)
    }

    public func encoded() throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: FrameCodec.maxSize)
        let capacity = out.count
        var written = 0
        var ack = dh_hello_ack(proto_version: protocolVersion,
                               status: status.rawValue,
                               build_type: buildType.rawValue,
                               channel_count: channelCount,
                               max_chunk: maxChunk)
        let rc = out.withUnsafeMutableBufferPointer { buffer in
            dh_hello_ack_encode(&ack, buffer.baseAddress, capacity, &written)
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }
}

public enum MessageType {
    public static let hello = UInt8(DH_MSG_HELLO.rawValue)
    public static let helloAck = UInt8(DH_MSG_HELLO_ACK.rawValue)
    public static let heartbeat = UInt8(DH_MSG_HEARTBEAT.rawValue)
    public static let deviceHeartbeat = UInt8(DH_MSG_DEVICE_HEARTBEAT.rawValue)
    public static let sessionEnd = UInt8(DH_MSG_SESSION_END.rawValue)
    public static let pairRequest = UInt8(DH_MSG_PAIR_REQUEST.rawValue)
    public static let pairGrant = UInt8(DH_MSG_PAIR_GRANT.rawValue)
}
