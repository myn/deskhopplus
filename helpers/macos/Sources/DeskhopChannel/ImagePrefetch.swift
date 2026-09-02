/*
 * Paste-side ownership of a large image while its bytes are prefetched.
 *
 * No AppKit here: the decision is whether a completed image may replace the
 * pasteboard version that existed when its offer was accepted. The adapter
 * supplies changeCount and performs the guarded write.
 */
public struct ImagePrefetch {
    public enum Completion: Equatable {
        case ordinary
        case publish(ifUnchangedSince: Int)
    }

    private var id: UInt32?
    private var changeCount = 0

    public init() {}

    @discardableResult
    public mutating func begin(id: UInt32, changeCount: Int) -> UInt32 {
        self.id = id
        self.changeCount = changeCount
        return id
    }

    public mutating func localReplacement() -> UInt32? {
        defer { id = nil }
        return id
    }

    public mutating func cancel(id: UInt32) {
        if self.id == id { self.id = nil }
    }

    public mutating func complete() -> Completion {
        guard id != nil else { return .ordinary }
        id = nil
        return .publish(ifUnchangedSince: changeCount)
    }

    /* prepareForNewContents normally advances changeCount once. A larger jump
       means another writer overlapped our non-atomic check/prepare pair. */
    public static func preparationWasExclusive(before: Int, after: Int) -> Bool {
        after == before &+ 1
    }
}
