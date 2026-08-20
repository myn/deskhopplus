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
            entropy: { count in
                var bytes = [UInt8](repeating: 0, count: count)
                guard SecRandomCopyBytes(kSecRandomDefault, count, &bytes) == errSecSuccess else {
                    fatalError("SecRandomCopyBytes failed")
                }
                return bytes
            })
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
        transport.start()

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
            transport.send(bytes)

        case .state(let state):
            Self.note("state: \(state.message ?? "(nothing to report)")")

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
