// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Derek Reynolds

import Foundation

/*
 * The read of one lazy file, at the moment the paste side asks for it (#182,
 * ADR-0011 amendment).
 *
 * The offer's total is the length that will be sent. This reads exactly that
 * many bytes. A file that grew since the copy gives its first offered-length
 * bytes. A file that shrank cannot give them, so the read fails. The size now
 * is returned with the bytes, so the caller can log how much the file grew.
 *
 * A failed open or read carries the Foundation error code (#181). The caller
 * logs it with the file's name and the step, so the log names the cause: a
 * locked file, a cloud placeholder or a missing file.
 *
 * In the channel library and not the helper for the reason `FileNaming` is:
 * a test can reach it here with a real file and no pasteboard.
 */
public enum FileRead {
    /// The bytes that were read, and how long the file is now.
    public struct Read: Equatable {
        public let bytes: [UInt8]
        public let sizeNow: UInt64
    }

    /// Why the offered length could not be read.
    public enum Failure: Error, Equatable {
        /// The file is `sizeNow` bytes, fewer than were offered.
        case shrank(sizeNow: UInt64)
        case openFailed(error: Int)
        case readFailed(error: Int)
    }

    /// The first `offered` bytes of the file at `url`, as they are now.
    public static func read(_ url: URL, offered: UInt64) -> Result<Read, Failure> {
        let handle: FileHandle
        do {
            handle = try FileHandle(forReadingFrom: url)
        } catch {
            return .failure(.openFailed(error: (error as NSError).code))
        }
        defer { try? handle.close() }
        do {
            let sizeNow = try handle.seekToEnd()
            try handle.seek(toOffset: 0)
            let bytes = try handle.read(upToCount: Int(offered)) ?? Data()
            guard UInt64(bytes.count) == offered else {
                return .failure(.shrank(sizeNow: sizeNow))
            }
            return .success(Read(bytes: [UInt8](bytes), sizeNow: sizeNow))
        } catch {
            return .failure(.readFailed(error: (error as NSError).code))
        }
    }
}
