import AppKit
import DeskhopChannel
import Foundation

/*
 * This computer's pasteboard: what was copied here, and what arrives from the
 * other computer (#52).
 *
 * Text and images are carried; files are #56.
 *
 * Two things here are not obvious and are both requirements rather than
 * choices.
 *
 * **Polling.** macOS has no pasteboard-change notification. `changeCount` is
 * the only signal there is, so a timer is not laziness; it is the API. A fifth
 * of a second is what mkroamer settled on and is below the threshold at which
 * a copy-then-switch feels like it did not happen.
 *
 * When a poll has actually *seen* a copy is subtler than it looks, because the
 * count moves before the bytes do — `CopyWatch` in the channel module owns
 * that, and says why.
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
    private final class ImageProvider: NSObject, NSPasteboardItemDataProvider {
        let id: UInt32
        let total: UInt64
        let request: (UInt32) -> Void
        let timedOut: (UInt32) -> Void
        var data: Data?
        init(id: UInt32, total: UInt64, request: @escaping (UInt32) -> Void,
             timedOut: @escaping (UInt32) -> Void) {
            self.id = id
            self.total = total
            self.request = request
            self.timedOut = timedOut
        }
        func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem,
                        provideDataForType type: NSPasteboard.PasteboardType) {
            request(id)
            let transferSeconds = Double(total) / (49 * 1024)
            let deadline = Date().addingTimeInterval(max(30, transferSeconds + 30))
            while data == nil && Date() < deadline {
                RunLoop.current.run(until: min(deadline, Date().addingTimeInterval(0.05)))
            }
            if let data { item.setData(data, forType: type) }
            else { timedOut(id) }
        }
    }
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
    var onLocalImage: (([UInt8]) -> Void)?
    var onLazyImageReplaced: ((UInt32) -> Void)?
    var log: ((String) -> Void)?

    private let pasteboard = NSPasteboard.general
    private var timer: Timer?
    private var imageProvider: ImageProvider?
    /*
     * Which changes have been accounted for, including the ones our own writes
     * produced: writing what arrived from the other computer otherwise looks
     * exactly like a fresh local copy, and the two helpers hand the same
     * payload back and forth for ever.
     */
    private var watch: CopyWatch

    init() {
        watch = CopyWatch(changeCount: pasteboard.changeCount)
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
        /* Read only when something has changed. macOS notices pasteboard
           reads, so there is no reason to make one five times a second. */
        guard count != watch.settledAt else { return }

        if let provider = imageProvider {
            imageProvider = nil
            onLazyImageReplaced?(provider.id)
        }

        let text = pasteboard.string(forType: .string).flatMap { $0.isEmpty ? nil : $0 }
        let image = pngFromPasteboard()
        guard case .take(let polls) = watch.looked(at: count,
                                                   foundText: text != nil || image != nil)
        else { return }

        /* More than one means a copy was caught mid-write and waited for —
           the case that used to be dropped in silence. */
        if polls > 1 { log?("a copy took \(polls) polls to become readable") }
        if let image { onLocalImage?(Array(image)) }
        else if let text { onLocalCopy?(text) }
    }

    private func pngFromPasteboard() -> Data? {
        if let png = pasteboard.data(forType: .png), !png.isEmpty { return png }
        guard let tiff = pasteboard.data(forType: .tiff),
              let bitmap = NSBitmapImageRep(data: tiff)
        else { return nil }
        return bitmap.representation(using: .png, properties: [:])
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

        /*
         * What is on the pasteboard now, kept so that a write which never
         * succeeds can put it back.
         *
         * `prepareForNewContents` *clears* — that is how the host-only option
         * is set — so the first failed attempt has already destroyed whatever
         * the user had copied. Without this they would be left with an empty
         * clipboard and a log line, having lost both the content that was
         * arriving and the content they already had. Losing one of the two is
         * the most this can cost.
         */
        let displaced = pasteboard.string(forType: .string)

        var delay = Self.firstRetryDelay
        for attempt in 1...Self.writeAttempts {
            /* Host-only: this is what keeps received content off Universal
               Clipboard. The option persists until the next
               `prepareForNewContents` or `clearContents`, so it must be set by
               the call that writes, not by an earlier one. */
            pasteboard.prepareForNewContents(with: .currentHostOnly)
            if pasteboard.setString(string, forType: .string) {
                /* Read back rather than assumed. The change count our own write
                   produced is what stops it being read as a fresh local copy on
                   the next poll. */
                watch.wrote(changeCount: pasteboard.changeCount)
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

        /* Put back what was displaced. Host-only again: it may itself have
           arrived from the other computer, and re-writing it as ordinary
           content would put it on Universal Clipboard after all. */
        guard let displaced else { return }
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        if pasteboard.setString(displaced, forType: .string) {
            watch.wrote(changeCount: pasteboard.changeCount)
            log?("what was on the pasteboard before was put back")
        } else {
            log?("what was on the pasteboard before could not be put back; it is now empty")
        }
    }

    func deliver(image bytes: [UInt8]) {
        let data = Data(bytes)
        if let imageProvider, pasteboard.changeCount == watch.settledAt {
            imageProvider.data = data
            self.imageProvider = nil
            return
        }
        imageProvider = nil
        let displacedType: NSPasteboard.PasteboardType?
        let displaced: Data?
        if let png = pasteboard.data(forType: .png) {
            displacedType = .png
            displaced = png
        } else if let tiff = pasteboard.data(forType: .tiff) {
            displacedType = .tiff
            displaced = tiff
        } else {
            displacedType = nil
            displaced = nil
        }

        var delay = Self.firstRetryDelay
        for attempt in 1...Self.writeAttempts {
            pasteboard.prepareForNewContents(with: .currentHostOnly)
            if pasteboard.setData(data, forType: .png) {
                watch.wrote(changeCount: pasteboard.changeCount)
                if attempt > 1 { log?("the pasteboard took \(attempt) attempts to accept an image") }
                return
            }
            if attempt < Self.writeAttempts {
                Thread.sleep(forTimeInterval: delay)
                delay *= 2
            }
        }

        log?("the pasteboard refused \(Self.writeAttempts) attempts to write an image; "
             + "the content did not arrive")
        guard let displacedType, let displaced else { return }
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        if pasteboard.setData(displaced, forType: displacedType) {
            watch.wrote(changeCount: pasteboard.changeCount)
            log?("what was on the pasteboard before was put back")
        } else {
            log?("what was on the pasteboard before could not be put back; it is now empty")
        }
    }

    func lazyImage(id: UInt32, total: UInt64, request: @escaping (UInt32) -> Void) {
        let provider = ImageProvider(id: id, total: total, request: request) { [weak self] id in
            self?.log?("lazy image \(id) did not arrive before the paste timed out")
        }
        let item = NSPasteboardItem()
        item.setDataProvider(provider, forTypes: [.png])
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        if pasteboard.writeObjects([item]) {
            imageProvider = provider
            watch.wrote(changeCount: pasteboard.changeCount)
        } else {
            log?("the pasteboard refused a lazy image of \(total) bytes")
        }
    }

    func cancelLazyImage(id: UInt32) {
        guard imageProvider?.id == id else { return }
        imageProvider = nil
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        watch.wrote(changeCount: pasteboard.changeCount)
        log?("lazy image \(id) was removed before it could be pasted")
    }
}
