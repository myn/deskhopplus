import AppKit
import UniformTypeIdentifiers
import DeskhopChannel
import Foundation

/*
 * This computer's pasteboard: what was copied here, and what arrives from the
 * other computer (#52, #55, #56).
 *
 * Text, images and files are carried. A file copy is read as a *list* and not
 * as bytes — the contents are not touched until the other computer's user
 * accepts the transfer, which is the whole of #56 and the reason `onLocalFiles`
 * hands over a closure rather than a payload.
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
    /// Files copied on this computer, and how to read them when asked.
    struct LocalFiles {
        let entries: [FileListEntry]
        /// Every file's contents run together in `entries` order, or nil if any
        /// of them can no longer be read at the promised length. Nil rather
        /// than short: a file edited between the copy and the paste must fail
        /// the transfer, not truncate it.
        let read: () -> [UInt8]?
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
    /// Files copied here: what they are called and how long they are, plus the
    /// closure that reads them, which is called only if the transfer is
    /// accepted on the other computer.
    var onLocalFiles: ((LocalFiles) -> Void)?
    /// Every non-self pasteboard transition, before its formats are inspected.
    var onLocalReplacement: (() -> Void)?
    var log: ((String) -> Void)?

    var changeCount: Int { pasteboard.changeCount }

    private let pasteboard = NSPasteboard.general
    private var timer: Timer?
    private var replacementObservedAt: Int
    /*
     * Which changes have been accounted for, including the ones our own writes
     * produced: writing what arrived from the other computer otherwise looks
     * exactly like a fresh local copy, and the two helpers hand the same
     * payload back and forth for ever.
     */
    private var watch: CopyWatch

    init() {
        let count = pasteboard.changeCount
        watch = CopyWatch(changeCount: count)
        replacementObservedAt = count
    }

    func start() {
        /* `.common`, not the default mode `Timer.scheduledTimer` would give it:
           a menu open on screen must not stop the helper noticing a copy. See
           HelperRuntime.everyMode for what that cost (#161). */
        let timer = Timer(timeInterval: Self.pollInterval, repeats: true) { [weak self] _ in
            self?.poll()
        }
        RunLoop.main.add(timer, forMode: .common)
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

        if count != replacementObservedAt {
            replacementObservedAt = count
            onLocalReplacement?()
        }

        /*
         * Files, then image, then text.
         *
         * **Files before text**, because copying one in Finder also puts its
         * path on the pasteboard as a string, and reading text first would
         * send the path instead of the file.
         *
         * **An image before files only when the files *are* images.** A
         * screenshot tool writes its capture to a temporary file and puts both
         * on the pasteboard, and sending that as a file made it paste into
         * Explorer as a .png instead of into Paint as a picture — so an image
         * still wins there.
         *
         * But the old rule read the image first unconditionally, on the claim
         * that "image data and a file URL together" is the screenshot case and
         * nothing else. That claim is wrong: macOS synthesises an icon or
         * QuickLook preview for *any* file put on the pasteboard, so a copied
         * .bin sent its generic document icon — the same 201084 bytes every
         * time — in place of the file. It bit only single selections, because
         * a multiple selection has no one icon to synthesise, and only
         * sometimes, because the preview is generated asynchronously. That is
         * the whole of "sometimes it copies and sometimes it does not" (#56).
         */
        let files = filesFromPasteboard()
        let image = files == nil || allImages(files!) ? pngFromPasteboard() : nil
        let filesToSend = image == nil ? files : nil
        let text = image == nil && filesToSend == nil
            ? pasteboard.string(forType: .string).flatMap { $0.isEmpty ? nil : $0 } : nil
        guard case .take(let polls) = watch.looked(
            at: count, foundContent: filesToSend != nil || text != nil || image != nil)
        else { return }

        /* More than one means a copy was caught mid-write and waited for —
           the case that used to be dropped in silence. */
        if polls > 1 { log?("a copy took \(polls) polls to become readable") }
        if let image { onLocalImage?(Array(image)) }
        else if let filesToSend { onLocalFiles?(filesToSend) }
        else if let text { onLocalCopy?(text) }
    }

    /// Whether every copied file is itself a picture — the one case where an
    /// image representation on the pasteboard is the thing the user meant,
    /// rather than an icon macOS drew for something else.
    private func allImages(_ files: LocalFiles) -> Bool {
        files.entries.allSatisfy { entry in
            guard let type = UTType(filenameExtension: (entry.name as NSString).pathExtension)
            else { return false }
            return type.conforms(to: .image)
        }
    }

    /*
     * The files on the pasteboard, as a list and a way to read them later.
     *
     * Directories are skipped rather than walked. A folder is a tree, and the
     * offer's metadata is a flat list of names with no room for the paths
     * inside one — so carrying a folder would need a wire change, not a loop
     * here. Skipped visibly, because a copied folder that silently transfers
     * nothing is the kind of quiet failure #42 exists to avoid.
     */
    private func filesFromPasteboard() -> LocalFiles? {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [.urlReadingFileURLsOnly: true]
        guard let urls = pasteboard.readObjects(forClasses: [NSURL.self],
                                                options: options) as? [URL],
              !urls.isEmpty
        else { return nil }

        var entries: [FileListEntry] = []
        var readable: [(url: URL, size: UInt64)] = []
        var skipped = 0
        for url in urls {
            let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values?.isRegularFile == true, let size = values?.fileSize else {
                skipped += 1
                continue
            }
            entries.append(FileListEntry(name: url.lastPathComponent, size: UInt64(size)))
            readable.append((url, UInt64(size)))
        }
        if skipped > 0 {
            log?("\(skipped) copied item(s) were not ordinary files — a folder is not carried "
                 + "— and were left out")
        }
        guard !entries.isEmpty else { return nil }

        return LocalFiles(entries: entries) {
            var payload: [UInt8] = []
            for file in readable {
                /*
                 * Measured again, not assumed from the offer. A file that has
                 * *grown* since the copy would otherwise have its first bytes
                 * sent as the whole thing — a truncated file presented as
                 * complete, which is the one outcome #56 names as
                 * unacceptable. Reading the whole file and comparing catches
                 * that as well as a file that shrank.
                 */
                guard let bytes = try? Data(contentsOf: file.url),
                      UInt64(bytes.count) == file.size
                else { return nil }
                payload += bytes
            }
            return payload
        }
    }

    /*
     * Write file references for a set that arrived. The files themselves are
     * already on disk (FileStore) — this is only the reference the user pastes.
     *
     * Host-only, exactly as text and images are: a project whose premise is
     * removing the radio between these two computers would otherwise put every
     * received file straight back onto Universal Clipboard.
     */
    @discardableResult
    func deliver(files urls: [URL]) -> Bool {
        var delay = Self.firstRetryDelay
        for attempt in 1...Self.writeAttempts {
            pasteboard.prepareForNewContents(with: .currentHostOnly)
            if pasteboard.writeObjects(urls as [NSURL]) {
                watch.wrote(changeCount: pasteboard.changeCount)
                if attempt > 1 {
                    log?("the pasteboard took \(attempt) attempts to accept the files")
                }
                return true
            }
            if attempt < Self.writeAttempts {
                Thread.sleep(forTimeInterval: delay)
                delay *= 2
            }
        }
        log?("the pasteboard refused \(Self.writeAttempts) attempts to write \(urls.count) "
             + "file reference(s); the files are on disk but cannot be pasted")
        return false
    }

    private func pngFromPasteboard() -> Data? {
        if let png = pasteboard.data(forType: .png), !png.isEmpty { return png }
        guard let tiff = pasteboard.data(forType: .tiff),
              let bitmap = NSBitmapImageRep(data: tiff)
        else { return nil }
        return bitmap.representation(using: .png, properties: [:])
    }

    private func prepareForImageWrite(permittedChangeCount: inout Int?) -> Bool {
        let before = pasteboard.changeCount
        if let permittedChangeCount, before != permittedChangeCount { return false }
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        let after = pasteboard.changeCount
        if permittedChangeCount != nil,
           !ImagePrefetch.preparationWasExclusive(before: before, after: after) {
            log?("a prefetched image write overlapped a newer Mac copy; publication stopped")
            return false
        }
        if permittedChangeCount != nil { permittedChangeCount = after }
        return true
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

    @discardableResult
    func deliver(image bytes: [UInt8], ifUnchangedSince expected: Int? = nil) -> Bool {
        let data = Data(bytes)
        if let expected, pasteboard.changeCount != expected {
            log?("a prefetched image was discarded because a newer Mac copy exists")
            return false
        }
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
        var permittedChangeCount = expected
        for attempt in 1...Self.writeAttempts {
            guard prepareForImageWrite(permittedChangeCount: &permittedChangeCount) else {
                log?("a prefetched image was discarded because a newer Mac copy exists")
                return false
            }
            if pasteboard.setData(data, forType: .png) {
                watch.wrote(changeCount: pasteboard.changeCount)
                if attempt > 1 { log?("the pasteboard took \(attempt) attempts to accept an image") }
                return true
            }
            if attempt < Self.writeAttempts {
                Thread.sleep(forTimeInterval: delay)
                delay *= 2
            }
        }

        log?("the pasteboard refused \(Self.writeAttempts) attempts to write an image; "
             + "the content did not arrive")
        guard let displacedType, let displaced else { return false }
        guard prepareForImageWrite(permittedChangeCount: &permittedChangeCount) else { return false }
        if pasteboard.setData(displaced, forType: displacedType) {
            watch.wrote(changeCount: pasteboard.changeCount)
            log?("what was on the pasteboard before was put back")
        } else {
            log?("what was on the pasteboard before could not be put back; it is now empty")
        }
        return false
    }
}
