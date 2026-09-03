import DHCore
import Foundation

/*
 * The chunked transfer machine (`dh_xfer`, #48), as a binding rather than a
 * machine of its own — the same shape HelperSession has, and for the same
 * reason: credit, retransmission, the received-set and abandon-on-drop are
 * rules both helpers must agree on, so they have one implementation and two
 * bindings.
 *
 * Two things this file owns, because C cannot: keeping the outgoing payload
 * alive at a stable address for as long as the core holds a pointer into it,
 * and giving the received one somewhere to assemble.
 */

/*
 * The transfer's unsealed messages: a transfer id, sometimes a sequence
 * number, and nothing else. They carry no user bytes, so sealing them would
 * add sixteen bytes each to hide nothing (docs/protocol.md).
 *
 * The offer's *head* is here for the same reason it exists at all — a receiver
 * has to read which transfer and which seal a message names before it holds
 * anything it can trust, and a helper that is refusing the transfer outright
 * never opens the seal.
 */
public enum ClipCodec {
    public static func id(_ id: UInt32) -> [UInt8] {
        encode(4) { dh_clip_encode_id(id, $0, $1) }
    }

    public static func retransmit(id: UInt32, seq: UInt32) -> [UInt8] {
        encode(8) { dh_clip_encode_retransmit(id, seq, $0, $1) }
    }

    public static func credit(id: UInt32, credits: UInt16) -> [UInt8] {
        encode(6) { dh_clip_encode_credit(id, credits, $0, $1) }
    }

    public static func decodeID(_ body: [UInt8]) -> UInt32? {
        var value: UInt32 = 0
        let ok = body.withUnsafeBufferPointer { dh_clip_decode_id($0.baseAddress, $0.count, &value) }
        return ok ? value : nil
    }

    public static func decodeRetransmit(_ body: [UInt8]) -> (id: UInt32, seq: UInt32)? {
        var id: UInt32 = 0
        var seq: UInt32 = 0
        let ok = body.withUnsafeBufferPointer {
            dh_clip_decode_retransmit($0.baseAddress, $0.count, &id, &seq)
        }
        return ok ? (id, seq) : nil
    }

    public static func decodeCredit(_ body: [UInt8]) -> (id: UInt32, credits: UInt16)? {
        var id: UInt32 = 0
        var credits: UInt16 = 0
        let ok = body.withUnsafeBufferPointer {
            dh_clip_decode_credit($0.baseAddress, $0.count, &id, &credits)
        }
        return ok ? (id, credits) : nil
    }

    /// Which transfer a sealed offer names, from its clear head alone.
    public static func offerID(ofSealedOffer body: [UInt8]) -> UInt32? {
        var head = dh_clip_offer_head()
        let ok = body.withUnsafeBufferPointer {
            dh_clip_decode_offer_head($0.baseAddress, $0.count, &head)
        }
        return ok ? head.id : nil
    }

    private static func encode(_ capacity: Int,
                               _ body: (UnsafeMutablePointer<UInt8>, Int) -> Int32) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: capacity)
        let written = out.withUnsafeMutableBufferPointer { body($0.baseAddress!, $0.count) }
        /* The capacities above are the layouts in dh_clip.h. A negative return
           is this file disagreeing with that header, which is a bug here
           rather than anything a caller can be handed. */
        precondition(written > 0, "a clipboard control message would not encode")
        return Array(out.prefix(Int(written)))
    }
}

/// One thing the transfer machine wants done. A thin view of `dh_xfer_action`;
/// the fields it does not use for a given type are zero.
public struct TransferAction: Equatable {
    public let type: dh_xfer_action_type
    public let reason: dh_xfer_fail_reason
    public let credits: UInt16
    public let id: UInt32
    public let seq: UInt32

    init(_ raw: dh_xfer_action) {
        type = dh_xfer_action_type(rawValue: UInt32(raw.type))
        reason = dh_xfer_fail_reason(rawValue: UInt32(raw.reason))
        credits = raw.credits
        id = raw.id
        seq = raw.seq
    }
}

public final class Transfer {
    /*
     * Big enough for the widest call. Every entry point but one is bounded by
     * DH_XFER_BATCH_MAX + 2; `handle_done` sweeps for gaps and wants one action
     * per missing chunk, which it truncates safely — a truncated round of
     * re-requests simply repeats at the next DONE.
     */
    private static let actionCapacity = 64

    /* Heap-allocated with stable addresses: `dh_xfer` is far too large to copy
       in and out of a Swift value, and the core keeps `rx_buf` for its life. */
    private let machine = UnsafeMutablePointer<dh_xfer>.allocate(capacity: 1)
    /* Not `let`: the buffer is sized against the board's clipboard size cap,
       which the board states per session and can change while this helper is
       running (#56). */
    private var rxBuffer: UnsafeMutableBufferPointer<UInt8>

    /*
     * The payload being sent, if any. It is *this* helper's copy, at an address
     * the core keeps until the transfer ends — a Swift array would not do, as
     * its buffer may move and its address is only borrowed for the duration of
     * a `withUnsafeBufferPointer` call.
     */
    private var txStorage: UnsafeMutableBufferPointer<UInt8>?

    /*
     * The offer's metadata, held the same way and for the same reason: `dh_xfer`
     * keeps `tx.meta` as a bare pointer for the whole transfer and hands it
     * back from `dh_xfer_offer_info` on every re-offer, so a pointer borrowed
     * for the duration of one `withUnsafeBufferPointer` call would dangle.
     *
     * Text and images carry none; a file offer's list lives here for the whole
     * of the transfer, which since #56 is minutes rather than moments — the
     * offer goes out, is held for the user, and is re-offered on every seal
     * that goes stale in between. A pointer borrowed for one call would dangle
     * across all of that, and would appear as a payload occasionally wrong
     * rather than as a crash.
     */
    private var metaStorage: UnsafeMutableBufferPointer<UInt8>?

    /// `capacity` is the largest payload this helper will accept; a bigger
    /// offer is refused with a cancel by the core rather than truncated.
    public init(capacity: Int) {
        rxBuffer = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: capacity)
        rxBuffer.initialize(repeating: 0)
        machine.initialize(to: dh_xfer())
        dh_xfer_init(machine, rxBuffer.baseAddress, capacity)
    }

    deinit {
        releaseOutgoing()
        rxBuffer.deallocate()
        machine.deallocate()
    }

    // MARK: - What this end starts

    /// Offer a payload eagerly. The bytes are copied, because the core holds a
    /// pointer to them until the transfer ends and the caller's array will not
    /// stay put. A new offer supersedes anything already in flight.
    public func offer(kind: UInt8, meta: [UInt8] = [], data: [UInt8]) -> [TransferAction] {
        releaseOutgoing()
        txAwaitingData = false
        let storage = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: max(data.count, 1))
        _ = storage.initialize(from: data)
        txStorage = storage

        let metaCopy = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: max(meta.count, 1))
        _ = metaCopy.initialize(from: meta)
        metaStorage = metaCopy

        return collect { acts, cap in
            dh_xfer_offer(machine, kind, meta.isEmpty ? nil : metaCopy.baseAddress,
                          UInt16(meta.count), storage.baseAddress, UInt64(data.count), acts, cap)
        }
    }


    /*
     * Offer a payload that has not been read yet (#56).
     *
     * Files are lazy because reading them costs real time and real memory, and
     * a copy the user never pastes should cost neither. Nothing leaves until
     * the far end asks — and on this project the far end asks only once its
     * own user has said yes, so a copied folder that is never pasted is never
     * even opened.
     *
     * `total` is what the offer promises and what `provide` must then deliver
     * exactly; the metadata is where the file names and their individual sizes
     * live (FileList).
     */
    public func offerLazy(kind: UInt8, meta: [UInt8], total: UInt64) -> [TransferAction] {
        releaseOutgoing()
        txAwaitingData = true

        let metaCopy = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: max(meta.count, 1))
        _ = metaCopy.initialize(from: meta)
        metaStorage = metaCopy

        return collect { acts, cap in
            dh_xfer_offer(machine, kind, meta.isEmpty ? nil : metaCopy.baseAddress,
                          UInt16(meta.count), nil, total, acts, cap)
        }
    }

    /// Answer NEED_DATA with the bytes. Copied, for the reason `offer` copies:
    /// the core keeps a pointer to them until the transfer ends.
    public func provide(data: [UInt8]) -> [TransferAction] {
        txAwaitingData = false
        let storage = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: max(data.count, 1))
        _ = storage.initialize(from: data)
        /* Handed over before the old buffer is released, not after: between the
           two the core is holding whichever pointer it was last given, and it
           must never be one that has been freed. */
        let actions = collect { acts, cap in
            dh_xfer_provide(machine, storage.baseAddress, acts, cap)
        }
        let previous = txStorage
        txStorage = storage
        previous?.deallocate()
        return actions
    }

    /*
     * Point the receive buffer at a buffer of a different size, because the
     * board stated a different clipboard size cap (#56).
     *
     * False when the core refuses — something is arriving, and it is
     * assembling into the buffer this would move. The old buffer is kept in
     * that case rather than freed, because the core is still writing into it.
     */
    public func setReceiveCapacity(_ bytes: Int) -> Bool {
        guard bytes > 0 else { return false }
        guard bytes != rxBuffer.count else { return true }
        /* Asked before the allocation, not after: a caller retrying a refused
           swap on its tick would otherwise allocate and free up to 64 MB
           several times a second for the whole of the transfer it is waiting
           on. */
        guard canSetReceiveCapacity else { return false }
        let replacement = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: bytes)
        replacement.initialize(repeating: 0)
        guard dh_xfer_set_rx_buffer(machine, replacement.baseAddress, bytes) else {
            replacement.deallocate()
            return false
        }
        rxBuffer.deallocate()
        rxBuffer = replacement
        return true
    }

    /// How large a payload this end will accept, in bytes.
    public var receiveCapacity: Int { rxBuffer.count }

    /// Whether a change of capacity would be accepted right now. False while
    /// anything is arriving, a held offer included.
    public var canSetReceiveCapacity: Bool { dh_xfer_can_set_rx_buffer(machine) }

    /*
     * Offer the payload already in flight again, as a fresh transfer.
     *
     * This is what a SEAL_STALE costs: the far end holds no key for the seal
     * the offer went out under, so as far as it is concerned the transfer never
     * happened, and re-sending the same offer under a new key would leave the
     * two ends disagreeing about which transfer id is live. Starting over is
     * the honest form of that.
     *
     * The bytes do not move — only the transfer around them starts again — so
     * this costs no copy of the payload.
     */
    public func reoffer() -> [TransferAction] {
        guard let current = outgoingOffer() else { return [] }
        /* A lazy transfer whose payload was never asked for has no bytes to
           point at, and must not acquire any here: re-offering it eagerly
           would read the files at the moment a seal went stale rather than at
           the moment someone asked for them (#56). */
        if txAwaitingData || txStorage == nil {
            let meta = metaStorage
            let metaLength = Int(current.meta.count)
            txAwaitingData = true
            return collect { acts, cap in
                dh_xfer_offer(machine, current.kind, metaLength > 0 ? meta?.baseAddress : nil,
                              UInt16(metaLength), nil, current.total, acts, cap)
            }
        }
        guard let storage = txStorage else { return [] }
        /* `metaStorage` is the same buffer the core is already holding, so this
           re-offers the metadata in place rather than handing over a pointer
           that dies with this call. */
        let meta = metaStorage
        let metaLength = Int(current.meta.count)
        return collect { acts, cap in
            dh_xfer_offer(machine, current.kind, metaLength > 0 ? meta?.baseAddress : nil,
                          UInt16(metaLength), storage.baseAddress, current.total, acts, cap)
        }
    }


    /// Whether the outgoing transfer is a lazy one whose payload has not been
    /// read yet. A stale seal re-offers such a transfer lazily, and a NEED_DATA
    /// for one this end never promised is a defect rather than a request.
    public private(set) var txAwaitingData = false

    /// What is arriving: its kind, how much is promised, and how much of it has
    /// landed. Nil when nothing is. `received` is chunk-granular and never
    /// overstates — the last chunk is short and the total is what bounds it.
    public var arriving: (kind: UInt8, received: UInt64, total: UInt64)? {
        guard isReceiving else { return nil }
        let total = dh_xfer_delivered_len(machine)
        let assembled = UInt64(receivedChunks) * UInt64(DH_XFER_CHUNK_SIZE)
        return (dh_xfer_delivered_kind(machine), min(assembled, total), total)
    }

    /// Whether a payload is on its way out — the question a stale seal has to
    /// ask before it knows whether there is anything to start again.
    public var isSending: Bool { dh_xfer_is_sending(machine) }

    /// Whether a payload is arriving.
    public var isReceiving: Bool { dh_xfer_is_receiving(machine) }

    /// Whether anything at all is incoming, a lazy offer held for a decision
    /// included. Wider than `isReceiving`, and the honest way to ask whether an
    /// incoming transfer is over — a transfer id cannot answer that, because
    /// ids collide across the two directions (#136).
    public var isIncomingBusy: Bool { dh_xfer_rx_busy(machine) }

    /// Whether an offer is being held for a decision — accepted into the
    /// machine, nothing requested, not one byte moving. The question to ask
    /// before putting an offer to the user; `receivedOfferID` alone is not,
    /// because it survives an offer the machine has already refused.
    public var isIncomingHeld: Bool { dh_xfer_rx_is_held(machine) }

    /// How far each direction has got, for a stall that has to say more than
    /// "no progress" — which covers a transfer whose chunks never arrived and
    /// one whose chunks all arrived and were refused.
    ///
    /// The re-request pair is on both directions and always printed, zeros
    /// included: a stall where nothing was ever asked for again is a different
    /// fault from one where it was asked for and nothing came back, and
    /// neither end said which before #145.
    public var progressLine: String {
        var parts: [String] = []
        if isSending {
            parts.append("sending \(dh_xfer_tx_next_seq(machine))/\(dh_xfer_tx_chunks(machine)) "
                         + "chunks" + (dh_xfer_tx_streaming(machine) ? "" : ", never requested")
                         + ", produced \(dh_xfer_tx_offer_retries(machine)) offer retries"
                         + ", asked for \(dh_xfer_tx_retx_asked(machine)) again and sent "
                         + "\(dh_xfer_tx_retx_sent(machine))")
        }
        if isReceiving {
            parts.append("receiving \(dh_xfer_rx_received(machine))/"
                         + "\(dh_xfer_rx_chunks(machine)) chunks, asked for "
                         + "\(dh_xfer_rx_retx_asked(machine)) again and got "
                         + "\(dh_xfer_rx_retx_answered(machine)) back, observed "
                         + "\(dh_xfer_rx_duplicate_offers(machine)) duplicate offers")
        }
        return parts.isEmpty ? "nothing in flight" : parts.joined(separator: ", ")
    }

    /// Chunks this end has assembled — read before and after handing one over,
    /// which is the only way to see the machine refuse one: a chunk for the
    /// wrong transfer, out of range, or with a CRC32 that does not match is
    /// dropped with no action at all.
    public var receivedChunks: UInt32 { dh_xfer_rx_received(machine) }

    /// Emit the next credit-gated batch of chunks. Empty when nothing is owed,
    /// which is the ordinary answer.
    public func pump() -> [TransferAction] {
        collect { acts, cap in dh_xfer_pump(machine, acts, cap) }
    }

    /// Ask again for what an arriving transfer is waiting on. The core has no
    /// clock, so the caller's tick decides when — see `dh_xfer_sweep_rx`.
    public func sweepReceive() -> [TransferAction] {
        collect { acts, cap in dh_xfer_sweep_rx(machine, acts, cap) }
    }

    public func retryOffer() -> [TransferAction] {
        collect { acts, cap in dh_xfer_retry_offer(machine, acts, cap) }
    }

    public var isAwaitingRequest: Bool { dh_xfer_tx_awaiting_request(machine) }
    public var offerRetries: UInt32 { dh_xfer_tx_offer_retries(machine) }
    public var duplicateOffers: UInt32 { dh_xfer_rx_duplicate_offers(machine) }
    public var receivedOfferID: UInt32? {
        dh_xfer_rx_has_offer(machine) ? dh_xfer_rx_offer_id(machine) : nil
    }

    /// Answer NEED_DATA with a refusal: the files could not be read. The
    /// transfer fails rather than hanging, and the far end is told.
    public func provideFail() -> [TransferAction] {
        txAwaitingData = false
        return collect { acts, cap in dh_xfer_provide_fail(machine, acts, cap) }
    }

    public func cancelOutgoing() -> [TransferAction] {
        collect { acts, cap in dh_xfer_cancel_tx(machine, acts, cap) }
    }

    public func cancelIncoming() -> [TransferAction] {
        collect { acts, cap in dh_xfer_cancel_rx(machine, acts, cap) }
    }

    /// The far helper offered a fresh seal, so its process — and the offer-id
    /// namespace ordered inside it — started over. The incoming direction is
    /// forgotten, ordering included; the outgoing one is untouched. See
    /// `dh_xfer_rx_seal_replaced`.
    public func incomingSealReplaced() -> [TransferAction] {
        collect { acts, cap in dh_xfer_rx_seal_replaced(machine, acts, cap) }
    }

    /// The session went away: both directions are abandoned. Partial data is
    /// never kept — a half-written clipboard is worse than none.
    public func linkDown() -> [TransferAction] {
        let actions = collect { acts, cap in dh_xfer_link_down(machine, acts, cap) }
        releaseOutgoing()
        return actions
    }

    // MARK: - What the far helper says

    public func handle(offer: ClipOffer) -> [TransferAction] {
        collect { acts, cap in
            offer.meta.withUnsafeBufferPointer { m in
                var message = dh_clip_offer(id: offer.id, kind: offer.kind, total: offer.total,
                                            meta: m.baseAddress,
                                            meta_len: UInt16(offer.meta.count))
                return dh_xfer_handle_offer(machine, &message, acts, cap)
            }
        }
    }

    public func handleLazy(offer: ClipOffer) -> [TransferAction] {
        offer.meta.withUnsafeBytes { meta in
            var message = dh_clip_offer(id: offer.id, kind: offer.kind, total: offer.total,
                                        meta: meta.isEmpty ? nil : meta.bindMemory(to: UInt8.self).baseAddress,
                                        meta_len: UInt16(meta.count))
            return collect { acts, cap in
                dh_xfer_handle_offer_lazy(machine, &message, acts, cap)
            }
        }
    }

    public func requestLazy(id: UInt32) -> [TransferAction] {
        collect { acts, cap in dh_xfer_request_lazy(machine, id, acts, cap) }
    }

    public func handle(chunk: ClipChunk) -> [TransferAction] {
        collect { acts, cap in
            chunk.data.withUnsafeBufferPointer { d in
                var message = dh_clip_chunk(id: chunk.id, seq: chunk.seq, crc32: chunk.crc32,
                                            data: d.baseAddress, data_len: UInt16(chunk.data.count))
                return dh_xfer_handle_chunk(machine, &message, acts, cap)
            }
        }
    }

    public func handleRequest(id: UInt32) -> [TransferAction] {
        collect { acts, cap in dh_xfer_handle_request(machine, id, acts, cap) }
    }

    public func handleDone(id: UInt32) -> [TransferAction] {
        collect { acts, cap in dh_xfer_handle_done(machine, id, acts, cap) }
    }

    public func handleCancel(id: UInt32) -> [TransferAction] {
        collect { acts, cap in dh_xfer_handle_cancel(machine, id, acts, cap) }
    }

    public func handleRetransmit(id: UInt32, seq: UInt32) -> [TransferAction] {
        collect { acts, cap in dh_xfer_handle_retransmit(machine, id, seq, acts, cap) }
    }

    public func handleCredit(id: UInt32, credits: UInt16) -> [TransferAction] {
        collect { acts, cap in dh_xfer_handle_credit(machine, id, credits, acts, cap) }
    }

    // MARK: - Reading what is in flight

    /// The wire form of the outgoing offer, or nil when there is no transfer.
    public func outgoingOffer() -> ClipOffer? {
        var message = dh_clip_offer()
        guard dh_xfer_offer_info(machine, &message) else { return nil }
        let meta = message.meta.map { Array(UnsafeBufferPointer(start: $0,
                                                               count: Int(message.meta_len))) }
        return ClipOffer(id: message.id, kind: message.kind, total: message.total, meta: meta ?? [])
    }

    /// One chunk of the outgoing payload, with its CRC32 computed. Nil when
    /// there is no such chunk in flight.
    public func outgoingChunk(seq: UInt32) -> ClipChunk? {
        var message = dh_clip_chunk()
        guard dh_xfer_chunk_at(machine, seq, &message), let data = message.data else { return nil }
        return ClipChunk(id: message.id, seq: message.seq, crc32: message.crc32,
                         data: Array(UnsafeBufferPointer(start: data,
                                                         count: Int(message.data_len))))
    }

    /// What was delivered, valid until the next incoming offer.
    public func delivered() -> (kind: UInt8, bytes: [UInt8]) {
        let length = Int(dh_xfer_delivered_len(machine))
        let bytes = Array(UnsafeBufferPointer(start: rxBuffer.baseAddress,
                                              count: min(length, rxBuffer.count)))
        return (dh_xfer_delivered_kind(machine), bytes)
    }

    // MARK: - Internals

    private func releaseOutgoing() {
        txAwaitingData = false
        txStorage?.deallocate()
        txStorage = nil
        metaStorage?.deallocate()
        metaStorage = nil
    }

    private func collect(
        _ body: (UnsafeMutablePointer<dh_xfer_action>, Int) -> Int
    ) -> [TransferAction] {
        var raw = [dh_xfer_action](repeating: dh_xfer_action(), count: Self.actionCapacity)
        let count = raw.withUnsafeMutableBufferPointer { body($0.baseAddress!, $0.count) }
        return raw.prefix(min(count, Self.actionCapacity)).map(TransferAction.init)
    }
}
