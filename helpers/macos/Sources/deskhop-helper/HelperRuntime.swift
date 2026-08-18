import DeskhopChannel
import Foundation

/*
 * The loop: transport events into the session engine, engine outputs back out
 * to the transport. Everything decided here is decided in SessionEngine; this
 * file only carries messages and owns the clock.
 */
final class HelperRuntime {
    private let secrets = SecretStore()
    private lazy var engine = SessionEngine(secret: secrets.load())
    private let transport = ChannelTransport()

    /*
     * Process-wide rather than per-instance, so the engine's clock and the
     * log's elapsed column are the same number: a duration read off the log is
     * the duration the engine saw, with no correspondence to establish. Set on
     * first use, which is inside `run()` and microseconds into the process.
     */
    private static let started = ProcessInfo.processInfo.systemUptime
    private static let stamp = LogStamp()

    /// The engine's tick — fine enough that a heartbeat is never late by much.
    private static let tickInterval: TimeInterval = 0.25

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

    /// The engine's clock, and the same origin the log's elapsed column counts from.
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
        for output in engine.handle(input, at: now) {
            apply(output)
        }
    }

    private func apply(_ output: SessionOutput) {
        switch output {
        case .storeSecret(let secret):
            /* The only local state the helper keeps; everything else is the
               device's. See SecretStore for why this is a file. */
            if !secrets.save(secret) {
                Self.note("paired, but the secret could not be stored — pairing "
                          + "will not survive a restart")
            }

        case .openChannels:
            transport.acquire()

        case .closeChannels:
            transport.release()

        case .send(let bytes):
            transport.send(bytes)

        case .state(let state):
            /* Until the menu-bar item exists (#54), the log is the surface —
               and it is the same sentence the menu bar will carry. */
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

    /* Stamped with both clocks — see LogStamp for why a log with neither cost
       two sittings a duration each (#103). */
    private static func note(_ message: String) {
        let line = stamp.line(message, wall: Date(), elapsed: elapsed)
        FileHandle.standardError.write(Data((line + "\n").utf8))
    }
}
