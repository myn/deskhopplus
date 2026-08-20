import DeskhopChannel
import DHCore

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
    ("a hello round-trips through its body codec", testHelloRoundTrips),
    ("pairing and refusal messages round-trip", testPairingMessagesRoundTrip),
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

    let idle = [UInt8](repeating: 0, count: ChannelIdentity.reportSize)
    Check.equal(try stream.push(idle), [], "an idle report produced frames")
}

func testMalformedInputIsRejected() throws {
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

    let stream = FrameStream()
    Check.throwsError(.unknownType, "the reader accepted an unknown type") {
        try stream.push(unknown)
    }
    Check.equal(try stream.push(GoldenVectors.named("heartbeat")).count, 1,
                "the reader did not recover after being reset")
}

/* v2: a hello carries the key id, nonce, and correlation — and is authenticated
   under k_hello. This round-trips encode → auth_open → decode → re-encode. */
func testHelloRoundTrips() throws {
    let helper = try SoftwareIdentity(privateKey: [UInt8](0x01...0x20))
    let board = try SoftwareIdentity(privateKey: [UInt8](0x21...0x40))
    let nonce = [UInt8](0x00...0x0F)
    let correlation: UInt64 = 0xDEADBEEFCAFE0DF0

    let kHello = try helper.deriveHelloKey(boardPublicKey: board.publicKey, helperNonce: nonce)

    let hello = Hello(correlation: correlation, helperKeyId: helper.keyId, helperNonce: nonce)
    let bytes = try hello.encoded(key: kHello)

    let (frame, consumed) = try FrameCodec.decode(bytes)
    Check.equal(consumed, bytes.count, "the hello is not a complete frame")
    Check.equal(frame.type, MessageType.hello, "wrong type")

    var counter = dh_auth_counter()
    dh_auth_counter_init(&counter)
    let body = try AuthFrame.open(frame: frame, key: kHello, counter: &counter)

    let decoded = try Hello.decode(body: body)
    Check.equal(decoded.protocolVersion, UInt16(DH_PROTO_VERSION), "wrong version")
    Check.equal(decoded.os, 1, "wrong OS")
    Check.equal(decoded.correlation, correlation, "wrong correlation")
    Check.equal(decoded.helperKeyId, helper.keyId, "wrong key id")
    Check.equal(decoded.helperNonce, nonce, "wrong nonce")
}

func testPairingMessagesRoundTrip() throws {
    let correlation: UInt64 = 0x1234567890ABCDEF
    let pubKey = [UInt8](repeating: 0x42, count: Int(DH_P256_PUBLIC_SIZE))

    let request = PairRequest(correlation: correlation, helperPublicKey: pubKey)
    let reqBytes = try request.encoded()
    let reqDecoded = try PairRequest.decode(payload: FrameCodec.decode(reqBytes).frame.payload)
    Check.equal(reqDecoded.correlation, correlation, "pair_request correlation")
    Check.equal(reqDecoded.helperPublicKey, pubKey, "pair_request public key")

    let grant = PairGrant(correlation: correlation, boardPublicKey: pubKey)
    let grantBytes = try grant.encoded()
    let grantDecoded = try PairGrant.decode(payload: FrameCodec.decode(grantBytes).frame.payload)
    Check.equal(grantDecoded.correlation, correlation, "pair_grant correlation")
    Check.equal(grantDecoded.boardPublicKey, pubKey, "pair_grant public key")

    let refused = PairRefused(correlation: correlation, reason: .noWindow)
    let refBytes = try refused.encoded()
    let refDecoded = try PairRefused.decode(payload: FrameCodec.decode(refBytes).frame.payload)
    Check.equal(refDecoded.correlation, correlation, "pair_refused correlation")
    Check.equal(refDecoded.reason, .noWindow, "pair_refused reason")

    let hRefused = HelloRefused(correlation: correlation, protocolVersion: 2, status: .unpaired)
    let hRefBytes = try hRefused.encoded()
    let hRefDecoded = try HelloRefused.decode(payload: FrameCodec.decode(hRefBytes).frame.payload)
    Check.equal(hRefDecoded.correlation, correlation, "hello_refused correlation")
    Check.equal(hRefDecoded.status, .unpaired, "hello_refused status")
}

func testTheHelloThisHelperSendsIsWellFormed() throws {
    let helper = try SoftwareIdentity(privateKey: [UInt8](0x01...0x20))
    let board = try SoftwareIdentity(privateKey: [UInt8](0x21...0x40))
    let nonce = [UInt8](repeating: 0xAA, count: Int(DH_NONCE_SIZE))

    let kHello = try helper.deriveHelloKey(boardPublicKey: board.publicKey, helperNonce: nonce)
    let bytes = try Hello(helperKeyId: helper.keyId, helperNonce: nonce)
        .encoded(key: kHello)

    let (frame, consumed) = try FrameCodec.decode(bytes)
    Check.equal(consumed, bytes.count, "the hello is not a whole frame")
    Check.equal(frame.type, MessageType.hello, "the hello is not a hello")

    var counter = dh_auth_counter()
    dh_auth_counter_init(&counter)
    let body = try AuthFrame.open(frame: frame, key: kHello, counter: &counter)
    let hello = try Hello.decode(body: body)

    Check.equal(hello.os, 1, "the macOS helper must identify as macOS")
    Check.equal(hello.channelCount, ChannelIdentity.requestedChannelCount, "wrong channel count")
    Check.equal(hello.maxChunk, ChannelIdentity.requestedMaxChunk, "wrong chunk size")
    Check.equal(hello.protocolVersion, UInt16(DH_PROTO_VERSION), "wrong protocol version")
}
