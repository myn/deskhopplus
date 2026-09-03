import DHCore
import Foundation

/*
 * The clipboard path, from a local copy to a frame body and back (#52).
 *
 * Everything it decides is already decided elsewhere: what a transfer does is
 * `dh_xfer`, what a seal does is `dh_seal`, and whether a direction is allowed
 * is the board's. What this file owns is the *joining* of those three — which
 * seal a payload waits on, which message carries which action, and what
 * happens to a transfer when a toggle changes underneath it.
 *
 * No AppKit and no IOKit, so the whole path is testable on a laptop with no
 * device attached: two of these talking to each other is a full round trip.
 * The Windows twin is helpers/windows/src/clip_service.cpp, and the reason
 * there are two of them rather than two *machines* is the same reason
 * HelperSession gives — the rules both ends must agree on have one
 * implementation, and only the joining is written twice.
 */

/// The payload kinds on the wire (docs/protocol.md, CLIP_OFFER).
public enum ClipKind: UInt8 {
    case text = 0
    case png = 1
    case files = 2
}

/// Files the other computer has offered, waiting for this computer's user to
/// say yes (#56). Nothing crosses the link until they do.
public struct FileOffer: Equatable {
    public let id: UInt32
    public let total: UInt64
    public let files: [FileListEntry]

    /// How long the transfer will take at the route's measured rate, in
    /// seconds. Stated because a cap the user can raise to something that
    /// takes a quarter of an hour deserves a duration in the dialog, not just
    /// a size (#39, #56).
    public var estimatedSeconds: Int {
        Int((total + UInt64(ClipboardService.measuredBytesPerSecond) - 1)
            / UInt64(ClipboardService.measuredBytesPerSecond))
    }
}

/// Files that arrived whole, ready to be written somewhere real. The bytes are
/// every file's contents run together in `files` order — the transfer carries
/// no boundaries of its own, which is what the list is for.
public struct FileDelivery: Equatable {
    public let files: [FileListEntry]
    public let bytes: [UInt8]
}

public enum ClipboardOutput: Equatable {
    /// A frame body for the session to authenticate and send.
    case send(type: UInt8, body: [UInt8])
    /// A complete payload, to be written to this computer's pasteboard.
    case deliver(kind: UInt8, bytes: [UInt8])
    case lazyImage(id: UInt32, total: UInt64)
    case cancelLazyImage(id: UInt32)
    /// Files are being offered and nothing has moved yet: ask the user.
    case fileOffer(FileOffer)
    /// That question no longer stands — answered, superseded, or gone.
    case fileOfferWithdrawn(id: UInt32)
    /// A complete set of files, to be written and put on this computer's
    /// pasteboard as references.
    case deliverFiles(FileDelivery)
    /// Diagnostics, never shown to the user.
    case note(String)
    /// A message the user needs to see, because it explains something they
    /// did that produced nothing. Logged as well, but a log is not telling
    /// anyone.
    case tellUser(String)
    case protocolError(String)
}

public final class ClipboardService {
    public static let eagerImageThreshold = 256 * 1024

    /*
     * Files at or below this size are accepted without asking. At the route's
     * measured rate they are a fraction of a second, and a dialog for a
     * quarter-second transfer is a dialog the user learns to dismiss without
     * reading — which is how the one that matters gets dismissed too.
     */
    public static let filePromptThreshold = 1024 * 1024

    /*
     * The end-to-end rate measured on hardware, in bytes per second.
     *
     * 33 KB/s, from 21 MB of real file transfers on 2026-09-03 (#39). It
     * replaces the 49 KB/s figure, which was itself a correction of the
     * arithmetic ~64 KB/s the transport was specified at. Sustained sets run
     * 28-33 KB/s and short ones touch 45, so the sustained figure is the one
     * taken: the prompt exists so the user can decide whether to wait, and an
     * estimate short of the truth is worse than none.
     */
    public static let measuredBytesPerSecond = 33 * 1024

    /*
     * How long a file offer waits for an answer before it is declined for the
     * user.
     *
     * It has to expire, and not because the question is urgent. An offer that
     * has been accepted-as-lazy and never requested leaves the *copy* side
     * awaiting a request, which it asks for again every two seconds for as
     * long as the session lasts (#78) — so an ignored prompt is a frame every
     * two seconds for ever. It also pins the receive buffer, which is what the
     * size cap sizes, so the cap cannot change while the question stands.
     *
     * Two minutes is human-scaled: long enough to finish a sentence and look
     * up, short enough that walking away from the desk costs a re-copy rather
     * than a link that never goes quiet.
     */
    public static let holdTimeout: TimeInterval = 120
    /*
     * The largest payload this helper will assemble. The spec's default cap is
     * 10 MB; an offer above it is refused by the transfer core with a cancel
     * rather than truncated, so the far end learns why.
     */
    public static let defaultCapacity = 10 * 1024 * 1024

    /*
     * How long an arriving transfer may make no progress before it is given up on.
     *
     * Enormously more than the link needs: a full credit window is 16 KB, which
     * at this transport's ~64 KB/s per direction is a quarter of a second. The
     * margin is deliberate — the cost of waiting too long is a stalled receive
     * reported late, and the cost of firing too early is abandoning a healthy
     * one, which is the worse of the two.
     */
    public static let stallTimeout: TimeInterval = 30

    /*
     * How long an arriving transfer may make no progress before this end asks
     * again for what it is waiting on.
     *
     * Well over a round trip on this link — a credit grant and the chunk it
     * pays for cross in tens of milliseconds — and well under `stallTimeout`,
     * so a receive that can be recovered is recovered long before the deadline
     * that reports it lost.
     *
     * A receiver has to be able to prompt itself. Every message that would
     * otherwise restart it — a credit grant, a retransmit request, the
     * CLIP_DONE that drives a sweep — crosses the same seams the payload does,
     * and a seam that refuses one has no retransmit beneath it (ADR-0005).
     * Before #145 losing any of them cost the whole transfer, at no consistent
     * size and no consistent fraction.
     */
    public static let sweepDelay: TimeInterval = 2

    private let seal: ClipboardSeal
    private let transfer: Transfer

    /*
     * The board's answer, and the default until it has given one. Both allowed,
     * matching what the toggles default to — a helper that refused until told
     * would refuse every copy made in the second before the policy frame
     * arrives, which reads at the desk as the clipboard not working.
     */
    private(set) public var maySend = true
    private(set) public var mayReceive = true

    /*
     * A copy that is waiting for a seal. A payload never goes out unsealed and
     * the exchange is a round trip, so the ordinary first copy after a session
     * begins waits here for one.
     *
     * Exactly one, deliberately: a second copy while the first is still waiting
     * supersedes it, because what the user last copied is what they mean to
     * paste.
     */
    private enum PendingCopy {
        /// Text and images: the bytes are already in hand.
        case eager(kind: UInt8, bytes: [UInt8])
        /// Files: only the list is in hand, and the bytes are read if and when
        /// the other computer asks for them.
        case files(files: [FileListEntry], meta: [UInt8], total: UInt64,
                   provider: () -> [UInt8]?)
    }
    private var pending: PendingCopy?

    /*
     * How the bytes of the outgoing file transfer are produced, held for as
     * long as that transfer could still ask for them.
     *
     * Separate from `pending` because the two have different lifetimes: a
     * pending copy ends the moment the offer goes out, and this ends when the
     * transfer does — NEED_DATA can arrive minutes later, once someone on the
     * other computer has said yes.
     */
    private var outgoingProvider: (() -> [UInt8]?)?

    /*
     * A file offer that has arrived and is waiting for this computer's user.
     *
     * The offer is *accepted* into the transfer machine as lazy, so the far end
     * knows it was heard and stops retrying (#78) — but no request goes out, so
     * not one byte crosses until `acceptFiles`. Exactly one, like `pending`:
     * a newer offer supersedes it, because what was last copied is what the
     * user means to paste.
     */
    private var heldFileOffer: FileOffer?

    /// The file list of the transfer now arriving, so that what is delivered
    /// can be split back into files without parsing the metadata twice.
    private var incomingFiles: [FileListEntry]?

    /*
     * The largest payload this helper will accept, and a change to it that is
     * waiting for the link to go quiet.
     *
     * The board is the single source of truth for the cap (#42), so it can
     * change while this helper is running; the receive buffer is sized against
     * it and cannot move under a transfer, so a change that arrives mid-flight
     * waits for the tick after it ends.
     */
    private var wantedCapacity: Int?

    /*
     * A transfer that was already on its way out when the far helper said it
     * holds no key for the seal. The payload is still in the transfer core, so
     * nothing has to be kept here — only the fact that once a fresh seal is
     * accepted, that transfer has to start again rather than carry on into a
     * far end that never saw its offer.
     */
    private var reofferWhenSealed = false
    /// Whether the held offer's question has actually been put to the user.
    /// False while it waits for them to arrive at this computer.
    private var heldAnnounced = false
    /// What the far computer could not send for being over the cap, waiting
    /// for the user to come here and wonder why nothing pasted.
    private var tooBigWaiting: String?
    /// The board's clipboard size cap, as the copy side needs it. Zero until
    /// the board has said, and nothing is refused on a cap nobody stated.
    private var capBytes = 0
    private var capMegabytes: UInt8 = 0
    /*
     * The handshake, made idempotent under retransmission (#161).
     *
     * `Seal.offer()` draws a new seal id and a new ephemeral key on every call
     * — the offerer owns the seal — and `Seal.accept(offer:)` draws a fresh one
     * on every call too. So a retry of either half silently replaced the key
     * the other half was at that moment answering. On a link whose round trip
     * runs past `sweepDelay` that is a livelock, not a race: every accept names
     * an offer already superseded, nothing is ever sealed, and no file is ever
     * offered.
     *
     * Both halves are now repeated verbatim. A retry re-sends the outstanding
     * offer, and an offer already answered is answered with the same accept —
     * which is the idempotence ADR-0009 gives the transfer layer, applied to
     * the exchange underneath it.
     */
    private var outstandingSealOffer: [UInt8]?
    private var lastSealAnswer: (offer: [UInt8], accept: [UInt8])?
    private var lazyImageID: UInt32?

    /*
     * Seal-wait, receive-timeout and offer-retry bookkeeping. A copy waiting
     * on a seal has no transfer yet, so it owns a separate terminal stamp and
     * two-second retry stamp. A retry deliberately moves only the latter.
     *
     * The marks below are what each transfer direction has actually done; the
     * stamps say when that count last moved.
     * Counting rather than time-stamping inside `render` is what keeps a clock
     * out of every code path that produces an action.
     *
     * The receiving side counts *arrivals* — chunks assembled — and not the
     * messages this end emits. A sweep emits messages, so counting those would
     * let a receive whose far end is gone reset its own deadline for ever
     * (#145). The count alone cannot see a *supersede*, though — a newer offer
     * replaces an incomplete transfer and resets the count to zero, leaving the
     * mark unchanged — so `onOffer` disarms the deadline outright and the next
     * tick arms a fresh one.
     */
    private var txProgress = 0
    private var offerRetryMark = 0
    private var offerRetrySince: TimeInterval?
    private var sealWaitingSince: TimeInterval?
    private var sealRetrySince: TimeInterval?
    private var receivingSince: TimeInterval?
    private var receivingMark: UInt32 = 0
    private var sweptSince: TimeInterval?
    /// When the offer now being held was first put to the user.
    private var heldSince: TimeInterval?

    public init(entropy: @escaping (Int) -> [UInt8], capacity: Int = ClipboardService.defaultCapacity) {
        seal = ClipboardSeal(entropy: entropy)
        transfer = Transfer(capacity: capacity)
    }

    // MARK: - What this computer does

    /// Something was copied here. Eager: the bytes go now, so that pasting on
    /// the other computer never waits for a round trip.
    public func localCopy(kind: ClipKind, bytes: [UInt8]) -> [ClipboardOutput] {
        guard maySend else {
            return [.note("a copy was not offered: the board has clipboard sending turned off "
                          + "in this direction")]
        }
        guard !bytes.isEmpty else { return [] }

        pending = .eager(kind: kind.rawValue, bytes: bytes)
        sealWaitingSince = nil
        sealRetrySince = nil
        return startPendingIfSealed()
    }

    /*
     * Files were copied here (#56). Lazy: what goes out now is the list and
     * nothing else, and `provider` is called only if the other computer's user
     * accepts the transfer — so copying a folder you never paste costs one
     * small frame and never opens a file.
     *
     * `provider` returns every file's contents run together in `files` order,
     * or nil when they can no longer be read. The sizes it produces must match
     * the ones offered; a file edited between the copy and the paste is
     * therefore a failed transfer rather than a truncated one.
     */
    public func localCopy(files: [FileListEntry],
                          provider: @escaping () -> [UInt8]?) -> [ClipboardOutput] {
        guard maySend else {
            return [.note("a file copy was not offered: the board has clipboard sending turned "
                          + "off in this direction")]
        }
        guard !files.isEmpty else { return [] }
        guard let meta = FileList.encode(files) else {
            return [.note("\(files.count) file(s) were copied and their names would not fit one "
                          + "offer, so nothing was sent")]
        }

        var total: UInt64 = 0
        for file in files {
            guard total <= UInt64.max - file.size else {
                return [.note("the copied files add up to more than can be transferred")]
            }
            total += file.size
        }

        /*
         * Refused here, where the user is, rather than across the link where
         * they are not. The far end drops an over-cap offer correctly and
         * says so in its own log — which is no use at all to the person who
         * pressed Cmd-C on this computer and saw nothing happen (#56).
         */
        /*
         * Deliberately *not* refused here. The person who needs to hear about a
         * set too large to cross is whoever goes to paste it, not whoever
         * copied it — most copies never travel, and interrupting this computer
         * about one that was not meant to is the noise the arrival rule exists
         * to stop. The offer is small, the far end already refuses it, and it
         * is the far end that says so when the user gets there (#56).
         */

        pending = .files(files: files, meta: meta, total: total, provider: provider)
        sealWaitingSince = nil
        sealRetrySince = nil
        return startPendingIfSealed()
    }

    /*
     * The cursor has arrived at this computer, so the user is here and a paste
     * is now possible. Anything held quietly is put to them at this moment and
     * not before.
     *
     * Idempotent: crossings are frequent and a question is asked once.
     */
    public func userIsHere() -> [ClipboardOutput] {
        var outputs: [ClipboardOutput] = []
        /* Whatever the far computer could not send, said once, here. */
        if let waiting = tooBigWaiting {
            tooBigWaiting = nil
            outputs.append(.tellUser(waiting))
        }
        guard let held = heldFileOffer, !heldAnnounced else { return outputs }
        heldAnnounced = true
        heldSince = nil
        return outputs + [.fileOffer(held)]
    }

    /*
     * The user accepted the files the other computer offered. This is where a
     * file transfer actually begins — on a decision made here, never on the
     * copy made over there (#56, ADR-0011).
     */
    public func acceptFiles(id: UInt32) -> [ClipboardOutput] {
        guard let offer = heldFileOffer, offer.id == id else { return [] }
        /*
         * The machine has to still be holding it. Without this the file list
         * is remembered for a transfer that will never run — and the *next*
         * transfer then arrives and is split by the wrong list, which reads at
         * the desk as a paste that silently never happens (#56).
         */
        guard transfer.isIncomingHeld else {
            heldFileOffer = nil
            heldAnnounced = false
            heldSince = nil
            return [.fileOfferWithdrawn(id: id),
                    .note("the files were accepted here, but that transfer is no longer "
                          + "waiting to be asked for; nothing was requested")]
        }
        heldFileOffer = nil
        heldAnnounced = false
        heldSince = nil
        incomingFiles = offer.files
        return render(transfer.requestLazy(id: id))
            + [.note("\(offer.files.count) file(s), \(offer.total) bytes, were accepted here "
                     + "and asked for")]
    }

    /// The user declined them. The far end is told, so its copy stops waiting
    /// and its offer retries stop.
    public func declineFiles(id: UInt32) -> [ClipboardOutput] {
        guard let offer = heldFileOffer, offer.id == id else { return [] }
        heldFileOffer = nil
        heldAnnounced = false
        heldSince = nil
        return render(transfer.cancelIncoming())
            + [.fileOfferWithdrawn(id: id),
               .note("\(offer.files.count) file(s) offered from the other computer were "
                     + "declined here")]
    }

    /// The user gave up on a transfer that is already running. Nothing partial
    /// is ever delivered, so this loses the whole of it.
    public func abortReceive() -> [ClipboardOutput] {
        if let offer = heldFileOffer { return declineFiles(id: offer.id) }
        guard transfer.isReceiving else { return [] }
        return render(transfer.cancelIncoming())
            + [.note("an arriving transfer was cancelled here; nothing partial is kept")]
    }

    /// The user gave up on a transfer going out.
    public func abortSend() -> [ClipboardOutput] {
        pending = nil
        outgoingProvider = nil
        guard transfer.isSending else { return [] }
        return render(transfer.cancelOutgoing())
            + [.note("a transfer leaving this computer was cancelled here")]
    }

    /*
     * The board stated the clipboard size cap (#56). The receive buffer is
     * sized against it, and cannot move while anything is arriving — so a
     * change that lands mid-transfer is remembered and applied on the tick
     * after that transfer ends.
     */
    public func capacityChanged(megabytes: UInt8) -> [ClipboardOutput] {
        let bytes = Int(dh_clip_cap_bytes(megabytes))
        /* Kept for the *copy* side as well. Both boards carry the same setting,
           so a set too large to arrive is one this end can refuse outright
           rather than send across the link to be dropped in silence. */
        capMegabytes = megabytes
        capBytes = bytes
        guard bytes != transfer.receiveCapacity else {
            wantedCapacity = nil
            return []
        }
        if transfer.setReceiveCapacity(bytes) {
            wantedCapacity = nil
            return [.note("the clipboard size cap is now \(megabytes) MB")]
        }
        wantedCapacity = bytes
        return [.note("the clipboard size cap changed to \(megabytes) MB and will take effect "
                      + "when nothing is arriving")]
    }

    /// What is arriving, for a progress display. Nil when nothing is.
    public var arriving: (kind: UInt8, received: UInt64, total: UInt64)? { transfer.arriving }

    /// The file offer waiting on this computer's user, if there is one.
    public var awaitingDecision: FileOffer? { heldFileOffer }

    /// Whether anything is still on its way out of this computer. False after a
    /// transfer the far end declined or that could not be read — the two ways a
    /// lazy send ends without delivering.
    public var awaitingSend: Bool { transfer.isSending }


    /// The board stated its clipboard policy. A direction turned off takes any
    /// transfer already crossing it with it — otherwise turning a toggle off
    /// would leave the content that is already moving to finish arriving.
    public func policyChanged(flags: UInt8) -> [ClipboardOutput] {
        let couldSend = maySend
        let couldReceive = mayReceive
        maySend = (flags & UInt8(DH_CLIP_MAY_SEND)) != 0
        mayReceive = (flags & UInt8(DH_CLIP_MAY_RECEIVE)) != 0

        var outputs: [ClipboardOutput] = []
        if couldSend && !maySend {
            pending = nil
            sealWaitingSince = nil
            sealRetrySince = nil
            reofferWhenSealed = false
            outgoingProvider = nil
            outputs += render(transfer.cancelOutgoing())
            outputs.append(.note("clipboard sending was turned off; anything in flight was "
                                 + "abandoned"))
        }
        if couldReceive && !mayReceive {
            if let held = heldFileOffer {
                heldFileOffer = nil
                heldAnnounced = false
                heldSince = nil
                outputs.append(.fileOfferWithdrawn(id: held.id))
            }
            incomingFiles = nil
            outputs += render(transfer.cancelIncoming())
            outputs.append(.note("clipboard receiving was turned off; anything in flight was "
                                 + "abandoned"))
        }
        return outputs
    }

    /// The session went away. Both halves of the seal go with it — #107
    /// measured 586 teardowns in sixteen hours, and re-offering is cheap where
    /// a key whose peer may no longer exist is not.
    public func sessionEnded() -> [ClipboardOutput] {
        reofferWhenSealed = false
        outgoingProvider = nil
        incomingFiles = nil
        /* Both halves of the exchange belong to the session that made them. */
        outstandingSealOffer = nil
        lastSealAnswer = nil
        /*
         * A copy still waiting for a seal is *kept*. What is on the clipboard
         * does not change because the link wobbled, and the pasteboard is only
         * read again when the user copies something else — so dropping it here
         * lost the copy for good, in silence. `sealWaitingSince` is deliberately
         * left running: the 30s budget is counted from the copy, across as many
         * session ends as the link manages, and `tick` still gives up out loud
         * at the end of it.
         */
        var withdrawn: [ClipboardOutput] = []
        if let waiting = pending {
            withdrawn.append(.note("the session went away; \(describe(waiting)) copied here "
                                   + "are still waiting for one that can carry them"))
        }
        if let held = heldFileOffer {
            heldFileOffer = nil
            heldAnnounced = false
            heldSince = nil
            withdrawn.append(.fileOfferWithdrawn(id: held.id))
        }
        let outputs = withdrawn + render(transfer.linkDown())
        seal.reset()
        /* Sends produced here have nowhere to go: there is no session to
           authenticate them. Dropped rather than handed on, so a caller cannot
           mistake them for frames that went out. */
        return outputs.filter { if case .send = $0 { return false } else { return true } }
    }

    /// A chance to push more chunks — after every arriving frame, and on the
    /// tick. Empty when nothing is owed, which is the ordinary answer.
    public func pump() -> [ClipboardOutput] {
        render(transfer.pump())
    }

    public func requestLazyImage(id: UInt32) -> [ClipboardOutput] {
        render(transfer.requestLazy(id: id))
    }

    public func lazyImageWasReplaced(id: UInt32) -> [ClipboardOutput] {
        guard lazyImageID == id else { return [] }
        return render(transfer.cancelIncoming())
    }

    /*
     * Recover an arriving transfer that has stopped moving.
     *
     * The other two interruptions #52 names — an unplug and a config-mode
     * reboot — both end this helper's own session, and `sessionEnded` already
     * abandons everything. The third does not: when the helper on the *other*
     * computer crashes, this one's session is perfectly healthy. The paste
     * side can therefore time out an incomplete receive. The copy side cannot:
     * v2 has no delivery acknowledgement, so a completed send and a missing
     * helper look identical. Its payload remains available for late
     * retransmits until supersede, cancellation, or session loss (#134).
     *
     * Measured against *progress*, not against the transfer's total duration: a
     * large payload legitimately takes minutes on this link, and a deadline on
     * the whole transfer would abandon healthy ones.
     *
     * `boardDrops` is what the board has said it dropped (#133), quoted into
     * the abandonment note. A stall and a board that has been losing frames is
     * a different fault from a stall with clean seams, and the numbers are
     * only worth reading at the moment one happens — which is here. Passed on
     * every tick rather than held, because the board restates them whenever
     * they move and nothing tells this service when that was.
     */
    public func tick(at now: TimeInterval, boardDrops: BoardDrops? = nil) -> [ClipboardOutput] {
        var outputs: [ClipboardOutput] = []
        let drops = boardDrops?.line ?? "the board has stated no drop totals"

        /* A size cap that changed while something was arriving (#56). Tried
           here rather than remembered for ever, because the transfer it waited
           on ends without telling anyone. */
        /* A question nobody answered. See `holdTimeout` for why it cannot be
           left standing: the copy side re-offers every two seconds until it is
           requested, and the buffer the size cap sizes stays pinned. */
        if let held = heldFileOffer, heldAnnounced {
            if heldSince == nil {
                heldSince = now
            } else if now - heldSince! >= Self.holdTimeout {
                outputs += declineFiles(id: held.id)
                outputs.append(.note("a file offer went unanswered for "
                                     + "\(Int(Self.holdTimeout))s and was declined"))
            }
        } else {
            heldSince = nil
        }

        if let wanted = wantedCapacity, transfer.canSetReceiveCapacity,
           transfer.setReceiveCapacity(wanted) {
            wantedCapacity = nil
            outputs.append(.note("the clipboard size cap that was waiting is now in force: "
                                 + "\(wanted / (1024 * 1024)) MB"))
        }

        if pending != nil, seal.canSeal {
            /*
             * Sealed while a copy was parked. The flush normally happens on the
             * accept that made it usable; this is the one place that guarantees
             * it, and it exists because the state it covers is silent. Grouped
             * with `pending == nil` before, a parked copy under a live seal was
             * neither offered nor abandoned nor mentioned — it simply never
             * happened, which is how it was reported (#161).
             */
            sealWaitingSince = nil
            sealRetrySince = nil
            outputs += startPendingIfSealed()
        } else if pending == nil {
            sealWaitingSince = nil
            sealRetrySince = nil
        } else if sealWaitingSince == nil {
            sealWaitingSince = now
            sealRetrySince = now
        } else if now - sealWaitingSince! >= Self.stallTimeout {
            pending = nil
            sealWaitingSince = nil
            sealRetrySince = nil
            outputs.append(.note("a copy waiting for a seal made no progress for "
                                 + "\(Int(Self.stallTimeout))s and was abandoned (\(drops))"))
        } else if now - (sealRetrySince ?? now) >= Self.sweepDelay {
            sealRetrySince = now
            outputs += resendSealOffer()
        }

        if !transfer.isSending {
            offerRetrySince = nil
        } else if offerRetrySince == nil || offerRetryMark != txProgress {
            offerRetrySince = now
            offerRetryMark = txProgress
        } else if transfer.isAwaitingRequest && now - (offerRetrySince ?? now) >= Self.sweepDelay {
            offerRetrySince = now
            outputs += render(transfer.retryOffer())
        }

        if !transfer.isReceiving {
            receivingSince = nil
            sweptSince = nil
        } else if receivingSince == nil || receivingMark != transfer.receivedChunks {
            receivingSince = now
            sweptSince = now
            receivingMark = transfer.receivedChunks
        } else if now - receivingSince! >= Self.stallTimeout {
            let line = transfer.progressLine
            receivingSince = nil
            sweptSince = nil
            outputs += render(transfer.cancelIncoming())
            outputs.append(.note("an arriving transfer made no progress for "
                                 + "\(Int(Self.stallTimeout))s and was abandoned (\(line); "
                                 + "\(drops)); nothing partial is ever written"))
        } else if now - sweptSince! >= Self.sweepDelay {
            sweptSince = now
            outputs += sweep()
        }
        return outputs
    }

    /*
     * Ask again for what a stopped receive is waiting on, and say so.
     *
     * Said out loud on every round rather than counted quietly, because a
     * stall that recovers is otherwise invisible: the transfer completes and
     * nothing in the log says the link lost anything. A stall that does not
     * recover then reads as the same line repeating, which is the finding.
     */
    private func sweep() -> [ClipboardOutput] {
        let actions = transfer.sweepReceive()
        let restarted = actions.contains { $0.type == DH_XFER_ACT_SEND_REQUEST }
        let named = actions.filter { $0.type == DH_XFER_ACT_SEND_RETRANSMIT }.count
        return render(actions)
            + [.note("an arriving transfer made no progress for \(Int(Self.sweepDelay))s; "
                     + (restarted ? "nothing has arrived at all, so it was asked for again"
                                  : "\(named) chunk(s) asked for again")
                     + " (\(transfer.progressLine))")]
    }

    // MARK: - What the far helper says

    public func received(type: UInt8, body: [UInt8]) -> [ClipboardOutput] {
        switch type {
        case MessageType.sealOffer:
            return onSealOffered(body)
        case MessageType.sealAccept:
            return onSealAccepted(body)
        case MessageType.sealStale:
            return onSealStale(body)
        case MessageType.clipOffer:
            return onOffer(body)
        case MessageType.clipChunk:
            return onChunk(body)
        case MessageType.clipRequest:
            guard let id = ClipCodec.decodeID(body) else { return [malformed(type)] }
            let awaiting = transfer.isAwaitingRequest
            let retries = transfer.offerRetries
            var outputs = render(transfer.handleRequest(id: id)) + pump()
            if awaiting && !transfer.isAwaitingRequest && retries > 0 {
                outputs.append(.note("an offer succeeded after \(retries) retry action(s) were produced"))
            }
            return outputs
        case MessageType.clipDone:
            guard let id = ClipCodec.decodeID(body) else { return [malformed(type)] }
            return render(transfer.handleDone(id: id))
        case MessageType.clipCancel:
            guard let id = ClipCodec.decodeID(body) else { return [malformed(type)] }
            return render(transfer.handleCancel(id: id))
        case MessageType.clipRetransmit:
            guard let m = ClipCodec.decodeRetransmit(body) else { return [malformed(type)] }
            return render(transfer.handleRetransmit(id: m.id, seq: m.seq)) + pump()
        case MessageType.clipCredit:
            guard let m = ClipCodec.decodeCredit(body) else { return [malformed(type)] }
            return render(transfer.handleCredit(id: m.id, credits: m.credits)) + pump()
        default:
            return [.note("a clipboard message of type \(type) arrived, which this helper does "
                          + "not carry")]
        }
    }

    // MARK: - The seal exchange

    /*
     * A fresh seal is the only word this end gets that the far helper's process
     * started over, because a helper offers one exactly when it holds no key to
     * send under. Its offer ids started over with it, so the offer-id frontier
     * this end measures them against belongs to a process that no longer exists (#151)
     * — kept, it would read the restarted helper's first offer as stale and
     * leave the clipboard dead in that direction until this end reset too.
     *
     * So the incoming direction is forgotten here. Anything half-arrived
     * belonged to the seal just replaced and can never be finished; it is
     * abandoned rather than delivered in part.
     *
     * A healthy receive cannot be thrown away this way. A duplicated frame
     * never reaches here — a counter seen once is refused for ever (dh_auth.h)
     * — and a genuinely fresh offer means the far end holds no key, which is
     * exactly the state in which it cannot be sending anything.
     */
    private func onSealOffered(_ body: [UInt8]) -> [ClipboardOutput] {
        /*
         * The same offer again, compared as bytes — a retry re-sends it
         * verbatim, so equality is the whole test and no parse of the body is
         * needed. Answering it with a freshly derived key would leave this end
         * holding one the offerer can never arrive at, because the offerer is
         * still answering the first accept; it would also reset a receive that
         * is not being replaced at all.
         */
        if let last = lastSealAnswer, last.offer == body {
            return [.send(type: MessageType.sealAccept, body: last.accept)]
        }
        let accept: [UInt8]
        do {
            accept = try seal.accept(offer: body)
        } catch {
            return [.note("a seal offer could not be accepted: \(error)")]
        }
        lastSealAnswer = (body, accept)
        return [.send(type: MessageType.sealAccept, body: accept)]
            + render(transfer.incomingSealReplaced())
    }

    private func onSealAccepted(_ body: [UInt8]) -> [ClipboardOutput] {
        do {
            try seal.accepted(body)
            outstandingSealOffer = nil
        } catch {
            return [.note("a seal accept could not be used: \(error)")]
        }
        /* Appended after whatever this produces, not before it: a caller that
           reads the first output is reading for a frame. */
        let sealedNote = ClipboardOutput.note("the seal is live; this end can send now")
        /* The copy that was waiting for exactly this, or the transfer a stale
           seal knocked back to the start. */
        if reofferWhenSealed {
            reofferWhenSealed = false
            if pending == nil {
                return render(transfer.reoffer()) + [sealedNote]
            }
        }
        return startPendingIfSealed() + [sealedNote]
    }

    /*
     * The far helper holds no key for a seal it was sent — the ordinary
     * recovery when its session ended and this one's did not. The seal is
     * discarded and the payload waits on a fresh offer, which is why the copy
     * is put back in `pending` rather than abandoned.
     */
    private func onSealStale(_ body: [UInt8]) -> [ClipboardOutput] {
        guard let sealID = ClipboardSeal.sealID(fromStale: body) else {
            return [.note("a SEAL_STALE would not decode")]
        }
        outstandingSealOffer = nil
        guard seal.discardSeal(sealID) else {
            /* Naming some other seal changes nothing: this end has already
               moved on, and re-offering would restart a transfer that is
               working. */
            return []
        }
        /*
         * Something has to be waiting, or a fresh seal is bytes spent on
         * nothing. A transfer already in flight counts: its offer went out
         * under the dead key, so the far end never saw it and the transfer has
         * to start again once there is a key it can open (Transfer.reoffer).
         */
        if pending == nil {
            guard transfer.isSending else {
                return [.note("the far helper lost the seal; nothing was waiting on it")]
            }
            reofferWhenSealed = true
        }
        return offerSeal() + [.note("the far helper lost the seal; offering a fresh one")]
    }

    // MARK: - Receiving a payload

    /*
     * A newer offer replaces whatever was arriving, and a question that was
     * being held about the old one no longer stands.
     *
     * Needed on the *non-file* path too, which is what makes it worth having
     * once: a text or image copy supersedes a held file offer inside the
     * transfer machine, and without this the menu bar goes on offering Accept
     * for a transfer the far end has already moved past — where accepting does
     * nothing at all and says nothing either.
     */
    private func withdrawHeldOffer(supersededBy id: UInt32) -> [ClipboardOutput] {
        guard let held = heldFileOffer, held.id != id else { return [] }
        heldFileOffer = nil
        heldAnnounced = false
        heldSince = nil
        incomingFiles = nil
        return [.fileOfferWithdrawn(id: held.id)]
    }

    private func onOffer(_ body: [UInt8]) -> [ClipboardOutput] {
        /*
         * Refused before the seal is opened, deliberately. A helper told not to
         * receive has no business decrypting the payload first — and the clear
         * head carries everything a refusal needs.
         */
        guard mayReceive else {
            guard let id = ClipCodec.offerID(ofSealedOffer: body) else {
                return [malformed(MessageType.clipOffer)]
            }
            return [.send(type: MessageType.clipCancel, body: ClipCodec.id(id)),
                    .note("an offer was refused: the board has clipboard receiving turned off "
                          + "in this direction")]
        }

        do {
            let offer = try seal.openOffer(body)
            /*
             * A new offer starts a new deadline. Disarmed here rather than
             * inferred in the tick, because what the tick can see — the
             * received count — is zero for the transfer being replaced and zero
             * for the one replacing it, and the transfer id cannot separate
             * them either: ids are per far-helper *process* and start again at
             * one when that process restarts. Without this the second transfer
             * inherits whatever is left of the first one's thirty seconds and
             * is abandoned seconds old (#145). No clock is read here, which is
             * what keeps one out of every path that produces an action.
             */
            let previousID = transfer.receivedOfferID
            if offer.kind == ClipKind.files.rawValue {
                return onFileOffer(offer, previousID: previousID)
            }
            let lazy = offer.kind == ClipKind.png.rawValue &&
                       offer.total > UInt64(Self.eagerImageThreshold)
            var outputs = withdrawHeldOffer(supersededBy: offer.id)
            outputs += render(lazy ? transfer.handleLazy(offer: offer)
                                   : transfer.handle(offer: offer))
            if lazy && previousID != offer.id && transfer.receivedOfferID == offer.id {
                lazyImageID = offer.id
                outputs.append(.lazyImage(id: offer.id, total: offer.total))
            }
            if transfer.receivedOfferID != previousID {
                receivingSince = nil
                sweptSince = nil
            }
            return outputs
        } catch SealError.unknownSeal {
            return staleReply(for: MessageType.clipOffer, body: body)
        } catch {
            return [.note("an offer could not be opened: \(error)")]
        }
    }

    /*
     * Files are offered from the other computer.
     *
     * Accepted into the transfer machine as **lazy** and then left there: the
     * far end learns its offer was heard and stops repeating it (#78), while
     * not one byte crosses the link until this computer's user says so. Small
     * sets skip the question — see `filePromptThreshold` for why a prompt for
     * a quarter-second transfer makes the prompt that matters worthless.
     *
     * The metadata is checked before anything is accepted, and a list that
     * does not add up to the offer's own total is refused. Those two numbers
     * come from the same far helper, so a disagreement is that helper being
     * wrong or being tampered with, and either way this end would otherwise
     * write files by slicing a payload at offsets it has no reason to trust.
     */
    private func onFileOffer(_ offer: ClipOffer, previousID: UInt32?) -> [ClipboardOutput] {
        guard let listed = FileList.decode(offer.meta) else {
            return [.send(type: MessageType.clipCancel, body: ClipCodec.id(offer.id)),
                    .note("a file offer named files this helper will not write, so it was "
                          + "refused")]
        }
        /*
         * The list's own total, as the core summed it, against the total the
         * same offer promises. Both come from the same far helper, so a
         * disagreement is that helper being wrong or being tampered with — and
         * either way this end would otherwise write files by slicing a payload
         * at offsets it has no reason to trust.
         */
        guard listed.total == offer.total else {
            return [.send(type: MessageType.clipCancel, body: ClipCodec.id(offer.id)),
                    .note("a file offer promised \(offer.total) bytes and listed "
                          + "\(listed.total), so it was refused")]
        }
        let files = listed.files

        /* An identical retry of the offer already being held is the far end
           repeating itself, not a second question to ask (#78, ADR-0009). */
        let waiting = FileOffer(id: offer.id, total: offer.total, files: files)
        if heldFileOffer == waiting { return [] }

        var outputs = withdrawHeldOffer(supersededBy: offer.id)
        heldFileOffer = nil
        heldAnnounced = false
        heldSince = nil
        outputs += render(transfer.handleLazy(offer: offer))
        /*
         * Held, and held for *this* offer — the only state in which there is a
         * question to ask.
         *
         * Not `receivedOfferID == offer.id`, which was the bug: that survives
         * an offer the machine has already refused for being over the size
         * cap, so a 2.5 MB file against a 2 MB cap was put to the user and
         * accepting it did nothing. It also survives the answer, so every
         * two-second offer retry re-asked a question already answered — three
         * toasts for one file, observed on hardware.
         */
        guard transfer.isIncomingHeld, transfer.receivedOfferID == offer.id else {
            /*
             * Refused inside the machine, and until now without a word. A set
             * over the size cap produced no question, no transfer and no line
             * anywhere — which at the desk is indistinguishable from the
             * clipboard having stopped working, and was reported as exactly
             * that.
             */
            if offer.total > UInt64(transfer.receiveCapacity) {
                let cap = transfer.receiveCapacity / (1024 * 1024)
                let name = files.count == 1 ? files[0].name : "\(files.count) files"
                outputs.append(.note("\(files.count) file(s), \(offer.total) bytes, are over "
                                     + "the \(cap) MB size cap; the user is told when they "
                                     + "arrive"))
                /* Held for the same moment the question is: saying it now would
                   interrupt whoever is at this computer about a copy made on
                   the other one. */
                tooBigWaiting = "\(name) is \(offer.total / (1024 * 1024)) MB, larger than the "
                    + "\(cap) MB clipboard limit, so it was not brought over. Raise the limit "
                    + "on the board's config page."
            }
            return outputs
        }

        if transfer.receivedOfferID != previousID {
            receivingSince = nil
            sweptSince = nil
        }

        if offer.total <= UInt64(Self.filePromptThreshold) {
            incomingFiles = files
            /* Said out loud. A set under the line crosses with no question, and
               from the desk that is indistinguishable from a question that
               failed to appear — which is exactly how it was reported. */
            return outputs + render(transfer.requestLazy(id: offer.id))
                + [.note("\(files.count) file(s), \(offer.total) bytes, are under the "
                         + "\(Self.filePromptThreshold / (1024 * 1024)) MB line, so they were "
                         + "taken without asking")]
        }
        heldFileOffer = waiting
        /*
         * Held quietly. The question is put when the user arrives at this
         * computer (`userIsHere`), not when the copy happened on the other one.
         *
         * A copy is not a request to interrupt anybody: most copies are made to
         * be pasted where they were made. Asking on the copy meant a 5 MB file
         * copied on the Mac and never meant to travel still interrupted whoever
         * was at the Windows machine. A paste over here can only follow the
         * cursor arriving over here, so arrival is the moment the question
         * becomes worth asking — and if it never comes, it never is (#56).
         *
         * `heldSince` stays nil until then: the hold deadline is time the user
         * had to answer, and they have had none.
         */
        heldSince = nil
        heldAnnounced = false
        return outputs + [.note("\(files.count) file(s), \(offer.total) bytes, are held; the "
                                + "question waits until the cursor comes to this computer")]
    }

    private func onChunk(_ body: [UInt8]) -> [ClipboardOutput] {
        /*
         * Refused before the seal is opened, like the offer — the invariant is
         * that a helper told not to receive never decrypts a payload it has
         * already decided to refuse (docs/protocol.md).
         *
         * This is reachable in the ordinary way: turning the toggle off
         * mid-transfer cancels the transfer, but the chunks already in flight
         * behind that cancel keep arriving. Silent because the cancel already
         * said why, and there are up to a credit window of these — a line each
         * would bury the reason under its own consequences.
         */
        guard mayReceive else { return [] }

        do {
            let chunk = try seal.openChunk(body)
            /*
             * Before and after, because the transfer machine refuses a chunk by
             * doing nothing: one for the wrong transfer, out of range, or whose
             * CRC32 does not match is dropped with no action. Without this the
             * difference between "no chunk arrived" and "every chunk arrived
             * and was refused" is invisible, and those two have nothing in
             * common to fix.
             */
            let before = transfer.receivedChunks
            var outputs = render(transfer.handle(chunk: chunk))
            if transfer.receivedChunks == before {
                outputs.append(.note("chunk \(chunk.seq) of transfer \(chunk.id) opened but the "
                                     + "transfer machine refused it (\(transfer.progressLine))"))
            }
            return outputs
        } catch SealError.unknownSeal {
            return staleReply(for: MessageType.clipChunk, body: body)
        } catch {
            return [.note("a chunk could not be opened: \(error)")]
        }
    }

    /// "I hold no key for this id." The sender discards that seal and offers a
    /// fresh one, which is how two ends whose sessions ended at different
    /// moments find each other again.
    private func staleReply(for type: UInt8, body: [UInt8]) -> [ClipboardOutput] {
        guard let sealID = ClipboardSeal.sealID(ofMessage: type, body: body) else {
            return [malformed(type)]
        }
        return [.send(type: MessageType.sealStale, body: ClipboardSeal.staleBody(sealID))]
    }

    // MARK: - Sending a payload

    private func startPendingIfSealed() -> [ClipboardOutput] {
        guard let waiting = pending else { return [] }
        /*
         * No key yet, so the copy waits for one. Said out loud because a copy
         * that parks in silence reads at the desk as a copy that did nothing —
         * and on a link that is reconnecting, parking is the ordinary case,
         * not the rare one.
         */
        guard seal.canSeal else {
            return offerSeal() + [.note("\(describe(waiting)) copied here are waiting for a "
                                        + "seal before anything is offered")]
        }

        pending = nil
        sealWaitingSince = nil
        sealRetrySince = nil
        switch waiting {
        case .eager(let kind, let bytes):
            outgoingProvider = nil
            /* Said out loud, as a file copy is. Text and images crossing in
               silence meant a log could not show that an image copy had
               superseded a file transfer, which is what it was doing. */
            return render(transfer.offer(kind: kind, data: bytes))
                + [.note("\(bytes.count) bytes copied here were offered")]
        case .files(let files, let meta, let total, let provider):
            outgoingProvider = provider
            return render(transfer.offerLazy(kind: ClipKind.files.rawValue, meta: meta,
                                             total: total))
                + [.note("\(files.count) file(s), \(total) bytes, were offered without being "
                         + "read")]
        }
    }

    /// What a parked copy is, for the notes that report one waiting.
    private func describe(_ waiting: PendingCopy) -> String {
        switch waiting {
        case .eager(_, let bytes): return "\(bytes.count) bytes"
        case .files(let files, _, let total, _): return "\(files.count) file(s), \(total) bytes"
        }
    }

    /// Ask for a seal, if this end has not asked already. Sending the
    /// outstanding offer again belongs to the retry and not here: two identical
    /// offers in one pass buy nothing and make the peer answer twice.
    private func offerSeal() -> [ClipboardOutput] {
        if outstandingSealOffer != nil { return [] }
        return resendSealOffer()
    }

    /// The retry: the same bytes again, because the peer may be answering them
    /// at this moment. Only a handshake this end has abandoned mints a new one.
    private func resendSealOffer() -> [ClipboardOutput] {
        if let outstanding = outstandingSealOffer {
            return [.send(type: MessageType.sealOffer, body: outstanding)]
        }
        do {
            let body = try seal.offer()
            outstandingSealOffer = body
            /* The exchange had no line in the log at all, which is why two
               faults in it were diagnosed by inference rather than by reading
               (#161). Said on the mint and on the accept only — a retry is
               silent, so this stays two lines per exchange. */
            return [.send(type: MessageType.sealOffer, body: body),
                    .note("offering a seal so this end can send")]  // frame first
        } catch {
            pending = nil
            sealWaitingSince = nil
            sealRetrySince = nil
            outstandingSealOffer = nil
            return [.note("a seal could not be offered, so nothing can be sent: \(error)")]
        }
    }

    // MARK: - Actions into messages

    private func render(_ actions: [TransferAction]) -> [ClipboardOutput] {
        var outputs: [ClipboardOutput] = []
        for action in actions {
            switch action.type {
            case DH_XFER_ACT_SEND_OFFER:
                txProgress += 1
                outputs += sealedOffer()
            case DH_XFER_ACT_SEND_OFFER_RETRY:
                outputs += sealedOffer()
            case DH_XFER_ACT_SEND_CHUNK:
                txProgress += 1
                outputs += sealedChunk(seq: action.seq)
            case DH_XFER_ACT_SEND_DONE:
                txProgress += 1
                outputs.append(.send(type: MessageType.clipDone, body: ClipCodec.id(action.id)))
            case DH_XFER_ACT_SEND_REQUEST:
                outputs.append(.send(type: MessageType.clipRequest, body: ClipCodec.id(action.id)))
            case DH_XFER_ACT_SEND_RETRANSMIT:
                outputs.append(.send(type: MessageType.clipRetransmit,
                                     body: ClipCodec.retransmit(id: action.id, seq: action.seq)))
            case DH_XFER_ACT_SEND_CREDIT:
                outputs.append(.send(type: MessageType.clipCredit,
                                     body: ClipCodec.credit(id: action.id,
                                                            credits: action.credits)))
            case DH_XFER_ACT_SEND_CANCEL:
                outputs.append(.send(type: MessageType.clipCancel, body: ClipCodec.id(action.id)))
            case DH_XFER_ACT_DELIVERED:
                let payload = transfer.delivered()
                if lazyImageID == action.id { lazyImageID = nil }
                if payload.kind == ClipKind.files.rawValue {
                    outputs += deliverFiles(payload.bytes)
                } else {
                    outputs.append(.deliver(kind: payload.kind, bytes: payload.bytes))
                }
                if transfer.duplicateOffers > 0 {
                    outputs.append(.note("a transfer completed after "
                                         + "\(transfer.duplicateOffers) duplicate offer(s) "
                                         + "were observed"))
                }
            case DH_XFER_ACT_FAILED:
                if lazyImageID == action.id {
                    lazyImageID = nil
                    outputs.append(.cancelLazyImage(id: action.id))
                }
                /*
                 * Which direction failed is asked of the machine, not of the
                 * id: ids are per direction and collide across the two (#136),
                 * so a send that failed would otherwise throw away a healthy
                 * receive's file list — and the receive would then arrive with
                 * nothing to split it by.
                 */
                if !transfer.isIncomingBusy {
                    if let held = heldFileOffer {
                        heldFileOffer = nil
                        heldAnnounced = false
                        heldSince = nil
                        outputs.append(.fileOfferWithdrawn(id: held.id))
                    }
                    incomingFiles = nil
                }
                if !transfer.isSending { outgoingProvider = nil }
                outputs.append(.note("transfer \(action.id) was abandoned: "
                                     + Self.reason(action.reason)))
            case DH_XFER_ACT_PROTOCOL_ERROR:
                outputs.append(.protocolError("offer \(action.id) reused immutable identity "
                                              + "with different content"))
            case DH_XFER_ACT_NEED_DATA:
                guard let provider = outgoingProvider else {
                    outputs.append(.note("a lazy payload was asked for and this end promised "
                                         + "none"))
                    outputs += render(transfer.provideFail())
                    break
                }
                /*
                 * Read here and nowhere earlier: this is the moment #56 exists
                 * for, when someone on the other computer has said yes.
                 *
                 * It costs a pause and it costs memory, and both are bounded
                 * and accepted rather than overlooked. The whole set is read
                 * into RAM and then copied once into the transfer's own
                 * storage, so a 64 MB paste — the largest cap the board can
                 * state — peaks at roughly twice that. The read happens on the
                 * run loop, which also carries the heartbeat: at SSD speeds
                 * that is tens of milliseconds against a three-second
                 * deadline, and input itself never touches this helper, so
                 * what a long read could cost is one late cursor placement.
                 * Streaming from disk instead would mean the transfer core
                 * holding a callback rather than a pointer, which is a change
                 * to the shared machine both helpers run.
                 */
                guard let bytes = provider() else {
                    outgoingProvider = nil
                    outputs.append(.note("the copied files could not be read, so the transfer "
                                         + "was abandoned rather than sent short"))
                    outputs += render(transfer.provideFail())
                    break
                }
                /*
                 * The offer promised a length and the core will read exactly
                 * that many bytes from what it is given, so a short read here
                 * is an overread there. It is also the ordinary case of a file
                 * edited between the copy and the paste — which must fail the
                 * transfer rather than truncate it.
                 */
                let promised = transfer.outgoingOffer()?.total ?? 0
                guard UInt64(bytes.count) == promised else {
                    outgoingProvider = nil
                    outputs.append(.note("the copied files were \(bytes.count) bytes and "
                                         + "\(promised) were offered, so the transfer was "
                                         + "abandoned rather than sent short"))
                    outputs += render(transfer.provideFail())
                    break
                }
                outputs.append(.note("the copied files were read: \(bytes.count) bytes"))
                outputs += render(transfer.provide(data: bytes))
            default:
                outputs.append(.note("the transfer core produced action \(action.type.rawValue), "
                                     + "which this helper does not carry"))
            }
        }
        return outputs
    }

    /*
     * Split a delivered file payload back into files.
     *
     * The list comes from the offer, checked against that offer's own total
     * before a single byte was asked for — so the offsets here are arithmetic
     * over numbers already agreed, not a second parse of anything the far end
     * says now. The bounds check stays all the same: a payload shorter than
     * the list claims is a bug on this path, and the answer to one is a
     * refused delivery, never a short file presented as whole.
     */
    private func deliverFiles(_ bytes: [UInt8]) -> [ClipboardOutput] {
        guard let files = incomingFiles else {
            return [.note("a file payload arrived with no list to split it by; nothing was "
                          + "written")]
        }
        incomingFiles = nil

        let listed = files.reduce(UInt64(0)) { $0 &+ $1.size }
        guard listed == UInt64(bytes.count) else {
            return [.note("a file payload of \(bytes.count) bytes did not match the "
                          + "\(listed) its list named; nothing was written")]
        }
        return [.deliverFiles(FileDelivery(files: files, bytes: bytes)),
                .note("\(files.count) file(s), \(bytes.count) bytes, arrived whole")]
    }

    private func sealedOffer() -> [ClipboardOutput] {
        guard let offer = transfer.outgoingOffer() else {
            return [.note("an offer was asked for with no transfer in flight")]
        }
        do {
            return [.send(type: MessageType.clipOffer, body: try seal.seal(offer))]
        } catch {
            return [.note("an offer could not be sealed: \(error)")]
        }
    }

    private func sealedChunk(seq: UInt32) -> [ClipboardOutput] {
        guard let chunk = transfer.outgoingChunk(seq: seq) else {
            return [.note("chunk \(seq) was asked for and there is no such chunk")]
        }
        do {
            return [.send(type: MessageType.clipChunk, body: try seal.seal(chunk))]
        } catch {
            return [.note("chunk \(seq) could not be sealed: \(error)")]
        }
    }

    private func malformed(_ type: UInt8) -> ClipboardOutput {
        .note("a clipboard message of type \(type) would not decode")
    }

    private static func reason(_ reason: dh_xfer_fail_reason) -> String {
        switch reason {
        case DH_XFER_FAIL_CANCELLED: return "it was cancelled"
        case DH_XFER_FAIL_LINK_DROP: return "the session went away"
        case DH_XFER_FAIL_NO_DATA: return "the payload could not be produced"
        case DH_XFER_FAIL_SEAL_REPLACED: return "the far helper started a fresh seal"
        default: return "reason \(reason.rawValue)"
        }
    }
}
