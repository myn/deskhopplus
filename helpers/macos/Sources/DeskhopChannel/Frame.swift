import DHCore

/*
 * The binding to the shared C core's frame codec — and it is only a binding.
 * Not one byte of the wire format is decided here: every encode and decode is
 * a call into src/core, the same object code the firmware runs, gated by
 * test-vectors/frames.txt. A Swift reimplementation would be a second
 * implementation of the format wearing a binding's name (#64).
 */

public struct Frame: Equatable {
    public let type: UInt8
    public let flags: UInt8
    public let payload: [UInt8]

    public init(type: UInt8, flags: UInt8 = 0, payload: [UInt8] = []) {
        self.type = type
        self.flags = flags
        self.payload = payload
    }
}

public enum ChannelError: Error, Equatable {
    case oversize
    case unknownType
    case bufferTooSmall
    case malformedPayload
    case truncated

    static func from(_ result: dh_frame_result) -> ChannelError {
        switch result {
        case DH_FRAME_ERR_OVERSIZE: return .oversize
        case DH_FRAME_ERR_UNKNOWN_TYPE: return .unknownType
        case DH_FRAME_ERR_BUFFER: return .bufferTooSmall
        case DH_FRAME_AGAIN: return .truncated
        default: return .malformedPayload
        }
    }
}

public enum FrameCodec {
    public static let headerSize = Int(DH_FRAME_HEADER_SIZE)
    public static let maxPayload = Int(DH_FRAME_MAX_PAYLOAD)
    public static let maxSize = headerSize + maxPayload

    /* Encode one complete frame. */
    public static func encode(_ frame: Frame) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: maxSize)
        let capacity = out.count
        var written = 0
        let rc = frame.payload.withUnsafeBufferPointer { payload in
            out.withUnsafeMutableBufferPointer { buffer in
                dh_frame_encode(frame.type, frame.flags, payload.baseAddress, payload.count,
                                buffer.baseAddress, capacity, &written)
            }
        }
        guard rc == DH_FRAME_OK else { throw ChannelError.from(rc) }
        return Array(out.prefix(written))
    }

    /* Decode exactly one frame at the start of bytes, with what it consumed.
       The decoded view points into `bytes`, whose pointer is only guaranteed
       for the length of the closure — so the payload is copied inside it. */
    public static func decode(_ bytes: [UInt8]) throws -> (frame: Frame, consumed: Int) {
        var view = dh_frame_view()
        var consumed = 0
        var frame: Frame?
        let rc = bytes.withUnsafeBufferPointer { buffer -> dh_frame_result in
            let rc = dh_frame_decode(buffer.baseAddress, buffer.count, &view, &consumed)
            if rc == DH_FRAME_OK {
                frame = Frame(type: view.hdr.type, flags: view.hdr.flags,
                              payload: view.payloadBytes)
            }
            return rc
        }
        guard rc == DH_FRAME_OK, let frame else { throw ChannelError.from(rc) }
        return (frame, consumed)
    }

    /*
     * Pack frames into fixed-size reports, padding the tail. A report has no
     * length of its own, so the padding byte is what tells a decoder where the
     * frames stopped (docs/protocol.md, "The report carrier").
     */
    public static func reports(for frames: [Frame],
                               size: Int = ChannelIdentity.reportSize) throws -> [[UInt8]] {
        var stream: [UInt8] = []
        for frame in frames { stream += try encode(frame) }

        var reports: [[UInt8]] = []
        var offset = 0
        while offset < stream.count {
            var report = Array(stream[offset..<min(offset + size, stream.count)])
            report += [UInt8](repeating: UInt8(DH_FRAME_PAD), count: size - report.count)
            reports.append(report)
            offset += size
        }
        return reports
    }
}

/*
 * The incremental reader, over the same fixed storage the C core uses. Frame
 * boundaries never align with report boundaries, so every arriving report goes
 * through here rather than being decoded on its own.
 */
public final class FrameStream {
    private let reader = UnsafeMutablePointer<dh_frame_reader>.allocate(capacity: 1)

    public init() { dh_frame_reader_init(reader) }
    deinit { reader.deallocate() }

    /* A protocol error resets the reader; the caller drops the connection. */
    public func reset() { dh_frame_reader_init(reader) }

    public func push(_ bytes: [UInt8]) throws -> [Frame] {
        var frames: [Frame] = []
        var offset = 0
        var failure: ChannelError?

        bytes.withUnsafeBufferPointer { buffer in
            while offset < buffer.count {
                var view = dh_frame_view()
                var consumed = 0
                let rc = dh_frame_reader_push(reader, buffer.baseAddress! + offset,
                                              buffer.count - offset, &consumed, &view)
                if rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN {
                    failure = ChannelError.from(rc)
                    return
                }
                offset += consumed
                if rc == DH_FRAME_OK {
                    frames.append(Frame(type: view.hdr.type, flags: view.hdr.flags,
                                        payload: view.payloadBytes))
                } else if consumed == 0 {
                    return /* nothing more to take from this slice */
                }
            }
        }

        if let failure {
            reset()
            throw failure
        }
        return frames
    }
}

extension dh_frame_view {
    /* The core hands back a view of its own buffer, valid only until the next
       push — so the payload is copied out at the binding's edge. */
    var payloadBytes: [UInt8] {
        guard hdr.len > 0, let payload else { return [] }
        return Array(UnsafeBufferPointer(start: payload, count: Int(hdr.len)))
    }
}
