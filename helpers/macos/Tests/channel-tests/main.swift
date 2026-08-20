import DeskhopChannel
import Foundation

/*
 * The macOS helper's host tests. See Package.swift for why this is an
 * executable and not a .testTarget.
 *
 *   swift run channel-tests
 */

enum Check {
    static var failures = 0

    static func that(_ condition: Bool, _ what: String,
                     file: StaticString = #fileID, line: UInt = #line) {
        guard !condition else { return }
        failures += 1
        print("FAIL \(file):\(line) \(what)")
    }

    static func equal<T: Equatable>(_ actual: T, _ expected: T, _ what: String,
                                    file: StaticString = #fileID, line: UInt = #line) {
        guard actual != expected else { return }
        failures += 1
        print("FAIL \(file):\(line) \(what)\n  expected: \(expected)\n  actual:   \(actual)")
    }

    static func unequal<T: Equatable>(_ actual: T, _ unwanted: T, _ what: String,
                                      file: StaticString = #fileID, line: UInt = #line) {
        guard actual == unwanted else { return }
        failures += 1
        print("FAIL \(file):\(line) \(what) (was \(unwanted))")
    }

    /// The call must throw, and throw this error.
    static func throwsError<T>(_ expected: ChannelError, _ what: String,
                               file: StaticString = #fileID, line: UInt = #line,
                               _ body: () throws -> T) {
        do {
            _ = try body()
            failures += 1
            print("FAIL \(file):\(line) \(what) — nothing was thrown")
        } catch {
            guard (error as? ChannelError) != expected else { return }
            failures += 1
            print("FAIL \(file):\(line) \(what) — threw \(error), wanted \(expected)")
        }
    }

    static func doesNotThrow<T>(_ what: String, file: StaticString = #fileID, line: UInt = #line,
                                _ body: () throws -> T) -> T? {
        do {
            return try body()
        } catch {
            failures += 1
            print("FAIL \(file):\(line) \(what) — threw \(error)")
            return nil
        }
    }
}

let suites: [(String, () throws -> Void)] = bindingTests + helperSessionTests + logStampTests

for (name, body) in suites {
    do {
        try body()
    } catch {
        Check.failures += 1
        print("FAIL \(name) — threw \(error)")
    }
}

if Check.failures > 0 {
    print("\(Check.failures) helper check(s) failed across \(suites.count) tests")
    exit(1)
}
print("\(suites.count) helper tests passed")
