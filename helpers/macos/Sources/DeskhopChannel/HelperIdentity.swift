import DHCore

/*
 * This helper's key pair, abstracted because the private half is unreachable:
 * it lives in the Secure Enclave and cannot be handed to C at all. What the
 * enclave *can* do is one ECDH, so that is the whole seam — the same one
 * `dh_helper_identity.ecdh` is shaped around (#80).
 *
 * The HKDF over the result stays in the shared core. A helper deriving its own
 * session keys would be a second implementation of the rule both ends must
 * agree on, which is what lifting the machine into C exists to prevent — so
 * the two `derive*` methods this protocol used to carry are gone with the
 * Swift session machine, and `tests/helper_test.c` covers what they did.
 */
public protocol HelperIdentity {
    /// 64 raw bytes, X || Y, big-endian — the format the wire uses.
    var publicKey: [UInt8] { get }
    /// SHA-256(publicKey)[0..8], the id the hello carries.
    var keyId: [UInt8] { get }

    /// ECDH against the board's public key — the raw 32-byte X coordinate of
    /// the agreed point, which is what the board's `dh_p256_ecdh` writes.
    ///
    /// `nil` when the enclave will not answer, or the board's key is not a
    /// point on the curve. The core treats both the same way: a device this
    /// helper cannot use, reported as one rather than passed off as a frame
    /// that would not encode.
    func sharedSecret(with boardPublicKey: [UInt8]) -> [UInt8]?
}
