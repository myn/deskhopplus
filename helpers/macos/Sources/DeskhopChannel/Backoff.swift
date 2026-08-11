import Foundation

/*
 * Reconnection delay: exponential, capped at a few seconds. The cap is what
 * matters — a config-mode round trip can take five minutes, and a helper that
 * had backed off to minutes would leave the user staring at a dead menu bar
 * long after the device came back.
 */
public struct Backoff {
    public let first: TimeInterval
    public let cap: TimeInterval
    private var current: TimeInterval

    public init(first: TimeInterval = 0.25, cap: TimeInterval = 4.0) {
        self.first = first
        self.cap = cap
        self.current = first
    }

    /* The delay to wait before the next attempt, doubling until the cap. */
    public mutating func next() -> TimeInterval {
        let delay = current
        current = min(current * 2, cap)
        return delay
    }

    /* Called on a working session — the next failure starts short again. */
    public mutating func reset() { current = first }
}
