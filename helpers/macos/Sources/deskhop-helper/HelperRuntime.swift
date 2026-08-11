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
    private let started = ProcessInfo.processInfo.systemUptime

    /// The engine's tick — fine enough that a heartbeat is never late by much.
    private static let tickInterval: TimeInterval = 0.25

    /*
     * Monotonic, deliberately. `Date()` is not: a backwards clock correction
     * of more than a couple of seconds — routine on a laptop coming out of
     * sleep — would stall the heartbeat past the device's three-second
     * timeout and kill a healthy session.
     */
    private var now: TimeInterval { ProcessInfo.processInfo.systemUptime - started }

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

    private static func note(_ message: String) {
        FileHandle.standardError.write(Data("deskhop-helper: \(message)\n".utf8))
    }
}
