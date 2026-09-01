import DHCore
import DeskhopChannel
import Foundation

/*
 * The clipboard payload path (#52, #55), driven as **two helpers talking to each
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
    ("an image copied on one computer arrives byte-identically", testImageCrossesTheLink),
    ("the payload is byte-identical end to end", testFidelityIsPreserved),
    ("nothing leaves before a seal is accepted", testNothingLeavesUnsealed),
    ("a lost seal offer is retried", testALostSealOfferIsRetried),
    ("a lost seal accept is retried", testALostSealAcceptIsRetried),
    ("seal retries do not extend the copy deadline", testSealRetriesDoNotExtendTheCopyDeadline),
    ("a payload larger than one chunk is reassembled", testAMultiChunkPayloadArrives),
    ("a second copy supersedes the first", testASecondCopySupersedes),
    ("sending turned off stops this direction only", testSendingOffStopsOneDirection),
    ("receiving turned off refuses without opening the seal", testReceivingOffRefusesTheOffer),
    ("a toggle turned off abandons what is in flight", testATurnedOffToggleAbandonsInFlight),
    ("a lost session abandons the transfer and the seal", testALostSessionAbandonsEverything),
    ("a seal the far end lost is offered again", testAStaleSealIsReoffered),
    ("a malformed control message is refused, not acted on", testMalformedControlMessages),
    ("an unanswered send is retained without a false failure", testAStalledSendIsRetained),
    ("a transfer that is still moving is left alone", testProgressKeepsATransferAlive),
    ("a receive abandonment says what the board has dropped", testAStallSaysWhatTheBoardHasDropped),
    ("a chunk is not opened when receiving is off", testChunksAreRefusedBeforeTheSealIsOpened),
    ("copy after copy after copy all arrive", testRepeatedCopiesAllArrive),
    ("every payload size arrives intact", testEveryPayloadSizeArrives),
    ("a receive the link starved recovers on its own", testAStalledReceiveAsksAgain),
    ("a copy-side deadline does not defeat a later receive sweep", testACopySideDeadlineDoesNotDefeatSweep),
    ("sweeping does not keep a dead receive alive", testASweptReceiveIsStillAbandoned),
    ("a superseding offer gets its own deadline", testASupersededReceiveResetsTheDeadline),
    ("a lost offer retries until a retention boundary", testALostOfferRetries),
    ("a conflicting authenticated offer ends the local session", testConflictingOfferEndsSession),
    ("a restarted far helper is not heard as stale", testARestartedFarHelperIsHeard),
    ("a restarted far helper's reused id is not a conflict", testARestartedIdIsNotAConflict),
    ("a receive under a replaced seal is abandoned", testAReplacedSealAbandonsTheReceive),
    ("an offer under the replaced seal cannot revive it", testADelayedOfferCannotRevive),
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
    /* `a` is replaceable because one of the failures these drive is the far
       helper's *process* going away and coming back (#151): its offer ids are
       ordered inside that process and start again at one with it, which no
       call on a living service can reproduce. */
    private(set) var a: ClipboardService
    let b: ClipboardService
    var deliveredToA: [(kind: UInt8, bytes: [UInt8])] = []
    var deliveredToB: [(kind: UInt8, bytes: [UInt8])] = []
    var notes: [String] = []
    /// Frames carried across the link, so a test can count what a direction cost.
    var carried = 0
    var lazyImages = 0
    /*
     * Frames the link loses before they reach the far end, counted down by
     * message type. This is the seam ADR-0005 describes — a bounded queue
     * refusing a frame with no retransmit beneath it — and it is the only way
     * to reach the failure #145 reports, because on a healthy link nothing is
     * ever lost.
     */
    var dropNext: [UInt8: Int] = [:]
    /// Every frame put on the link, in order, so a test can hand one over
    /// again later — the only way to reach a message that was sealed under a
    /// key its receiver has since replaced.
    var carriedFrames: [(type: UInt8, body: [UInt8])] = []

    private let capacity: Int

    init(capacity: Int = 64 * 1024) {
        self.capacity = capacity
        a = ClipboardService(entropy: counterEntropy(1), capacity: capacity)
        b = ClipboardService(entropy: counterEntropy(2), capacity: capacity)
    }

    /// A's helper process went away and came back: a new service, with an
    /// offer-id namespace that starts at one again. B is untouched, which is
    /// the whole asymmetry — its session never ended.
    func restartA() {
        a = ClipboardService(entropy: counterEntropy(3), capacity: capacity)
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
                carriedFrames.append((type, body))
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
            case .lazyImage(let id, _):
                lazyImages += 1
                let near = side == .a ? a : b
                queue += near.requestLazyImage(id: id).map { (side, $0) }
            case .cancelLazyImage:
                break
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

private func testImageCrossesTheLink() {
    for (png, expectedLazy) in [
        ([UInt8](repeating: 0x55, count: 1024), 0),
        ([UInt8](repeating: 0xaa, count: ClipboardService.eagerImageThreshold + 1), 1),
    ] {
        let pair = Pair(capacity: png.count + 1024)
        pair.settle(pair.a.localCopy(kind: .png, bytes: png), from: .a)
        Check.equal(pair.deliveredToB.first?.kind, ClipKind.png.rawValue,
                    "the payload did not arrive as an image")
        Check.equal(pair.deliveredToB.first?.bytes, png,
                    "the eager/lazy image was not byte-identical end to end")
        Check.equal(pair.lazyImages, expectedLazy,
                    "the image did not take the threshold-selected path")
    }
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

private func testALostSealOfferIsRetried() {
    let pair = Pair()
    pair.dropNext[MessageType.sealOffer] = 1
    pair.copyOnA("survives a lost seal offer")

    _ = pair.a.tick(at: 0)
    pair.settle(pair.a.tick(at: ClipboardService.sweepDelay), from: .a)

    Check.equal(text(pair.deliveredToB), ["survives a lost seal offer"],
                "a copy did not recover after its SEAL_OFFER was lost")
}

private func testALostSealAcceptIsRetried() {
    let pair = Pair()
    pair.dropNext[MessageType.sealAccept] = 1
    pair.copyOnA("survives a lost seal accept")

    _ = pair.a.tick(at: 0)
    pair.settle(pair.a.tick(at: ClipboardService.sweepDelay), from: .a)

    Check.equal(text(pair.deliveredToB), ["survives a lost seal accept"],
                "a copy did not recover after its SEAL_ACCEPT was lost")
}

private func testSealRetriesDoNotExtendTheCopyDeadline() {
    let pair = Pair()
    pair.dropNext[MessageType.sealOffer] = 100
    pair.copyOnA("never sealed")

    _ = pair.a.tick(at: 0)
    var now = ClipboardService.sweepDelay
    while now < ClipboardService.stallTimeout {
        pair.settle(pair.a.tick(at: now), from: .a)
        now += ClipboardService.sweepDelay
    }
    pair.settle(pair.a.tick(at: ClipboardService.stallTimeout), from: .a)

    Check.that(pair.sawNote(containing: "waiting for a seal") &&
               pair.sawNote(containing: "was abandoned"),
               "a copy whose seal exchange never completed was not reported abandoned")
    Check.that(pair.a.tick(at: ClipboardService.stallTimeout + ClipboardService.sweepDelay).isEmpty,
               "a seal retry kept the terminal deadline open")
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
 * A helper on the other computer may be gone, or it may have accepted the
 * payload and gone quiet. Without a delivery acknowledgement those states are
 * indistinguishable, so silence alone cannot end or diagnose the send.
 */
private func testAStalledSendIsRetained() {
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

    /* There is no delivery acknowledgement in v2. Silence is equally
       consistent with success, so it cannot abandon or diagnose the send. */
    Check.that(a.tick(at: 1).isEmpty, "a transfer was abandoned on its first tick")
    let late = a.tick(at: ClipboardService.stallTimeout * 3)
    Check.that(!late.contains {
                   if case .note(let note) = $0 { return note.contains("not answering") }
                   if case .send(let type, _) = $0 { return type == MessageType.clipCancel }
                   return false
               }, "an unanswered send was falsely diagnosed or abandoned")
}

/*
 * The hardware failure in #146: the copy side reaches DONE, but one chunk and
 * the paste side's early retransmit requests are lost. The copy side's own
 * old copy-side deadline then passes before a request finally crosses. DONE is
 * not acknowledged, so elapsed time must not discard the payload that the
 * protocol promises to retain for a late retransmit.
 */
private func testACopySideDeadlineDoesNotDefeatSweep() {
    let pair = Pair()
    pair.copyOnA("warm the seal")

    pair.dropNext[MessageType.clipChunk] = 1
    pair.dropNext[MessageType.clipRetransmit] = 10_000
    let payload = String(repeating: "recovery ", count: 5_000)
    pair.copyOnA(payload)
    Check.equal(pair.deliveredToB.count, 1,
                "the deliberately incomplete transfer unexpectedly arrived")

    _ = pair.a.tick(at: 0)
    _ = pair.a.tick(at: ClipboardService.stallTimeout + 1)

    pair.dropNext = [:]
    _ = pair.b.tick(at: 0) // arm the paste side's sweep clock
    pair.settle(pair.b.tick(at: ClipboardService.sweepDelay + 1), from: .b)

    Check.equal(text(pair.deliveredToB), ["warm the seal", payload],
                "the old copy-side deadline discarded the payload before the late retransmit")
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
        let pair = Pair()
        pair.copyOnA("warm the seal")
        pair.dropNext[MessageType.clipChunk] = 10_000
        pair.settle(pair.a.localCopy(kind: .text, bytes: Array(repeating: 1, count: 5_000)),
                    from: .a)
        /* The first tick arms the stall clock; the second is past it. */
        _ = pair.b.tick(at: 1, boardDrops: drops)
        let abandoned = pair.b.tick(at: ClipboardService.stallTimeout + 1, boardDrops: drops)
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
    Check.that(!dead.sawNote(containing: "was abandoned"),
               "an unanswered offer was falsely diagnosed or abandoned")

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

/*
 * The asymmetric restart (#151): the far computer's helper process goes away
 * and comes back while this end's session never falters.
 *
 * Offer ids are ordered inside the *copy side helper's* namespace, so a fresh
 * process starts again at one. Without a boundary at the fresh seal, this end
 * measures that one against the dead process's offer-id frontier, calls it stale, and
 * the clipboard stays dead in that direction until this end resets too.
 */
private func testARestartedFarHelperIsHeard() {
    let pair = Pair()
    pair.copyOnA("first")
    pair.copyOnA("second") /* the frontier is now above one */
    Check.equal(text(pair.deliveredToB), ["first", "second"],
                "the copies before the restart did not arrive")

    pair.restartA()
    pair.copyOnA("after the restart")

    Check.equal(text(pair.deliveredToB), ["first", "second", "after the restart"],
                "the restarted helper's first offer was ignored as stale")
}

/*
 * The same restart one copy earlier, where the reused id is not older but
 * *equal* — and the payload behind it is a different one. Answered as a fresh
 * transfer, not as an identity conflict, which would end the session over a
 * far helper doing nothing wrong.
 */
private func testARestartedIdIsNotAConflict() {
    let pair = Pair()
    pair.copyOnA("first")
    Check.equal(pair.deliveredToB.count, 1, "the copy before the restart did not arrive")

    pair.restartA()
    pair.copyOnA("a different payload under the same id")

    Check.equal(text(pair.deliveredToB), ["first", "a different payload under the same id"],
                "the restarted helper's reused id did not carry its payload")
    Check.that(!pair.sawNote(containing: "protocol error"),
               "a restarted helper's reused offer id was read as a conflict")
}

/*
 * A half-arrived transfer belongs to the seal it arrived under. The helper
 * that sent it has forgotten it, so it can never be finished — abandoned
 * whole, and never written to the pasteboard in part.
 */
private func testAReplacedSealAbandonsTheReceive() {
    let pair = Pair()
    pair.copyOnA("first, to establish a seal")

    /* Every chunk of the second copy is refused at a seam with no retransmit
       beneath it, so B is left holding an offer and no payload. */
    pair.dropNext[MessageType.clipChunk] = 10_000
    pair.copyOnA(String(repeating: "lost ", count: 1000))
    Check.equal(pair.deliveredToB.count, 1, "the payload arrived through a link that dropped it")

    pair.dropNext = [:]
    pair.restartA()
    pair.copyOnA("after the restart")

    Check.equal(text(pair.deliveredToB), ["first, to establish a seal", "after the restart"],
                "a partial payload was delivered, or the copy after the restart was not")
    Check.that(pair.sawNote(containing: "abandoned: the far helper started a fresh seal"),
               "the receive under the replaced seal was abandoned without saying why")
}

/*
 * The straggler. An offer sealed under the replaced key can still be in flight
 * when the fresh one is accepted, and arriving late it must not be able to
 * recreate the receive state that was just given up. It cannot be opened at
 * all: this end holds one incoming seal, and the fresh one replaced it.
 */
private func testADelayedOfferCannotRevive() {
    let pair = Pair()
    pair.copyOnA("first, to establish a seal")
    guard let old = pair.carriedFrames.last(where: { $0.type == MessageType.clipOffer }) else {
        Check.that(false, "no offer was carried to hold back")
        return
    }

    pair.restartA()
    pair.copyOnA("after the restart")

    let outputs = pair.b.received(type: MessageType.clipOffer, body: old.body)
    Check.that(outputs.contains {
                   if case .send(let type, _) = $0 { return type == MessageType.sealStale }
                   return false
               }, "an offer under the replaced seal was opened rather than refused")
    Check.equal(text(pair.deliveredToB), ["first, to establish a seal", "after the restart"],
                "a delayed offer under the replaced seal changed what arrived")
}
