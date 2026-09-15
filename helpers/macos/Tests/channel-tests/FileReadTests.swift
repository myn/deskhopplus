import DeskhopChannel
import Foundation

/*
 * The read of one lazy file at the offered length (#182, #181).
 *
 * Each test writes a real temporary file. It offers the file at one length,
 * changes the file, and then checks only what the read gives back. No
 * pasteboard and no service: the rule is the read function's alone.
 */

let fileReadTests: [(String, () throws -> Void)] = [
    ("a file that grew is sent as its first offered bytes", testAGrownFileIsSentAtTheOfferedLength),
    ("a file that shrank fails and says how long it is now", testAShrunkFileFailsWithItsSizeNow),
    ("a missing file names the open step and the error", testAMissingFileFailsAtOpen),
    ("an empty file offered at zero bytes is sent", testAnEmptyFileIsSent),
]

/// A fresh temporary file that holds `bytes`.
private func temporaryFile(_ bytes: [UInt8]) throws -> URL {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("deskhop-file-read-\(UUID().uuidString).bin")
    try Data(bytes).write(to: url)
    return url
}

private func testAGrownFileIsSentAtTheOfferedLength() throws {
    let offered: [UInt8] = Array("first sixteen by".utf8)
    let url = try temporaryFile(offered)
    defer { try? FileManager.default.removeItem(at: url) }

    let handle = try FileHandle(forWritingTo: url)
    try handle.seekToEnd()
    try handle.write(contentsOf: Data("tes and then some more".utf8))
    try handle.close()

    switch FileRead.read(url, offered: 16) {
    case .success(let read):
        Check.equal(read.bytes, offered, "the first offered-length bytes are what is sent")
        Check.equal(read.sizeNow, 38, "the size now is reported so the log can say how much it grew")
    case .failure(let why):
        Check.that(false, "a grown file was refused: \(why)")
    }
}

private func testAShrunkFileFailsWithItsSizeNow() throws {
    let url = try temporaryFile(Array("first sixteen by".utf8))
    defer { try? FileManager.default.removeItem(at: url) }

    let handle = try FileHandle(forWritingTo: url)
    try handle.truncate(atOffset: 5)
    try handle.close()

    Check.equal(FileRead.read(url, offered: 16), .failure(.shrank(sizeNow: 5)),
                "a file shorter than offered fails and carries its size now")
}

private func testAMissingFileFailsAtOpen() throws {
    let url = try temporaryFile([])
    try FileManager.default.removeItem(at: url)

    Check.equal(FileRead.read(url, offered: 16),
                .failure(.openFailed(error: NSFileNoSuchFileError)),
                "a missing file fails at open with the Foundation error")
}

/// A size-0 file is a real offer. Foundation gives nil, not empty, for a
/// zero-byte read, and that must not read as a file that shrank.
private func testAnEmptyFileIsSent() throws {
    let url = try temporaryFile([])
    defer { try? FileManager.default.removeItem(at: url) }

    switch FileRead.read(url, offered: 0) {
    case .success(let read):
        Check.equal(read.bytes, [], "an empty file is sent as no bytes")
        Check.equal(read.sizeNow, 0, "an empty file is 0 bytes now")
    case .failure(let why):
        Check.that(false, "an empty file was refused: \(why)")
    }
}
