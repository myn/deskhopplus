import Foundation

/*
 * What each output *does* — the shim's two switches, with the platform behind
 * a protocol (#152).
 *
 * Both services below it are covered in depth: `HelperSession` binds the
 * shared C core, `ClipboardService` joins the seal to the transfer, and both
 * have their own suites. The layer that turns each of their outputs into a
 * real effect had none, so every arm was correct by reading only — a service
 * could be proved to emit the right output while the shim dropped it, called
 * the wrong thing, or fell through. That is the shape of #93 and #94: a helper
 * that reports healthy while doing nothing.
 *
 * No IOKit and no AppKit here, which is why this file lives in DeskhopChannel
 * rather than beside HelperRuntime. The transport, the pasteboard and the
 * Keychain stay in the executable target behind `HelperEffects`, and this file
 * only decides which of them an output reaches.
 *
 * The Windows twin is output_dispatch.h/.cpp, and the two are deliberately the
 * same shape — a divergence between them is a clipboard that works on one
 * computer and not the other. One arm is not the same, and by decision: the
 * State arm there checks that the wording knows the state (#119) and drives a
 * tray. The same check happens here one layer up — `HelperSession.convert`
 * turns a state `HelperState` has no case for into a `.note` before it becomes
 * an output — so by the time a `.state` arrives it is already worded; and this
 * helper has no tray.
 *
 * Single-threaded by construction, like the rest of the helper.
 */

/*
 * Every effect an output can have, named once.
 *
 * HelperRuntime implements each of these in a line or two over the real object
 * — the IOKit transport, the pasteboard, the Keychain — and a test implements
 * them by writing down what it was asked to do.
 */
public protocol HelperEffects: AnyObject {
    /// The Keychain. False when the key could not be written.
    func storeBoardKey(_ key: [UInt8]) -> Bool

    /* The transport. `send` is false when it would not take the frame, which
       is a different reading from a quiet link (#107, #132). */
    func acquireChannels()
    func releaseChannels()
    func send(_ frame: [UInt8]) -> Bool

    /* The session. The counter space belongs to the session key, so a frame is
       built there and never here. */
    func buildFrame(type: UInt8, body: [UInt8]) -> [UInt8]?
    func noteSent()
    func noteSendRefused()

    /*
     * The menu bar. Its Windows twin has had this since #85; macOS had nowhere
     * to put a state until #56 gave it one, so the state output was logged and
     * nothing else. Logged *as well*, because the log is still where a fault is
     * read back from afterwards.
     */
    func show(state: HelperState)

    /// This computer's pasteboard.
    func deliver(text: [UInt8])
    func deliver(image: [UInt8])
    func lazyImage(id: UInt32, total: UInt64)
    func cancelLazyImage(id: UInt32)
    /* Files (#56). `askAboutFiles` puts the acceptance to the user: nothing has
       crossed the link yet, and nothing will until the user answers. */
    func askAboutFiles(_ offer: FileOffer)
    func withdrawFileQuestion(id: UInt32)
    func deliver(files: FileDelivery)

    /// The run loop's retry timer, and the conditions it re-checks when it fires.
    func scheduleRetry(after: TimeInterval)

    /* The clipboard service. The board is the single source of truth for the
       policy and the size cap, so a direction turning off — or a cap moving —
       has to reach the service that honours it (#52, #56). */
    func clipPolicyChanged(flags: UInt8, capMegabytes: UInt8) -> [ClipboardOutput]

    func note(_ message: String)
}

public final class OutputDispatch {
    /// Unowned: HelperRuntime owns this object, not the other way round, and a
    /// strong reference back would be a cycle. `effects` must outlive it.
    private unowned let effects: HelperEffects

    public init(effects: HelperEffects) { self.effects = effects }

    /* Neither switch below has a `default:`, deliberately. An output case added
       to a service and forgotten here is then a compile error rather than a
       silent fall-through, so `swift run channel-tests` fails rather than
       passing over a shim that does nothing. */
    public func apply(_ outputs: [SessionOutput]) {
        for output in outputs { apply(output) }
    }

    public func apply(_ output: SessionOutput) {
        switch output {
        case .storeBoardKey(let key):
            if !effects.storeBoardKey(key) {
                effects.note("paired, but the board key could not be stored — pairing "
                             + "will not survive a restart")
            }

        case .openChannels:
            effects.acquireChannels()

        case .closeChannels:
            effects.releaseChannels()

        case .send(let bytes):
            /* The same rule as a clipboard frame below, and #107 is what it
               cost to have it in only one of the two places: the idle timer is
               charged for what the transport actually took. A beat charged for
               one it refused bought a full interval of silence, and three of
               those has the board evict this helper. Said out loud too — a
               refusal here used to be indistinguishable from a healthy quiet
               link. */
            if effects.send(bytes) {
                effects.noteSent()
            } else {
                effects.noteSendRefused()
                effects.note("a session frame was not taken by the transport and is lost")
            }

        case .state(let state):
            effects.note("state: \(state.message ?? "(nothing to report)")")
            effects.show(state: state)

        case .clipPolicy(let flags, let capMegabytes):
            emit(effects.clipPolicyChanged(flags: flags, capMegabytes: capMegabytes))

        case .retry(let after):
            effects.scheduleRetry(after: after)

        case .note(let note):
            effects.note(note)
        }
    }

    /*
     * The clipboard's outputs: frames to authenticate and send, payloads to
     * write, and diagnostics.
     *
     * Every frame goes out through `buildFrame` — HelperSession.emit — never
     * with a counter of this layer's own, because the counter space belongs to
     * the session key and the heartbeat is already writing into it. `noteSent`
     * is what keeps ADR-0004's beat out of a direction that is far from idle.
     */
    public func emit(_ outputs: [ClipboardOutput]) {
        for output in outputs { emit(output) }
    }

    public func emit(_ output: ClipboardOutput) {
        switch output {
        case .send(let type, let body):
            guard let frame = effects.buildFrame(type: type, body: body) else {
                effects.note("a clipboard frame could not be built; there is no session")
                return
            }
            /* The idle timer is charged only for a frame the transport
               actually took. Charging for one it refused would suppress a
               beat that ADR-0004 owed the board — which is exactly what
               `HelperSession.emit` says not to do.

               A refusal is said out loud (#132): dropped here in silence, a
               frame the transport would not take is indistinguishable from
               one lost on the wire, and the two have nothing in common to
               fix. */
            if effects.send(frame) {
                effects.noteSent()
            } else {
                effects.noteSendRefused()
                effects.note("a clipboard frame of type \(type) was not taken by the "
                             + "transport and is lost")
            }

        case .deliver(let kind, let bytes):
            if kind == ClipKind.text.rawValue {
                effects.deliver(text: bytes)
            } else if kind == ClipKind.png.rawValue {
                effects.deliver(image: bytes)
            } else {
                effects.note("a payload of kind \(kind) arrived, which this helper does not "
                             + "write")
            }

        case .lazyImage(let id, let total):
            effects.lazyImage(id: id, total: total)
        case .cancelLazyImage(let id):
            effects.cancelLazyImage(id: id)

        case .fileOffer(let offer):
            effects.askAboutFiles(offer)
        case .fileOfferWithdrawn(let id):
            effects.withdrawFileQuestion(id: id)
        case .deliverFiles(let delivery):
            effects.deliver(files: delivery)

        case .note(let note):
            effects.note(note)

        case .protocolError(let note):
            effects.note("clipboard protocol error: \(note); dropping the connection")
            effects.releaseChannels()
        }
    }
}
