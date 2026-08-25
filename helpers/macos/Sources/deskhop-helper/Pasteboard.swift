import AppKit
import DeskhopChannel
import Foundation

/*
 * This computer's pasteboard: what was copied here, and what arrives from the
 * other computer (#52).
 *
 * Only text in this slice — images are #55, files are #56.
 *
 * Two things here are not obvious and are both requirements rather than
 * choices.
 *
 * **Polling.** macOS has no pasteboard-change notification. `changeCount` is
 * the only signal there is, so a timer is not laziness; it is the API. A fifth
 * of a second is what mkroamer settled on and is below the threshold at which
 * a copy-then-switch feels like it did not happen.
 *
 * **Host-only writes.** Received content is written with
 * `NSPasteboardContentsCurrentHostOnly`, so it does not travel onward over
 * Universal Clipboard. A project whose premise is removing the radio between
 * these two computers would otherwise put the clipboard straight back on one
 * (spec #42, "Clipboard behaviour"). This is the whole of that guarantee, and
 * it is one flag — which is exactly why it is worth a paragraph: it is
 * invisible when it is missing.
 */
final class Pasteboard {
    /// macOS has no change notification; `changeCount` is the only signal.
    static let pollInterval: TimeInterval = 0.2

    /*
     * Writing can fail while another application holds the pasteboard. Retried
     * with a widening delay rather than once: the applications that hold it do
     * so for a moment, and a single failed write loses the payload silently
     * after it has already crossed the link.
     */
    private static let writeAttempts = 5
    private static let firstRetryDelay: TimeInterval = 0.01

    /// Text copied on this computer, already filtered for this helper's own
    /// writes.
    var onLocalCopy: ((String) -> Void)?
    var log: ((String) -> Void)?

    private let pasteboard = NSPasteboard.general
    private var timer: Timer?
    private var lastChangeCount: Int
    /*
     * The change count our own write produced. Without it, writing what
     * arrived from the other computer looks exactly like a fresh local copy,
     * and the two helpers hand the same payload back and forth for ever.
     */
    private var selfChangeCount = -1

    init() {
        lastChangeCount = pasteboard.changeCount
    }

    func start() {
        let timer = Timer.scheduledTimer(withTimeInterval: Self.pollInterval, repeats: true) {
            [weak self] _ in
            self?.poll()
        }
        self.timer = timer
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func poll() {
        let count = pasteboard.changeCount
        guard count != lastChangeCount else { return }
        lastChangeCount = count
        guard count != selfChangeCount else { return }
        guard let string = pasteboard.string(forType: .string), !string.isEmpty else { return }
        onLocalCopy?(string)
    }

    /*
     * Write what arrived from the other computer.
     *
     * The bytes are handed to the platform exactly as they arrived: ADR-0003
     * makes this channel fidelity-preserving, so the only transform anywhere is
     * the pasteboard's own encoding conversion at this edge. Malformed UTF-8
     * converts best-effort with the OS default and is not rejected — a
     * validator here is the thing that ADR explicitly declines to add.
     */
    func deliver(text bytes: [UInt8]) {
        let string = String(decoding: bytes, as: UTF8.self)

        var delay = Self.firstRetryDelay
        for attempt in 1...Self.writeAttempts {
            /* Host-only: this is what keeps received content off Universal
               Clipboard. `prepareForNewContents` clears the pasteboard as
               `clearContents` does, and the option persists until the next
               call — so it must be this call, not a later one. */
            pasteboard.prepareForNewContents(with: .currentHostOnly)
            if pasteboard.setString(string, forType: .string) {
                /* Read back rather than assumed. The change count our own write
                   produced is what stops it being read as a fresh local copy on
                   the next poll. */
                selfChangeCount = pasteboard.changeCount
                lastChangeCount = selfChangeCount
                if attempt > 1 { log?("the pasteboard took \(attempt) attempts to accept a write") }
                return
            }
            if attempt < Self.writeAttempts {
                Thread.sleep(forTimeInterval: delay)
                delay *= 2
            }
        }
        log?("the pasteboard refused \(Self.writeAttempts) attempts to write \(bytes.count) "
             + "bytes; the content did not arrive")
    }
}
