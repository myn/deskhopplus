import DeskhopChannel

let imagePrefetchTests: [(String, () throws -> Void)] = [
    ("a large image prefetch remembers the pasteboard it may replace", {
        var prefetch = ImagePrefetch()
        Check.equal(prefetch.begin(id: 17, changeCount: 42), 17,
                    "the accepted offer was not returned for immediate request")
        Check.equal(prefetch.complete(), .publish(ifUnchangedSince: 42),
                    "completion lost the pasteboard version from offer time")
        Check.equal(prefetch.complete(), .ordinary,
                    "one prefetched image completion was consumed twice")
    }),

    ("any local pasteboard replacement invalidates the prefetch", {
        var prefetch = ImagePrefetch()
        prefetch.begin(id: 17, changeCount: 42)
        Check.equal(prefetch.localReplacement(), 17,
                    "the transfer to cancel was not returned")
        Check.equal(prefetch.complete(), .ordinary,
                    "an invalidated image could still replace the pasteboard")
    }),

    ("only the matching protocol cancellation clears the prefetch", {
        var prefetch = ImagePrefetch()
        prefetch.begin(id: 17, changeCount: 42)
        prefetch.cancel(id: 16)
        Check.equal(prefetch.complete(), .publish(ifUnchangedSince: 42),
                    "an unrelated cancellation discarded the image")

        prefetch.begin(id: 18, changeCount: 43)
        prefetch.cancel(id: 18)
        Check.equal(prefetch.complete(), .ordinary,
                    "the matching cancellation left the image publishable")
    }),

    ("an overlapping pasteboard writer is detected after preparation", {
        Check.that(ImagePrefetch.preparationWasExclusive(before: 42, after: 43),
                   "our single prepare transition was rejected")
        Check.that(!ImagePrefetch.preparationWasExclusive(before: 42, after: 44),
                   "a racing writer's extra transition was accepted")
    }),
]
