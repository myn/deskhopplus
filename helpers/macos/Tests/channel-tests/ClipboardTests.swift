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
                let far = side == .a ? b : a
                let farSide: Side = side == .a ? .b : .a
                queue += far.received(type: type, body: body).map { (farSide, $0) }
                /* A frame going out is a chance to push the next credit-gated
                   batch, which is what the runtime does on the same seam. */
                let near = side == .a ? a : b
                queue += near.pump().map { (side, $0) }
            case .deliver(let kind, let bytes):
                if side == .a { deliveredToA.append((kind, bytes)) }
                else { deliveredToB.append((kind, bytes)) }
            case .note(let note):
                notes.append("\(side): \(note)")
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
    Check.that(a.tick(at: ClipboardService.stallTimeout - 1).isEmpty,
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
