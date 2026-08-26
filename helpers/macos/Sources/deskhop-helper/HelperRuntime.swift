import DeskhopChannel
import Foundation
import Security

/*
 * The loop: transport events into the session, its outputs back out to the
 * transport. Nothing here is decided here — every decision is the shared C
 * core's, reached through HelperSession. This file carries messages, owns the
 * clock, and turns an output into a log line.
 */
final class HelperRuntime {
    private let secrets = SecretStore()
    private let session: HelperSession
    private let transport = ChannelTransport()
    private let clipboard: ClipboardService
    private let pasteboard = Pasteboard()

    /*
     * Whether the last thing the session said was that bulk may cross. The
     * clipboard has to be told when a session *ends* — its seal and any
     * transfer go with it — and the session reports a state rather than an
     * event, so the transition is worked out here.
     */
    private var bulkWasAllowed = false

    init() {
        /*
         * A blob that will not decode is regenerated inside `loadIdentity`, so
         * reaching this is the machine having no Secure Enclave to generate a
         * key in at all — an Intel Mac without a T2. That is a hard
         * requirement of ADR-0008 rather than a fault to recover from, so it
         * names itself and stops instead of retrying something that cannot
         * work. launchd will restart it; the log line says why each time.
         */
        let identity: EnclaveIdentity
        do {
            identity = try secrets.loadIdentity()
        } catch {
            Self.note("no Secure Enclave identity, and one could not be created: \(error). "
                      + "This helper requires a Mac with a Secure Enclave.")
            exit(EXIT_FAILURE)
        }
        /*
         * The key id every hello carries, and the value the board's config page
         * shows as *Paired helper* (#114) — same byte order, same spelling, so
         * the two can be compared by eye. Without it the page answers "what is
         * paired to this board?" with a number nothing else in the system ever
         * prints.
         */
        Self.note("helper key id: " + identity.keyId.map { String(format: "%02x", $0) }.joined())

        let boardKey = secrets.loadBoardKey()

        session = HelperSession(
            identity: identity, boardPublicKey: boardKey,
            entropy: Self.entropy)
        /* The seal draws ephemeral keys and nonces from the same source the
           session's correlation values come from. A short draw would key a
           seal on bytes nobody chose, which `Self.entropy` refuses to do. */
        clipboard = ClipboardService(entropy: Self.entropy)
    }

    private static func entropy(_ count: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: count)
        guard SecRandomCopyBytes(kSecRandomDefault, count, &bytes) == errSecSuccess else {
            fatalError("SecRandomCopyBytes failed")
        }
        return bytes
    }

    private static let started = ProcessInfo.processInfo.systemUptime
    private static let stamp = LogStamp()

    /// The session's tick — fine enough that a heartbeat is never late by much.
    static let tickInterval: TimeInterval = 0.25

    /*
     * Monotonic, deliberately. `Date()` is not: a backwards clock correction
     * of more than a couple of seconds — routine on a laptop coming out of
     * sleep — would stall the heartbeat past the device's three-second
     * timeout and kill a healthy session. The log prints this beside the wall
     * clock rather than instead of it (#103); LogStamp says why both.
     */
    private static var elapsed: TimeInterval {
        /* The origin is read *first*, deliberately. `started` is a lazy static,
           so the very first reading initialises it — and had this been written
           as one subtraction, Swift would evaluate the left operand before
           triggering that initialisation, making the first log line of every
           run a negative elapsed. It did, and the first real run showed it. */
        let origin = started
        return ProcessInfo.processInfo.systemUptime - origin
    }

    /// The session's clock, and the same origin the log's elapsed column counts from.
    private var now: TimeInterval { Self.elapsed }

    func run() {
        transport.log = { message in Self.note(message) }
        transport.onEvent = { [weak self] event in self?.feed(event) }

        /* Verified bulk frames, straight from the core. Nothing here re-reads
           the stream: decode, tag and replay counter are all upstream of this. */
        session.onPayload = { [weak self] type, body in
            guard let self else { return }
            self.emit(self.clipboard.received(type: type, body: body))
        }

        pasteboard.log = { message in Self.note(message) }
        pasteboard.onLocalCopy = { [weak self] text in
            guard let self else { return }
            /* Nothing is offered without a session to carry it. The state the
               user is shown and this answer come from the same core, so a
               helper that says "connected" and refuses a copy is not a state
               this can reach. */
            guard self.session.canSendBulk else { return }
            self.emit(self.clipboard.localCopy(kind: .text, bytes: Array(text.utf8)))
        }

        transport.start()
        pasteboard.start()

        Timer.scheduledTimer(withTimeInterval: Self.tickInterval, repeats: true) { [weak self] _ in
            self?.feed(.tick)
        }

        Self.note("deskhop helper started; waiting for the channel")
        RunLoop.current.run()
    }

    private func feed(_ input: SessionInput) {
        for output in session.handle(input, at: now) {
            apply(output)
        }

        /*
         * A session that has gone takes the seal and any transfer with it.
         *
         * Asked of `canSendBulk` — the *session's* answer — and on every input,
         * not of the state the user is shown and not only when that state
         * changes. `dh_helper_allows_bulk` counts `reconnectingRepeatedly` as
         * allowing bulk, and that is precisely the state a teardown lands in
         * once the flap rate has tripped: the session is gone, its keys are
         * cleared, and the state reads `true` before and after. Worse, the core
         * reports that state only on the transition, so the second and
         * subsequent drops of a burst produce no state output at all. #107
         * measured 586 teardowns in sixteen hours — the exact condition that
         * trips the rate — so the edge that matters is the one this misses.
         */
        let live = session.canSendBulk
        if bulkWasAllowed && !live {
            emit(clipboard.sessionEnded())
        }
        bulkWasAllowed = live

        /* A chance to push the next credit-gated batch. On the tick as well as
           on arriving frames, so a transfer whose last credit grant was lost
           still finishes rather than sitting still. */
        if live {
            emit(clipboard.pump())
        }
        /* And a chance to give up on one that has stopped moving — the far
           helper having crashed leaves this end's session perfectly healthy,
           so nothing else here would ever notice. */
        if case .tick = input {
            /* The board's drop totals go with the tick so that an abandonment
               can quote them (#133). Read here rather than held there: the
               board restates them whenever they move, and nothing tells the
               clipboard when that was. */
            emit(clipboard.tick(at: now, boardDrops: session.boardDrops))
        }
    }

    /*
     * The clipboard's outputs: frames to authenticate and send, payloads to
     * write, and diagnostics.
     *
     * Every frame goes out through `session.emit`, never with a counter of this
     * file's own — the counter space belongs to the session key and the
     * heartbeat is already writing into it. `noteSent` is what keeps ADR-0004's
     * beat out of a direction that is far from idle.
     */
    private func emit(_ outputs: [ClipboardOutput]) {
        for output in outputs {
            switch output {
            case .send(let type, let body):
                guard let frame = session.emit(type: type, body: body) else {
                    Self.note("a clipboard frame could not be built; there is no session")
                    continue
                }
                /* The idle timer is charged only for a frame the transport
                   actually took. Charging for one it refused would suppress a
                   beat that ADR-0004 owed the board — which is exactly what
                   `HelperSession.emit` says not to do.

                   A refusal is said out loud (#132): dropped here in silence,
                   a frame the transport would not take is indistinguishable
                   from one lost on the wire, and the two have nothing in
                   common to fix. */
                if transport.send(frame) {
                    session.noteSent(at: now)
                } else {
                    Self.note("a clipboard frame of type \(type) was not taken by the "
                              + "transport and is lost")
                }

            case .deliver(let kind, let bytes):
                guard kind == ClipKind.text.rawValue else {
                    Self.note("a payload of kind \(kind) arrived, which this slice does not "
                              + "write — images are #55 and files are #56")
                    continue
                }
                pasteboard.deliver(text: bytes)

            case .note(let note):
                Self.note(note)
            }
        }
    }

    private func apply(_ output: SessionOutput) {
        switch output {
        case .storeBoardKey(let key):
            if !secrets.saveBoardKey(key) {
                Self.note("paired, but the board key could not be stored — pairing "
                          + "will not survive a restart")
            }

        case .openChannels:
            transport.acquire()

        case .closeChannels:
            transport.release()

        case .send(let bytes):
            /* The same rule as a clipboard frame below, and #107 is what it
               cost to have it in only one of the two places: the idle timer is
               charged for what the transport actually took. A beat charged for
               one it refused bought a full interval of silence, and three of
               those has the board evict this helper. Said out loud too — a
               refusal here used to be indistinguishable from a healthy quiet
               link. */
            if transport.send(bytes) {
                session.noteSent(at: now)
            } else {
                Self.note("a session frame was not taken by the transport and is lost")
            }

        case .state(let state):
            Self.note("state: \(state.message ?? "(nothing to report)")")

        case .clipPolicy(let flags):
            emit(clipboard.policyChanged(flags: flags))

        case .retry(let after):
            DispatchQueue.main.asyncAfter(deadline: .now() + after) { [weak self] in
                guard let self, self.transport.hasDevice, !self.transport.isHoldingChannels else {
                    return
                }
                self.transport.acquire()
            }

        case .note(let note):
            Self.note(note)
        }
    }

    private static func note(_ message: String) {
        let line = stamp.line(message, wall: Date(), elapsed: elapsed)
        FileHandle.standardError.write(Data((line + "\n").utf8))
    }
}
