import DeskhopChannel

/*
 * The binding's gate: every golden vector decodes through the shared C core
 * and re-encodes byte-identically. This is the acceptance criterion that says
 * the helper consumes the core rather than reimplementing it — if a Swift
 * codec ever crept in, these are the tests it would have to fool.
 */

let bindingTests: [(String, () throws -> Void)] = [
    ("every golden vector round-trips", testEveryGoldenVectorRoundTrips),
    ("the stream reader recovers every vector", testStreamReaderRecoversEveryVector),
    ("report padding is written and skipped", testReportPaddingIsWrittenAndSkipped),
    ("malformed input is rejected", testMalformedInputIsRejected),
    ("hello carries version and platform", testHelloCarriesVersionAndPlatform),
    ("hello_ack statuses are distinguishable", testHelloAckStatusesAreDistinguishable),
    ("the hello this helper sends is well formed", testTheHelloThisHelperSendsIsWellFormed),
]

func testEveryGoldenVectorRoundTrips() throws {
    let vectors = try GoldenVectors.load()
    Check.that(vectors.count >= 15, "vector file missing or too small")

    for vector in vectors {
        let (frame, consumed) = try FrameCodec.decode(vector.bytes)
        Check.equal(consumed, vector.bytes.count, "\(vector.name): decode left bytes over")
        Check.equal(try FrameCodec.encode(frame), vector.bytes,
                    "\(vector.name): re-encode is not byte-identical")
    }
}

/* Frame boundaries never align with report boundaries, so the stream reader —
   not one-shot decode — is what the transport actually uses. */
func testStreamReaderRecoversEveryVector() throws {
    let vectors = try GoldenVectors.load()
    let wire = vectors.flatMap(\.bytes)

    for sliceSize in [1, 7, 64, wire.count] {
        let stream = FrameStream()
        var recovered: [Frame] = []
        var offset = 0
        while offset < wire.count {
            let slice = Array(wire[offset..<min(offset + sliceSize, wire.count)])
            recovered += try stream.push(slice)
            offset += slice.count
        }

        Check.equal(recovered.count, vectors.count, "lost frames at \(sliceSize)-byte slices")
        for (frame, vector) in zip(recovered, vectors) {
            Check.equal(try FrameCodec.encode(frame), vector.bytes,
                        "\(vector.name): mismatch at \(sliceSize)-byte slices")
        }
    }
}

/* A report is a fixed 64 bytes with no length of its own; the tail is padding,
   and the reader must skip it without mistaking payload zeroes for it. */
func testReportPaddingIsWrittenAndSkipped() throws {
    let frames = [Frame(type: MessageType.heartbeat),
                  try FrameCodec.decode(GoldenVectors.named("clip_offer_text2")).frame]
    let reports = try FrameCodec.reports(for: frames)

    for report in reports {
        Check.equal(report.count, ChannelIdentity.reportSize, "report is not report-sized")
    }

    let stream = FrameStream()
    var recovered: [Frame] = []
    for report in reports { recovered += try stream.push(report) }
    Check.equal(recovered, frames, "packed frames did not survive the carrier")

    /* An all-padding report is idle traffic: no frames, no error. */
    let idle = [UInt8](repeating: 0, count: ChannelIdentity.reportSize)
    Check.equal(try stream.push(idle), [], "an idle report produced frames")
}

func testMalformedInputIsRejected() throws {
    /* Length 4097, one over the maximum. */
    let oversize: [UInt8] = [MessageType.heartbeat, 0x00, 0x01, 0x10]
    Check.throwsError(.oversize, "a 4097-byte length was accepted") {
        try FrameCodec.decode(oversize)
    }

    let unknown: [UInt8] = [0xEE, 0x00, 0x00, 0x00]
    Check.throwsError(.unknownType, "an unknown type was accepted") {
        try FrameCodec.decode(unknown)
    }

    let truncated = Array(try GoldenVectors.named("clip_chunk_hi").dropLast())
    Check.throwsError(.truncated, "a truncated frame was presented whole") {
        try FrameCodec.decode(truncated)
    }

    /* A protocol error resets the reader, and the caller drops the connection
       rather than resynchronising on garbage. */
    let stream = FrameStream()
    Check.throwsError(.unknownType, "the reader accepted an unknown type") {
        try stream.push(unknown)
    }
    Check.equal(try stream.push(GoldenVectors.named("heartbeat")).count, 1,
                "the reader did not recover after being reset")
}

/*
 * The four frames below were golden vectors until #109 rewrote
 * test-vectors/frames.txt for protocol v2 (ADR-0008). The core's hello codec
 * still speaks v1 — #110 and #112 are what move it — so the v1 bytes are
 * frozen here rather than read from a file describing a different protocol.
 * Frozen, not copied: nothing new is written against v1, so they cannot drift.
 *
 * WHEN #110 LANDS: delete these and point the two tests below back at
 * GoldenVectors.named(...).
 */
enum FrozenV1 {
    static let helloMac: [UInt8] = [
        0x01, 0x00, 0x17, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04,
        0xEF, 0xBE, 0xAD, 0xDE, 0xEF, 0xBE, 0xAD, 0xDE,
        0xEF, 0xBE, 0xAD, 0xDE, 0xEF, 0xBE, 0xAD, 0xDE,
    ]
    static let helloAckOK: [UInt8] = [0x02, 0x00, 0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04]
    static let helloAckAuthFailed: [UInt8] = [0x02, 0x00, 0x07, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00]
    static let helloAckVersionMismatch: [UInt8] = [0x02, 0x00, 0x07, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00]
}

func testHelloCarriesVersionAndPlatform() throws {
    let vector = FrozenV1.helloMac
    let (frame, _) = try FrameCodec.decode(vector)
    Check.equal(frame.type, MessageType.hello, "hello_mac is not a hello")

    let hello = try Hello.decode(payload: frame.payload)
    Check.equal(hello.protocolVersion, 1, "wrong protocol version")
    Check.equal(hello.os, 1, "the mac hello does not say mac")
    Check.equal(hello.buildType, .release, "wrong build type")
    Check.equal(hello.channelCount, 1, "wrong requested channel count")
    Check.equal(hello.maxChunk, 1024, "wrong requested chunk size")
    Check.equal(hello.token.count, 16, "wrong token length")

    Check.equal(try hello.encoded(), vector, "hello re-encode is not byte-identical")
}

func testHelloAckStatusesAreDistinguishable() throws {
    let cases: [(String, [UInt8], HelloStatus, UInt8, UInt16)] = [
        ("hello_ack_ok", FrozenV1.helloAckOK, .ok, 1, 1024),
        ("hello_ack_auth_failed", FrozenV1.helloAckAuthFailed, .authenticationFailed, 0, 0),
        ("hello_ack_version_mismatch", FrozenV1.helloAckVersionMismatch, .versionIncompatible, 0, 0),
    ]

    for (name, vector, status, channels, chunk) in cases {
        let (frame, _) = try FrameCodec.decode(vector)
        let ack = try HelloAck.decode(payload: frame.payload)

        Check.equal(ack.status, status, "\(name): wrong status")
        Check.equal(ack.channelCount, channels, "\(name): wrong effective channel count")
        Check.equal(ack.maxChunk, chunk, "\(name): wrong effective chunk size")
        Check.equal(try ack.encoded(), vector, "\(name): re-encode is not byte-identical")
    }
}

/* This helper asks for what it was built against, and the device's answer is
   what the session runs with — the two are not assumed equal. */
func testTheHelloThisHelperSendsIsWellFormed() throws {
    let bytes = try Hello().encoded()
    let (frame, consumed) = try FrameCodec.decode(bytes)
    Check.equal(consumed, bytes.count, "the hello is not a whole frame")
    Check.equal(frame.type, MessageType.hello, "the hello is not a hello")

    let hello = try Hello.decode(payload: frame.payload)
    Check.equal(hello.os, 1, "the macOS helper must identify as macOS")
    Check.equal(hello.channelCount, ChannelIdentity.requestedChannelCount, "wrong channel count")
    Check.equal(hello.maxChunk, ChannelIdentity.requestedMaxChunk, "wrong chunk size")
}
