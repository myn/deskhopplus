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

/// The payload kinds on the wire (docs/protocol.md, CLIP_OFFER). Only text
/// travels in this slice; images are #55 and files are #56.
public enum ClipKind: UInt8 {
    case text = 0
    case png = 1
    case files = 2
}

public enum ClipboardOutput: Equatable {
    /// A frame body for the session to authenticate and send.
    case send(type: UInt8, body: [UInt8])
    /// A complete payload, to be written to this computer's pasteboard.
    case deliver(kind: UInt8, bytes: [UInt8])
    /// Diagnostics, never shown to the user.
    case note(String)
}

public final class ClipboardService {
    /*
     * The largest payload this helper will assemble. The spec's default cap is
     * 10 MB; an offer above it is refused by the transfer core with a cancel
     * rather than truncated, so the far end learns why.
     */
    public static let defaultCapacity = 10 * 1024 * 1024

    /*
     * How long a transfer may make no progress before it is given up on.
     *
     * Enormously more than the link needs: a full credit window is 16 KB, which
     * at this transport's ~64 KB/s per direction is a quarter of a second. The
     * margin is deliberate — the cost of waiting too long is a stalled transfer
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
    private var pending: (kind: UInt8, bytes: [UInt8])?

    /*
     * A transfer that was already on its way out when the far helper said it
     * holds no key for the seal. The payload is still in the transfer core, so
     * nothing has to be kept here — only the fact that once a fresh seal is
     * accepted, that transfer has to start again rather than carry on into a
     * far end that never saw its offer.
     */
    private var reofferWhenSealed = false

    /*
     * The stall timeout's bookkeeping. The marks are what each direction has
     * actually done; the `…Since` stamps say when that count last moved.
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
    private var sendingSince: TimeInterval?
    private var sendingMark = 0
    private var receivingSince: TimeInterval?
    private var receivingMark: UInt32 = 0
    private var sweptSince: TimeInterval?

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

        pending = (kind.rawValue, bytes)
        return startPendingIfSealed()
    }

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
            reofferWhenSealed = false
            outputs += render(transfer.cancelOutgoing())
            outputs.append(.note("clipboard sending was turned off; anything in flight was "
                                 + "abandoned"))
        }
        if couldReceive && !mayReceive {
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
        pending = nil
        reofferWhenSealed = false
        let outputs = render(transfer.linkDown())
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

    /*
     * Give up on a transfer that has stopped moving.
     *
     * The other two interruptions #52 names — an unplug and a config-mode
     * reboot — both end this helper's own session, and `sessionEnded` already
     * abandons everything. The third does not: when the helper on the *other*
     * computer crashes, this one's session is perfectly healthy and simply
     * stops being answered. No message arrives, so nothing message-driven can
     * fire, and without this the transfer would sit holding its payload until
     * the next copy happened to supersede it.
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

        if !transfer.isSending {
            sendingSince = nil
        } else if sendingSince == nil || sendingMark != txProgress {
            sendingSince = now
            sendingMark = txProgress
        } else if now - sendingSince! >= Self.stallTimeout {
            let line = transfer.progressLine
            sendingSince = nil
            pending = nil
            reofferWhenSealed = false
            outputs += render(transfer.cancelOutgoing())
            outputs.append(.note("a transfer made no progress for \(Int(Self.stallTimeout))s and "
                                 + "was abandoned (\(line); \(drops)); the other computer's "
                                 + "helper is not answering"))
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
            return render(transfer.handleRequest(id: id)) + pump()
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

    private func onSealOffered(_ body: [UInt8]) -> [ClipboardOutput] {
        do {
            return [.send(type: MessageType.sealAccept, body: try seal.accept(offer: body))]
        } catch {
            return [.note("a seal offer could not be accepted: \(error)")]
        }
    }

    private func onSealAccepted(_ body: [UInt8]) -> [ClipboardOutput] {
        do {
            try seal.accepted(body)
        } catch {
            return [.note("a seal accept could not be used: \(error)")]
        }
        /* The copy that was waiting for exactly this, or the transfer a stale
           seal knocked back to the start. */
        if reofferWhenSealed {
            reofferWhenSealed = false
            if pending == nil { return render(transfer.reoffer()) }
        }
        return startPendingIfSealed()
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
            receivingSince = nil
            sweptSince = nil
            return render(transfer.handle(offer: offer))
        } catch SealError.unknownSeal {
            return staleReply(for: MessageType.clipOffer, body: body)
        } catch {
            return [.note("an offer could not be opened: \(error)")]
        }
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
        guard seal.canSeal else { return offerSeal() }

        pending = nil
        return render(transfer.offer(kind: waiting.kind, data: waiting.bytes))
    }

    private func offerSeal() -> [ClipboardOutput] {
        do {
            return [.send(type: MessageType.sealOffer, body: try seal.offer())]
        } catch {
            pending = nil
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
                outputs.append(.deliver(kind: payload.kind, bytes: payload.bytes))
            case DH_XFER_ACT_FAILED:
                outputs.append(.note("transfer \(action.id) was abandoned: "
                                     + Self.reason(action.reason)))
            case DH_XFER_ACT_NEED_DATA:
                /* Nothing here offers lazily, so this is the core asking for a
                   payload that was never promised. Refused rather than left as
                   a transfer that never finishes. */
                outputs.append(.note("a lazy payload was asked for, which this slice never "
                                     + "offers"))
                outputs += render(transfer.provideFail())
            default:
                outputs.append(.note("the transfer core produced action \(action.type.rawValue), "
                                     + "which this helper does not carry"))
            }
        }
        return outputs
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
        default: return "reason \(reason.rawValue)"
        }
    }
}
