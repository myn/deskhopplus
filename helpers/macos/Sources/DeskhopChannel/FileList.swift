import DHCore
import Foundation

/*
 * The file list a kind-2 offer carries (#56), as a binding rather than a
 * parser — `dh_file_list` is the parser, and it is shared for the reason
 * every other codec here is: two implementations of a format are two formats.
 *
 * The rule that matters is not the JSON. It is that a name arriving from the
 * other computer becomes a path in this one's temporary directory, and the
 * core refuses one that could leave it. Nothing on this side re-checks that,
 * and nothing on this side may build a path from a name this did not return.
 */

/// One file in a transfer: what it is called, and how many bytes of the
/// payload belong to it.
public struct FileListEntry: Equatable {
    public let name: String
    public let size: UInt64

    public init(name: String, size: UInt64) {
        self.name = name
        self.size = size
    }
}

public enum FileList {
    /// The metadata for these files, with every name cleaned on the way out.
    /// Nil when the list is empty, longer than the core holds, or would not
    /// fit one CLIP_OFFER — which is the real limit, and the one a long list
    /// of long names reaches first.
    public static func encode(_ entries: [FileListEntry]) -> [UInt8]? {
        guard !entries.isEmpty, entries.count <= Int(DH_FILE_LIST_MAX) else { return nil }

        /*
         * Every name in one contiguous buffer, with the offsets beside it.
         * `dh_file_entry` holds a pointer into whatever it is given, so the
         * bytes must stay at one address for the length of the call — and a
         * pointer taken from a per-name `withUnsafeBufferPointer` is only
         * valid inside that call, which is exactly the mistake this shape
         * removes rather than documents.
         */
        var names: [UInt8] = []
        var spans: [(at: Int, count: Int)] = []
        for entry in entries {
            let bytes = Array(entry.name.utf8)
            guard bytes.count <= Int(UInt16.max) else { return nil }
            spans.append((names.count, bytes.count))
            names += bytes
        }

        var out = [UInt8](repeating: 0, count: dh_file_list_encode_max())
        let written: Int32 = names.withUnsafeBufferPointer { source in
            guard let base = source.baseAddress else { return -1 }
            let chars = UnsafeRawPointer(base).assumingMemoryBound(to: CChar.self)
            var raw: [dh_file_entry] = []
            raw.reserveCapacity(entries.count)
            for (index, span) in spans.enumerated() {
                raw.append(dh_file_entry(name: chars + span.at,
                                         name_len: UInt16(span.count),
                                         size: entries[index].size))
            }
            return out.withUnsafeMutableBufferPointer { buffer in
                raw.withUnsafeBufferPointer { list in
                    buffer.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buffer.count) {
                        dh_file_list_encode(list.baseAddress, UInt16(list.count), $0, buffer.count)
                    }
                }
            }
        }
        guard written > 0 else { return nil }
        return Array(out.prefix(Int(written)))
    }

    /// The files named by metadata that arrived, and the total the core summed
    /// them to. Nil when the metadata is malformed, when any name in it could
    /// name somewhere other than the directory it is about to be written into,
    /// or when the sizes overflow their total.
    ///
    /// The total comes back rather than being re-summed by the caller: the
    /// core has already added these up once, with the overflow check, and a
    /// second sum somewhere else is a second answer waiting to disagree.
    public static func decode(_ meta: [UInt8]) -> (files: [FileListEntry], total: UInt64)? {
        var list = dh_file_list()
        let ok = meta.withUnsafeBufferPointer { buffer -> Bool in
            guard let base = buffer.baseAddress else { return false }
            return base.withMemoryRebound(to: CChar.self, capacity: buffer.count) {
                dh_file_list_decode($0, buffer.count, &list)
            }
        }
        guard ok else { return nil }

        var entries: [FileListEntry] = []
        entries.reserveCapacity(Int(list.count))
        withUnsafeBytes(of: &list.entries) { raw in
            let base = raw.baseAddress!.assumingMemoryBound(to: dh_file_entry.self)
            for index in 0..<Int(list.count) {
                let entry = base[index]
                guard let name = entry.name else { continue }
                let bytes = UnsafeBufferPointer(start: name, count: Int(entry.name_len))
                entries.append(FileListEntry(
                    name: String(decoding: bytes.map { UInt8(bitPattern: $0) }, as: UTF8.self),
                    size: entry.size))
            }
        }
        guard entries.count == Int(list.count) else { return nil }
        return (entries, list.total)
    }
}

/*
 * What a file is called once it is on this computer's disk (#56).
 *
 * The core already guarantees a name is safe to join to a directory path. What
 * it cannot guarantee is that two names in one delivery are *different*:
 * cleaning maps several spellings onto one, and two files copied from
 * different folders can share a name outright. Either way the second must not
 * quietly overwrite the first, so it is renamed rather than dropped — a
 * transfer that says it delivered three files has to have delivered three.
 *
 * Here rather than beside the filesystem call, because it is a rule and not a
 * platform: both helpers follow it and a test can drive it.
 */
public enum FileNaming {
    /// `name`, or the first suffixed form of it that `used` has not seen. The
    /// suffix goes before the extension, so the file still opens in whatever
    /// the user expects.
    public static func unused(_ name: String, among used: inout Set<String>) -> String {
        if used.insert(name).inserted { return name }

        let dot = name.lastIndex(of: ".")
        /* A leading dot is a hidden file, not an extension: `.gitignore`
           suffixed as `-2.gitignore` would be a different kind of file. */
        let stem = dot.map { $0 == name.startIndex ? name : String(name[name.startIndex..<$0]) }
            ?? name
        let ext = dot.flatMap { $0 == name.startIndex ? nil : String(name[name.index(after: $0)...]) }

        for attempt in 2...(Int(DH_FILE_LIST_MAX) + 1) {
            let candidate = ext.map { "\(stem)-\(attempt).\($0)" } ?? "\(stem)-\(attempt)"
            if used.insert(candidate).inserted { return candidate }
        }
        /* Unreachable: a list holds at most DH_FILE_LIST_MAX names, so that
           many attempts cannot all collide. Named rather than left to fall
           through, because falling through would return the name unchanged and
           overwrite the file it collided with. Spelled exactly as
           `unused_file_name` spells it — an unreachable branch that differs
           between the twins is still two answers to one question. */
        let last = ext.map { "\(stem)-\(used.count + 1).\($0)" } ?? "\(stem)-\(used.count + 1)"
        used.insert(last)
        return last
    }
}
