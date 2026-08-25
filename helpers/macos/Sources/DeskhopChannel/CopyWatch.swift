/*
 * When a pasteboard poll has actually seen a copy (#132 follow-up).
 *
 * macOS has no copy notification, so the helper polls `changeCount`. That
 * count is bumped by `clearContents` / `prepareForNewContents` — *before* a
 * single byte is written — so a poll can land inside a copy that has not
 * finished yet and read nothing at all.
 *
 * Recording the count on that first look, as this used to, meant nothing ever
 * looked again: the copy was dropped in silence, and the user saw a copy that
 * sometimes crossed and sometimes did not. Only this direction had the
 * problem. The Windows helper is *told* about a copy and reads a finished one.
 *
 * So a change is settled when its contents have been read, not when they have
 * been noticed. The wait is bounded, because an unreadable pasteboard is also
 * what a copy this slice does not carry looks like — an image (#55) or a file
 * (#56) — and those must not be re-read for ever.
 *
 * No AppKit here on purpose: this is the part worth testing, and NSPasteboard
 * is not reachable from the host tests.
 */
public struct CopyWatch {
    public enum Step: Equatable {
        /// Nothing new since the last settled change.
        case ignore
        /// Seen, not readable yet. Look again next poll.
        case wait
        /// Readable. `afterPolls` is how many polls it took; 1 is the ordinary
        /// case, and more than 1 means a copy was caught mid-write.
        case take(afterPolls: Int)
        /// Waited long enough. Not something this slice sends.
        case giveUp
    }

    /// Polls to wait for contents before letting a change go. Five at the
    /// helper's 0.2s interval is one second.
    public static let readAttempts = 5

    private var settled: Int
    private var pending: Int?
    private var polls = 0

    public init(changeCount: Int) { settled = changeCount }

    /// The last change accounted for. The caller checks this before reading the
    /// pasteboard at all, because macOS notices pasteboard reads and there is
    /// no reason to make one every poll.
    public var settledAt: Int { settled }

    /// Account for a write this helper made itself. Without it, writing what
    /// arrived from the other computer reads back as a fresh local copy and the
    /// two helpers hand the same payload back and forth for ever.
    public mutating func wrote(changeCount: Int) { settle(changeCount) }

    /// One poll. `foundText` is whether the pasteboard yielded usable text.
    public mutating func looked(at changeCount: Int, foundText: Bool) -> Step {
        guard changeCount != settled else { return .ignore }
        if pending != changeCount {
            pending = changeCount
            polls = 0
        }
        polls += 1

        if foundText {
            let waited = polls
            settle(changeCount)
            return .take(afterPolls: waited)
        }
        guard polls >= Self.readAttempts else { return .wait }
        settle(changeCount)
        return .giveUp
    }

    private mutating func settle(_ changeCount: Int) {
        settled = changeCount
        pending = nil
        polls = 0
    }
}
