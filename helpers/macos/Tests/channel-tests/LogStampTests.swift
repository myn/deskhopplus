import DeskhopChannel
import Foundation

/*
 * The log line's shape (#103).
 *
 * The helper's log is the verification surface until the menu bar exists
 * (#54), and until now every line carried order and nothing else. Two sittings
 * lost a duration to that, and one of them manufactured a wrong number rather
 * than merely missing one: file mtime against a later baseline suggested
 * config mode had run 2.5 minutes and contradicted a 300 s timeout that was
 * working correctly.
 *
 * So the format is pinned by test rather than left to whoever edits the
 * emitter next. Each check below is a way the log has been, or could be, read
 * back wrong.
 */

let logStampTests: [(String, () throws -> Void)] = [
    ("both clocks and the message", testBothClocksAndTheMessage),
    ("columns hold as elapsed grows", testColumnsHoldAsElapsedGrows),
    ("a long run is never truncated", testALongRunIsNeverTruncated),
    ("a run crossing midnight keeps its date", testARunCrossingMidnightKeepsItsDate),
    ("a backwards elapsed stays legible", testABackwardsElapsedStaysLegible),
]

/* UTC and a fixed instant, so the expected strings are the same on any
   machine. The helper itself stamps in local time, which is what a person at
   the desk cross-references against `system_profiler`. */
private let stamp = LogStamp(timeZone: TimeZone(identifier: "UTC")!)

/* 2026-08-17 21:04:07.001 UTC — the helper start that #107 is written about. */
private let helperStart = Date(timeIntervalSince1970: 1_787_000_647.001)

func testBothClocksAndTheMessage() throws {
    Check.equal(stamp.line("state: Connected and paired", wall: helperStart, elapsed: 0),
                "2026-08-17 21:04:07.001  +0.0s         deskhop-helper: state: Connected and paired",
                "the line's shape has changed")
}

/* The whole point of the elapsed column is scanning it for a gap. A column
   that moves per line cannot be scanned, only parsed. */
func testColumnsHoldAsElapsedGrows() throws {
    /* Past 16 h, deliberately. The agent runs under launchd with KeepAlive,
       so multi-day uptime is the ordinary case and not the edge — an earlier
       column width held alignment only to 27.8 h, which every sitting would
       have passed through without anyone choosing it. */
    let elapsed: [TimeInterval] = [0, 0.05, 98.34, 3600, 57602.94, 8 * 24 * 3600, 90 * 24 * 3600]
    let columns = elapsed.map { value -> Int in
        let line = stamp.line("x", wall: helperStart, elapsed: value)
        return line.distance(from: line.startIndex,
                             to: line.range(of: "deskhop-helper:")!.lowerBound)
    }

    Check.equal(Set(columns).count, 1,
                "the message column moves with the elapsed value: \(columns)")
}

/* Padding that truncated would lose the one number the line exists to carry,
   and would do it only on the longest runs — which are the ones nobody wants
   to repeat. #107's window was 16 hours. */
func testALongRunIsNeverTruncated() throws {
    let twoWeeks: TimeInterval = 14 * 24 * 3600
    let line = stamp.line("x", wall: helperStart, elapsed: twoWeeks)

    Check.that(line.contains("+1209600.0s"), "a long elapsed was truncated: \(line)")
}

/*
 * Caught on the first real run rather than here: the sign belongs to the
 * number, not to the column. Writing a literal "+" in front of a formatted
 * value renders a negative one as `+-0.0s`, which is neither readable nor
 * parseable.
 *
 * A negative elapsed is not hypothetical — it is what a lazily initialised
 * clock origin produces when the reading is taken before the origin is set.
 * Rendering it honestly is what made that visible in one line of output.
 */
func testABackwardsElapsedStaysLegible() throws {
    let line = stamp.line("x", wall: helperStart, elapsed: -0.04)

    Check.that(!line.contains("+-"), "a negative elapsed rendered as '+-': \(line)")
    Check.that(line.contains("-0.0s"), "the sign was lost: \(line)")
}

/* A 16-hour run crosses midnight, so a time-only stamp would put the small
   hours before the evening and read as time running backwards. */
func testARunCrossingMidnightKeepsItsDate() throws {
    let sixteenHours: TimeInterval = 16 * 3600
    let line = stamp.line("state: Device not connected",
                          wall: helperStart.addingTimeInterval(sixteenHours),
                          elapsed: sixteenHours)

    Check.that(line.hasPrefix("2026-08-18 13:04:07"),
               "the date did not roll over with the clock: \(line)")
}
