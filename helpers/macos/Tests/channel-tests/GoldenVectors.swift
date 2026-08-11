import Foundation

/*
 * test-vectors/frames.txt, read from the repository rather than copied into
 * the test target. A copy would be a second definition of the wire format,
 * which is the one thing the vector file exists to prevent.
 */
enum GoldenVectors {
    struct Missing: Error { let name: String }

    static let repositoryRoot: URL = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // channel-tests
        .deletingLastPathComponent()  // Tests
        .deletingLastPathComponent()  // macos
        .deletingLastPathComponent()  // helpers
        .deletingLastPathComponent()  // repository root

    static let path = repositoryRoot.appendingPathComponent("test-vectors/frames.txt")

    /// Every vector in file order: name, then bytes.
    static func load() throws -> [(name: String, bytes: [UInt8])] {
        let text = try String(contentsOf: path, encoding: .utf8)
        return text.split(separator: "\n").compactMap { line in
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty, !trimmed.hasPrefix("#") else { return nil }
            let parts = trimmed.split(separator: "|", maxSplits: 1)
            guard parts.count == 2 else { return nil }

            let name = parts[0].trimmingCharacters(in: .whitespaces)
            let hex = parts[1].filter { !$0.isWhitespace }
            var bytes: [UInt8] = []
            var index = hex.startIndex
            while index < hex.endIndex {
                guard let next = hex.index(index, offsetBy: 2, limitedBy: hex.endIndex),
                      let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
                bytes.append(byte)
                index = next
            }
            return (name, bytes)
        }
    }

    static func named(_ name: String) throws -> [UInt8] {
        guard let match = try load().first(where: { $0.name == name }) else {
            throw Missing(name: name)
        }
        return match.bytes
    }
}
