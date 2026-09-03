import DeskhopChannel
import Foundation

/*
 * Where files that arrive from the other computer are written (#56).
 *
 * Copying a file puts a *reference* on the clipboard, not its contents, so
 * pasting one that came over the link means there has to be a real file at a
 * real path to point at. This is that path — and because these are files
 * nobody asked to keep, it is a temporary directory that is swept when the
 * helper starts, rather than a folder in the user's own space that would slowly
 * fill up with things they never chose to save (#42, story 11).
 *
 * Emptied on **start** rather than on a timer or at exit, deliberately. A
 * helper that crashes never runs an exit path, and a timer would delete a file
 * the user is still working on.
 *
 * **The newest set survives that sweep**, and the reason is the crash. A file
 * reference on the pasteboard is a path, and a path outlives the process that
 * put it there — so a helper that crashed and was restarted by launchd,
 * seconds after files arrived, would delete the very files the user is about
 * to paste and leave a reference pointing at nothing. Keeping one set costs a
 * bounded amount of disk and is not accumulation: every run leaves at most one
 * behind.
 *
 * ---------------------------------------------------------------------------
 * NOTHING PARTIAL IS EVER LEFT BEHIND
 *
 * A transfer that fails delivers nothing at all — the transfer machine
 * discards an incomplete payload rather than handing it over — so this is only
 * ever called with every byte in hand. What can still fail is the writing, and
 * a set half-written to disk would put working references to truncated files
 * on the pasteboard. So a failure removes the whole directory and reports
 * nothing, which is the same rule one layer up.
 */
final class FileStore {
    /// Where a set of files landed, in the order they were named.
    struct Written {
        let directory: URL
        let urls: [URL]
    }

    var log: ((String) -> Void)?

    private let root: URL
    /// Distinguishes two sets that arrive inside the same second.
    private var sequence = 0

    init(root: URL = FileStore.defaultRoot) {
        self.root = root
    }

    static var defaultRoot: URL {
        URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
            .appendingPathComponent("deskhopplus", isDirectory: true)
    }

    /// Throw away what previous runs left, keeping only the newest set — see
    /// above for why that one is kept. Called once, at startup.
    func collectGarbage() {
        let manager = FileManager.default
        guard let names = try? manager.contentsOfDirectory(atPath: root.path) else { return }

        /* Newest by the counter in the name, which is what `write` builds it
           from: seconds since the epoch, then a per-run sequence. Sorting the
           names as numbers rather than as text keeps "10" after "9". */
        let newest = names.max { Self.ordering(of: $0) < Self.ordering(of: $1) }
        var removed = 0
        for name in names where name != newest {
            do {
                try manager.removeItem(at: root.appendingPathComponent(name))
                removed += 1
            } catch {
                log?("a set of files left by a previous run could not be removed: \(error)")
            }
        }
        if removed > 0 {
            log?("removed \(removed) set(s) of files left by a previous run; the newest was "
                 + "kept in case it is still on the pasteboard")
        }
    }

    /// The two numbers a set's directory name is built from, for ordering.
    private static func ordering(of name: String) -> (Int, Int) {
        let parts = name.split(separator: "-", maxSplits: 1)
        return (Int(parts.first ?? "") ?? 0, parts.count > 1 ? Int(parts[1]) ?? 0 : 0)
    }

    /*
     * Write a delivered set and return where each file landed.
     *
     * The names are the ones the shared core already accepted — it refuses any
     * that could name somewhere other than the directory they are joined to —
     * so nothing here re-examines them. What it does own is *collisions*:
     * cleaning can map two different names onto one, and two files copied from
     * different folders can share a name outright, and neither may quietly
     * overwrite the other.
     */
    func write(_ delivery: FileDelivery) -> Written? {
        sequence += 1
        let directory = root.appendingPathComponent(
            "\(Int(Date().timeIntervalSince1970))-\(sequence)", isDirectory: true)

        do {
            try FileManager.default.createDirectory(at: directory,
                                                    withIntermediateDirectories: true)
        } catch {
            log?("a directory for the arriving files could not be made: \(error)")
            return nil
        }

        var urls: [URL] = []
        var used: Set<String> = []
        var at = 0
        for file in delivery.files {
            let size = Int(file.size)
            guard at + size <= delivery.bytes.count else {
                log?("the arriving files did not add up to the payload; nothing was written")
                discard(directory)
                return nil
            }
            let name = FileNaming.unused(file.name, among: &used)
            let url = directory.appendingPathComponent(name)
            do {
                try Data(delivery.bytes[at..<(at + size)]).write(to: url, options: .atomic)
            } catch {
                log?("\(name) could not be written: \(error); the whole set was discarded")
                discard(directory)
                return nil
            }
            urls.append(url)
            at += size
        }

        guard !urls.isEmpty else {
            discard(directory)
            return nil
        }
        return Written(directory: directory, urls: urls)
    }

    private func discard(_ directory: URL) {
        try? FileManager.default.removeItem(at: directory)
    }
}
