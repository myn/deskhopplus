import DHCore
import Foundation

/*
 * The helper's identity, abstracted so the engine can be tested without the
 * Secure Enclave. Both implementations hold a P-256 key pair and compute the
 * three session keys the protocol needs; only EnclaveIdentity keeps the
 * private half non-extractable.
 */

public protocol HelperIdentity {
    /// 64 raw bytes, X || Y, big-endian — the format the wire uses.
    var publicKey: [UInt8] { get }
    /// SHA-256(publicKey)[0..8], the id the hello carries.
    var keyId: [UInt8] { get }

    /// k_hello, derived from ECDH(self, boardPublicKey) + helperNonce.
    func deriveHelloKey(boardPublicKey: [UInt8], helperNonce: [UInt8]) throws -> [UInt8]

    /// (k_h2b, k_b2h), derived from ECDH(self, boardPublicKey) + both nonces.
    func deriveSessionKeys(boardPublicKey: [UInt8], helperNonce: [UInt8],
                           boardNonce: [UInt8]) throws -> (kH2B: [UInt8], kB2H: [UInt8])
}

public enum IdentityError: Error, Equatable {
    case badKey
    case badNonce
    case ecdhFailed
}

/*
 * The C derivation functions take `const uint8_t[DH_NONCE_SIZE]`, which in
 * Swift is a bare pointer with the length in the comment. A short array read
 * through it is an out-of-bounds read, not a compile error — and the nonce
 * reaches here from an injectable entropy closure. Checked once, here, so both
 * implementations get it.
 */
func checkNonce(_ nonce: [UInt8]) throws {
    guard nonce.count == Int(DH_NONCE_SIZE) else { throw IdentityError.badNonce }
}

/// A software P-256 identity using the C core. Deterministic when the private
/// key is fixed, which is what makes it testable.
public final class SoftwareIdentity: HelperIdentity {
    public let publicKey: [UInt8]
    public let keyId: [UInt8]
    private let privateKey: [UInt8]

    public init(privateKey: [UInt8]) throws {
        guard privateKey.count == Int(DH_P256_PRIVATE_SIZE) else { throw IdentityError.badKey }
        var pub = [UInt8](repeating: 0, count: Int(DH_P256_PUBLIC_SIZE))
        guard privateKey.withUnsafeBufferPointer({ priv in
            pub.withUnsafeMutableBufferPointer { out in
                dh_p256_public_from_private(priv.baseAddress, out.baseAddress)
            }
        }) else { throw IdentityError.badKey }

        var kid = [UInt8](repeating: 0, count: Int(DH_KEY_ID_SIZE))
        pub.withUnsafeBufferPointer { pubPtr in
            kid.withUnsafeMutableBufferPointer { out in
                dh_p256_key_id(pubPtr.baseAddress, out.baseAddress)
            }
        }

        self.privateKey = privateKey
        self.publicKey = pub
        self.keyId = kid
    }

    public func deriveHelloKey(boardPublicKey: [UInt8], helperNonce: [UInt8]) throws -> [UInt8] {
        try checkNonce(helperNonce)
        return deriveHelloKeyFromSharedSecret(try ecdh(boardPublicKey: boardPublicKey),
                                              helperNonce: helperNonce)
    }

    public func deriveSessionKeys(boardPublicKey: [UInt8], helperNonce: [UInt8],
                                  boardNonce: [UInt8]) throws -> (kH2B: [UInt8], kB2H: [UInt8]) {
        try checkNonce(helperNonce)
        try checkNonce(boardNonce)
        return deriveSessionKeysFromSharedSecret(try ecdh(boardPublicKey: boardPublicKey),
                                                 helperNonce: helperNonce,
                                                 boardNonce: boardNonce)
    }

    private func ecdh(boardPublicKey: [UInt8]) throws -> [UInt8] {
        guard boardPublicKey.count == Int(DH_P256_PUBLIC_SIZE) else { throw IdentityError.badKey }
        var ss = [UInt8](repeating: 0, count: Int(DH_P256_SHARED_SIZE))
        let ok = privateKey.withUnsafeBufferPointer { priv in
            boardPublicKey.withUnsafeBufferPointer { peer in
                ss.withUnsafeMutableBufferPointer { out in
                    dh_p256_ecdh(priv.baseAddress, peer.baseAddress, out.baseAddress)
                }
            }
        }
        guard ok else { throw IdentityError.ecdhFailed }
        return ss
    }
}
