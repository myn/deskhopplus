// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Derek Reynolds

import DHCore
import Foundation

/*
 * The bundle a kind-3 offer carries (#195), as a binding rather than a codec
 * — `dh_bundle` is the codec, and it is shared for the reason every other
 * codec here is: two implementations of a format are two formats.
 *
 * `ClipBundle`, not `Bundle`: Foundation already owns that name.
 */
public enum ClipBundle {
    public struct Part: Equatable {
        public let kind: UInt8
        public let bytes: [UInt8]

        public init(kind: UInt8, bytes: [UInt8]) {
            self.kind = kind
            self.bytes = bytes
        }
    }

    /// The payload for these parts, in order. A part's kind is a `ClipKind`
    /// raw value — the wire reuses the offer kinds. Nil when the list is
    /// empty or longer than the core holds.
    public static func pack(_ parts: [Part]) -> [UInt8]? {
        guard !parts.isEmpty, parts.count <= Int(DH_BUNDLE_PARTS_MAX) else { return nil }
        /*
         * Every part's bytes in one contiguous buffer, with the offsets beside
         * it. `dh_bundle_part` holds a pointer into whatever it is given, so
         * the bytes must stay at one address for the length of the call — the
         * same shape FileList.encode uses, for the same reason.
         */
        var all: [UInt8] = []
        var spans: [(at: Int, count: Int)] = []
        for part in parts {
            guard part.bytes.count <= Int(UInt32.max) else { return nil }
            spans.append((all.count, part.bytes.count))
            all += part.bytes
        }
        return all.withUnsafeBufferPointer { source -> [UInt8]? in
            /* An empty part list is refused above, but every part may still be
               empty: `baseAddress` is then nil, and a part of no bytes points
               at nothing without being read. */
            let base = source.baseAddress
            var raw: [dh_bundle_part] = []
            for (index, span) in spans.enumerated() {
                raw.append(dh_bundle_part(kind: parts[index].kind,
                                          bytes: base.map { $0 + span.at },
                                          len: UInt32(span.count)))
            }
            var out = [UInt8](repeating: 0, count: dh_bundle_packed_len(raw, UInt8(raw.count)))
            let written = out.withUnsafeMutableBufferPointer { buffer in
                raw.withUnsafeBufferPointer { list in
                    dh_bundle_pack(list.baseAddress, UInt8(list.count),
                                   buffer.baseAddress, buffer.count)
                }
            }
            return written > 0 ? out : nil
        }
    }

    /// The parts this helper writes, from a payload that arrived. Nil when a
    /// part runs past the payload or nothing in it is a kind this helper
    /// writes; a part of an unknown kind is skipped, not refused.
    public static func unpack(_ payload: [UInt8]) -> [Part]? {
        var bundle = dh_bundle()
        let ok = payload.withUnsafeBufferPointer { buffer -> Bool in
            dh_bundle_unpack(buffer.baseAddress, buffer.count, &bundle)
        }
        guard ok else { return nil }

        var parts: [Part] = []
        withUnsafeBytes(of: &bundle.parts) { raw in
            let base = raw.baseAddress!.assumingMemoryBound(to: dh_bundle_part.self)
            for index in 0..<Int(bundle.count) {
                let part = base[index]
                let bytes = part.bytes.map { Array(UnsafeBufferPointer(start: $0, count: Int(part.len))) }
                parts.append(Part(kind: part.kind, bytes: bytes ?? []))
            }
        }
        return parts
    }
}
