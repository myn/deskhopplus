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
    ("a lazy offer retry does not reclaim the pasteboard", testALazyOfferRetryDoesNotReclaimThePasteboard),
    ("replacing a lazy image cancels its receive", testReplacingALazyImageCancelsItsReceive),
    ("the payload is byte-identical end to end", testFidelityIsPreserved),
    ("nothing leaves before a seal is accepted", testNothingLeavesUnsealed),
    ("a lost seal offer is retried", testALostSealOfferIsRetried),
    ("a lost seal accept is retried", testALostSealAcceptIsRetried),
    ("a slow seal accept survives a retry", testASlowSealAcceptSurvivesARetry),
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
    ("files copied on one computer arrive on the other", testFilesCrossTheLink),
    ("an over-cap copy is explained where the paste would be",
     testAnOverCapCopyIsExplainedWhereThePasteWouldBe),
    ("the question waits for the user to arrive", testTheQuestionWaitsForTheUserToArrive),
    ("copying files reads nothing until they are accepted", testFilesAreNotReadUntilAccepted),
    ("a copy waiting for a seal survives a session end",
     testACopyWaitingForASealSurvivesASessionEnd),
    ("a parked copy is abandoned out loud", testAParkedCopyIsAbandonedOutLoud),
    ("declining files reads nothing and delivers nothing", testDecliningFilesReadsNothing),
    ("a small set of files skips the question", testSmallFileSetsSkipTheQuestion),
    ("several files arrive in order and split correctly", testSeveralFilesSplitCorrectly),
    ("a file list that does not add up is refused", testAMismatchedFileListIsRefused),
    ("a payload that is not the length offered fails", testAShortReadFailsRatherThanTruncates),
    ("files over the size cap are refused before anything crosses", testFilesOverTheCapAreRefused),
    ("a size cap the board states is applied", testTheBoardsSizeCapIsApplied),
    ("files that cannot be read fail rather than truncate", testUnreadableFilesFailTheTransfer),
    ("a held question is withdrawn when the session goes", testAHeldQuestionIsWithdrawn),
    ("an accepted transfer reports its progress", testAnAcceptedTransferReportsProgress),
    ("a transfer can be abandoned from the pasting side", testATransferCanBeAbortedHere),
    ("a failed send leaves a healthy receive alone", testAFailedSendLeavesAHealthyReceiveAlone),
    ("a newer copy withdraws a held question", testANewerCopyWithdrawsAHeldQuestion),
    ("an unanswered question is declined in the end", testAnUnansweredQuestionIsDeclinedInTheEnd),
    ("a set past the size cap is never put to the user", testAnOverCapSetIsNeverPutToTheUser),
    ("offer retries do not re-ask after the answer", testOfferRetriesDoNotReAskAfterTheAnswer),
    ("an accept that cannot run poisons nothing", testAnAcceptThatCannotRunPoisonsNothing),
]

// MARK: - Files (#56)

/*
 * The file list these tests copy, and the payload it names. Written out rather
 * than generated, so that a change to how sizes and offsets are handled has a
 * fixed set of numbers to disagree with.
 */
private let threeFiles = [
    FileListEntry(name: "notes.txt", size: 5),
    FileListEntry(name: "empty.bin", size: 0),
    FileListEntry(name: "data.png", size: 11),
]
private let threeFilePayload: [UInt8] = Array("helloworld other".utf8)

/*
 * A set over `filePromptThreshold`, which is the only kind that is put to the
 * user. The small set above deliberately is not: below the threshold a
 * transfer is a fraction of a second and asking about it is how the prompt
 * that matters gets dismissed unread.
 */
/* Derived from the threshold, never written out. A fixture pinned to whatever
   the constant happened to be when it was written stops testing the question
   the moment the constant moves past it — quietly, which is worse than a
   failure. */
private let bigSize = ClipboardService.filePromptThreshold + 1024
private let bigFiles = [FileListEntry(name: "big.bin", size: UInt64(bigSize))]
private let bigPayload = [UInt8](repeating: 0x5a, count: bigSize)
private func bigPair() -> Pair { Pair(capacity: bigSize + 4096) }

private func testFilesCrossTheLink() {
    let pair = Pair()
    pair.copyFilesOnA(threeFiles, bytes: threeFilePayload)

    Check.equal(pair.filesToB.count, 1, "the files copied on A did not arrive on B")
    Check.equal(pair.filesToB.first?.files, threeFiles, "the file list did not survive the link")
    Check.equal(pair.filesToB.first?.bytes, threeFilePayload,
                "the file payload was not byte-identical end to end")
    Check.that(pair.filesToA.isEmpty, "A was handed its own files back")
}

/*
 * The whole of #56, in one check: a copy costs nothing until someone on the
 * other computer says yes. Everything else here is machinery for this.
 */
/*
 * A set larger than the board's cap is refused here, where the user is.
 *
 * The far end drops an over-cap offer correctly and records it in its own log,
 * which is no use at all to the person who pressed Cmd-C on this computer and
 * saw nothing happen — reported twice as "it never toasted" (#56).
 */
private func testAnOverCapCopyIsExplainedWhereThePasteWouldBe() {
    let pair = bigPair()
    pair.userArrives = false
    _ = pair.b.capacityChanged(megabytes: 1)

    let tooBig = [FileListEntry(name: "over.bin", size: 3 * 1024 * 1024)]
    pair.copyFilesOnA(tooBig, bytes: [UInt8](repeating: 0, count: 8))

    /* Nothing is said while nobody has gone to paste it. */
    Check.that(pair.toldUser.isEmpty,
               "a copy nobody had gone to paste interrupted someone anyway")
    Check.that(pair.fileQuestions.isEmpty, "an over-cap set was put to the user as a question")

    /* And now they cross, find nothing pastes, and are told why. */
    pair.settle(pair.b.userIsHere(), from: .b)
    Check.that(pair.toldUser.contains { $0.contains("larger than the 1 MB clipboard limit") },
               "arriving did not explain why nothing pasted")

    /* Once, not on every crossing. */
    pair.settle(pair.b.userIsHere(), from: .b)
    Check.equal(pair.toldUser.count, 1, "the same explanation was given twice")
}

/*
 * A copy is not a request to interrupt anybody.
 *
 * Most copies are made to be pasted where they were made, so asking on the copy
 * meant a file copied on the Mac and never meant to travel still interrupted
 * whoever was at the Windows machine. A paste over there can only follow the
 * cursor arriving over there, so arrival is the moment the question is worth
 * asking — and if it never comes, it never is (#56).
 */
private func testTheQuestionWaitsForTheUserToArrive() {
    let pair = bigPair()
    pair.userArrives = false
    pair.answerFileOffers = false
    let reads = Reads()

    pair.copyFilesOnA(bigFiles, bytes: bigPayload, reads: reads)
    Check.that(pair.fileQuestions.isEmpty,
               "the far computer was interrupted by a copy nobody had gone to paste")
    Check.equal(reads.count, 0, "the copied files were read for a question never asked")
    Check.that(pair.b.awaitingDecision != nil, "the offer was not held while it waited")

    /* And now the user crosses. */
    pair.settle(pair.b.userIsHere(), from: .b)
    Check.equal(pair.fileQuestions.count, 1, "arriving did not put the question")

    /* Arriving again asks nothing further: crossings are frequent. */
    pair.settle(pair.b.userIsHere(), from: .b)
    Check.equal(pair.fileQuestions.count, 1, "a second crossing asked the same question again")
}

private func testFilesAreNotReadUntilAccepted() {
    let pair = bigPair()
    pair.answerFileOffers = false
    let reads = Reads()

    pair.copyFilesOnA(bigFiles, bytes: bigPayload, reads: reads)
    Check.equal(reads.count, 0, "the copied files were read before anyone accepted them")
    Check.equal(pair.fileQuestions.count, 1, "B was not asked about the files")
    Check.equal(pair.fileQuestions.first?.offer.files, bigFiles,
                "the question did not name the files that were offered")
    Check.that(pair.filesToB.isEmpty, "files were delivered without being accepted")
    Check.equal(pair.b.awaitingDecision?.id, pair.fileQuestions.first?.offer.id,
                "the offer is not being held for an answer")

    /* And now the answer, which is what starts it. */
    guard let offer = pair.fileQuestions.first?.offer else { return }
    pair.settle(pair.b.acceptFiles(id: offer.id), from: .b)
    Check.equal(reads.count, 1, "accepting the files did not read them exactly once")
    Check.equal(pair.filesToB.first?.bytes, bigPayload, "the accepted files did not arrive")
    Check.that(pair.b.awaitingDecision == nil, "the offer is still being held after an answer")
}

private func testDecliningFilesReadsNothing() {
    let pair = bigPair()
    pair.acceptFileOffers = false
    let reads = Reads()

    pair.copyFilesOnA(bigFiles, bytes: bigPayload, reads: reads)
    Check.equal(reads.count, 0, "declined files were read anyway")
    Check.that(pair.filesToB.isEmpty, "declined files were delivered")
    Check.that(pair.withdrawnQuestions.contains(where: { _ in true }),
               "declining did not take the question back")
    /* The copy side is told, so its offer stops repeating. */
    Check.that(!pair.a.awaitingSend, "the declined transfer is still being offered")
}

private func testSmallFileSetsSkipTheQuestion() {
    let pair = Pair()
    pair.answerFileOffers = false
    let small = [FileListEntry(name: "tiny.txt", size: 4)]

    pair.copyFilesOnA(small, bytes: Array("abcd".utf8))
    Check.that(pair.fileQuestions.isEmpty,
               "a set well under the threshold asked a question anyway")
    Check.equal(pair.filesToB.first?.bytes, Array("abcd".utf8),
                "a set under the threshold did not arrive on its own")
}

private func testSeveralFilesSplitCorrectly() {
    let pair = Pair(capacity: 1024 * 1024)
    let files = [
        FileListEntry(name: "a", size: 1000),
        FileListEntry(name: "b", size: 1),
        FileListEntry(name: "c", size: 2000),
    ]
    let payload = [UInt8](repeating: 0x11, count: 1000) + [0x22]
        + [UInt8](repeating: 0x33, count: 2000)
    pair.copyFilesOnA(files, bytes: payload)

    guard let delivery = pair.filesToB.first else {
        Check.that(false, "the three files did not arrive")
        return
    }
    Check.equal(delivery.files, files, "the sizes did not survive")
    var at = 0
    for (index, file) in delivery.files.enumerated() {
        let slice = Array(delivery.bytes[at..<(at + Int(file.size))])
        let expected: UInt8 = [0x11, 0x22, 0x33][index]
        Check.that(slice.allSatisfy { $0 == expected },
                   "file \(file.name) did not slice out of the payload at the right offset")
        at += Int(file.size)
    }
}

/*
 * The offer's total and its list come from the same far helper, so a
 * disagreement between them is that helper being wrong or being tampered with
 * — and this end would otherwise write files by slicing a payload at offsets
 * it has no reason to trust.
 *
 * Checked at the codec's own seam. The sum is the core's — it adds the sizes
 * once, with an overflow check, and hands the total back — so what is asserted
 * here is that the total which comes back is the one the service compares.
 */
private func testAMismatchedFileListIsRefused() {
    guard let meta = FileList.encode(threeFiles), let listed = FileList.decode(meta) else {
        Check.that(false, "the three-file list would not round trip")
        return
    }
    Check.equal(listed.total, 16, "the core's total is not the sum of the list")
    Check.equal(listed.files, threeFiles, "the list did not survive the round trip")

    /* And sizes that overflow their total are refused outright, so no total
       ever comes back for the service to compare. */
    let overflowing = "[{\"name\":\"a\",\"size\":18446744073709551615},{\"name\":\"b\",\"size\":1}]"
    Check.that(FileList.decode(Array(overflowing.utf8)) == nil,
               "sizes that overflow their total were accepted")
}

/*
 * A file edited between the copy and the paste no longer reads at the length
 * that was offered. The core is about to read exactly the offered length from
 * whatever it is handed, so a short read here would be an overread there — and
 * a long one would deliver bytes nobody offered.
 */
private func testAShortReadFailsRatherThanTruncates() {
    for wrong in [[UInt8](repeating: 1, count: 8), [UInt8](repeating: 1, count: 32)] {
        let pair = Pair()
        pair.settle(pair.a.localCopy(files: threeFiles, provider: { wrong }), from: .a)
        Check.that(pair.filesToB.isEmpty,
                   "a payload of \(wrong.count) bytes against a 16-byte offer was delivered")
        Check.that(pair.sawNote(containing: "abandoned rather than sent short"),
                   "a payload that did not match its offer failed silently")
        Check.that(!pair.a.awaitingSend, "the mismatched transfer is still being offered")
    }
}

private func testFilesOverTheCapAreRefused() {
    let pair = Pair(capacity: 64 * 1024)
    pair.answerFileOffers = false
    let reads = Reads()
    let big = [FileListEntry(name: "big.bin", size: 128 * 1024)]

    pair.copyFilesOnA(big, bytes: [UInt8](repeating: 0, count: 128 * 1024), reads: reads)
    Check.that(pair.fileQuestions.isEmpty,
               "a set over the size cap was put to the user rather than refused")
    Check.equal(reads.count, 0, "a set over the size cap was read anyway")
    Check.that(pair.filesToB.isEmpty, "a set over the size cap was delivered")
}

private func testTheBoardsSizeCapIsApplied() {
    let pair = Pair(capacity: 1024 * 1024)
    pair.answerFileOffers = false

    /* The board says one megabyte. What was already in force is irrelevant —
       the device is the single source of truth (#42). */
    pair.settle(pair.b.capacityChanged(megabytes: 1), from: .b)

    let reads = Reads()
    let big = [FileListEntry(name: "big.bin", size: 2 * 1024 * 1024)]
    pair.copyFilesOnA(big, bytes: [UInt8](repeating: 0, count: 2 * 1024 * 1024), reads: reads)
    Check.that(pair.filesToB.isEmpty, "a set over the board's cap was delivered")
    Check.equal(reads.count, 0, "a set over the board's cap was read")

    /* And a set inside the new cap still crosses, so the cap narrowed rather
       than broke the direction. */
    pair.copyFilesOnA(threeFiles, bytes: threeFilePayload)
    Check.equal(pair.filesToB.count, 1, "a set inside the board's cap did not arrive")
}

/*
 * A file edited between the copy and the paste can no longer be read at the
 * length that was promised. That has to fail the transfer: a payload short of
 * its offer would be delivered as a complete file.
 */
private func testUnreadableFilesFailTheTransfer() {
    let pair = Pair()
    pair.settle(pair.a.localCopy(files: threeFiles, provider: { nil }), from: .a)

    Check.that(pair.filesToB.isEmpty, "files that could not be read were delivered anyway")
    Check.that(pair.sawNote(containing: "could not be read"),
               "files that could not be read failed silently")
    Check.that(!pair.a.awaitingSend, "the failed transfer is still being offered")
}

private func testAHeldQuestionIsWithdrawn() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    guard let offer = pair.fileQuestions.first?.offer else {
        Check.that(false, "no question was asked")
        return
    }

    pair.settle(pair.b.sessionEnded(), from: .b)
    Check.that(pair.withdrawnQuestions.contains(offer.id),
               "a session that ended left a question standing over a transfer that is gone")
    Check.that(pair.b.awaitingDecision == nil, "the offer is still held after the session ended")
}

/*
 * The two directions are independent, and a failure in one must not take the
 * other's state with it. Reachable in the ordinary way: this computer offers
 * files whose bytes can no longer be read while it is still holding a question
 * about files the other computer offered. Transfer ids collide across the two
 * directions (#136), so the id on the failure cannot say which one it was.
 */
private func testAFailedSendLeavesAHealthyReceiveAlone() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    guard let offer = pair.fileQuestions.first?.offer else {
        Check.that(false, "no question was asked")
        return
    }

    /* B now tries to send files of its own, and cannot read them. */
    pair.settle(pair.b.localCopy(files: threeFiles, provider: { nil }), from: .b)
    Check.that(pair.sawNote(containing: "could not be read"), "B's send did not fail")

    Check.equal(pair.b.awaitingDecision?.id, offer.id,
                "a failed send withdrew the question B was still holding")
    Check.that(pair.withdrawnQuestions.isEmpty,
               "a failed send took back a question about the other direction")

    /*
     * How far this can be taken today. The receive does *not* complete, and
     * for a reason outside this file: B's cancel names its own transfer id,
     * A's outgoing transfer holds the same id, and A abandons it
     * (#136 — "transfer ids collide across directions"). That is a live bug
     * with a ticket of its own, and #56 gives it a new way to happen, since a
     * file send that cannot read its files is a fresh source of CLIP_CANCEL.
     *
     * What is asserted above is the part this file owns: the *state* is kept.
     * When #136 lands, the check below becomes a delivery.
     */
    /*
     * How far this can be taken today. Accepting does *not* deliver, and for a
     * reason outside this file: B's cancel names its own transfer id, A's
     * outgoing transfer holds the same id, and A abandons it (#136 —
     * "transfer ids collide across directions"). That is a live bug with a
     * ticket of its own, and #56 gives it a new way to happen, since a file
     * send that cannot read its files is a fresh source of CLIP_CANCEL.
     *
     * What is asserted above is the part this file owns: the state is kept, so
     * the acceptance still reaches a transfer rather than falling on the floor.
     * When #136 lands, this becomes a delivery.
     */
    pair.settle(pair.b.acceptFiles(id: offer.id), from: .b)
    Check.that(pair.sawNote(containing: "were accepted here and asked for"),
               "the acceptance did not reach a transfer at all")
}

/*
 * A text copy made while a file question is still standing supersedes it
 * inside the transfer machine. The question has to go with it: left up, the
 * menu goes on offering Accept for a transfer the far end has moved past,
 * where accepting does nothing and says nothing either.
 */
private func testANewerCopyWithdrawsAHeldQuestion() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    guard let offer = pair.fileQuestions.first?.offer else {
        Check.that(false, "no question was asked")
        return
    }

    pair.copyOnA("something else entirely")
    Check.that(pair.withdrawnQuestions.contains(offer.id),
               "a newer copy left the question about the old transfer standing")
    Check.that(pair.b.awaitingDecision == nil, "the superseded offer is still held")
    Check.equal(text(pair.deliveredToB), ["something else entirely"],
                "the newer copy did not arrive")
}

/*
 * A question nobody answers cannot stand for ever. The copy side re-offers
 * every two seconds until its offer is requested, so an ignored prompt is a
 * frame every two seconds for the life of the session — and the receive buffer
 * the size cap sizes stays pinned while it stands.
 */
private func testAnUnansweredQuestionIsDeclinedInTheEnd() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    guard let offer = pair.fileQuestions.first?.offer else {
        Check.that(false, "no question was asked")
        return
    }

    /* The first tick arms the deadline; nothing expires on it. */
    pair.settle(pair.b.tick(at: 1000), from: .b)
    Check.equal(pair.b.awaitingDecision?.id, offer.id, "the first tick declined it outright")

    pair.settle(pair.b.tick(at: 1000 + ClipboardService.holdTimeout - 1), from: .b)
    Check.equal(pair.b.awaitingDecision?.id, offer.id, "it was declined a second early")

    pair.settle(pair.b.tick(at: 1000 + ClipboardService.holdTimeout), from: .b)
    Check.that(pair.b.awaitingDecision == nil, "an unanswered question stood past its deadline")
    Check.that(pair.withdrawnQuestions.contains(offer.id),
               "the expired question was not taken back")
    Check.that(pair.filesToB.isEmpty, "an expired question delivered its files anyway")
    Check.that(!pair.a.awaitingSend, "the copy side is still offering a declined transfer")
}

/*
 * A set past the size cap is *refused*, not put to the user.
 *
 * Hardware, 2026-09-02: with the cap at 2 MB a 2.46 MB file was toasted on
 * Windows, Accept did nothing, and the file never arrived. The predicate asked
 * whether an offer had been *seen*, which stays true for one the machine has
 * already cancelled — so the question was asked about a transfer that was
 * already declined.
 */
private func testAnOverCapSetIsNeverPutToTheUser() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.settle(pair.b.capacityChanged(megabytes: 2), from: .b)

    let reads = Reads()
    let big = [FileListEntry(name: "big.bin", size: 2_581_661)]
    pair.copyFilesOnA(big, bytes: [UInt8](repeating: 0, count: 2_581_661), reads: reads)

    Check.that(pair.fileQuestions.isEmpty,
               "a set past the size cap was put to the user rather than refused")
    Check.equal(reads.count, 0, "a set past the size cap was read")
    Check.that(pair.filesToB.isEmpty, "a set past the size cap was delivered")
    Check.that(pair.b.awaitingDecision == nil, "a refused offer is being held for an answer")
}

/*
 * The copy side repeats its offer every two seconds until it is requested
 * (#78). Once the answer has gone out, those repeats must not ask again.
 *
 * Hardware, 2026-09-02: one file produced three toasts and three Accepts.
 */
private func testOfferRetriesDoNotReAskAfterTheAnswer() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    guard let offer = pair.fileQuestions.first?.offer else {
        Check.that(false, "no question was asked")
        return
    }
    guard let sent = pair.carriedFrames.last(where: { $0.type == MessageType.clipOffer }) else {
        Check.that(false, "no offer crossed the link")
        return
    }
    pair.settle(pair.b.acceptFiles(id: offer.id), from: .b)
    Check.equal(pair.fileQuestions.count, 1, "accepting re-asked the question")

    /*
     * The copy side's retry, arriving *after* the answer — the frame that
     * crossed with the request on hardware. Handed over again as-is, which is
     * exactly what the copy side repeats.
     */
    for _ in 0..<3 {
        pair.settle(pair.b.received(type: sent.type, body: sent.body), from: .b)
    }
    Check.equal(pair.fileQuestions.count, 1,
                "an offer retry re-asked a question the user had already answered")
    Check.equal(pair.filesToB.count, 1, "the accepted transfer did not arrive")
}

/*
 * Accepting an offer the machine is no longer holding must not remember its
 * file list. It did, and the *next* transfer was then split by the wrong list
 * and silently written nowhere — which at the desk is a paste that never
 * happens, with the log line blaming a length mismatch.
 */
private func testAnAcceptThatCannotRunPoisonsNothing() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.settle(pair.b.capacityChanged(megabytes: 2), from: .b)

    /* Refused for being over the cap, so nothing is held — but ask anyway, as
       a stale menu item would. */
    let big = [FileListEntry(name: "big.bin", size: 2_581_661)]
    pair.copyFilesOnA(big, bytes: [UInt8](repeating: 0, count: 2_581_661))
    pair.settle(pair.b.acceptFiles(id: 1), from: .b)

    /* And now a transfer that is entirely fine. */
    pair.answerFileOffers = true
    pair.copyFilesOnA(threeFiles, bytes: threeFilePayload)
    Check.equal(pair.filesToB.count, 1, "a healthy transfer did not arrive after a dead accept")
    Check.equal(pair.filesToB.first?.bytes, threeFilePayload,
                "a healthy transfer was split by a dead transfer's list")
}

private func testAnAcceptedTransferReportsProgress() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)

    Check.that(pair.b.arriving == nil, "a held offer reported itself as arriving")
    guard let offer = pair.fileQuestions.first?.offer else {
        Check.that(false, "no question was asked about a set over the line")
        return
    }
    Check.equal(offer.total, UInt64(bigSize), "the question named the wrong size")
    /* The estimate is the whole point of the question: it is what the user
       weighs. Checked against the rate rather than a number, so it stays a
       check on the arithmetic and not on today's constants. */
    let expected = bigSize / ClipboardService.measuredBytesPerSecond
    Check.that(offer.estimatedSeconds >= expected,
               "the estimate is shorter than the measured rate allows")

    pair.settle(pair.b.acceptFiles(id: offer.id), from: .b)
    Check.equal(pair.filesToB.count, 1, "the accepted set did not arrive")
    Check.that(pair.b.arriving == nil, "a finished transfer still reports itself as arriving")
}

private func testATransferCanBeAbortedHere() {
    let pair = bigPair()
    pair.answerFileOffers = false
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    guard pair.fileQuestions.first?.offer != nil else {
        Check.that(false, "no question was asked")
        return
    }

    pair.settle(pair.b.abortReceive(), from: .b)
    Check.that(pair.filesToB.isEmpty, "an aborted transfer was delivered")
    Check.that(pair.b.awaitingDecision == nil, "the aborted offer is still held")
}

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
/*
 * How many times the provider was called, as an object.
 *
 * A local `var` handed over as `&count` will not do, and the way it fails is
 * worth the type: the pointer `&` produces is valid only for the duration of
 * the call it is passed to, and this provider is called minutes later, when the
 * far side accepts. In a debug build the local happens to still be where the
 * pointer says; in a release build the optimiser is free to move it, and the
 * increment lands somewhere else — so the test passed under `swift run` and
 * failed under `./tools/build.sh`, which builds for release.
 */
private final class Reads {
    var count = 0
}

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
    /// Messages put in front of the user, as distinct from diagnostics.
    var toldUser: [String] = []
    /// Frames carried across the link, so a test can count what a direction cost.
    var carried = 0
    var lazyImages = 0
    var lastLazyImageID: UInt32?
    var requestLazyImages = true
    /*
     * The paste-side acceptance (#56). Every file offer that reaches a side
     * is recorded, and — unless a test says otherwise — answered the way a user
     * clicking Accept would. A test that wants the *held* state, which is the
     * whole point of the gate, sets `answerFileOffers` to false and the offer
     * stays waiting.
     */
    var fileQuestions: [(side: Side, offer: FileOffer)] = []
    var withdrawnQuestions: [UInt32] = []
    var answerFileOffers = true
    /// Whether the user crosses to the far computer, which is what puts a held
    /// question on screen. False models a copy nobody ever went to paste.
    var userArrives = true
    var acceptFileOffers = true
    var filesToA: [FileDelivery] = []
    var filesToB: [FileDelivery] = []
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
                /* The user crosses to wherever they mean to paste, which is
                   what puts a held question on screen (#56). Idempotent, so
                   modelling it on every frame costs nothing; a test that wants
                   "they never went over there" clears the flag. */
                if userArrives { queue += far.userIsHere().map { (farSide, $0) } }
            case .deliver(let kind, let bytes):
                if side == .a { deliveredToA.append((kind, bytes)) }
                else { deliveredToB.append((kind, bytes)) }
            case .lazyImage(let id, _):
                lazyImages += 1
                lastLazyImageID = id
                if !requestLazyImages { continue }
                let near = side == .a ? a : b
                queue += near.requestLazyImage(id: id).map { (side, $0) }
            case .cancelLazyImage:
                break
            case .fileOffer(let offer):
                fileQuestions.append((side, offer))
                guard answerFileOffers else { continue }
                let near = side == .a ? a : b
                queue += (acceptFileOffers ? near.acceptFiles(id: offer.id)
                                           : near.declineFiles(id: offer.id)).map { (side, $0) }
            case .fileOfferWithdrawn(let id):
                withdrawnQuestions.append(id)
            case .deliverFiles(let delivery):
                if side == .a { filesToA.append(delivery) } else { filesToB.append(delivery) }
            case .note(let note):
                notes.append("\(side): \(note)")
            case .tellUser(let message):
                /* Recorded beside the notes, so `sawNote` finds it: a test
                   cares that it was said, not by which route. */
                notes.append("\(side): \(message)")
                toldUser.append(message)
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

    /// Files copied on A, with a provider that hands over `bytes` — the read
    /// that must not happen until the far side accepts.
    @discardableResult
    func copyFilesOnA(_ files: [FileListEntry], bytes: [UInt8],
                      reads: Reads? = nil) -> [ClipboardOutput] {
        let outputs = a.localCopy(files: files, provider: {
            reads?.count += 1
            return bytes
        })
        settle(outputs, from: .a)
        return outputs
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

private func testALazyOfferRetryDoesNotReclaimThePasteboard() {
    let png = [UInt8](repeating: 0x55, count: ClipboardService.eagerImageThreshold + 1)
    let pair = Pair(capacity: png.count + 1024)
    pair.requestLazyImages = false
    pair.settle(pair.a.localCopy(kind: .png, bytes: png), from: .a)
    Check.equal(pair.lazyImages, 1, "the first lazy offer did not claim the pasteboard once")

    _ = pair.a.tick(at: 0)
    pair.settle(pair.a.tick(at: ClipboardService.sweepDelay), from: .a)
    Check.equal(pair.lazyImages, 1,
                "an idempotent offer retry reclaimed and erased the pasteboard")
}

private func testReplacingALazyImageCancelsItsReceive() {
    let png = [UInt8](repeating: 0x55, count: ClipboardService.eagerImageThreshold + 1)
    let pair = Pair(capacity: png.count + 1024)
    pair.requestLazyImages = false
    pair.settle(pair.a.localCopy(kind: .png, bytes: png), from: .a)
    guard let id = pair.lastLazyImageID else {
        Check.that(false, "the lazy image never claimed the pasteboard")
        return
    }

    pair.settle(pair.b.lazyImageWasReplaced(id: id), from: .b)
    Check.that(pair.b.requestLazyImage(id: id).isEmpty,
               "a replaced lazy image could still start receiving")
    Check.that(pair.deliveredToB.isEmpty,
               "a replaced lazy image was delivered over the newer local copy")
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

    /* Counted as frames, not as outputs: the copy also says out loud that it
       is waiting, and a note is not on the wire. */
    let frames = first.compactMap { output -> UInt8? in
        if case .send(let type, _) = output { return type } else { return nil }
    }
    Check.equal(frames, [MessageType.sealOffer],
                "a copy with no seal put something other than one seal offer on the wire")
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

/*
 * A SEAL_ACCEPT that is slow rather than lost.
 *
 * `Seal.offer()` draws a new seal id and a new ephemeral key on every call —
 * "the offerer owns the seal" — so a retry threw away the key the peer was at
 * that moment answering, and the accept came back naming an id this end no
 * longer knew (DH_SEAL_ERR_UNKNOWN_ID). On a link whose round trip runs past
 * `sweepDelay` that is not a race but a livelock: every accept is answering an
 * offer that has already been replaced, so nothing is ever sealed and no file
 * is ever offered. Observed on hardware as "no toast, nothing at all".
 *
 * The invariant: an accept for an offer this end made stays usable across a
 * retry of that same handshake.
 */
private func testASlowSealAcceptSurvivesARetry() {
    let pair = Pair()
    /* Both accepts are held back, so the only one this end ever sees is the
       first — handed over by hand below, after a retry has gone out. */
    pair.dropNext[MessageType.sealAccept] = 2
    pair.copyOnA("survives a slow seal accept")

    guard let held = pair.carriedFrames.last(where: { $0.type == MessageType.sealAccept }) else {
        Check.that(false, "no seal accept was ever built")
        return
    }

    _ = pair.a.tick(at: 0)
    pair.settle(pair.a.tick(at: ClipboardService.sweepDelay), from: .a)

    /* And now the slow one lands. */
    pair.settle(pair.a.received(type: held.type, body: held.body), from: .a)
    Check.that(!pair.sawNote(containing: "seal accept could not be used"),
               "a retry replaced the key the in-flight accept was answering")
    Check.equal(text(pair.deliveredToB), ["survives a slow seal accept"],
                "the copy never crossed, so nothing was ever sealed")
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

/*
 * A copy made while the link is reconnecting is held, not thrown away.
 *
 * On a thrashing link a copy lands with no seal to send it under, so it parks.
 * The fault this pins was that the *next* session end dropped the parked copy
 * without a word — at the desk the file was copied, no question was ever put
 * to the far side, and neither helper's log said anything at all. Silence is
 * the part that made it unfindable, so both halves are checked: the copy still
 * goes out, and the wait is on the record.
 */
private func testACopyWaitingForASealSurvivesASessionEnd() {
    let pair = bigPair()

    /* The seal offer is lost, so the copy has no key and has to wait. */
    pair.dropNext[MessageType.sealOffer] = 1
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)
    Check.that(pair.fileQuestions.isEmpty,
               "the files were offered with no seal to send them under")
    Check.that(pair.sawNote(containing: "waiting for a seal"),
               "a copy parked with nothing said about it")

    /* The link wobbles again before the seal lands. */
    pair.settle(pair.a.sessionEnded(), from: .a)

    /* And now it comes back. The copy that was waiting goes out on its own,
       without the user having to copy it a second time. */
    _ = pair.a.tick(at: 0)
    pair.settle(pair.a.tick(at: ClipboardService.sweepDelay), from: .a)
    Check.equal(pair.fileQuestions.count, 1,
                "a copy made while the link was down never went out after it came back")
    Check.equal(pair.filesToB.first?.bytes, bigPayload,
                "the copy that survived the wobble did not arrive whole")
}

/*
 * The other end of the same fault: when a parked copy really is given up on,
 * it is given up on out loud. Thirty seconds of a link that never comes back
 * is the one case where a copy is allowed to vanish.
 */
private func testAParkedCopyIsAbandonedOutLoud() {
    let pair = bigPair()
    pair.dropNext[MessageType.sealOffer] = 99
    pair.copyFilesOnA(bigFiles, bytes: bigPayload)

    _ = pair.a.tick(at: 0)
    pair.settle(pair.a.tick(at: ClipboardService.stallTimeout), from: .a)
    Check.that(pair.sawNote(containing: "was abandoned"),
               "a parked copy was given up on in silence")
    Check.that(pair.filesToB.isEmpty, "files crossed a link that never sealed")
}

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
