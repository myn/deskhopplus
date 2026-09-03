import DeskhopChannel
import Foundation

/*
 * The Swift side of the file list (#56).
 *
 * The format itself is `tests/file_list_test.c`'s — this is the binding, and
 * what it can get wrong is different: a pointer into a buffer that has already
 * gone, a length in the wrong units, a name decoded as the wrong bytes. So
 * these drive round trips and awkward names rather than restating the wire
 * layout a second time.
 *
 * `FileNaming` is here rather than beside the filesystem call because it is a
 * rule both helpers follow, and because the case it exists for — two files in
 * one delivery that want the same name — is the one nobody produces by
 * accident while testing by hand.
 */

let fileListTests: [(String, () throws -> Void)] = [
    ("a list of files survives the round trip", testAListRoundTrips),
    ("awkward names are cleaned rather than refused", testAwkwardNamesAreCleaned),
    ("metadata that will not decode is refused", testBadMetadataIsRefused),
    ("a list too wordy for one offer is refused", testAnOversizeListIsRefused),
    ("colliding names are renamed, never overwritten", testCollidingNamesAreRenamed),
]

private func testAListRoundTrips() {
    let files = [
        FileListEntry(name: "notes.txt", size: 1234),
        FileListEntry(name: "empty.bin", size: 0),
        FileListEntry(name: "Ünïcödé näme.png", size: 4_294_967_296),
    ]
    guard let meta = FileList.encode(files) else {
        Check.that(false, "a three-file list would not encode")
        return
    }
    guard let back = FileList.decode(meta) else {
        Check.that(false, "the encoded list would not decode")
        return
    }
    Check.equal(back.files, files, "the list did not survive the round trip")
    Check.equal(back.total, 1234 + 4_294_967_296, "the core's total is not the sum of the list")
}

private func testAwkwardNamesAreCleaned() {
    let cases: [(given: String, expected: String)] = [
        ("reports/2026/q3.pdf", "reports_2026_q3.pdf"),
        ("he said \"hi\".txt", "he said _hi_.txt"),
        ("..", "file"),
        ("", "file"),
        ("trailing dots...", "trailing dots"),
        ("NUL", "_NUL"),
    ]
    for (given, expected) in cases {
        guard let meta = FileList.encode([FileListEntry(name: given, size: 1)]),
              let back = FileList.decode(meta)
        else {
            Check.that(false, "\"\(given)\" would not round trip")
            continue
        }
        Check.equal(back.files.first?.name, expected, "\"\(given)\" was not cleaned as expected")
    }
}

private func testBadMetadataIsRefused() {
    for bad in ["", "[]", "[{\"name\":\"../escape\",\"size\":1}]", "not json at all"] {
        Check.that(FileList.decode(Array(bad.utf8)) == nil,
                   "metadata that should not decode was accepted: \(bad)")
    }
    Check.that(FileList.encode([]) == nil, "a list naming no files encoded")
}

/*
 * The real limit is the wire, not the count: the metadata must fit one
 * CLIP_OFFER, so a short list of long names runs out of room long before
 * sixty-four entries would.
 */
private func testAnOversizeListIsRefused() {
    let long = String(repeating: "n", count: 200)
    let many = (0..<40).map { FileListEntry(name: "\(long)-\($0)", size: 1) }
    Check.that(FileList.encode(many) == nil,
               "a list too wordy for a single offer was encoded anyway")
}

private func testCollidingNamesAreRenamed() {
    var used: Set<String> = []
    Check.equal(FileNaming.unused("report.pdf", among: &used), "report.pdf", "the first was renamed")
    Check.equal(FileNaming.unused("report.pdf", among: &used), "report-2.pdf",
                "the second did not get a suffix before the extension")
    Check.equal(FileNaming.unused("report.pdf", among: &used), "report-3.pdf",
                "the third collided with the second")
    Check.equal(FileNaming.unused("noext", among: &used), "noext", "a name with no extension")
    Check.equal(FileNaming.unused("noext", among: &used), "noext-2",
                "a name with no extension got a stray dot")

    /* A leading dot is a hidden file, not an extension: suffixing `.gitignore`
       as `-2.gitignore` would deliver a different kind of file. */
    var hidden: Set<String> = [".gitignore"]
    Check.equal(FileNaming.unused(".gitignore", among: &hidden), ".gitignore-2",
                "a leading dot was treated as an extension")
}
