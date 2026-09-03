import DeskhopChannel

/*
 * The bug these pin: a copy noticed before the copying application had
 * finished writing was recorded as seen and never looked at again (#132
 * follow-up). Mac→Windows copied inconsistently; Windows→Mac never did,
 * because that helper is told about a copy rather than polling for one.
 */
let copyWatchTests: [(String, () throws -> Void)] = [
    ("an unchanged count is ignored", {
        var w = CopyWatch(changeCount: 7)
        Check.equal(w.looked(at: 7, foundContent: true), .ignore, "a stale count was acted on")
    }),

    ("a readable copy is taken on the first look", {
        var w = CopyWatch(changeCount: 7)
        Check.equal(w.looked(at: 8, foundContent: true), .take(afterPolls: 1), "not taken")
        Check.equal(w.looked(at: 8, foundContent: true), .ignore, "taken twice")
    }),

    ("a copy caught mid-write is waited for, not dropped", {
        var w = CopyWatch(changeCount: 7)
        /* changeCount has moved but the application has not written yet. */
        Check.equal(w.looked(at: 8, foundContent: false), .wait, "an unfinished copy was dropped")
        Check.equal(w.looked(at: 8, foundContent: false), .wait, "gave up too early")
        Check.equal(w.looked(at: 8, foundContent: true), .take(afterPolls: 3),
                    "the copy was not taken once it became readable")
        Check.equal(w.settledAt, 8, "the change was not settled after being taken")
    }),

    ("waiting is bounded, so an image or a file is let go", {
        var w = CopyWatch(changeCount: 7)
        for poll in 1..<CopyWatch.readAttempts {
            Check.equal(w.looked(at: 8, foundContent: false), .wait, "gave up at poll \(poll)")
        }
        Check.equal(w.looked(at: 8, foundContent: false), .giveUp, "waited past the bound")
        Check.equal(w.looked(at: 8, foundContent: false), .ignore, "kept re-reading after giving up")
    }),

    ("a newer copy while waiting restarts the wait", {
        var w = CopyWatch(changeCount: 7)
        Check.equal(w.looked(at: 8, foundContent: false), .wait, "not waiting")
        Check.equal(w.looked(at: 8, foundContent: false), .wait, "not waiting")
        /* The user copied again. The new one gets the full budget, not what
           was left of the old one's. */
        Check.equal(w.looked(at: 9, foundContent: false), .wait, "the newer copy inherited the wait")
        Check.equal(w.looked(at: 9, foundContent: true), .take(afterPolls: 2), "not taken")
    }),

    ("this helper's own write is never read back as a local copy", {
        var w = CopyWatch(changeCount: 7)
        w.wrote(changeCount: 8)
        Check.equal(w.looked(at: 8, foundContent: true), .ignore,
                    "a delivered payload was offered straight back")
    }),

    ("a write while waiting cancels the wait", {
        var w = CopyWatch(changeCount: 7)
        Check.equal(w.looked(at: 8, foundContent: false), .wait, "not waiting")
        w.wrote(changeCount: 9)
        Check.equal(w.looked(at: 9, foundContent: true), .ignore, "the write was read back")
    }),
]
