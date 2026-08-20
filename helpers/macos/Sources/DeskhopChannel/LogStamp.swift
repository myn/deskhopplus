import Foundation

/*
 * What every helper log line is prefixed with (#103).
 *
 * The log is the verification surface until the menu-bar item exists (#54),
 * and it used to carry order and nothing else — so any acceptance criterion
 * phrased as a duration became unanswerable the moment the sitting ended. Two
 * of them were: #88's config-mode timeout could only be bounded, and #92's
 * first criterion is a duration that had to be watched on a wall clock at the
 * desk because the log could not corroborate it.
 *
 * # Why two clocks
 *
 * Neither one alone is enough on a laptop.
 *
 * The **wall clock** is what a person cross-references: `system_profiler`
 * output, a USB identity flip, the time they wrote in a ticket. It is also the
 * one that lies — macOS corrects it backwards coming out of sleep, which is
 * why `HelperRuntime` refuses to use it for session timing at all.
 *
 * The **elapsed** count comes from `ProcessInfo.systemUptime`, which measures
 * time awake since boot: no clock correction can move it, and it does not
 * advance while the Mac sleeps. So a duration subtracted from it is real
 * awake-time. It cannot be cross-referenced against anything outside this
 * process.
 *
 * Printing both costs a column and buys a third thing neither gives alone:
 * the wall clock counts slept time and the elapsed column does not, so when
 * the two disagree across a pair of lines, *the machine slept*. A quiet
 * stretch in the log then reads as a sleeping Mac rather than as a device that
 * went quiet — which are opposite conclusions, and the difference between them
 * is live on #107.
 *
 * # Threading
 *
 * `DateFormatter` is not safe to share across threads, and this holds one. It
 * is fine here because the helper is single-threaded by construction: IOKit is
 * scheduled on the main run loop (`ChannelTransport`), the session tick is a
 * `Timer` on that same loop, and the only hop is `DispatchQueue.main`. Anything
 * logging from another thread needs its own instance.
 *
 * A class, so that "its own instance" means what it says. As a struct this
 * looked value-typed while its only stored property was a reference: copying
 * it into another thread would have produced two stamps sharing one formatter,
 * which is the race above, entered by someone who believed they had avoided
 * it. The sharing is now visible in the type.
 */
public final class LogStamp {
    /* Fixed pattern, POSIX locale: a log that renders differently on a machine
       set to another region is a log two sittings cannot be compared across. */
    private let clock: DateFormatter

    /*
     * Twelve holds alignment through `+99999999.9s` — about three years of
     * awake time, comfortably past any uptime this will see. Four of those
     * characters are spent on the sign, the point, the decimal and the `s`,
     * so the width buys eight integer digits and not twelve.
     *
     * Nine was the first attempt and was wrong by a factor of ten: it aligned
     * only to `+99999.9s`, or 27.8 hours. The agent runs under launchd with
     * KeepAlive, so every run passes that mark and the column would have
     * started shifting on the second day — silently, and precisely on the
     * long runs whose gaps are the reason to scan the column at all.
     *
     * Longer still pushes the column rather than losing digits: a ragged log
     * beats an unreadable duration.
     */
    private static let elapsedColumn = 12

    public init(timeZone: TimeZone = .current) {
        let clock = DateFormatter()
        clock.locale = Locale(identifier: "en_US_POSIX")
        clock.calendar = Calendar(identifier: .gregorian)
        clock.timeZone = timeZone
        /* The full date, not just the time: the run this was written for was 16
           hours long, and a time-only stamp puts its small hours before its
           evening. */
        clock.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        self.clock = clock
    }

    /// One log line, without its newline: `<wall>  <+elapsed>  deskhop-helper: <message>`.
    public func line(_ message: String, wall: Date, elapsed: TimeInterval) -> String {
        /* `%+` rather than a literal "+": the sign belongs to the number. A
           negative elapsed means the clock origin was read after the reading
           it is subtracted from, and that must look wrong rather than look
           like a plus. */
        let since = String(format: "%+.1fs", elapsed)
        let padding = String(repeating: " ", count: max(0, Self.elapsedColumn - since.count))

        return "\(clock.string(from: wall))  \(since)\(padding)  deskhop-helper: \(message)"
    }
}
