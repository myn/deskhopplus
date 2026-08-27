import DHCore
import DeskhopChannel
import Foundation

/*
 * The clipboard text path (#52), driven as **two helpers talking to each
 * other** rather than one talking to a script.
 *
 * That is the whole value of the shape. Every rule these exercise — the seal
 * exchange, the credit window, the received-set, abandon-on-drop — is the
 * shared core's, and a test where one end answers a mock of the other would be
 * checking the mock. Here a copy made on one side is carried, frame by frame,
 * through a relay that does nothing but hand bytes over, and the check is that
 * the bytes come out the far side identical.
 *
 * No AppKit: what these drive is the joining, not the pasteboard.
 */

let clipboardTests: [(String, () throws -> Void)] = [
    ("text copied on one computer arrives on the other", testTextCrossesTheLink),
    ("the payload is byte-identical end to end", testFidelityIsPreserved),
    ("nothing leaves before a seal is accepted", testNothingLeavesUnsealed),
    ("a payload larger than one chunk is reassembled", testAMultiChunkPayloadArrives),
    ("a second copy supersedes the first", testASecondCopySupersedes),
    ("sending turned off stops this direction only", testSendingOffStopsOneDirection),
    ("receiving turned off refuses without opening the seal", testReceivingOffRefusesTheOffer),
    ("a toggle turned off abandons what is in flight", testATurnedOffToggleAbandonsInFlight),
    ("a lost session abandons the transfer and the seal", testALostSessionAbandonsEverything),
    ("a seal the far end lost is offered again", testAStaleSealIsReoffered),
    ("a malformed control message is refused, not acted on", testMalformedControlMessages),
    ("a transfer that stops moving is abandoned and reported", testAStalledTransferIsAbandoned),
    ("a transfer that is still moving is left alone", testProgressKeepsATransferAlive),
    ("an abandonment says what the board has dropped", testAStallSaysWhatTheBoardHasDropped),
    ("a chunk is not opened when receiving is off", testChunksAreRefusedBeforeTheSealIsOpened),
    ("copy after copy after copy all arrive", testRepeatedCopiesAllArrive),
    ("every payload size arrives intact", testEveryPayloadSizeArrives),
    ("a receive the link starved recovers on its own", testAStalledReceiveAsksAgain),
    ("sweeping does not keep a dead receive alive", testASweptReceiveIsStillAbandoned),
    ("a superseding offer gets its own deadline", testASupersededReceiveResetsTheDeadline),
    ("a lost offer retries without extending its deadline", testALostOfferRetries),
    ("a conflicting authenticated offer ends the local session", testConflictingOfferEndsSession),
]

/*
 * Entropy that is deterministic but not constant: a seal needs a fresh
 * ephemeral key and nonce per exchange, and a source that returned the same
 * bytes twice would key both directions identically and hide a real mix-up.
 */
private func counterEntropy(_ seed: UInt8) -> (Int) -> [UInt8] {
    var step: UInt8 = 0
    return { count in
        step &+= 1
        return (0..<count).map { UInt8(truncatingIfNeeded: $0) &* 31 &+ seed &* 7 &+ step }
    }
}

/*
 * Two helpers and the link between them. `deliver` collects what each side
 * writes to its own pasteboard; `notes` keeps the diagnostics so a test can say
 * *why* nothing crossed rather than only that nothing did.
 *
 * The board is not modelled, deliberately. Between two helpers it is an opaque
 * relay that holds no key and reads no payload (ADR-0008), so a hop that hands
 * bytes over unchanged is exactly what one is.
 */
private final class Pair {
    let a: ClipboardService
    let b: ClipboardService
    var deliveredToA: [(kind: UInt8, bytes: [UInt8])] = []
    var deliveredToB: [(kind: UInt8, bytes: [UInt8])] = []
    var notes: [String] = []
    /// Frames carried across the link, so a test can count what a direction cost.
    var carried = 0
    /*
     * Frames the link loses before they reach the far end, counted down by
     * message type. This is the seam ADR-0005 describes — a bounded queue
     * refusing a frame with no retransmit beneath it — and it is the only way
     * to reach the failure #145 reports, because on a healthy link nothing is
     * ever lost.
     */
    var dropNext: [UInt8: Int] = [:]

    init(capacity: Int = 64 * 1024) {
        a = ClipboardService(entropy: counterEntropy(1), capacity: capacity)
        b = ClipboardService(entropy: counterEntropy(2), capacity: capacity)
    }

    /// Carry every frame to its far end, and everything that answers, until the
    /// two ends have nothing left to say. Bounded: a pair that will not settle
    /// is a defect, and a test that hangs reports it as a timeout an hour later.
    func settle(_ outputs: [ClipboardOutput], from origin: Side) {
        var queue: [(Side, ClipboardOutput)] = outputs.map { (origin, $0) }
        var rounds = 0

        while !queue.isEmpty {
            rounds += 1
            precondition(rounds < 100_000, "the two helpers never stopped answering each other")

            let (side, output) = queue.removeFirst()
            switch output {
            case .send(let type, let body):
                carried += 1
                /* A frame going out is a chance to push the next credit-gated
                   batch, which is what the runtime does on the same seam — and
                   it happens whether or not the link carries this one. */
                let near = side == .a ? a : b
                queue += near.pump().map { (side, $0) }
                if let left = dropNext[type], left > 0 {
                    dropNext[type] = left - 1
                    continue /* refused at a seam with no retransmit beneath it */
                }
                let far = side == .a ? b : a
                let farSide: Side = side == .a ? .b : .a
                queue += far.received(type: type, body: body).map { (farSide, $0) }
            case .deliver(let kind, let bytes):
                if side == .a { deliveredToA.append((kind, bytes)) }
                else { deliveredToB.append((kind, bytes)) }
            case .note(let note):
                notes.append("\(side): \(note)")
            case .protocolError(let note):
                notes.append("\(side): protocol error: \(note)")
            }
        }
    }

    enum Side { case a, b }

    func copyOnA(_ text: String) {
        settle(a.localCopy(kind: .text, bytes: Array(text.utf8)), from: .a)
    }

    func copyOnB(_ text: String) {
        settle(b.localCopy(kind: .text, bytes: Array(text.utf8)), from: .b)
    }

    func sawNote(containing fragment: String) -> Bool {
        notes.contains { $0.contains(fragment) }
    }
}

private func text(_ delivered: [(kind: UInt8, bytes: [UInt8])]) -> [String] {
    delivered.map { String(decoding: $0.bytes, as: UTF8.self) }
}

// MARK: - The path itself

private func testTextCrossesTheLink() {
    let pair = Pair()
    pair.copyOnA("hello from the Mac")

    Check.equal(text(pair.deliveredToB), ["hello from the Mac"],
                "text copied on A did not arrive on B")
    Check.equal(pair.deliveredToB.first?.kind, ClipKind.text.rawValue,
                "the payload did not arrive as text")
    Check.that(pair.deliveredToA.isEmpty, "A was handed its own copy back")

    /* And the other way, on the same pair: the two directions are independent
       and each has its own seal. */
    pair.copyOnB("hello from Windows")
    Check.equal(text(pair.deliveredToA), ["hello from Windows"],
                "text copied on B did not arrive on A")
}

/*
 * ADR-0003: the channel is fidelity-preserving. The wire payload is
 * byte-identical end to end and nothing on the path validates, filters or
 * normalizes it — so the bytes that must survive are the awkward ones.
 */
private func testFidelityIsPreserved() {
    let awkward = "tab\there\nnewline — em dash, 🧷 emoji, \u{0000} NUL, \u{FFFD} replacement"
    let pair = Pair()
    pair.copyOnA(awkward)

    Check.equal(pair.deliveredToB.count, 1, "the awkward payload did not arrive")
    Check.equal(pair.deliveredToB.first?.bytes, Array(awkward.utf8),
                "the payload was not byte-identical end to end")
}

/*
 * A payload never goes out unsealed, and the exchange is a round trip — so the
 * first copy after a session begins waits for a key rather than travelling
 * without one. What this checks is that the *first* thing on the wire is the
 * seal offer, not the offer.
 */
private func testNothingLeavesUnsealed() {
    let a = ClipboardService(entropy: counterEntropy(1))
    let first = a.localCopy(kind: .text, bytes: Array("waiting".utf8))

    Check.equal(first.count, 1, "a copy with no seal produced more than the seal offer")
    guard case .send(let type, _) = first.first else {
        Check.that(false, "a copy with no seal sent nothing")
        return
    }
    Check.equal(type, MessageType.sealOffer, "the first thing sent was not a seal offer")
}

/*
 * One chunk is 1024 bytes, the credit window is 16, and the batch cap is 16 —
 * so a payload of a few tens of kilobytes is the first one where credit,
 * batching and the received-set all actually run. Below that the machinery is
 * present and never exercised.
 */
private func testAMultiChunkPayloadArrives() {
    let long = String(repeating: "deskhopplus ", count: 4000) /* ~48 KB, ~47 chunks */
    let pair = Pair()
    pair.copyOnA(long)

    Check.equal(pair.deliveredToB.count, 1, "a multi-chunk payload did not arrive")
    Check.equal(pair.deliveredToB.first?.bytes, Array(long.utf8),
                "a multi-chunk payload was reassembled wrongly")
    /* Enough frames to prove it really was chunked, rather than a cap having
       quietly shortened it. */
    Check.that(pair.carried > 40, "a 48 KB payload crossed in \(pair.carried) frames, which is "
                                  + "too few to have been chunked")
}

/*
 * What the user last copied is what they mean to paste. A second copy while the
 * first is still waiting on a seal replaces it rather than queueing behind it.
 */
private func testASecondCopySupersedes() {
    let pair = Pair()
    /* Both before anything is carried: the first is still waiting on a seal. */
    let first = pair.a.localCopy(kind: .text, bytes: Array("first".utf8))
    let second = pair.a.localCopy(kind: .text, bytes: Array("second".utf8))
    pair.settle(first + second, from: .a)

    Check.equal(text(pair.deliveredToB), ["second"],
                "the superseded copy arrived, or the newer one did not")
}

// MARK: - The two toggles

private func testSendingOffStopsOneDirection() {
    let pair = Pair()
    /* A→B off: A may not send, B may not receive — which is the same fact told
       to each end in its own terms (dh_clip_policy_for). */
    pair.settle(pair.a.policyChanged(flags: UInt8(DH_CLIP_MAY_RECEIVE)), from: .a)
    pair.settle(pair.b.policyChanged(flags: UInt8(DH_CLIP_MAY_SEND)), from: .b)

    pair.copyOnA("this must not cross")
    Check.that(pair.deliveredToB.isEmpty, "a copy crossed a direction that is turned off")
    Check.that(pair.sawNote(containing: "clipboard sending turned off"),
               "nothing said why the copy did not cross")

    /* The other direction is untouched. This is the criterion in #52 that a
       single global toggle would pass by accident. */
    pair.copyOnB("this must still cross")
    Check.equal(text(pair.deliveredToA), ["this must still cross"],
                "turning off one direction stopped the other")
}

/*
 * A helper told not to receive refuses the offer from its clear head, without
 * opening the seal. Decrypting a payload it has already decided to refuse would
 * be the one place a turned-off direction still handled the content.
 */
private func testReceivingOffRefusesTheOffer() {
    let pair = Pair()
    pair.settle(pair.b.policyChanged(flags: UInt8(DH_CLIP_MAY_SEND)), from: .b)

    pair.copyOnA("refused at the far end")
    Check.that(pair.deliveredToB.isEmpty, "a payload was written to a pasteboard that is off")
    Check.that(pair.sawNote(containing: "clipboard receiving turned off"),
               "the far end did not say why it refused")
    /* The sender is told, so a transfer does not sit half-open waiting. */
    Check.that(pair.sawNote(containing: "was abandoned"),
               "the sending end was never told its transfer was cancelled")
}

/*
 * Turning a toggle off mid-transfer takes what is crossing with it. Letting the
 * bytes already on the wire finish arriving would make the control a control
 * over *new* copies only, which is not what "stop content leaving this machine"
 * means.
 */
private func testATurnedOffToggleAbandonsInFlight() {
    let pair = Pair()
    /* Get a seal established, then start something big enough to still be in
       flight when the toggle moves. */
    pair.copyOnA("warm the seal")
    let long = String(repeating: "x", count: 40_000)
    let started = pair.a.localCopy(kind: .text, bytes: Array(long.utf8))

    let stopped = pair.a.policyChanged(flags: UInt8(DH_CLIP_MAY_RECEIVE))
    pair.settle(started + stopped, from: .a)

    Check.equal(text(pair.deliveredToB), ["warm the seal"],
                "a transfer survived the toggle that was meant to stop it")
    Check.that(pair.sawNote(containing: "sending was turned off"),
               "nothing recorded the toggle stopping a transfer")
}

// MARK: - Losing the session

private func testALostSessionAbandonsEverything() {
    let pair = Pair()
    pair.copyOnA("before the drop")
    Check.equal(text(pair.deliveredToB), ["before the drop"], "the first copy did not arrive")

    /* The link goes. Partial data is never kept, and both halves of the seal go
       with the session — a key whose peer may no longer exist is worse than no
       key at all (#107). */
    let dropped = pair.a.sessionEnded()
    Check.that(!dropped.contains { if case .send = $0 { return true } else { return false } },
               "a frame was produced for a session that had already gone")

    /* The next copy re-offers a seal rather than reusing the dead one. */
    let after = pair.a.localCopy(kind: .text, bytes: Array("after the drop".utf8))
    guard case .send(let type, _) = after.first else {
        Check.that(false, "nothing was sent after the session came back")
        return
    }
    Check.equal(type, MessageType.sealOffer, "a copy after a lost session reused the dead seal")
}

/*
 * The ordinary recovery when one end's session ended and the other's did not:
 * the far helper holds no key for the seal it is sent, says so, and the sender
 * offers a fresh one with the payload still waiting behind it.
 */
private func testAStaleSealIsReoffered() {
    let pair = Pair()
    pair.copyOnA("first, to establish a seal")
    Check.equal(pair.deliveredToB.count, 1, "the first copy did not arrive")

    /* Only B's session ends. A still holds a seal B can no longer open. */
    _ = pair.b.sessionEnded()

    pair.copyOnA("second, across a seal the far end has forgotten")
    Check.equal(text(pair.deliveredToB), ["first, to establish a seal",
                                          "second, across a seal the far end has forgotten"],
                "a payload was lost when the far end forgot the seal")
    Check.that(pair.sawNote(containing: "offering a fresh one"),
               "the fresh seal offer was not recorded")
}

/*
 * A control message that will not decode is refused rather than acted on with
 * whatever the fields happened to read as. These carry transfer ids and
 * sequence numbers, so a misread one cancels or re-requests the wrong thing.
 */
private func testMalformedControlMessages() {
    let a = ClipboardService(entropy: counterEntropy(1))
    let cases: [(UInt8, String)] = [
        (MessageType.clipRequest, "request"),
        (MessageType.clipDone, "done"),
        (MessageType.clipCancel, "cancel"),
        (MessageType.clipRetransmit, "retransmit"),
        (MessageType.clipCredit, "credit"),
    ]
    for (type, name) in cases {
        let outputs = a.received(type: type, body: [0x01, 0x02])
        Check.that(outputs.allSatisfy { if case .note = $0 { return true } else { return false } },
                   "a malformed \(name) produced something other than a diagnostic")
    }

    /* A type this helper has no case for is named, not dropped silently. */
    let unknown = a.received(type: 0x3F, body: [])
    Check.that(unknown.contains { if case .note = $0 { return true } else { return false } },
               "an unknown clipboard message vanished without a word")
}


/*
 * The third interruption #52 names: the helper on the *other* computer crashes.
 * This end's session is untouched — it simply stops being answered — so nothing
 * message-driven can notice, and without the tick the transfer would sit
 * holding its payload until the next copy happened to supersede it.
 */
private func testAStalledTransferIsAbandoned() {
    let a = ClipboardService(entropy: counterEntropy(1))
    let b = ClipboardService(entropy: counterEntropy(2))

    /* Get as far as an offer sitting on the wire, then stop answering. */
    var outputs = a.localCopy(kind: .text, bytes: Array("into the void".utf8))
    for output in outputs {
        guard case .send(let type, let body) = output else { continue }
        for reply in b.received(type: type, body: body) {
            if case .send(let replyType, let replyBody) = reply {
                outputs += a.received(type: replyType, body: replyBody)
            }
        }
    }

    /* Well inside the timeout, nothing happens: abandoning a healthy transfer
       is the worse of the two mistakes. */
    Check.that(a.tick(at: 1).isEmpty, "a transfer was abandoned on its first tick")
    Check.that(!a.tick(at: ClipboardService.stallTimeout - 1).contains {
                   if case .note(let n) = $0 { return n.contains("abandoned") }
                   return false
               },
               "a transfer was abandoned before the timeout elapsed")

    let abandoned = a.tick(at: ClipboardService.stallTimeout + 1)
    Check.that(abandoned.contains { if case .note(let n) = $0 { return n.contains("no progress") }
                                    return false },
               "a stalled transfer was never abandoned")
    Check.that(abandoned.contains { if case .note(let n) = $0 { return n.contains("abandoned") }
                                    return false },
               "the abandonment was not reported")

    /* And it is gone: a second timeout produces nothing to abandon. */
    Check.that(a.tick(at: ClipboardService.stallTimeout * 3).isEmpty,
               "the abandoned transfer was abandoned twice")
}

/*
 * An abandonment quotes what the board says it has dropped (#133).
 *
 * A stall on a board that has been losing frames and a stall on a board with
 * clean seams are different faults, and until #133 the difference could not be
 * measured at all: the totals were readable only from the config page, which
 * is reachable only by rebooting the board that holds them.
 *
 * The three answers the line has to keep apart are "the board has said
 * nothing", "the board says nothing was dropped", and "the board says *this*
 * was dropped" — the first two being the pair that got read as each other.
 */
private func testAStallSaysWhatTheBoardHasDropped() {
    func stallNote(_ drops: BoardDrops?) -> String {
        let a = ClipboardService(entropy: counterEntropy(1))
        let b = ClipboardService(entropy: counterEntropy(2))
        var outputs = a.localCopy(kind: .text, bytes: Array("into the void".utf8))
        for output in outputs {
            guard case .send(let type, let body) = output else { continue }
            for reply in b.received(type: type, body: body) {
                if case .send(let replyType, let replyBody) = reply {
                    outputs += a.received(type: replyType, body: replyBody)
                }
            }
        }
        /* The first tick arms the stall clock; the second is past it. */
        _ = a.tick(at: 1, boardDrops: drops)
        let abandoned = a.tick(at: ClipboardService.stallTimeout + 1, boardDrops: drops)
        for output in abandoned {
            if case .note(let n) = output, n.contains("abandoned") { return n }
        }
        return ""
    }

    Check.that(stallNote(nil).contains("stated no drop totals"),
               "a stall on a board that has said nothing did not say so")

    var clean = dh_device_drops()
    Check.that(stallNote(BoardDrops(clean)).contains("no drops"),
               "a stall on a board with clean seams did not say so")

    /* One seam, named — and only that one, so the line is all signal. */
    clean.truncated = 3
    let named = stallNote(BoardDrops(clean))
    Check.that(named.contains("peer frames truncated 3"),
               "a stall did not name the seam the board says is losing frames")
    Check.that(!named.contains("orphan"),
               "a stall listed a seam that had lost nothing")

    /*
     * The outbound total is three causes in one number, and which of them is
     * moving decides what to do about it — a deeper bulk queue fixes nothing
     * if the single-frame priority band is what refused, and a bad header is
     * version skew rather than congestion at all (#142).
     *
     * The exact reading this was written for: the total climbing while the
     * bulk band, the one #141 deepened, is clean.
     */
    var split = dh_device_drops()
    split.outq = 7
    split.outq_priority = 7
    let bands = stallNote(BoardDrops(split))
    Check.that(bands.contains("outbound refused 7 (priority 7, bulk 0, bad header 0)"),
               "a stall did not say which band of the outbound queue refused")
}

/*
 * The timeout measures *progress*, not the transfer's total duration. A large
 * payload legitimately takes minutes on this link, and a deadline on the whole
 * transfer would abandon healthy ones — which is why the counters exist rather
 * than a single start time.
 */
private func testProgressKeepsATransferAlive() {
    let pair = Pair()
    pair.copyOnA("warm the seal")

    let long = String(repeating: "y", count: 40_000)
    pair.settle(pair.a.localCopy(kind: .text, bytes: Array(long.utf8)), from: .a)
    Check.equal(pair.deliveredToB.count, 2, "the long payload did not arrive")

    /* Ticks far beyond the timeout, on a pair that has been talking all along.
       Nothing is owed and nothing is running, so nothing is abandoned. */
    let late = pair.a.tick(at: ClipboardService.stallTimeout * 10)
    Check.that(late.isEmpty, "a finished transfer was reported as stalled")
}


/*
 * A helper told not to receive never decrypts a payload it has already decided
 * to refuse (docs/protocol.md) — and that has to hold for chunks, not only for
 * the offer that introduced them.
 *
 * It is reachable in the ordinary way: turning the toggle off mid-transfer
 * cancels the transfer, but up to a credit window of chunks is already in
 * flight behind that cancel.
 *
 * What makes the refusal observable is the *silence*. A chunk that reached the
 * seal under a key this end does not hold would come back as a SEAL_STALE; one
 * refused ahead of it produces nothing at all.
 */
private func testChunksAreRefusedBeforeTheSealIsOpened() {
    let a = ClipboardService(entropy: counterEntropy(1))
    let b = ClipboardService(entropy: counterEntropy(2))

    /* A real sealed chunk, built by A. */
    var chunks: [[UInt8]] = []
    let pair = Pair()
    _ = pair
    var queue = a.localCopy(kind: .text, bytes: Array(String(repeating: "z", count: 4000).utf8))
    var rounds = 0
    while !queue.isEmpty && rounds < 1000 {
        rounds += 1
        let output = queue.removeFirst()
        guard case .send(let type, let body) = output else { continue }
        if type == MessageType.clipChunk { chunks.append(body); continue }
        queue += b.received(type: type, body: body).compactMap { reply -> ClipboardOutput? in
            guard case .send(let replyType, let replyBody) = reply else { return nil }
            return a.received(type: replyType, body: replyBody).first
        }
        queue += a.pump()
    }
    Check.that(!chunks.isEmpty, "no sealed chunk was produced to test with")
    guard let chunk = chunks.first else { return }

    /* A third helper, holding no seal at all and told not to receive. Without
       the guard it would try to open the chunk, fail to find the seal, and
       answer SEAL_STALE — which is the payload having reached the cipher. */
    let refusing = ClipboardService(entropy: counterEntropy(3))
    _ = refusing.policyChanged(flags: UInt8(DH_CLIP_MAY_SEND))
    let outputs = refusing.received(type: MessageType.clipChunk, body: chunk)
    Check.that(outputs.isEmpty,
               "a chunk reached the seal on a helper that had already refused to receive")

    /* And the control: with receiving allowed, the same chunk *does* reach the
       seal, so the check above is measuring the guard rather than nothing. */
    let accepting = ClipboardService(entropy: counterEntropy(4))
    let answered = accepting.received(type: MessageType.clipChunk, body: chunk)
    Check.that(answered.contains { if case .send(let t, _) = $0 { return t == MessageType.sealStale }
                                   return false },
               "with receiving on, a chunk under an unknown seal was not answered with SEAL_STALE")
}


/*
 * The desk case that the single-copy tests miss entirely: a user copies, then
 * copies again, then again. Each one must arrive.
 *
 * Only the first copy of a session pays for the seal exchange; every one after
 * it supersedes a transfer the far end has already delivered, which is a
 * different path through both ends and one nothing here exercised.
 */
private func testRepeatedCopiesAllArrive() {
    let pair = Pair()
    let sent = ["first", "second", "third", "fourth", "fifth"]
    for text in sent { pair.copyOnA(text) }

    Check.equal(text(pair.deliveredToB), sent,
                "copies made one after another did not all arrive, or arrived out of order")
}

/*
 * Every length from empty to well past a chunk boundary. A payload is text,
 * and text is whatever length the user selected — so any size that does not
 * survive is a size that silently pastes the wrong thing.
 */
private func testEveryPayloadSizeArrives() {
    var failures: [Int] = []
    for size in 1...80 {
        let pair = Pair()
        let payload = String((0..<size).map { Character(UnicodeScalar(UInt8(65 + $0 % 26))) })
        pair.copyOnA(payload)
        if text(pair.deliveredToB) != [payload] { failures.append(size) }
    }
    /* A few sizes near a chunk boundary, where the arithmetic changes. */
    for size in [1023, 1024, 1025, 2047, 2048, 2049] {
        let pair = Pair()
        let payload = String(repeating: "x", count: size)
        pair.copyOnA(payload)
        if text(pair.deliveredToB) != [payload] { failures.append(size) }
    }
    Check.equal(failures, [], "these payload sizes did not arrive intact")
}

/*
 * A receive the link starved gets itself going again (#145).
 *
 * Every credit grant the first chunks earn is lost. The sender stops at zero
 * credit and nothing message-driven can fire on either end: no chunk arrives
 * to prompt the receiver, and no CLIP_DONE arrives to drive the sweep that
 * CLIP_DONE used to be the only way to reach. Before this the transfer sat
 * there until the thirty-second deadline reported it lost — at no consistent
 * size and no consistent fraction, which is exactly what the log showed.
 */
private func testAStalledReceiveAsksAgain() {
    let pair = Pair()
    pair.copyOnA("warm the seal")

    pair.dropNext[MessageType.clipCredit] = 6
    let long = String(repeating: "z", count: 40_000)
    pair.settle(pair.a.localCopy(kind: .text, bytes: Array(long.utf8)), from: .a)
    Check.equal(pair.deliveredToB.count, 1, "the transfer was expected to stall and did not")

    /* The receiving end's tick, at its own cadence and nothing else's. */
    var now = ClipboardService.sweepDelay
    var ticks = 0
    while pair.deliveredToB.count < 2 && now < ClipboardService.stallTimeout {
        pair.settle(pair.b.tick(at: now), from: .b)
        now += ClipboardService.sweepDelay
        ticks += 1
    }
    Check.equal(pair.deliveredToB.count, 2, "the starved receive never recovered")
    Check.equal(text(pair.deliveredToB).last, long, "the recovered payload is not the one sent")
    Check.that(pair.sawNote(containing: "asked for again"),
               "the receive recovered without saying it had stalled")
    Check.that(ticks > 0, "the recovery did not come from a tick")
}

/*
 * Sweeping must not keep a dead receive alive.
 *
 * The stall deadline counts *arrivals*, not the messages this end emits — and
 * a sweep emits messages. Counting those would let a receive whose far helper
 * has gone reset its own deadline for ever, turning the fix for #145 into a
 * transfer that is never reported at all.
 */
private func testASweptReceiveIsStillAbandoned() {
    let pair = Pair()
    pair.copyOnA("warm the seal")

    /* The far end never hears a request, so it never sends a chunk. */
    pair.dropNext[MessageType.clipRequest] = 10_000
    pair.settle(pair.a.localCopy(kind: .text, bytes: Array("into the void".utf8)), from: .a)
    Check.equal(pair.deliveredToB.count, 1, "the second copy was expected to stall")

    var abandoned: String?
    var now: TimeInterval = 0
    while now <= ClipboardService.stallTimeout + 1 {
        for output in pair.b.tick(at: now) {
            if case .note(let n) = output, n.contains("was abandoned") { abandoned = n }
        }
        now += ClipboardService.sweepDelay
    }
    guard let note = abandoned else {
        Check.that(false, "a receive that swept for the whole timeout was never abandoned")
        return
    }
    /* And the abandonment says what it asked for and what came back, which is
       the reading neither end produced before. */
    Check.that(note.contains("asked for") && note.contains("back"),
               "the abandonment did not say whether a retransmit was asked for: \(note)")
}

/*
 * A newer offer supersedes an incomplete receive, and the transfer that
 * replaces it is entitled to the whole deadline — not to whatever is left of
 * the one it displaced.
 *
 * The reset event for this timer is "something arrived for the transfer being
 * timed", and a supersede changes *which* transfer is being timed without
 * changing how much of it has arrived: both counts are zero. So the count
 * alone cannot see it, and the transfer that arrives second is abandoned for
 * the sins of the first — reported as thirty seconds of silence when it is
 * seconds old.
 */
private func testASupersededReceiveResetsTheDeadline() {
    let pair = Pair()
    pair.copyOnA("warm the seal")

    /* The far end never hears a request, so nothing ever arrives. */
    pair.dropNext[MessageType.clipRequest] = 10_000
    pair.settle(pair.a.localCopy(kind: .text, bytes: Array("the first copy".utf8)), from: .a)
    _ = pair.b.tick(at: 1) /* arms the deadline on the first transfer */

    /* A second copy supersedes it, most of the way through that deadline. */
    pair.settle(pair.a.localCopy(kind: .text, bytes: Array("the second copy".utf8)), from: .a)

    let late = pair.b.tick(at: ClipboardService.stallTimeout + 1)
    Check.that(!late.contains { if case .note(let n) = $0 { return n.contains("was abandoned") }
                                return false },
               "a transfer seconds old was abandoned on the deadline of the one it replaced")
}

private func testALostOfferRetries() {
    let pair = Pair()
    pair.copyOnA("warm the seal")
    pair.dropNext[MessageType.clipOffer] = 1
    pair.settle(pair.a.localCopy(kind: .text, bytes: Array("recovered offer".utf8)), from: .a)
    Check.equal(pair.deliveredToB.count, 1, "the deliberately lost offer arrived")
    pair.settle(pair.a.tick(at: 0), from: .a)
    pair.settle(pair.a.tick(at: ClipboardService.sweepDelay), from: .a)
    Check.equal(pair.deliveredToB.count, 2, "the lost offer was not retried")
    Check.that(pair.sawNote(containing: "retry action(s) were produced"),
               "successful offer recovery was not diagnosed")
    Check.equal(pair.notes.filter { $0.contains("retry action(s) were produced") }.count, 1,
                "offer recovery was diagnosed more than once")
    Check.that(!pair.a.tick(at: 2 * ClipboardService.sweepDelay).contains {
                   if case .send(let type, _) = $0 { return type == MessageType.clipOffer }
                   return false
               }, "offer retry continued after the request")

    let dead = Pair()
    dead.copyOnA("warm the seal")
    dead.dropNext[MessageType.clipOffer] = 10_000
    dead.settle(dead.a.localCopy(kind: .text, bytes: Array("unanswered".utf8)), from: .a)
    var now: TimeInterval = 0
    while now <= ClipboardService.stallTimeout {
        dead.settle(dead.a.tick(at: now), from: .a)
        now += ClipboardService.sweepDelay
    }
    Check.that(dead.sawNote(containing: "was abandoned"),
               "offer retries extended the terminal deadline")

    let duplicate = Pair()
    duplicate.copyOnA("warm the seal")
    duplicate.dropNext[MessageType.clipRequest] = 10_000
    duplicate.settle(duplicate.a.localCopy(kind: .text, bytes: Array("duplicate".utf8)), from: .a)
    now = 0
    while now <= ClipboardService.stallTimeout {
        duplicate.settle(duplicate.a.tick(at: now), from: .a)
        duplicate.settle(duplicate.b.tick(at: now), from: .b)
        now += ClipboardService.sweepDelay
    }
    Check.that(duplicate.sawNote(containing: "was abandoned")
               && duplicate.sawNote(containing: "duplicate offers"),
               "duplicate arrivals moved or hid the receive deadline")
}

private func testConflictingOfferEndsSession() throws {
    let service = ClipboardService(entropy: counterEntropy(1))
    let sender = ClipboardSeal(entropy: counterEntropy(2))
    let offered = try sender.offer()
    let accepted = service.received(type: MessageType.sealOffer, body: offered).compactMap {
        if case .send(let type, let body) = $0, type == MessageType.sealAccept { return body }
        return nil
    }
    Check.equal(accepted.count, 1, "the service did not accept the test seal")
    guard let accept = accepted.first else { return }
    try sender.accepted(accept)

    let first = try sender.seal(ClipOffer(id: 9, kind: 0, total: 1))
    _ = service.received(type: MessageType.clipOffer, body: first)
    let conflict = try sender.seal(ClipOffer(id: 9, kind: 0, total: 2))
    let outputs = service.received(type: MessageType.clipOffer, body: conflict)
    Check.that(outputs.contains { if case .protocolError = $0 { return true }; return false },
               "an authenticated offer identity conflict did not end the local session")
}
