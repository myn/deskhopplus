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
    private let rxBuffer: UnsafeMutableBufferPointer<UInt8>

    /*
     * The payload being sent, if any. It is *this* helper's copy, at an address
     * the core keeps until the transfer ends — a Swift array would not do, as
     * its buffer may move and its address is only borrowed for the duration of
     * a `withUnsafeBufferPointer` call.
     */
    private var txStorage: UnsafeMutableBufferPointer<UInt8>?

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
        let storage = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: max(data.count, 1))
        _ = storage.initialize(from: data)
        txStorage = storage

        return collect { acts, cap in
            meta.withUnsafeBufferPointer { m in
                dh_xfer_offer(machine, kind, m.baseAddress, UInt16(meta.count),
                              storage.baseAddress, UInt64(data.count), acts, cap)
            }
        }
    }

    /// Emit the next credit-gated batch of chunks. Empty when nothing is owed,
    /// which is the ordinary answer.
    public func pump() -> [TransferAction] {
        collect { acts, cap in dh_xfer_pump(machine, acts, cap) }
    }

    public func cancelOutgoing() -> [TransferAction] {
        collect { acts, cap in dh_xfer_cancel_tx(machine, acts, cap) }
    }

    public func cancelIncoming() -> [TransferAction] {
        collect { acts, cap in dh_xfer_cancel_rx(machine, acts, cap) }
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
        txStorage?.deallocate()
        txStorage = nil
    }

    private func collect(
        _ body: (UnsafeMutablePointer<dh_xfer_action>, Int) -> Int
    ) -> [TransferAction] {
        var raw = [dh_xfer_action](repeating: dh_xfer_action(), count: Self.actionCapacity)
        let count = raw.withUnsafeMutableBufferPointer { body($0.baseAddress!, $0.count) }
        return raw.prefix(min(count, Self.actionCapacity)).map(TransferAction.init)
    }
}
