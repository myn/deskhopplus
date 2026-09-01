import DeskhopChannel
import Foundation

/*
 * The shim's dispatch (#152): that each output case reaches the effect it
 * names.
 *
 * This is the layer #93 and #94 are shaped like — a helper reporting healthy
 * while doing nothing. `HelperSession` and `ClipboardService` are both covered
 * in depth, and until this file existed the code that turned each of their
 * outputs into a real effect was correct by reading only.
 *
 * What is *not* checked here is IOKit or AppKit. The transport, the pasteboard
 * and the Keychain stay behind `HelperEffects`, which a `Recorder` implements
 * by writing down what it was asked to do. That is the whole reason the
 * dispatch was split out of HelperRuntime, and it is why this runs on a laptop
 * with no device attached.
 *
 * The Windows twin is helpers/windows/tests/output_dispatch_test.cpp.
 */

/*
 * Every effect, written down in the order it was asked for.
 *
 * One list rather than a flag per effect, deliberately: the ordering is part
 * of what is being claimed — a refused frame must be counted *and* said out
 * loud, and a protocol error must be logged before the connection goes.
 */
private final class Recorder: HelperEffects {
    var effects: [String] = []

    /* What the platform answers back, for the arms that branch on it. */
    var storeSucceeds = true
    var transportTakes = true
    var frameBuilds = true
    var policyReply: [ClipboardOutput] = []

    func storeBoardKey(_ key: [UInt8]) -> Bool {
        effects.append("storeBoardKey(\(key.count))")
        return storeSucceeds
    }
    func acquireChannels() { effects.append("acquire") }
    func releaseChannels() { effects.append("release") }
    func send(_ frame: [UInt8]) -> Bool {
        effects.append("send(\(frame.count))")
        return transportTakes
    }
    func buildFrame(type: UInt8, body: [UInt8]) -> [UInt8]? {
        effects.append("build(\(type),\(body.count))")
        guard frameBuilds else { return nil }
        /* A frame is longer than its body; a distinct length is what lets the
           send below be checked against the *built* frame rather than the body
           that went into it. */
        return [UInt8](repeating: 0xAB, count: body.count + 4)
    }
    func noteSent() { effects.append("noteSent") }
    func noteSendRefused() { effects.append("noteSendRefused") }
    func deliver(text bytes: [UInt8]) {
        effects.append("deliver(\(String(decoding: bytes, as: UTF8.self)))")
    }
    func deliver(image bytes: [UInt8]) { effects.append("deliverImage(\(bytes.count))") }
    func lazyImage(id: UInt32, total: UInt64) { effects.append("lazyImage(\(id),\(total))") }
    func cancelLazyImage(id: UInt32) { effects.append("cancelLazyImage(\(id))") }
    func scheduleRetry(after: TimeInterval) { effects.append("retry(\(after))") }
    func clipPolicyChanged(flags: UInt8) -> [ClipboardOutput] {
        effects.append("clipPolicy(\(flags))")
        return policyReply
    }
    func note(_ message: String) { effects.append("note: " + message) }

    /* Logs are matched by prefix rather than in full: what is being claimed is
       that the right thing was said, not the exact wording of a sentence that
       will be reworded. */
    func noted(_ prefix: String) -> Bool {
        effects.contains { $0.hasPrefix("note: " + prefix) }
    }
    func did(_ effect: String) -> Bool { effects.contains(effect) }
    func indexOf(_ effect: String) -> Int? { effects.firstIndex(of: effect) }
    var wroteToThePasteboard: Bool { effects.contains { $0.hasPrefix("deliver(") } }
}

/*
 * The census: every output case, and the effect it must reach.
 *
 * Neither switch has a `default:`, which is the point of them. A case added to
 * `SessionOutput` or `ClipboardOutput` and forgotten stops this file
 * compiling, so `swift run channel-tests` fails rather than passing over a
 * shim that silently does nothing with it. The names are what the failures
 * below quote.
 */
private func effectNamed(by output: SessionOutput) -> String {
    switch output {
    case .storeBoardKey: return "the board key is stored"
    case .openChannels: return "the channels are acquired"
    case .closeChannels: return "the channels are released"
    case .send: return "the frame goes to the transport"
    case .state: return "the state is logged in this helper's own words"
    case .retry: return "a retry is scheduled"
    case .note: return "the note is logged"
    case .clipPolicy: return "the clipboard service is told"
    }
}

private func effectNamed(by output: ClipboardOutput) -> String {
    switch output {
    case .send: return "the body is built into a frame and sent"
    case .deliver: return "the payload reaches this computer's pasteboard"
    case .lazyImage: return "the lazy image reaches this computer's pasteboard"
    case .cancelLazyImage: return "the lazy image is removed from the pasteboard"
    case .note: return "the note is logged"
    case .protocolError: return "the connection is dropped"
    }
}

private func fixture() -> (Recorder, OutputDispatch) {
    let recorder = Recorder()
    return (recorder, OutputDispatch(effects: recorder))
}

// MARK: - session outputs

private func storeBoardKeyReachesTheKeychain() {
    let (recorder, dispatch) = fixture()
    let output = SessionOutput.storeBoardKey([UInt8](repeating: 0x11, count: 64))
    dispatch.apply(output)

    Check.that(recorder.did("storeBoardKey(64)"), effectNamed(by: output))
    Check.that(!recorder.noted("paired, but"), "a key that was stored says nothing about pairing")
}

/* A key that cannot be written is pairing that will not survive a restart, and
   silence there is how a helper that is a stranger again after a reboot looks
   like the board forgetting. */
private func aRefusedBoardKeyIsSaidOutLoud() {
    let (recorder, dispatch) = fixture()
    recorder.storeSucceeds = false
    dispatch.apply(.storeBoardKey([UInt8](repeating: 0x11, count: 64)))

    Check.that(recorder.noted("paired, but the board key could not be stored"),
               "a board key that could not be written is reported")
}

private func theChannelOutputsReachTheTransport() {
    let (opening, openDispatch) = fixture()
    openDispatch.apply(.openChannels)
    Check.that(opening.did("acquire"), effectNamed(by: .openChannels))

    let (closing, closeDispatch) = fixture()
    closeDispatch.apply(.closeChannels)
    Check.that(closing.did("release"), effectNamed(by: .closeChannels))
}

private func aSentFrameChargesTheIdleTimer() {
    let (recorder, dispatch) = fixture()
    let output = SessionOutput.send([UInt8](repeating: 0x22, count: 32))
    dispatch.apply(output)

    Check.that(recorder.did("send(32)"), effectNamed(by: output))
    Check.that(recorder.did("noteSent"), "a frame the transport took charges the idle timer")
    Check.that(!recorder.did("noteSendRefused"), "a frame that went out is not counted as refused")
}

/*
 * #107, in one test. Charging the idle timer for a frame the transport refused
 * buys a full interval of silence the helper has not earned, and the board
 * evicts after three of them.
 */
private func aRefusedFrameIsCountedAndSaidOutLoud() {
    let (recorder, dispatch) = fixture()
    recorder.transportTakes = false
    dispatch.apply(.send([UInt8](repeating: 0x22, count: 32)))

    Check.that(!recorder.did("noteSent"), "a refused frame does not charge the idle timer")
    Check.that(recorder.did("noteSendRefused"), "a refused frame is counted")
    Check.that(recorder.noted("a session frame was not taken by the transport"),
               "a refused frame is distinguishable from a quiet link")
}

private func aStateIsLoggedInWords() {
    let (recorder, dispatch) = fixture()
    let output = SessionOutput.state(.deviceAbsent)
    dispatch.apply(output)

    Check.that(recorder.noted("state: "), effectNamed(by: output))
    Check.that(!recorder.noted("state: (nothing to report)"),
               "a state with words does not read as the quiet one")
}

/* The quiet state shows nothing at all — a device that disappears for a moment
   is ordinary. The log still says a state arrived, so a helper stuck in it is
   not invisible in the one place a sitting can read. */
private func theQuietStateStillReachesTheLog() {
    let (recorder, dispatch) = fixture()
    dispatch.apply(.state(.quiet))

    Check.that(recorder.noted("state: (nothing to report)"),
               "the quiet state is logged as having nothing to report")
}

/* The board is the single source of truth for the policy, so the output has to
   reach the service that honours it — and whatever that service says in reply
   has to be carried out in the same pass. */
private func aClipPolicyReachesTheServiceAndItsReplyIsCarriedOut() {
    let (recorder, dispatch) = fixture()
    recorder.policyReply = [.note("the far end may no longer receive")]
    let output = SessionOutput.clipPolicy(flags: 0x01)
    dispatch.apply(output)

    Check.that(recorder.did("clipPolicy(1)"), effectNamed(by: output))
    Check.that(recorder.noted("the far end may no longer receive"),
               "what the clipboard service answers is carried out, not dropped")
}

private func aRetryIsHandedToTheRunLoop() {
    let (recorder, dispatch) = fixture()
    let output = SessionOutput.retry(after: 4)
    dispatch.apply(output)

    Check.that(recorder.did("retry(4.0)"), effectNamed(by: output))
}

private func aSessionNoteIsLogged() {
    let (recorder, dispatch) = fixture()
    let output = SessionOutput.note("the listener was detected 4 times in 10000ms")
    dispatch.apply(output)

    Check.that(recorder.noted("the listener was detected 4 times in 10000ms"),
               effectNamed(by: output))
}

/* A batch is carried out in order, and one output's effect does not swallow
   the next. */
private func aBatchOfSessionOutputsIsCarriedOutInOrder() {
    let (recorder, dispatch) = fixture()
    dispatch.apply([.note("first"), .closeChannels])

    Check.equal(recorder.indexOf("note: first"), 0, "the first output is carried out first")
    Check.that(recorder.did("release"), "the second output is carried out too")
}

// MARK: - clipboard outputs

private func aClipboardBodyIsBuiltIntoAFrameAndSent() {
    let (recorder, dispatch) = fixture()
    let output = ClipboardOutput.send(type: 0x40, body: [UInt8](repeating: 0x33, count: 16))
    dispatch.emit(output)

    Check.that(recorder.did("build(64,16)"), "the body goes through the session's counter space")
    Check.that(recorder.did("send(20)"),
               "the *built frame* is what reaches the transport, not the body")
    Check.that(recorder.did("noteSent"), effectNamed(by: output))
}

/* No session means no counter to send under. Nothing goes out, and nothing is
   charged for a beat that never happened. */
private func aClipboardFrameWithNoSessionIsDroppedLoudly() {
    let (recorder, dispatch) = fixture()
    recorder.frameBuilds = false
    dispatch.emit(.send(type: 0x40, body: [UInt8](repeating: 0x33, count: 16)))

    Check.that(recorder.noted("a clipboard frame could not be built"),
               "a frame with no session to carry it is reported")
    Check.that(!recorder.did("send(20)"), "nothing is handed to the transport")
    Check.that(!recorder.did("noteSent"), "the idle timer is not charged")
}

/* #132: dropped in silence, a frame the transport would not take is
   indistinguishable from one lost on the wire. */
private func aRefusedClipboardFrameIsCountedAndSaidOutLoud() {
    let (recorder, dispatch) = fixture()
    recorder.transportTakes = false
    dispatch.emit(.send(type: 0x40, body: [UInt8](repeating: 0x33, count: 16)))

    Check.that(!recorder.did("noteSent"),
               "a refused clipboard frame does not charge the idle timer")
    Check.that(recorder.did("noteSendRefused"), "a refused clipboard frame is counted")
    Check.that(recorder.noted("a clipboard frame of type 64 was not taken by the transport"),
               "the refusal names the message type")
}

private func aTextPayloadReachesThePasteboard() {
    let (recorder, dispatch) = fixture()
    let output = ClipboardOutput.deliver(kind: ClipKind.text.rawValue, bytes: Array("hello".utf8))
    dispatch.emit(output)

    Check.that(recorder.did("deliver(hello)"), effectNamed(by: output))
}

private func anImagePayloadReachesThePasteboard() {
    let (recorder, dispatch) = fixture()
    dispatch.emit(.deliver(kind: ClipKind.png.rawValue, bytes: [0x89, 0x50, 0x4e, 0x47]))
    Check.that(recorder.did("deliverImage(4)"), "the PNG reaches the pasteboard")
}

/* Files are #56. Until then an arriving one is named rather than written. */
private func aPayloadThisSliceCannotWriteIsNamed() {
    let (recorder, dispatch) = fixture()
    dispatch.emit(.deliver(kind: ClipKind.files.rawValue, bytes: [UInt8](repeating: 0x44, count: 8)))

    Check.that(recorder.noted("a payload of kind 2 arrived"), "the unwritable kind is named")
    Check.that(!recorder.wroteToThePasteboard, "nothing is written to the pasteboard")
}

private func aClipboardNoteIsLogged() {
    let (recorder, dispatch) = fixture()
    let output = ClipboardOutput.note("offer superseded by a newer copy")
    dispatch.emit(output)

    Check.that(recorder.noted("offer superseded by a newer copy"), effectNamed(by: output))
}

/*
 * #148/#149's teardown, which was the question this whole ticket came from: an
 * authenticated identity conflict drops the connection. Logged *before* the
 * release, so the reason survives in the log that the drop then explains.
 */
private func aProtocolErrorDropsTheConnection() {
    let (recorder, dispatch) = fixture()
    let output = ClipboardOutput.protocolError("a second helper claims this seal")
    dispatch.emit(output)

    Check.that(recorder.did("release"), effectNamed(by: output))
    Check.that(recorder.noted("clipboard protocol error: a second helper claims this seal"),
               "the conflict is named in the log")
    let said = recorder.indexOf("note: clipboard protocol error: a second helper claims this "
                                + "seal; dropping the connection")
    let released = recorder.indexOf("release")
    Check.that(said != nil && released != nil && said! < released!,
               "the reason is logged before the connection goes")
}

private func aBatchOfClipboardOutputsIsCarriedOutInOrder() {
    let (recorder, dispatch) = fixture()
    dispatch.emit([.note("first"), .protocolError("second")])

    Check.equal(recorder.indexOf("note: first"), 0, "the first output is carried out first")
    Check.that(recorder.did("release"), "the second output is carried out too")
}

let outputDispatchTests: [(String, () throws -> Void)] = [
    ("storeBoardKey reaches the Keychain", storeBoardKeyReachesTheKeychain),
    ("a refused board key is said out loud", aRefusedBoardKeyIsSaidOutLoud),
    ("the channel outputs reach the transport", theChannelOutputsReachTheTransport),
    ("a sent frame charges the idle timer", aSentFrameChargesTheIdleTimer),
    ("a refused frame is counted and said out loud", aRefusedFrameIsCountedAndSaidOutLoud),
    ("a state is logged in words", aStateIsLoggedInWords),
    ("the quiet state still reaches the log", theQuietStateStillReachesTheLog),
    ("a clip policy reaches the service and its reply is carried out",
     aClipPolicyReachesTheServiceAndItsReplyIsCarriedOut),
    ("a retry is handed to the run loop", aRetryIsHandedToTheRunLoop),
    ("a session note is logged", aSessionNoteIsLogged),
    ("a batch of session outputs is carried out in order",
     aBatchOfSessionOutputsIsCarriedOutInOrder),

    ("a clipboard body is built into a frame and sent", aClipboardBodyIsBuiltIntoAFrameAndSent),
    ("a clipboard frame with no session is dropped loudly",
     aClipboardFrameWithNoSessionIsDroppedLoudly),
    ("a refused clipboard frame is counted and said out loud",
     aRefusedClipboardFrameIsCountedAndSaidOutLoud),
    ("a text payload reaches the pasteboard", aTextPayloadReachesThePasteboard),
    ("an image payload reaches the pasteboard", anImagePayloadReachesThePasteboard),
    ("a payload this slice cannot write is named", aPayloadThisSliceCannotWriteIsNamed),
    ("a clipboard note is logged", aClipboardNoteIsLogged),
    ("a protocol error drops the connection", aProtocolErrorDropsTheConnection),
    ("a batch of clipboard outputs is carried out in order",
     aBatchOfClipboardOutputsIsCarriedOutInOrder),
]
