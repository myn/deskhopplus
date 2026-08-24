import CryptoKit
import DHCore
import Foundation

/*
 * This helper's end of the clipboard seal (#113, ADR-0008).
 *
 * The bulk payload is encrypted between the two *helpers*, so the boards relay
 * ciphertext and hold no key that opens it. Everything that decides what is
 * sealed, under which key and with which nonce lives in the shared core
 * (`dh_seal.h`), for the same reason the session machine does: a rule both
 * helpers must agree on cannot have two implementations to drift between.
 *
 * What is here is the two things only this platform can provide.
 *
 * **The cipher.** ADR-0008 chose AES-256-GCM because both helpers already have
 * it — CryptoKit here, CNG on Windows — so no hand-written AES ships and the
 * firmware links none at all. `cryptoKitAEAD` below is that seam, and nothing
 * else in this file knows what the cipher is.
 *
 * **The entropy.** The core draws nothing of its own: an ephemeral key, a
 * nonce and a seal id are bytes handed in, exactly as the hello's nonce and
 * correlation value already are.
 *
 * The identity key in `SecretStore` is *not* used here. A seal's keys are
 * ephemeral, per seal, and there is no long-term identity in the exchange —
 * what vouches for the peer is that board A relays only what its own
 * registered helper authenticated (docs/protocol.md, "What each board vouches
 * for"), which ADR-0008 records as the decision's one uncomfortable lean.
 */

/// The offer that opens a transfer, once it is out of its seal.
public struct ClipOffer: Equatable {
    public var id: UInt32
    public var kind: UInt8
    public var total: UInt64
    public var meta: [UInt8]

    public init(id: UInt32, kind: UInt8, total: UInt64, meta: [UInt8] = []) {
        self.id = id
        self.kind = kind
        self.total = total
        self.meta = meta
    }
}

/// One chunk of a transfer, with the CRC32 that travelled inside the seal
/// beside it — fidelity, checked by the transfer machine.
public struct ClipChunk: Equatable {
    public var id: UInt32
    public var seq: UInt32
    public var crc32: UInt32
    public var data: [UInt8]

    public init(id: UInt32, seq: UInt32, crc32: UInt32, data: [UInt8]) {
        self.id = id
        self.seq = seq
        self.crc32 = crc32
        self.data = data
    }
}

public enum SealError: Error, Equatable {
    case malformed
    /// No key for the seal this message names — answer with SEAL_STALE.
    case unknownSeal
    /// The GCM tag did not verify: not this key's message, or it was edited.
    case notAuthenticated
    case bufferTooSmall
    /// Nothing to seal under. There is no unsealed path to fall back to.
    case noSeal
    /// An ephemeral key that is not usable, either end's.
    case badKey

    static func from(_ result: dh_seal_result) -> SealError {
        switch result {
        case DH_SEAL_ERR_UNKNOWN_ID: return .unknownSeal
        case DH_SEAL_ERR_AUTH: return .notAuthenticated
        case DH_SEAL_ERR_BUFFER: return .bufferTooSmall
        case DH_SEAL_ERR_NO_SEAL: return .noSeal
        case DH_SEAL_ERR_KEY: return .badKey
        default: return .malformed
        }
    }
}

/*
 * AES-256-GCM, from CryptoKit. Free functions rather than methods because the
 * core takes C function pointers, which cannot capture anything — the seam
 * carries no context and needs none.
 */
private let cryptoKitAEAD = dh_seal_aead(
    ctx: nil,
    seal: { _, key, nonce, aad, aadLength, plain, plainLength, cipherOut, tagOut in
        guard let key, let nonce, let plain, let cipherOut, let tagOut,
              let gcmNonce = try? AES.GCM.Nonce(
                  data: Data(bytes: nonce, count: Int(DH_SEAL_NONCE_SIZE))),
              let box = try? AES.GCM.seal(
                  Data(bytes: plain, count: plainLength),
                  using: SymmetricKey(data: Data(bytes: key, count: Int(DH_SEAL_KEY_SIZE))),
                  nonce: gcmNonce,
                  authenticating: aad.map { Data(bytes: $0, count: aadLength) } ?? Data()),
              box.ciphertext.count == plainLength,
              box.tag.count == Int(DH_SEAL_TAG_SIZE)
        else { return false }

        _ = box.ciphertext.copyBytes(to: UnsafeMutableBufferPointer(start: cipherOut,
                                                                    count: plainLength))
        _ = box.tag.copyBytes(to: UnsafeMutableBufferPointer(start: tagOut,
                                                             count: Int(DH_SEAL_TAG_SIZE)))
        return true
    },
    open: { _, key, nonce, aad, aadLength, cipher, cipherLength, tag, plainOut in
        guard let key, let nonce, let cipher, let tag, let plainOut,
              let gcmNonce = try? AES.GCM.Nonce(
                  data: Data(bytes: nonce, count: Int(DH_SEAL_NONCE_SIZE))),
              let box = try? AES.GCM.SealedBox(
                  nonce: gcmNonce,
                  ciphertext: Data(bytes: cipher, count: cipherLength),
                  tag: Data(bytes: tag, count: Int(DH_SEAL_TAG_SIZE))),
              let plain = try? AES.GCM.open(
                  box,
                  using: SymmetricKey(data: Data(bytes: key, count: Int(DH_SEAL_KEY_SIZE))),
                  authenticating: aad.map { Data(bytes: $0, count: aadLength) } ?? Data()),
              plain.count == cipherLength
        else { return false }

        _ = plain.copyBytes(to: UnsafeMutableBufferPointer(start: plainOut, count: cipherLength))
        return true
    })

/// Both directions of this helper's seal: the one it offered for what it
/// sends, and the one it accepted for what it receives.
public final class ClipboardSeal {
    /// The largest body either direction can produce, which is a frame's
    /// payload less the hop's authentication prefix.
    public static let maxBody = Int(DH_FRAME_MAX_PAYLOAD) - Int(DH_FRAME_AUTH_PREFIX_SIZE)
    /// The largest chunk of user data one frame can carry once sealed.
    public static let maxChunkData = Int(DH_SEAL_MAX_CHUNK_DATA)

    /* Heap-allocated with stable addresses, like the session machine's state:
       the core is handed pointers to these. */
    private let tx = UnsafeMutablePointer<dh_seal_tx>.allocate(capacity: 1)
    private let rx = UnsafeMutablePointer<dh_seal_rx>.allocate(capacity: 1)
    private let entropy: (Int) -> [UInt8]

    /// `entropy` must return exactly the number of bytes it is asked for: it
    /// feeds an ephemeral private key and a nonce, so a short draw would key a
    /// seal on bytes nobody chose.
    public init(entropy: @escaping (Int) -> [UInt8]) {
        self.entropy = entropy
        tx.initialize(to: dh_seal_tx())
        rx.initialize(to: dh_seal_rx())
        dh_seal_tx_init(tx)
        dh_seal_rx_init(rx)
    }

    deinit {
        tx.deallocate()
        rx.deallocate()
    }

    /// Both halves are dropped when this helper's session ends. Re-offering is
    /// cheap; the alternative is a key whose peer may no longer exist, and
    /// #107 measured 586 teardowns in sixteen hours.
    public func reset() {
        dh_seal_tx_init(tx)
        dh_seal_rx_init(rx)
    }

    /// Whether a payload can go out right now, or a seal has to be offered
    /// first. There is no third answer: a payload never goes out unsealed.
    public var canSeal: Bool { tx.pointee.live }

    /// The seal this end will open incoming payloads with, if it holds one.
    public var acceptedSealID: UInt32? { rx.pointee.live ? rx.pointee.seal_id : nil }

    // MARK: - The exchange

    /// The SEAL_OFFER body for a fresh outgoing seal. Whatever this end held
    /// is discarded: the offerer owns the seal.
    public func offer() throws -> [UInt8] {
        let sealID = try drawSealID()
        let nonce = try draw(Int(DH_NONCE_SIZE))
        return try withFreshKey { privateKey, out, capacity, written in
            dh_seal_tx_offer(tx, sealID, privateKey, nonce, out, capacity, &written)
        }
    }

    /// The peer answered this end's offer: from here it can seal.
    public func accepted(_ body: [UInt8]) throws {
        let rc = body.withUnsafeBufferPointer { dh_seal_tx_accepted(tx, $0.baseAddress, $0.count) }
        guard rc == DH_SEAL_OK else { throw SealError.from(rc) }
    }

    /// The peer offered a seal: derive this end's key and answer with the
    /// SEAL_ACCEPT body that closes the exchange.
    public func accept(offer body: [UInt8]) throws -> [UInt8] {
        let nonce = try draw(Int(DH_NONCE_SIZE))
        return try withFreshKey { privateKey, out, capacity, written in
            body.withUnsafeBufferPointer { offered in
                dh_seal_rx_offered(rx, offered.baseAddress, offered.count, privateKey, nonce, out,
                                   capacity, &written)
            }
        }
    }

    /// The peer holds no key for a seal it was sent. If it is this end's
    /// current one, it is discarded and the next payload waits on a fresh
    /// offer; a stale naming any other seal changes nothing.
    @discardableResult
    public func discardSeal(_ sealID: UInt32) -> Bool { dh_seal_tx_stale(tx, sealID) }

    // MARK: - The payload

    public func seal(_ offer: ClipOffer) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: Self.maxBody)
        var written = 0
        let rc = offer.meta.withUnsafeBufferPointer { meta in
            var message = dh_clip_offer(id: offer.id, kind: offer.kind, total: offer.total,
                                        meta: meta.baseAddress,
                                        meta_len: UInt16(offer.meta.count))
            return out.withUnsafeMutableBufferPointer {
                dh_seal_encode_offer(tx, sealAEAD, &message, $0.baseAddress, $0.count, &written)
            }
        }
        guard rc == DH_SEAL_OK else { throw SealError.from(rc) }
        return Array(out.prefix(written))
    }

    public func seal(_ chunk: ClipChunk) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: Self.maxBody)
        var written = 0
        let rc = chunk.data.withUnsafeBufferPointer { data in
            var message = dh_clip_chunk(id: chunk.id, seq: chunk.seq, crc32: chunk.crc32,
                                        data: data.baseAddress,
                                        data_len: UInt16(chunk.data.count))
            return out.withUnsafeMutableBufferPointer {
                dh_seal_encode_chunk(tx, sealAEAD, &message, $0.baseAddress, $0.count, &written)
            }
        }
        guard rc == DH_SEAL_OK else { throw SealError.from(rc) }
        return Array(out.prefix(written))
    }

    public func openOffer(_ body: [UInt8]) throws -> ClipOffer {
        var plain = [UInt8](repeating: 0, count: Self.maxBody)
        var message = dh_clip_offer()
        let rc = body.withUnsafeBufferPointer { sealed in
            plain.withUnsafeMutableBufferPointer { out in
                dh_seal_open_offer(rx, sealAEAD, sealed.baseAddress, sealed.count,
                                   out.baseAddress, out.count, &message)
            }
        }
        guard rc == DH_SEAL_OK else { throw SealError.from(rc) }
        /* `message.meta` views `plain`, which is about to go out of scope. */
        let metaLength = Int(message.meta_len)
        let meta = metaLength > 0 ? Array(plain[Int(DH_CLIP_OFFER_PLAIN_FIXED)...]
                                            .prefix(metaLength))
                                  : []
        return ClipOffer(id: message.id, kind: message.kind, total: message.total, meta: meta)
    }

    public func openChunk(_ body: [UInt8]) throws -> ClipChunk {
        var plain = [UInt8](repeating: 0, count: Self.maxBody)
        var message = dh_clip_chunk()
        let rc = body.withUnsafeBufferPointer { sealed in
            plain.withUnsafeMutableBufferPointer { out in
                dh_seal_open_chunk(rx, sealAEAD, sealed.baseAddress, sealed.count,
                                   out.baseAddress, out.count, &message)
            }
        }
        guard rc == DH_SEAL_OK else { throw SealError.from(rc) }
        let length = Int(message.data_len)
        let data = Array(plain[Int(DH_CLIP_CHUNK_PLAIN_FIXED)...].prefix(length))
        return ClipChunk(id: message.id, seq: message.seq, crc32: message.crc32, data: data)
    }

    // MARK: - SEAL_STALE, and reading a clear head

    /// Which seal a sealed message names, from its clear head alone — what
    /// this end needs to say SEAL_STALE for a key it does not hold.
    public static func sealID(ofMessage type: UInt8, body: [UInt8]) -> UInt32? {
        var id: UInt32 = 0
        let ok = body.withUnsafeBufferPointer { dh_seal_peek_id(type, $0.baseAddress, $0.count, &id) }
        return ok ? id : nil
    }

    public static func staleBody(_ sealID: UInt32) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: Int(DH_SEAL_STALE_LEN))
        let n = out.withUnsafeMutableBufferPointer {
            dh_seal_encode_stale(sealID, $0.baseAddress, $0.count)
        }
        return n > 0 ? Array(out.prefix(Int(n))) : []
    }

    public static func sealID(fromStale body: [UInt8]) -> UInt32? {
        var id: UInt32 = 0
        let ok = body.withUnsafeBufferPointer { dh_seal_decode_stale($0.baseAddress, $0.count, &id) }
        return ok ? id : nil
    }

    // MARK: - Internals

    private func draw(_ count: Int) throws -> [UInt8] {
        let bytes = entropy(count)
        guard bytes.count == count else { throw SealError.badKey }
        return bytes
    }

    private func drawSealID() throws -> UInt32 {
        let bytes = try draw(4)
        return UInt32(bytes[0]) | UInt32(bytes[1]) << 8 | UInt32(bytes[2]) << 16
            | UInt32(bytes[3]) << 24
    }

    /*
     * Random 32 bytes are a usable P-256 scalar all but about once in 2^32
     * draws, which is rare enough to be a retry and far too common to be a
     * crash. A handful of attempts is already beyond any plausible run of bad
     * luck; past that, the entropy source is what is wrong.
     */
    private func withFreshKey(
        _ body: (UnsafePointer<UInt8>, UnsafeMutablePointer<UInt8>, Int, inout Int)
            -> dh_seal_result
    ) throws -> [UInt8] {
        var out = [UInt8](repeating: 0, count: Int(DH_SEAL_EXCHANGE_LEN))
        for _ in 0..<8 {
            let privateKey = try draw(Int(DH_P256_PRIVATE_SIZE))
            var written = 0
            let rc = privateKey.withUnsafeBufferPointer { key in
                out.withUnsafeMutableBufferPointer { buffer in
                    body(key.baseAddress!, buffer.baseAddress!, buffer.count, &written)
                }
            }
            if rc == DH_SEAL_OK { return Array(out.prefix(written)) }
            guard rc == DH_SEAL_ERR_KEY else { throw SealError.from(rc) }
        }
        throw SealError.badKey
    }
}

/*
 * The seam handed to the core, at a stable address for the life of the
 * process. Allocated rather than a `var` whose address is taken, because the
 * address of a Swift variable is only valid inside the call that borrows it.
 */
private let sealAEAD: UnsafePointer<dh_seal_aead> = {
    let storage = UnsafeMutablePointer<dh_seal_aead>.allocate(capacity: 1)
    storage.initialize(to: cryptoKitAEAD)
    return UnsafePointer(storage)
}()
